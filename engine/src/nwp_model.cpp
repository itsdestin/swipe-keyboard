// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "swipekb/nwp_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace swipekb {
namespace {
constexpr char kMagic[8] = {'S', 'W', 'K', 'B', 'N', 'W', 'P', '1'};
constexpr size_t kHeader = 40;   // magic + 6 x u32 + 2 x f32

// FNV-1a 64: the same byte-wise definition as scripts/lm/nwp/export.py.
uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ULL;
    return h;
}
size_t pad16(size_t n) { return (n + 15) & ~size_t(15); }

// WHY target_clones: the library is built for the baseline x86-64 CPU so one file runs everywhere, but the
// two hot loops (the 24k-row output layer and the LSTM matrices) are 5-8x faster with AVX2 + FMA. The loader
// picks the best version at start-up; machines without AVX2 run the plain one.
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__)
#define SWKB_CLONES __attribute__((target_clones("avx2,fma", "default")))
#else
#define SWKB_CLONES
#endif

// out[r] (+)= scale[r] * xs * sum_c q[r*cols+c] * xq[c]   (whole-number arithmetic; xq = the vector rounded to 13 bits)
// WHY whole numbers: a float sum must be added in a fixed order, so the compiler cannot spread it over SIMD lanes
// without help; integer sums have no such rule and run 10x faster (measured: 0.86 ms -> 0.07 ms for the 24k-word
// output layer). The vector is rounded to +-8191 first (error ~1e-4 of its largest entry, far below ranking noise);
// 127 * 8191 * 2048 columns stays below 2^31, so the sum cannot overflow (load() limits columns to 2048).
SWKB_CLONES void dotInt8(const int8_t *q, const float *scale, const int16_t *xq, float xs, float *out, uint32_t rows,
                         uint32_t cols, bool add)
{
    for (uint32_t r = 0; r < rows; ++r) {
        const int8_t *row = q + size_t(r) * cols;
        int32_t acc = 0;
        for (uint32_t c = 0; c < cols; ++c) acc += int32_t(row[c]) * int32_t(xq[c]);
        const float v = float(acc) * (scale[r] * xs);
        out[r] = add ? out[r] + v : v;
    }
}
// Rounds x to whole numbers in [-8191, 8191]; returns the factor that turns them back into x.
SWKB_CLONES float quantize(const float *x, int16_t *xq, uint32_t n)
{
    float peak = 0.f;
    for (uint32_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(x[i]));
    if (peak == 0.f) { for (uint32_t i = 0; i < n; ++i) xq[i] = 0; return 1.f; }
    const float mul = 8191.f / peak;
    for (uint32_t i = 0; i < n; ++i) xq[i] = int16_t(std::lrintf(x[i] * mul));
    return peak / 8191.f;
}
SWKB_CLONES void dotFloat(const float *w, const float *x, float *out, uint32_t rows, uint32_t cols, bool add)
{
    const uint32_t body = cols & ~15u;
    for (uint32_t r = 0; r < rows; ++r) {
        const float *row = w + size_t(r) * cols;
        float lane[16] = {};
        for (uint32_t c = 0; c < body; c += 16)
            for (int k = 0; k < 16; ++k) lane[k] += row[c + k] * x[c + k];
        float acc = 0.f;
        for (int k = 0; k < 16; ++k) acc += lane[k];
        for (uint32_t c = body; c < cols; ++c) acc += row[c] * x[c];
        out[r] = add ? out[r] + acc : acc;
    }
}

// WHY our own exp: std::exp is a scalar library call, and one word costs ~5,000 of them in the LSTM plus 24,000 in
// the output softmax. This one is plain arithmetic (2^x split into a whole part, set directly in the float's
// exponent bits, and a short polynomial for the rest), so the compiler turns the loops into SIMD. Relative error
// about 3e-6 - far below what changes a ranking (the PyTorch equivalence test bounds it at 1e-4).
inline float fastExp(float x)
{
    x = x < -80.f ? -80.f : (x > 80.f ? 80.f : x);
    const float t = x * 1.44269504f;
    const float n = std::floor(t + 0.5f);
    const float f = t - n;
    const float p = 1.f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    const uint32_t bits = uint32_t(int32_t(n) + 127) << 23;
    float scale;
    std::memcpy(&scale, &bits, 4);
    return p * scale;
}
inline float sigmoid(float v) { return 1.f / (1.f + fastExp(-v)); }
inline float fastTanh(float v) { return 2.f * sigmoid(2.f * v) - 1.f; }

