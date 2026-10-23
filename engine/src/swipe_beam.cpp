#include "swipekb/swipe_beam.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <numeric>

namespace swipekb {

namespace {
constexpr float kNegInf = -1e30f;
inline float lae(float a, float b) {   // log(exp(a)+exp(b))
    if (a < b) std::swap(a, b);
    return b <= kNegInf ? a : a + std::log1p(std::exp(b - a));
}
}

void SwipeLexicon::add(std::string_view word, int freq, const SwipeLayout &layout) {
    int raw = 0;
    const auto k = layout.wordKeys(word, &raw);
    if (k.empty() || k.size() > 40 || layout.size() > 255) return;
    Pending p;
    p.keys.assign(k.begin(), k.end());
    // WHY the lower-cased spelling (accents kept) is the identity: "que" and "qu\u00e9", "ou" and "o\u00f9", "see" and "se" are
    // different words that share a key path, and the sentence prior must be able to choose between them.
    p.lower = lowerUtf8(word);
    p.text = std::string(word); p.freq = std::clamp(freq, 0, 255);
    pending_.push_back(std::move(p));
}

size_t SwipeLexicon::loadTsv(const std::string &path, const SwipeLayout &layout) {
    std::ifstream in(path);
    if (!in) return 0;
    std::string line;
    size_t n = 0;
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        add(std::string_view(line).substr(0, tab), std::atoi(line.c_str() + tab + 1), layout);
        ++n;
    }
    return n;
}

void SwipeLexicon::finish() {
    // Dedupe by lower-cased spelling, keeping the most frequent capitalisation ("The"/"the").
    std::sort(pending_.begin(), pending_.end(), [](const Pending &a, const Pending &b) {
        if (a.keys != b.keys) return a.keys < b.keys;
        if (a.lower != b.lower) return a.lower < b.lower;
        if (a.freq != b.freq) return a.freq > b.freq;
        return a.text < b.text;
    });
    words_.clear(); nodes_.clear();
    nodes_.emplace_back();   // root
    const Pending *last = nullptr;
    // Stack of (node, depth) along the current key string to attach new branches without searching.
    std::vector<int32_t> path{0};
    std::vector<int32_t> lastChild{-1};   // last child added under path[i]
    for (const auto &p : pending_) {
        if (last && last->keys == p.keys && last->lower == p.lower) continue;   // same word, lower frequency
        last = &p;
        // Longest shared prefix with the previous word's path.
        size_t shared = 0;
        const auto &prevKeys = words_.empty() ? std::vector<uint8_t>{} : words_.back().keys;
        while (shared < prevKeys.size() && shared < p.keys.size() && prevKeys[shared] == p.keys[shared]) ++shared;
        path.resize(shared + 1); lastChild.resize(shared + 1);
        for (size_t d = shared; d < p.keys.size(); ++d) {
            const int32_t id = int32_t(nodes_.size());
            Node n; n.key = p.keys[d];
            nodes_.push_back(n);
            const int32_t parent = path.back();
            if (lastChild.back() < 0) nodes_[parent].firstChild = id; else nodes_[lastChild.back()].nextSibling = id;
            lastChild.back() = id;
            path.push_back(id); lastChild.push_back(-1);
        }
        words_.push_back({p.text, p.keys, p.freq});
        Node &term = nodes_[path.back()];
        if (term.wordEnd == term.wordBegin) term.wordBegin = uint32_t(words_.size() - 1);
        term.wordEnd = uint32_t(words_.size());
        // bestFreq for all nodes on this path (words are visited in key order, any order within a node is fine here).
        for (int32_t id : path) nodes_[id].bestFreq = uint8_t(std::max<int>(nodes_[id].bestFreq, p.freq));
    }
    // Words at a node are sorted by raw key string then frequency; the search wants the best prior first, which it
    // computes itself, so no re-sort is needed. Pending data is no longer needed.
    pending_.clear(); pending_.shrink_to_fit();
}

int32_t SwipeLexicon::find(const std::vector<int> &keys) const {
    int32_t cur = 0;
    for (int k : keys) {
        int32_t c = nodes_[cur].firstChild;
        while (c >= 0 && nodes_[c].key != k) c = nodes_[c].nextSibling;
        if (c < 0) return -1;
        cur = c;
    }
    return cur;
}

