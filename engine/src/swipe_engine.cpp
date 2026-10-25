// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "swipekb/swipe_engine.hpp"

#include <algorithm>
#include <cmath>

namespace swipekb {

SwipeEngine::SwipeEngine(const SwipeEngine &o) : params(o.params), d_(o.d_), prior_(o.prior_), hints_(o.hints_) {
    if (d_) search_ = std::make_unique<SwipeBeamSearch>(d_->lex);
}

void SwipeEngine::rebuild() {
    if (!d_) return;
    d_->geo = std::make_unique<GeoDecoder>(d_->lex, d_->layout);
    search_ = std::make_unique<SwipeBeamSearch>(d_->lex);
}

void SwipeEngine::setLayout(SwipeLayout layout) {
    if (!d_) d_ = std::make_shared<Data>();
    d_->layout = std::move(layout);
}

bool SwipeEngine::loadLayout(const std::string &tsv) {
    SwipeLayout l;
    if (!l.loadTsv(tsv)) return false;
    setLayout(std::move(l));
    return true;
}

size_t SwipeEngine::loadWords(const std::string &tsv) {
    if (!d_) d_ = std::make_shared<Data>();
    d_->lex = SwipeLexicon();
    const size_t n = d_->lex.loadTsv(tsv, d_->layout);
    d_->lex.finish();
    rebuild();
    return n;
}

size_t SwipeEngine::setWords(const std::vector<std::pair<std::string, int>> &words) {
    if (!d_) d_ = std::make_shared<Data>();
    d_->lex = SwipeLexicon();
    for (const auto &[w, f] : words) d_->lex.add(w, f, d_->layout);
    d_->lex.finish();
    rebuild();
    return words.size();
}

bool SwipeEngine::loadModel(const std::string &path) {
    if (!d_) d_ = std::make_shared<Data>();
    return d_->net.load(path);
}

void SwipeEngine::setPrior(std::function<float(uint32_t)> prior, std::vector<uint32_t> hints) {
    prior_ = std::move(prior); hints_ = std::move(hints);
}

std::vector<SwipeCandidate> SwipeEngine::decode(const std::vector<SwipePoint> &trace, float aspect, int topN) {
    lastUsedNet_ = false;
    if (!ready() || trace.empty() || topN <= 0) return {};
    if (d_->net.ready() && d_->net.buildInput(trace, d_->layout, aspect, in_) && d_->net.forward(in_, lp_)) {
        lastUsedNet_ = true;
        BeamParams bp = params.beam;
        bp.freqWeight = params.freqWeight;
        const bool blend = params.geoWeight > 0;
        auto res = search_->decode(lp_.data(), in_.steps, in_.keyCount, bp, blend ? std::max(topN, params.rerank) : topN, prior_, hints_);
        if (!res.empty()) {
            if (blend) {
                // WHY: swipers unlike the training data confuse the network more than they confuse plain geometry;
                // subtracting the geometric cost keeps its best words from losing to near-misses.
                const auto prepared = d_->geo->prepare(trace, aspect, params.geo);
                for (auto &c : res) c.score -= params.geoWeight * d_->geo->cost(prepared, c.word, aspect, params.geo);
                std::stable_sort(res.begin(), res.end(), [](const SwipeCandidate &a, const SwipeCandidate &b) { return a.score > b.score; });
                if (res.size() > size_t(topN)) res.resize(size_t(topN));
            }
            return res;
        }
    }
    lastUsedNet_ = false;
    return d_->geo->decode(trace, aspect, topN, params.geo);
}

std::vector<float> SwipeEngine::pathScores(const std::vector<std::string> &words) const {
    if (!lastUsedNet_ || !d_) return {};
    constexpr float kNegInf = -1e30f;
    const auto lae = [](float a, float b) {
        if (a < b) std::swap(a, b);
        return b <= kNegInf ? a : a + std::log1p(std::exp(b - a));
    };
    const int T = in_.steps, K = in_.keyCount;
    std::vector<float> out;
    out.reserve(words.size());
    std::vector<float> a, b;
    for (const auto &w : words) {
        const auto keys = d_->layout.wordKeys(w);   // doubled letters already collapsed, as in the lexicon
        if (keys.empty() || T <= 0) { out.push_back(kNegInf); continue; }
        // CTC forward over the states blank, k1, blank, k2, ..., blank. Neighbouring keys differ (collapsed), so a
        // letter may always follow the previous letter directly.
        const size_t S = 2 * keys.size() + 1;
        a.assign(S, kNegInf);
        b.assign(S, kNegInf);
        const float *row0 = lp_.data();
        a[0] = row0[0];
        a[1] = row0[1 + keys[0]];
        for (int t = 1; t < T; ++t) {
            const float *row = lp_.data() + size_t(t) * (K + 1);
            for (size_t s = 0; s < S; ++s) {
                if (s % 2 == 0) {
                    float v = a[s];
                    if (s >= 1) v = lae(v, a[s - 1]);
                    b[s] = v <= kNegInf ? kNegInf : v + row[0];
                } else {
                    const int k = keys[s / 2];
                    float v = lae(a[s], a[s - 1]);   // held, or started after a blank
                    if (s >= 2) v = lae(v, a[s - 2]);   // started right after the previous letter
                    b[s] = v <= kNegInf ? kNegInf : v + row[1 + k];
                }
            }
            std::swap(a, b);
        }
        out.push_back(lae(a[S - 1], a[S - 2]));
    }
    return out;
}

}
