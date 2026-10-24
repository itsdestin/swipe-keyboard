// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "swipekb/nwp_predictor.hpp"
#include "swipekb/prediction.hpp"

#include <QString>
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace swipekb {
namespace {
constexpr size_t kCandidates = 32;   // per side: the n-gram's best and the neural model's best
}

std::string NwpPredictor::siblingPath(const std::string &model)
{
    static const std::string tail = ".ngram";
    if (model.size() < tail.size() || model.compare(model.size() - tail.size(), tail.size(), tail) != 0) return {};
    return model.substr(0, model.size() - tail.size()) + ".nwp.bin";
}

bool NwpPredictor::load(const std::string &ngramModel, const std::string &vocab, const std::string &nwp, bool siblingFallback)
{
    useLm_ = false;
    if (!ngram_.load(ngramModel, vocab)) return false;
    const std::string path = !nwp.empty() ? nwp : (siblingFallback ? siblingPath(ngramModel) : std::string());
    // WHY a failed neural load is not an error: the pack still works as before (n-gram only).
    if (path.empty() || !lm_.load(path)) return true;
    const auto &words = ngram_.vocabulary();
    lmApostrophes_ = false;
    for (uint32_t t = 3; t < lm_.vocabSize() && !lmApostrophes_; ++t) lmApostrophes_ = lm_.word(t).find('\'') != std::string::npos;
    tokenToIndex_.assign(lm_.vocabSize(), -1);
    indexToToken_.assign(words.size(), NwpModel::kUnk);
    nextSameToken_.assign(words.size(), -1);
    std::vector<int32_t> lastOfToken(lm_.vocabSize(), -1);
    size_t shared = 0;
    for (uint32_t i = 0; i < words.size(); ++i) {
        const uint32_t t = lm_.token(lmApostrophes_ ? ngram_.model().form(words[i]) : ngram_.alphaOf(i));
        if (t == NwpModel::kUnk) continue;
        // WHY every word of the token, not only the first: with an apostrophe n-gram and a letters-only neural model,
        // "were" and "we're" both take the neural score of "were", split between them by the n-gram (predict()).
        indexToToken_[i] = t;
        if (tokenToIndex_[t] < 0) { tokenToIndex_[t] = int32_t(i); ++shared; }
        else nextSameToken_[size_t(lastOfToken[t])] = int32_t(i);
        lastOfToken[t] = int32_t(i);
    }
    // WHY a floor on overlap: a model built for another language/word list would only add noise.
    if (shared * 2 < size_t(lm_.vocabSize())) return true;
    lambda_ = lm_.lambda(); oovBonus_ = lm_.oovBonus();
    cachedSeq_.clear(); state_ = lm_.start(); logpValid_ = false;
    useLm_ = true;
    return true;
}

std::string NwpPredictor::lmForm(const std::string &word) const
{
    // WHY: a neural model with apostrophes reads "we're" as written; a letters-only one as "were".
    return lmApostrophes_ ? NgramLM::apostropheForm(word) : NgramLM::alphaForm(word);
}

std::vector<std::string> NwpPredictor::predict(const std::vector<std::string> &context, int topK,
                                               const std::string &prefix) const
{
    if (!useLm_) return ngram_.predict(context, topK, prefix);
    if (!ready() || topK <= 0) return {};
    topK = std::min(topK, 64);
    // The sentence as the model reads it: <s>, then each word (0 = unknown to the model). WHY <s> only when the
    // context is shorter than the 20-word cap: a capped context starts mid-sentence, as in training.
    const auto bounded = boundedContext(context);
    std::vector<uint32_t> seq;
    if (bounded.size() < 20) seq.push_back(NwpModel::kBos);
    for (const auto &word : bounded) {
        const auto form = lmForm(word);
        if (!form.empty()) seq.push_back(lm_.token(form));
    }
    // WHY reuse: while a word is typed, and as a sentence grows by one word, only the new tokens are read.
    const bool extends = cachedSeq_.size() <= seq.size() && std::equal(cachedSeq_.begin(), cachedSeq_.end(), seq.begin());
    if (!extends) { state_ = lm_.start(); cachedSeq_.clear(); logpValid_ = false; }
    if (cachedSeq_.size() != seq.size() || !logpValid_) {
        for (size_t i = cachedSeq_.size(); i < seq.size(); ++i) lm_.step(state_, seq[i]);
        cachedSeq_ = seq;
        lm_.logProbs(state_, logp_);
        logpValid_ = true;
    }

    const auto ng = ngram_.rank(context, kCandidates, prefix);
    const std::string typed = prefix.empty() ? std::string() : NgramLM::alphaForm(prefix);
    if (!prefix.empty() && typed.empty()) return {};
    // The neural model's own best words (matching the typed letters), so a word the n-gram ranks low can still win.
    // WHY a small heap instead of sorting the 24k matching words: all but a handful are rejected by one compare.
    std::vector<std::pair<float, uint32_t>> lmTop;
    const auto worse = [](const auto &a, const auto &b) { return a.first > b.first; };   // min-heap on score
    for (uint32_t t = 3; t < lm_.vocabSize(); ++t) {
        if (lmTop.size() == kCandidates && logp_[t] <= lmTop.front().first) continue;
        if (tokenToIndex_[t] < 0) continue;
        if (!typed.empty() && ngram_.alphaOf(uint32_t(tokenToIndex_[t])).compare(0, typed.size(), typed) != 0) continue;
        if (lmTop.size() == kCandidates) { std::pop_heap(lmTop.begin(), lmTop.end(), worse); lmTop.pop_back(); }
        lmTop.emplace_back(logp_[t], t);
        std::push_heap(lmTop.begin(), lmTop.end(), worse);
    }
    const size_t keep = lmTop.size();

    constexpr float kLog10 = 2.30258509f;
    std::unordered_set<uint32_t> seen;
    std::vector<std::pair<float, uint32_t>> blended;
    const auto add = [&](uint32_t index) {
        if (!seen.insert(index).second) return;
        const float n = ngram_.score(ng.state, index);
        const uint32_t t = indexToToken_[index];
        float neural = t >= 3 ? logp_[t] / kLog10 : 0.f;
        if (t >= 3 && (tokenToIndex_[t] != int32_t(index) || nextSameToken_[index] >= 0)) {
            // Several words share this neural word: each gets its share by the n-gram's odds in this context
            // (log10 of its probability over the group's total), so the shares add up to the neural score.
            float total = 0;
            for (int32_t j = tokenToIndex_[t]; j >= 0; j = nextSameToken_[size_t(j)]) total += std::pow(10.f, ngram_.score(ng.state, uint32_t(j)));
            if (total > 0) neural += n - std::log10(total);
        }
        // Words the neural model knows: blend both opinions. Others: the n-gram alone (plus a tuned offset).
        blended.emplace_back(t >= 3 ? lambda_ * neural + (1.f - lambda_) * n : n + oovBonus_, index);
    };
    for (const auto &[score, index] : ng.top) add(index);
    for (size_t k = 0; k < keep; ++k)
        for (int32_t j = tokenToIndex_[lmTop[k].second]; j >= 0; j = nextSameToken_[size_t(j)]) add(uint32_t(j));
    const auto &words = ngram_.vocabulary();
    std::sort(blended.begin(), blended.end(), [&](const auto &a, const auto &b) {
        return a.first == b.first ? words[a.second] < words[b.second] : a.first > b.first;
    });
    std::vector<std::pair<std::string, float>> shown;
    for (const auto &[score, index] : blended) shown.emplace_back(words[index], score);
    return filterPredictedWords(shown, topK);
}

}
