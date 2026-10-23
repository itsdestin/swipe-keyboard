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

// out[r] (+)= scale[r] * sum_c q[r*cols+c] * x[c]
SWKB_CLONES void dotInt8(const int8_t *q, const float *scale, const float *x, float *out, uint32_t rows,
                         uint32_t cols, bool add)
{
    for (uint32_t r = 0; r < rows; ++r) {
        const int8_t *row = q + size_t(r) * cols;
        float acc = 0.f;
        for (uint32_t c = 0; c < cols; ++c) acc += float(row[c]) * x[c];
        acc *= scale[r];
        out[r] = add ? out[r] + acc : acc;
    }
}
SWKB_CLONES void dotFloat(const float *w, const float *x, float *out, uint32_t rows, uint32_t cols, bool add)
{
    for (uint32_t r = 0; r < rows; ++r) {
        const float *row = w + size_t(r) * cols;
        float acc = 0.f;
        for (uint32_t c = 0; c < cols; ++c) acc += row[c] * x[c];
        out[r] = add ? out[r] + acc : acc;
    }
}
float sigmoid(float v) { return 1.f / (1.f + std::exp(-v)); }
}

void NwpModel::matvec(const Matrix &m, const float *x, float *out, bool add) const
{
    if (m.q) dotInt8(m.q, m.scale, x, out, m.rows, m.cols, add);
    else dotFloat(m.f, x, out, m.rows, m.cols, add);
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
    if (version != 1 || dtype > 1 || V < 4 || V > 262144 || D == 0 || D > 1024 || H == 0 || H > 8192 ||
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
    gates_.assign(size_t(4) * H, 0.f); x_.assign(D, 0.f); hfull_.assign(H, 0.f); hnew_.assign(D, 0.f);
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
    embedRow(token, x_.data());
    // gates = W_ih x + W_hh h + b, in PyTorch's order: input, forget, cell candidate, output.
    matvec(wih_, x_.data(), gates_.data(), false);
    matvec(whh_, s.h.data(), gates_.data(), true);
    const uint32_t H = hidden_;
    for (uint32_t j = 0; j < H; ++j) {
        const float i = sigmoid(gates_[j] + bias_[j]);
        const float fg = sigmoid(gates_[H + j] + bias_[H + j]);
        const float g = std::tanh(gates_[2 * H + j] + bias_[2 * H + j]);
        const float o = sigmoid(gates_[3 * H + j] + bias_[3 * H + j]);
        s.c[j] = fg * s.c[j] + i * g;
        hfull_[j] = o * std::tanh(s.c[j]);
    }
    // The projection squeezes the hidden layer to `dim` numbers; the output layer then reuses the word vectors.
    matvec(proj_, hfull_.data(), s.h.data(), false);
}

void NwpModel::logProbs(const State &s, std::vector<float> &out) const
{
    out.resize(vocab_);
    if (emb_.q) dotInt8(emb_.q, emb_.scale, s.h.data(), out.data(), vocab_, dim_, false);
    else dotFloat(emb_.f, s.h.data(), out.data(), vocab_, dim_, false);
    float best = -INFINITY;
    for (uint32_t t = 0; t < vocab_; ++t) { out[t] += outBias_[t]; best = std::max(best, out[t]); }
    double sum = 0;
    for (uint32_t t = 0; t < vocab_; ++t) sum += std::exp(double(out[t] - best));
    const float logZ = best + float(std::log(sum));
    for (uint32_t t = 0; t < vocab_; ++t) out[t] -= logZ;
}

} // namespace swipekb