// One LSTM cell update over `H` units; gates hold W_ih x + W_hh h (before bias) in order input, forget, cell, output.
SWKB_CLONES void lstmCell(const float *gates, const float *bias, float *c, float *hfull, uint32_t H)
{
    for (uint32_t j = 0; j < H; ++j) {
        const float i = sigmoid(gates[j] + bias[j]);
        const float fg = sigmoid(gates[H + j] + bias[H + j]);
        const float g = fastTanh(gates[2 * H + j] + bias[2 * H + j]);
        const float o = sigmoid(gates[3 * H + j] + bias[3 * H + j]);
        c[j] = fg * c[j] + i * g;
        hfull[j] = o * fastTanh(c[j]);
    }
}

// out[t] = exp(out[t] - best) summed; separate so the SIMD clone is chosen at start-up like the dot products.
SWKB_CLONES float sumExp(const float *v, float best, uint32_t n)
{
    float lane[8] = {};
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8)
        for (int k = 0; k < 8; ++k) lane[k] += fastExp(v[i + k] - best);
    float sum = 0.f;
    for (int k = 0; k < 8; ++k) sum += lane[k];
    for (; i < n; ++i) sum += fastExp(v[i] - best);
    return sum;
}
}

void NwpModel::matvec(const Matrix &m, const float *x, float *out, bool add) const
{
    if (m.q) {
        const float xs = quantize(x, xq_.data(), m.cols);
        dotInt8(m.q, m.scale, xq_.data(), xs, out, m.rows, m.cols, add);
    } else dotFloat(m.f, x, out, m.rows, m.cols, add);
}

bool NwpModel::load(const std::string &path)
{
    vocab_ = 0; blob_.clear(); words_.clear(); ids_.clear();
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    // WHY a size cap: a damaged length must not allocate gigabytes; real files are a few MB.
    if (size < long(kHeader + 8) || size > (64L << 20)) { std::fclose(f); return false; }
    blob_.resize(size_t(size));
    const bool read = std::fread(blob_.data(), 1, blob_.size(), f) == blob_.size();
    std::fclose(f);
    const auto fail = [this] { vocab_ = 0; blob_.clear(); words_.clear(); ids_.clear(); return false; };
    if (!read || std::memcmp(blob_.data(), kMagic, 8) != 0) return fail();
    uint32_t version, dtype, V, D, H, wordBytes;
    float lambda, oov;
    std::memcpy(&version, &blob_[8], 4); std::memcpy(&dtype, &blob_[12], 4);
    std::memcpy(&V, &blob_[16], 4); std::memcpy(&D, &blob_[20], 4); std::memcpy(&H, &blob_[24], 4);
    std::memcpy(&wordBytes, &blob_[28], 4); std::memcpy(&lambda, &blob_[32], 4); std::memcpy(&oov, &blob_[36], 4);
    if (version != 1 || dtype > 1 || V < 4 || V > 262144 || D == 0 || D > 1024 || H == 0 || H > 2048 ||
        wordBytes > (32u << 20) || !std::isfinite(lambda) || !std::isfinite(oov))
        return fail();
    const bool q8 = dtype == 1;
    // Expected size, section by section, so a truncated or padded file is refused before any pointer is made.
    const auto matBytes = [&](size_t rows, size_t cols) {
        return q8 ? pad16(rows * cols) + rows * 4 : rows * cols * 4;
    };
    size_t at = kHeader + pad16(wordBytes);
    const size_t embAt = at;      at += matBytes(V, D);
    const size_t outBiasAt = at;  at += size_t(V) * 4;
    const size_t wihAt = at;      at += matBytes(size_t(4) * H, D);
    const size_t whhAt = at;      at += matBytes(size_t(4) * H, D);
    const size_t biasAt = at;     at += size_t(4) * H * 4;
    const size_t projAt = at;     at += matBytes(D, H);
    if (at + 8 != blob_.size()) return fail();
    uint64_t stored;
    std::memcpy(&stored, &blob_[at], 8);
    if (stored != fnv1a(blob_.data(), at)) return fail();

    // Words: '\n' separated, V-3 of them, then zero padding.
    const char *w = reinterpret_cast<const char *>(&blob_[kHeader]);
    size_t used = wordBytes;
    while (used && w[used - 1] == '\0') --used;
    words_.reserve(V - 3);
    size_t from = 0;
    for (size_t i = 0; i <= used; ++i)
        if (i == used || w[i] == '\n') { words_.emplace_back(w + from, i - from); from = i + 1; }
    if (words_.size() != V - 3) return fail();
    for (uint32_t i = 0; i < words_.size(); ++i) ids_.emplace(words_[i], i + 3);

    const auto matrix = [&](size_t offset, uint32_t rows, uint32_t cols) {
        Matrix m;
        m.rows = rows; m.cols = cols;
        if (q8) {
            m.q = reinterpret_cast<const int8_t *>(&blob_[offset]);
            m.scale = reinterpret_cast<const float *>(&blob_[offset + pad16(size_t(rows) * cols)]);
        } else m.f = reinterpret_cast<const float *>(&blob_[offset]);
        return m;
    };
    vocab_ = V; dim_ = D; hidden_ = H; lambda_ = lambda; oovBonus_ = oov; int8_ = q8; fileBytes_ = blob_.size();
    emb_ = matrix(embAt, V, D); wih_ = matrix(wihAt, 4 * H, D); whh_ = matrix(whhAt, 4 * H, D); proj_ = matrix(projAt, D, H);
    outBias_ = reinterpret_cast<const float *>(&blob_[outBiasAt]);
    bias_ = reinterpret_cast<const float *>(&blob_[biasAt]);
    gates_.assign(size_t(4) * H, 0.f); x_.assign(D, 0.f); hfull_.assign(H, 0.f); hnew_.assign(D, 0.f); xq_.assign(std::max(D, H), 0);
    return true;
}

