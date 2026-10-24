// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Next-word suggestions from a language's n-gram pack plus, when the pack has one, its neural model
// (LANG.nwp.bin next to LANG.ngram): the path of swipekb::Predictor for Spanish, French, German and English.
//
// WHY a hybrid and not the neural model alone: the neural model knows the 24k most common words and sees the
// whole sentence; the n-gram knows ~100k words and exact word pairs. Words are scored by both (log-linear
// blend, weight stored in the model file); words the neural model lacks keep their n-gram score.
//
// Plain C++ interface: the scoreboard tools and tests use this directly. One instance is single-threaded
// (it remembers the sentence state between calls, so typing one more letter or word is cheap).
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "swipekb/ngram_predictor.hpp"
#include "swipekb/nwp_model.hpp"

namespace swipekb {

class NwpPredictor {
public:
    // "x/es.ngram" -> "x/es.nwp.bin"; "x/context.ngram" -> "x/context.nwp.bin"; empty for any other name.
    static std::string siblingPath(const std::string &ngramModel);

    // The n-gram pack must load. `nwp` (default: siblingPath) is optional: absent or damaged = n-gram only.
    // `siblingFallback=false` means "use exactly `nwp`, and no neural model when it is empty": the keyboard plugin
    // passes only a file it has verified against the lock, so an unverified sibling must never be picked up.
    bool load(const std::string &ngramModel, const std::string &vocab, const std::string &nwp = {}, bool siblingFallback = true);
    bool ready() const { return ngram_.ready(); }
    bool hasLm() const { return lm_.loaded(); }
    const NgramPredictor &ngram() const { return ngram_; }
    const NwpModel &lm() const { return lm_; }
    // Overrides the blend stored in the model file (tuning and tests).
    void setBlend(float lambda, float oovBonus) { lambda_ = lambda; oovBonus_ = oovBonus; }
    void disableLm() { useLm_ = false; }
    const std::vector<std::string> &vocabulary() const { return ngram_.vocabulary(); }

    // Same contract as NgramPredictor::predict.
    std::vector<std::string> predict(const std::vector<std::string> &context, int topK,
                                     const std::string &prefix = {}) const;

private:
    NgramPredictor ngram_;
    NwpModel lm_;
    bool useLm_ = false;
    float lambda_ = 0.5f, oovBonus_ = 0.f;
    std::vector<int32_t> tokenToIndex_;    // neural token -> keyboard vocabulary index, -1 when the keyboard lacks it
    std::vector<uint32_t> indexToToken_;   // keyboard vocabulary index -> neural token (kUnk when the model lacks it)
    // Further keyboard words sharing a neural token (n-gram pack with apostrophes, neural model without: "were" and
    // "we're" are one neural word). -1 ends the chain that starts at tokenToIndex_.
    std::vector<int32_t> nextSameToken_;
    bool lmApostrophes_ = false;           // the neural model's words have apostrophes (same form as the n-gram)
    std::string lmForm(const std::string &word) const;
    // Sentence state after reading cachedSeq_, and the next-word log-probabilities it gives.
    mutable std::vector<uint32_t> cachedSeq_;
    mutable NwpModel::State state_;
    mutable std::vector<float> logp_;
    mutable bool logpValid_ = false;
};

}
