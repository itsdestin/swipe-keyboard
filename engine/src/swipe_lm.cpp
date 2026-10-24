// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "swipekb/swipe_lm.hpp"

#include <algorithm>
#include <cmath>

namespace swipekb {

NgramWordPrior::NgramWordPrior(const NgramLM &lm, const SwipeLexicon &lex) : lm_(lm), lex_(lex) {
    ids_.resize(lex.wordCount());
    for (uint32_t w = 0; w < lex.wordCount(); ++w) ids_[w] = lm.id(lm.form(lex.text(w)));
    ctx_ = lm.context({});
    // Lexicon words grouped by model number (compressed rows), so the context's favoured words map back quickly.
    uint32_t maxId = 0;
    for (uint32_t id : ids_) maxId = std::max(maxId, id);
    idSlot_.assign(size_t(maxId) + 2, 0);
    for (uint32_t id : ids_) ++idSlot_[id + 1];
    for (size_t i = 1; i < idSlot_.size(); ++i) idSlot_[i] += idSlot_[i - 1];
    wordsOfId_.assign(ids_.size(), 0);
    std::vector<uint32_t> fill(idSlot_.begin(), idSlot_.end() - 1);
    for (uint32_t w = 0; w < ids_.size(); ++w) wordsOfId_[fill[ids_[w]]++] = w;
}

void NgramWordPrior::setContext(const std::vector<std::string> &previousWords) { ctx_ = lm_.context(previousWords); }

float NgramWordPrior::operator()(uint32_t w) const {
    constexpr float kLn10 = 2.302585093f;
    const uint32_t id = ids_[w];
    // Known word: its model number gives the exact score. Unknown word: the model's own fallback from frequency.
    const float lg10 = id != NgramLM::kUnk ? (unigramOnly ? lm_.unigramLogProb(id) : lm_.logProb(ctx_, id))
                                           : lm_.logProb(ctx_, lm_.form(lex_.text(w)), float(lex_.freq(w)));
    return lmWeight * kLn10 * lg10 + freqWeight * lex_.freq(w);
}

std::vector<uint32_t> NgramWordPrior::hints(size_t maxHints) const {
    std::vector<uint32_t> cont, out;
    lm_.continuations(ctx_, cont);
    for (uint32_t id : cont) {
        if (id == NgramLM::kUnk || id + 1 >= idSlot_.size()) continue;
        for (uint32_t i = idSlot_[id]; i < idSlot_[id + 1]; ++i) {
            out.push_back(wordsOfId_[i]);
            if (out.size() >= maxHints) return out;
        }
    }
    return out;
}

}