uint32_t NwpModel::token(std::string_view alpha) const
{
    const auto it = ids_.find(alpha);
    return it == ids_.end() ? kUnk : it->second;
}

NwpModel::State NwpModel::start() const { return State{std::vector<float>(dim_, 0.f), std::vector<float>(hidden_, 0.f)}; }

void NwpModel::embedRow(uint32_t token, float *out) const
{
    if (emb_.q) {
        const int8_t *row = emb_.q + size_t(token) * dim_;
        const float s = emb_.scale[token];
        for (uint32_t i = 0; i < dim_; ++i) out[i] = float(row[i]) * s;
    } else std::memcpy(out, emb_.f + size_t(token) * dim_, size_t(dim_) * 4);
}

void NwpModel::step(State &s, uint32_t token) const
{
    if (!loaded() || token >= vocab_) return;
    // gates = W_ih x + W_hh h + b, in PyTorch's order: input, forget, cell candidate, output.
    if (emb_.q) {
        // The word vector is already whole numbers (int8) with its own scale: use it as it is, no rounding at all.
        const int8_t *row = emb_.q + size_t(token) * dim_;
        for (uint32_t i = 0; i < dim_; ++i) xq_[i] = row[i];
        dotInt8(wih_.q, wih_.scale, xq_.data(), emb_.scale[token], gates_.data(), wih_.rows, wih_.cols, false);
    } else {
        embedRow(token, x_.data());
        matvec(wih_, x_.data(), gates_.data(), false);
    }
    matvec(whh_, s.h.data(), gates_.data(), true);
    lstmCell(gates_.data(), bias_, s.c.data(), hfull_.data(), hidden_);
    // The projection squeezes the hidden layer to `dim` numbers; the output layer then reuses the word vectors.
    matvec(proj_, hfull_.data(), s.h.data(), false);
}

void NwpModel::logProbs(const State &s, std::vector<float> &out) const
{
    out.resize(vocab_);
    if (emb_.q) {
        const float xs = quantize(s.h.data(), xq_.data(), dim_);
        dotInt8(emb_.q, emb_.scale, xq_.data(), xs, out.data(), vocab_, dim_, false);
    }
    else dotFloat(emb_.f, s.h.data(), out.data(), vocab_, dim_, false);
    float best = -INFINITY;
    for (uint32_t t = 0; t < vocab_; ++t) { out[t] += outBias_[t]; best = std::max(best, out[t]); }
    // WHY float exp: 24k of them per sentence state; float precision is far finer than the ranking needs.
    const float logZ = best + std::log(sumExp(out.data(), best, vocab_));
    for (uint32_t t = 0; t < vocab_; ++t) out[t] -= logZ;
}

} // namespace swipekb
