// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "swipekb/swipe_net.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace swipekb {

namespace {

float halfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
    float v;
    if (exp == 0) v = std::ldexp(float(man), -24);
    else if (exp == 31) v = man ? NAN : INFINITY;
    else v = std::ldexp(float(man | 0x400), int(exp) - 25);
    return sign ? -v : v;
}

struct Reader {
    const std::vector<uint8_t> &b;
    size_t pos = 0;
    bool half = false, ok = true;
    uint32_t u32() {
        if (pos + 4 > b.size()) { ok = false; return 0; }
        uint32_t v; std::memcpy(&v, b.data() + pos, 4); pos += 4; return v;
    }
    // Next `n` weights as floats.
    std::vector<float> tensor(size_t n) {
        std::vector<float> out;
        const size_t bytes = n * (half ? 2 : 4);
        if (pos + bytes > b.size()) { ok = false; return out; }
        out.resize(n);
        if (half) for (size_t i = 0; i < n; ++i) { uint16_t h; std::memcpy(&h, b.data() + pos + 2 * i, 2); out[i] = halfToFloat(h); }
        else std::memcpy(out.data(), b.data() + pos, n * 4);
        pos += bytes;
        return out;
    }
};

// out[r][c] = in[c][r] for an (rows x cols) row-major matrix: weights are stored [out][in] but the runner wants
// the output index innermost so the multiply-add loop vectorises.
std::vector<float> transpose(const std::vector<float> &m, size_t rows, size_t cols) {
    std::vector<float> t(m.size());
    for (size_t r = 0; r < rows; ++r) for (size_t c = 0; c < cols; ++c) t[c * rows + r] = m[r * cols + c];
    return t;
}

// WHY our own exp/tanh/sigmoid: libm calls do not vectorise, and the gates + pointer head evaluate ~60k of them per
// swipe. exp(x) = 2^n * e^r with n = round(x / ln2), |r| <= ln2/2, e^r by a degree-6 Taylor polynomial (error ~1e-7,
// far below the 2e-4 tolerance against PyTorch). Plain float arithmetic, so the compiler can use SIMD.
inline float fastExp(float x) {
    x = x < -87.f ? -87.f : (x > 87.f ? 87.f : x);
    const float t = x * 1.44269504089f;
    const float n = (t + 12582912.f) - 12582912.f;          // round to nearest integer without a libm call
    const float r = (t - n) * 0.69314718056f;
    float p = 1.f / 720.f;
    p = p * r + 1.f / 120.f; p = p * r + 1.f / 24.f; p = p * r + 1.f / 6.f; p = p * r + 0.5f; p = p * r + 1.f; p = p * r + 1.f;
    const int32_t bits = (int32_t(n) + 127) << 23;           // 2^n built directly in the exponent field
    float scale;
    std::memcpy(&scale, &bits, 4);
    return p * scale;
}
// out[o] += sum_i x[i] * W[i * nOut + o]. WHY four rows at a time: the weight rows stream from cache once, but the
// output vector would otherwise be loaded and stored once per row; blocking quarters that traffic.
inline void gemvAccum(float *__restrict out, int nOut, const float *__restrict x, int nIn, const float *__restrict W) {
    int i = 0;
    for (; i + 4 <= nIn; i += 4) {
        const float x0 = x[i], x1 = x[i + 1], x2 = x[i + 2], x3 = x[i + 3];
        const float *w0 = W + size_t(i) * nOut, *w1 = w0 + nOut, *w2 = w1 + nOut, *w3 = w2 + nOut;
        for (int o = 0; o < nOut; ++o) out[o] += x0 * w0[o] + x1 * w1[o] + x2 * w2[o] + x3 * w3[o];
    }
    for (; i < nIn; ++i) {
        const float xv = x[i];
        const float *w = W + size_t(i) * nOut;
        for (int o = 0; o < nOut; ++o) out[o] += xv * w[o];
    }
}

inline float sigmoid(float x) { return 1.f / (1.f + fastExp(-x)); }
inline float fastTanh(float x) { return 1.f - 2.f / (fastExp(2.f * x) + 1.f); }

}