std::vector<SwipeCandidate> SwipeBeamSearch::decode(const float *lp, int steps, int K, const BeamParams &p, int topN,
                                                   const std::function<float(uint32_t)> &prior,
                                                   const std::vector<uint32_t> &hints) {
    std::vector<SwipeCandidate> result;
    if (steps <= 0 || K <= 0 || lex_.nodeCount() <= 1) return result;
    if (stamp_.size() != lex_.nodeCount()) {   // scratch sized to the lexicon (stamps avoid clearing 1.6M entries)
        stamp_.assign(lex_.nodeCount(), 0); slot_.assign(lex_.nodeCount(), 0);
        bonusStamp_.assign(lex_.nodeCount(), 0); bonus_.assign(lex_.nodeCount(), 0.f);
        generation_ = 0;
    }
    // Context hints: walk each favoured word's path and mark its nodes with a pruning bonus.
    const uint32_t hintGen = ++generation_;
    for (uint32_t w : hints) {
        if (w >= lex_.wordCount()) continue;
        int32_t cur = 0;
        for (uint8_t k : lex_.keys(w)) {
            int32_t c = lex_.node(cur).firstChild;
            while (c >= 0 && lex_.node(c).key != k) c = lex_.node(c).nextSibling;
            if (c < 0) break;
            cur = c;
            bonusStamp_[cur] = hintGen; bonus_[cur] = p.hintBonus;
        }
    }
    auto look = [&](int32_t n) {
        float v = p.lookahead * lex_.node(n).bestFreq;
        if (bonusStamp_[n] == hintGen) v += bonus_[n];
        return v;
    };
    struct Beam { int32_t node; float pb, pnb; };
    std::vector<Beam> beams{{0, 0.f, kNegInf}}, next;
    std::vector<int> order(K);
    std::vector<uint8_t> allowed(K);
    for (int t = 0; t < steps; ++t) {
        const float *row = lp + size_t(t) * (K + 1);
        std::iota(order.begin(), order.end(), 0);
        const int keep = std::min(p.keysPerStep, K);
        std::partial_sort(order.begin(), order.begin() + keep, order.end(), [&](int a, int b) { return row[1 + a] > row[1 + b]; });
        std::fill(allowed.begin(), allowed.end(), 0);
        for (int i = 0; i < keep; ++i) allowed[order[i]] = 1;
        const uint32_t gen = ++generation_;
        next.clear();
        auto touch = [&](int32_t node) -> Beam & {
            if (stamp_[node] != gen) { stamp_[node] = gen; slot_[node] = int32_t(next.size()); next.push_back({node, kNegInf, kNegInf}); }
            return next[slot_[node]];
        };
        for (const Beam &b : beams) {
            const float tot = lae(b.pb, b.pnb);
            Beam &same = touch(b.node);
            same.pb = lae(same.pb, tot + row[0]);                       // blank: stay on this prefix
            if (b.node != 0) {
                const float stay = b.pnb + row[1 + lex_.node(b.node).key];   // same key held over several steps
                touch(b.node).pnb = lae(touch(b.node).pnb, stay);
            }
            for (int32_t c = lex_.node(b.node).firstChild; c >= 0; c = lex_.node(c).nextSibling) {
                const int k = lex_.node(c).key;
                if (!allowed[k]) continue;
                Beam &nb = touch(c);
                nb.pnb = lae(nb.pnb, tot + row[1 + k]);
            }
        }
        const size_t B = size_t(std::max(1, p.beam));
        if (next.size() > B) {
            std::vector<std::pair<float, uint32_t>> sc(next.size());
            for (size_t i = 0; i < next.size(); ++i) sc[i] = {lae(next[i].pb, next[i].pnb) + look(next[i].node), uint32_t(i)};
            std::nth_element(sc.begin(), sc.begin() + B, sc.end(), [](auto &a, auto &b) { return a.first > b.first; });
            beams.clear();
            for (size_t i = 0; i < B; ++i) beams.push_back(next[sc[i].second]);
        } else {
            beams = next;
        }
    }
    // Finished words: key-sequence probability plus the word prior.
    for (const Beam &b : beams) {
        const auto &n = lex_.node(b.node);
        if (n.wordBegin == n.wordEnd) continue;
        const float ctc = lae(b.pb, b.pnb);
        for (uint32_t w = n.wordBegin; w < n.wordEnd; ++w) {
            const float pr = prior ? prior(w) : p.freqWeight * lex_.freq(w);
            result.push_back({w, ctc + pr, ctc});
        }
    }
    const size_t n = std::min<size_t>(result.size(), size_t(std::max(1, topN)));
    std::partial_sort(result.begin(), result.begin() + n, result.end(), [](const SwipeCandidate &a, const SwipeCandidate &b) {
        return a.score != b.score ? a.score > b.score : a.word < b.word; });
    result.resize(n);
    return result;
}

}
