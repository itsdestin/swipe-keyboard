// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "swipekb/swipe_geo.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace swipekb {

namespace {

struct Pt { float x, y; };

// Equal arc-length resample of a polyline into n points; `length` receives its total length.
void resample(const std::vector<Pt> &in, int n, std::vector<Pt> &out, float &length) {
    out.resize(n);
    std::vector<float> cum(in.size(), 0.f);
    for (size_t i = 1; i < in.size(); ++i) cum[i] = cum[i - 1] + std::hypot(in[i].x - in[i - 1].x, in[i].y - in[i - 1].y);
    length = cum.back();
    if (length < 1e-6f) { std::fill(out.begin(), out.end(), in[0]); return; }
    size_t seg = 1;
    for (int j = 0; j < n; ++j) {
        const float s = length * float(j) / float(n - 1);
        while (seg + 1 < in.size() && cum[seg] < s) ++seg;
        const float d = cum[seg] - cum[seg - 1];
        const float f = d > 0 ? std::clamp((s - cum[seg - 1]) / d, 0.f, 1.f) : 1.f;
        out[j] = {in[seg - 1].x + f * (in[seg].x - in[seg - 1].x), in[seg - 1].y + f * (in[seg].y - in[seg - 1].y)};
    }
}

// Banded DTW between two n-point paths. Point cost = max(0, distance - radius). Returns the summed cost along the
// best warp path, or +inf as soon as every cell of a row exceeds `abandonAbove` (the cost can only grow).
float dtw(const Pt *u, const Pt *t, int n, int band, float radius, float abandonAbove) {
    constexpr float kInf = 1e30f;
    float prevRow[128], curRow[128];
    if (n > 128) return kInf;
    int plo = 0, phi = -1;
    for (int i = 0; i < n; ++i) {
        const float ctr = float(i);   // both paths have n points, so the band follows the diagonal
        const int lo = std::max(0, int(std::floor(ctr - band))), hi = std::min(n - 1, int(std::ceil(ctr + band)));
        float rowMin = kInf;
        for (int j = lo; j <= hi; ++j) {
            const float d = std::max(0.f, std::hypot(u[i].x - t[j].x, u[i].y - t[j].y) - radius);
            float best;
            if (i == 0 && j == 0) best = 0.f;
            else {
                best = kInf;
                if (i > 0 && j >= plo && j <= phi) best = std::min(best, prevRow[j]);
                if (j > lo) best = std::min(best, curRow[j - 1]);
                if (i > 0 && j > 0 && j - 1 >= plo && j - 1 <= phi) best = std::min(best, prevRow[j - 1]);
            }
            curRow[j] = best + d;
            rowMin = std::min(rowMin, curRow[j]);
        }
        if (rowMin > abandonAbove) return kInf;
        std::memcpy(prevRow + lo, curRow + lo, sizeof(float) * size_t(hi - lo + 1));
        plo = lo; phi = hi;
    }
    return prevRow[n - 1];
}

}

GeoDecoder::GeoDecoder(const SwipeLexicon &lex, const SwipeLayout &layout) : lex_(lex), layout_(layout) {
    const size_t K = layout.size();
    bucket_.assign(K * K, {});
    for (uint32_t w = 0; w < lex.wordCount(); ++w) {
        const auto &k = lex.keys(w);
        if (!k.empty() && k.front() < K && k.back() < K) bucket_[size_t(k.front()) * K + k.back()].push_back(w);
    }
}

