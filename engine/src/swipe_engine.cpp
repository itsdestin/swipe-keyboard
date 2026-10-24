// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "swipekb/swipe_engine.hpp"

#include <algorithm>

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
    if (!ready() || trace.empty() || topN <= 0) return {};
    if (d_->net.ready() && d_->net.buildInput(trace, d_->layout, aspect, in_) && d_->net.forward(in_, lp_)) {
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
    return d_->geo->decode(trace, aspect, topN, params.geo);
}

}
