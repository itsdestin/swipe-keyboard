// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Sentence-context word prior for the swipe search, from the project's own n-gram model (ngram.hpp).
//
// WHY a prior object instead of baking the language model into the search: the same decoder then runs with no
// model (dictionary frequency), with the n-gram file, or with any future scorer; and "the previous words" are
// fixed per swipe, so the context is prepared once and each candidate word costs a couple of table lookups.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "swipekb/ngram.hpp"
#include "swipekb/swipe_beam.hpp"

namespace swipekb {

class NgramWordPrior {
public:
    // Both must outlive this object. Resolves every lexicon word to its model number once (slow-ish: do it at load).
    NgramWordPrior(const NgramLM &lm, const SwipeLexicon &lex);
    // Previous words of the current sentence, oldest first; empty = sentence start. Cheap; call once per swipe.
    void setContext(const std::vector<std::string> &previousWords);
    // Natural-log prior of a lexicon word in the current context: weight * ln P_ngram + freqWeight * frequency.
    float operator()(uint32_t word) const;
    // Lexicon words the model has seen right after the context (at most maxHints), for the search's pruning bonus.
    std::vector<uint32_t> hints(size_t maxHints = 200) const;

    // True: ignore the previous words and use the plain word frequency from the model (single-word swipes, benchmarks).
    bool unigramOnly = false;
    float lmWeight = 1.0f;     // multiplies ln P from the n-gram file
    float freqWeight = 0.0f;   // extra nats per dictionary-frequency point (the model already contains frequency)

private:
    const NgramLM &lm_;
    const SwipeLexicon &lex_;
    NgramLM::Context ctx_;
    std::vector<uint32_t> ids_;                                // model number per lexicon word
    std::vector<uint32_t> wordsOfId_, idSlot_;                 // lexicon words grouped by model number (CSR rows)
};

}