std::vector<SwipeCandidate> GeoDecoder::decode(const std::vector<SwipePoint> &trace, float aspect, int topN, const GeoParams &p) const {
    std::vector<SwipeCandidate> result;
    const size_t K = layout_.size();
    if (trace.empty() || K == 0 || topN <= 0 || p.steps < 2 || p.steps > 128) return result;
    const float u = layout_.unit();
    std::vector<Pt> keyC(K);
    for (size_t k = 0; k < K; ++k) keyC[k] = {layout_.keys()[k].cx * u, layout_.keys()[k].cy * u * aspect};
    // Finger path in key-width units without repeated points.
    std::vector<Pt> pts;
    for (const auto &q : trace) {
        const Pt c{q.x * u, q.y * u * aspect};
        if (pts.empty() || std::hypot(c.x - pts.back().x, c.y - pts.back().y) > 1e-4f) pts.push_back(c);
    }
    std::vector<Pt> U;
    float L = 0;
    resample(pts, p.steps, U, L);
    // Nearest keys to the first and last touch.
    auto nearest = [&](const Pt &q) {
        std::vector<int> idx(K);
        std::iota(idx.begin(), idx.end(), 0);
        const size_t m = std::min<size_t>(size_t(p.endKeys), K);
        std::partial_sort(idx.begin(), idx.begin() + m, idx.end(), [&](int a, int b) {
            return std::hypot(keyC[a].x - q.x, keyC[a].y - q.y) < std::hypot(keyC[b].x - q.x, keyC[b].y - q.y); });
        idx.resize(m);
        return idx;
    };
    const auto firstKeys = nearest(pts.front()), lastKeys = nearest(pts.back());
    // Per key: how close did the path come (letter coverage).
    std::vector<float> miss(K);
    for (size_t k = 0; k < K; ++k) {
        float best = 1e30f;
        for (const auto &q : U) best = std::min(best, std::hypot(q.x - keyC[k].x, q.y - keyC[k].y));
        miss[k] = std::max(0.f, best - p.coverRadius);
    }
    struct Cand { float cheap; uint32_t word; };
    std::vector<Cand> cands;
    for (int a : firstKeys) for (int b : lastKeys) for (uint32_t w : bucket_[size_t(a) * K + b]) {
        const auto &k = lex_.keys(w);
        float len = 0, cov = 0;
        for (size_t i = 0; i < k.size(); ++i) {
            cov += miss[k[i]];
            if (i) len += std::hypot(keyC[k[i]].x - keyC[k[i - 1]].x, keyC[k[i]].y - keyC[k[i - 1]].y);
        }
        if (L > 1.f && !(len > p.lenLo * L - 1.f && len < p.lenHi * L + 1.5f)) continue;
        cands.push_back({p.coverWeight * cov - p.freqWeight * float(lex_.freq(w)), w});
    }
    if (cands.empty()) return result;
    // Cheapest terms first: the DTW can only add cost, so once the cheap part alone is worse than the current
    // topN-th best total, nothing later can enter (exact), and abandoned DTWs are skipped early.
    std::sort(cands.begin(), cands.end(), [](const Cand &x, const Cand &y) { return x.cheap < y.cheap; });
    const float norm = float(2 * p.steps);
    std::vector<std::pair<float, uint32_t>> best;   // (total cost, word), sorted ascending, size <= topN
    std::vector<Pt> poly, T;
    for (const auto &c : cands) {
        float worst = best.size() >= size_t(topN) ? best.back().first : 1e30f;
        if (c.cheap >= worst) break;
        const auto &k = lex_.keys(c.word);
        poly.clear();
        for (uint8_t kk : k) poly.push_back(keyC[kk]);
        float tl;
        resample(poly, p.steps, T, tl);
        const float limit = worst >= 1e29f ? 1e30f : (worst - c.cheap) * norm;
        const float d = dtw(U.data(), T.data(), p.steps, p.band, p.radius, limit);
        if (d >= 1e29f) continue;
        const float total = d / norm + c.cheap;
        if (total >= worst) continue;
        best.insert(std::upper_bound(best.begin(), best.end(), std::make_pair(total, c.word)), {total, c.word});
        if (best.size() > size_t(topN)) best.pop_back();
    }
    for (const auto &b : best) result.push_back({b.second, -b.first, -b.first});
    return result;
}

}

namespace swipekb {

GeoPrepared GeoDecoder::prepare(const std::vector<SwipePoint> &trace, float aspect, const GeoParams &p) const {
    GeoPrepared out;
    const size_t K = layout_.size();
    if (trace.empty() || K == 0 || p.steps < 2 || p.steps > 128) return out;
    const float u = layout_.unit();
    std::vector<Pt> pts;
    for (const auto &q : trace) {
        const Pt c{q.x * u, q.y * u * aspect};
        if (pts.empty() || std::hypot(c.x - pts.back().x, c.y - pts.back().y) > 1e-4f) pts.push_back(c);
    }
    std::vector<Pt> U;
    resample(pts, p.steps, U, out.length);
    out.path.reserve(U.size());
    for (const auto &q : U) out.path.push_back({q.x, q.y});
    out.miss.resize(K);
    for (size_t k = 0; k < K; ++k) {
        const Pt c{layout_.keys()[k].cx * u, layout_.keys()[k].cy * u * aspect};
        float best = 1e30f;
        for (const auto &q : U) best = std::min(best, std::hypot(q.x - c.x, q.y - c.y));
        out.miss[k] = std::max(0.f, best - p.coverRadius);
    }
    out.valid = true;
    return out;
}

float GeoDecoder::cost(const GeoPrepared &swipe, uint32_t word, float aspect, const GeoParams &p) const {
    if (!swipe.valid || word >= lex_.wordCount()) return 1e9f;
    const float u = layout_.unit();
    const auto &k = lex_.keys(word);
    std::vector<Pt> poly, T, U(swipe.path.size());
    float cov = 0, tl;
    for (uint8_t kk : k) {
        poly.push_back({layout_.keys()[kk].cx * u, layout_.keys()[kk].cy * u * aspect});
        cov += swipe.miss[kk];
    }
    for (size_t i = 0; i < U.size(); ++i) U[i] = {swipe.path[i].x, swipe.path[i].y};
    resample(poly, p.steps, T, tl);
    const float d = dtw(U.data(), T.data(), p.steps, p.band, p.radius, 1e30f);
    return d / float(2 * p.steps) + p.coverWeight * cov;
}

}