bool SwipeNet::load(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    return loadBytes(std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
}

bool SwipeNet::loadBytes(const std::vector<uint8_t> &bytes) {
    steps_ = 0; convs_.clear(); dirs_.clear(); params_ = 0;
    if (bytes.size() < 24 || std::memcmp(bytes.data(), "SKNN", 4) != 0) return false;
    Reader r{bytes, 4};
    const uint32_t version = r.u32(), dtype = r.u32(), steps = r.u32(), fin = r.u32(), kf = r.u32();
    const uint32_t nconv = r.u32(), kernel = r.u32(), hidden = r.u32(), layers = r.u32(), pointer = r.u32();
    // WHY strict checks: this file comes from a download; anything unexpected must fall back, never misread.
    if (!r.ok || version != 1 || dtype > 1 || fin != kInFeatures || kf != kKeyFeatures || steps < 2 || steps > 512
        || nconv > 8 || kernel == 0 || kernel > 15 || (kernel % 2) == 0 || hidden == 0 || hidden > 512 || layers == 0
        || layers > 4 || pointer == 0 || pointer > 256) return false;
    r.half = dtype == 1;
    std::vector<Conv> convs;
    std::vector<uint32_t> chans;
    for (uint32_t i = 0; i < nconv; ++i) { chans.push_back(r.u32()); if (chans.back() == 0 || chans.back() > 1024) return false; }
    if (!r.ok) return false;
    size_t params = 0;
    uint32_t cin = fin;
    for (uint32_t i = 0; i < nconv; ++i) {
        Conv c; c.cin = int(cin); c.cout = int(chans[i]);
        // exported as [cout][cin][k]; runner wants [k][cin][cout]
        auto raw = r.tensor(size_t(c.cout) * c.cin * kernel);
        c.b = r.tensor(c.cout);
        if (!r.ok) return false;
        c.w.resize(raw.size());
        for (int co = 0; co < c.cout; ++co) for (int ci = 0; ci < c.cin; ++ci) for (uint32_t k = 0; k < kernel; ++k)
            c.w[(size_t(k) * c.cin + ci) * c.cout + co] = raw[(size_t(co) * c.cin + ci) * kernel + k];
        params += raw.size() + c.b.size();
        convs.push_back(std::move(c));
        cin = chans[i];
    }
    std::vector<Dir> dirs;
    uint32_t in = cin;
    const size_t H = hidden;
    for (uint32_t l = 0; l < layers; ++l) {
        for (int d = 0; d < 2; ++d) {
            Dir dir;
            auto wih = r.tensor(3 * H * in), whh = r.tensor(3 * H * H);
            dir.bih = r.tensor(3 * H); dir.bhh = r.tensor(3 * H);
            if (!r.ok) return false;
            dir.wih = transpose(wih, 3 * H, in); dir.whh = transpose(whh, 3 * H, H);
            params += wih.size() + whh.size() + dir.bih.size() + dir.bhh.size();
            dirs.push_back(std::move(dir));
        }
        in = uint32_t(2 * H);
    }
    auto A = r.tensor(size_t(pointer) * 2 * H); auto Ab = r.tensor(pointer);
    auto B = r.tensor(size_t(pointer) * kKeyFeatures); auto Bb = r.tensor(pointer);
    auto w = r.tensor(pointer); auto wb = r.tensor(1);
    auto bw = r.tensor(2 * H); auto bb = r.tensor(1);
    if (!r.ok || r.pos != bytes.size()) return false;
    params += A.size() + Ab.size() + B.size() + Bb.size() + w.size() + wb.size() + bw.size() + bb.size();
    convs_ = std::move(convs); dirs_ = std::move(dirs);
    A_ = transpose(A, pointer, 2 * H); Ab_ = std::move(Ab); B_ = transpose(B, pointer, kKeyFeatures); Bb_ = std::move(Bb);
    w_ = std::move(w); wb_ = std::move(wb); blankW_ = std::move(bw); blankB_ = std::move(bb);
    kernel_ = int(kernel); hidden_ = int(hidden); layers_ = int(layers); pointer_ = int(pointer);
    params_ = params; steps_ = int(steps);
    return true;
}

bool SwipeNet::buildInput(const std::vector<SwipePoint> &trace, const SwipeLayout &layout, float aspect, SwipeInput &out) const {
    if (steps_ <= 0 || trace.empty() || layout.size() == 0) return false;
    const double u = layout.unit();
    const size_t n = trace.size();
    std::vector<double> X(n), Y(n), TT(n), s(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        X[i] = double(trace[i].x) * u; Y[i] = double(trace[i].y) * u * aspect; TT[i] = trace[i].t - trace[0].t;
        if (i) s[i] = s[i - 1] + std::hypot(X[i] - X[i - 1], Y[i] - Y[i - 1]);
    }
    const double L = s[n - 1];
    const int T = steps_;
    std::vector<double> px(T), py(T), pt(T);
    for (int j = 0; j < T; ++j) {
        if (L < 1e-6) { px[j] = X[0]; py[j] = Y[0]; pt[j] = TT[n - 1] * j / (T - 1); continue; }
        const double target = L * j / (T - 1);
        size_t i = size_t(std::lower_bound(s.begin(), s.end(), target) - s.begin());
        if (i == 0) { px[j] = X[0]; py[j] = Y[0]; pt[j] = TT[0]; continue; }
        if (i >= n) i = n - 1;
        const double d = s[i] - s[i - 1];
        const double f = d > 0 ? (target - s[i - 1]) / d : 1.0;
        px[j] = X[i - 1] + f * (X[i] - X[i - 1]);
        py[j] = Y[i - 1] + f * (Y[i] - Y[i - 1]);
        pt[j] = TT[i - 1] + f * (TT[i] - TT[i - 1]);
    }
    out.steps = T; out.keyCount = int(layout.size());
    out.feats.assign(size_t(T) * kInFeatures, 0.f);
    out.pos.assign(size_t(T) * 2, 0.f);
    const double glob = std::log1p(L) / 3.0;
    for (int j = 0; j < T; ++j) {
        const double dx = j ? px[j] - px[j - 1] : 0.0, dy = j ? py[j] - py[j - 1] : 0.0;
        const double dt = j ? pt[j] - pt[j - 1] : 0.0;
        float *f = &out.feats[size_t(j) * kInFeatures];
        f[0] = float(px[j] / u); f[1] = float(py[j] / (u * aspect));
        f[2] = float(dx); f[3] = float(dy);
        f[4] = float(std::log1p(std::max(dt, 0.0)) / 4.0);
        f[5] = L >= 1e-6 ? float(double(j) / (T - 1)) : 0.f;
        f[6] = float(std::hypot(dx, dy) / 2.0);
        f[7] = float(glob);
        out.pos[size_t(j) * 2] = float(px[j]); out.pos[size_t(j) * 2 + 1] = float(py[j]);
    }
    out.keys.resize(layout.size() * 4);
    for (size_t k = 0; k < layout.size(); ++k) {
        const auto &key = layout.keys()[k];
        out.keys[k * 4 + 0] = float(key.cx * u); out.keys[k * 4 + 1] = float(key.cy * u * aspect);
        out.keys[k * 4 + 2] = float(2 * key.hw * u); out.keys[k * 4 + 3] = float(2 * key.hh * u * aspect);
    }
    return true;
}

// WHY target_clones: one binary runs the AVX2/FMA version on CPUs that have it and the baseline elsewhere (portable).
#if defined(__GNUC__) && defined(__x86_64__)
__attribute__((target_clones("avx2,fma", "default")))
#endif
bool SwipeNet::forward(const SwipeInput &in, std::vector<float> &logProbs) const {
    if (steps_ <= 0 || in.steps != steps_ || in.keyCount <= 0) return false;
    const int T = steps_, K = in.keyCount, H = hidden_, P = pointer_;
    // ---- conv stack (same padding, ReLU), channels innermost
    std::vector<float> cur(in.feats), nxt;
    int ch = kInFeatures;
    const int pad = kernel_ / 2;
    for (const auto &c : convs_) {
        nxt.assign(size_t(T) * c.cout, 0.f);
        for (int t = 0; t < T; ++t) {
            float *o = &nxt[size_t(t) * c.cout];
            std::memcpy(o, c.b.data(), sizeof(float) * c.cout);
            for (int k = 0; k < kernel_; ++k) {
                const int tt = t + k - pad;
                if (tt < 0 || tt >= T) continue;
                gemvAccum(o, c.cout, &cur[size_t(tt) * c.cin], c.cin, &c.w[size_t(k) * c.cin * c.cout]);
            }
            for (int co = 0; co < c.cout; ++co) o[co] = std::max(0.f, o[co]);
        }
        cur.swap(nxt); ch = c.cout;
    }
    // ---- bidirectional GRU layers
    std::vector<float> gi(size_t(T) * 3 * H), gh(3 * H), h(H), out;
    for (int l = 0; l < layers_; ++l) {
        out.assign(size_t(T) * 2 * H, 0.f);
        for (int d = 0; d < 2; ++d) {
            const Dir &dir = dirs_[size_t(l) * 2 + d];
            for (int t = 0; t < T; ++t) {   // input projection for every step at once
                float *g = &gi[size_t(t) * 3 * H];
                std::memcpy(g, dir.bih.data(), sizeof(float) * 3 * H);
                gemvAccum(g, 3 * H, &cur[size_t(t) * ch], ch, dir.wih.data());
            }
            std::fill(h.begin(), h.end(), 0.f);
            for (int s = 0; s < T; ++s) {
                const int t = d == 0 ? s : T - 1 - s;
                std::memcpy(gh.data(), dir.bhh.data(), sizeof(float) * 3 * H);
                gemvAccum(gh.data(), 3 * H, h.data(), H, dir.whh.data());
                const float *g = &gi[size_t(t) * 3 * H];
                float *dst = &out[size_t(t) * 2 * H + size_t(d) * H];
                for (int j = 0; j < H; ++j) {
                    const float r = sigmoid(g[j] + gh[j]);
                    const float z = sigmoid(g[H + j] + gh[H + j]);
                    const float nn = fastTanh(g[2 * H + j] + r * gh[2 * H + j]);   // PyTorch GRU: r scales the hidden part only
                    h[j] = (1.f - z) * nn + z * h[j];
                    dst[j] = h[j];
                }
            }
        }
        cur = out; ch = 2 * H;
    }
    // ---- pointer head: every step scores every key from where the finger is relative to that key
    logProbs.assign(size_t(T) * (K + 1), 0.f);
    std::vector<float> a(P), ab(P);
    for (int t = 0; t < T; ++t) {
        const float *z = &cur[size_t(t) * 2 * H];
        std::memcpy(a.data(), Ab_.data(), sizeof(float) * P);
        gemvAccum(a.data(), P, z, 2 * H, A_.data());
        float *row = &logProbs[size_t(t) * (K + 1)];
        float bl = blankB_[0];
        for (int i = 0; i < 2 * H; ++i) bl += z[i] * blankW_[i];
        row[0] = bl;
        const float px = in.pos[size_t(t) * 2], py = in.pos[size_t(t) * 2 + 1];
        for (int p = 0; p < P; ++p) ab[p] = a[p] + Bb_[p];
        const float *B0 = &B_[0], *B1 = &B_[size_t(P)], *B2 = &B_[size_t(2 * P)], *B3 = &B_[size_t(3 * P)];
        for (int k = 0; k < K; ++k) {
            const float *kk = &in.keys[size_t(k) * 4];
            const float f0 = px - kk[0], f1 = py - kk[1], f2 = kk[2], f3 = kk[3];
            float acc = 0.f;
            for (int p = 0; p < P; ++p)   // vectorises: independent lanes, one tanh each
                acc += w_[p] * fastTanh(ab[p] + f0 * B0[p] + f1 * B1[p] + f2 * B2[p] + f3 * B3[p]);
            row[1 + k] = acc + wb_[0];
        }
        float mx = row[0];
        for (int k = 1; k <= K; ++k) mx = std::max(mx, row[k]);
        double sum = 0;
        for (int k = 0; k <= K; ++k) sum += std::exp(double(row[k] - mx));
        const float lse = mx + float(std::log(sum));
        for (int k = 0; k <= K; ++k) row[k] -= lse;
    }
    return true;
}

}
