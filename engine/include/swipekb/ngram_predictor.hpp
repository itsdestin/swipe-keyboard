// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Next-word suggestions from a language's n-gram pack (context.ngram + vocab.tsv): the Spanish, French and
// German path of swipekb::Predictor.
//
// WHY its own file: the offline scorer (tools/swipekb-nextword-eval) must rank words with exactly the code the
// keyboard runs, from a handful of sources.
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "swipekb/ngram.hpp"

namespace swipekb {

// The last 20 context words, each at most 256 bytes: the bound every prediction model sees.
std::vector<std::string> boundedContext(const std::vector<std::string> &context);

class NgramPredictor {
public:
    // False when the model is missing/damaged or the vocabulary is empty or unreadable.
    bool load(const std::string &model, const std::string &vocab);
    bool ready() const { return ngram_.loaded() && !vocabulary_.empty(); }
    // Up to topK (max 64) display spellings, best first. May throw on allocation failure; callers catch.
    // A non-empty prefix (the letters typed so far) keeps only words whose letters start with it: completion
    // of a half-typed word, ranked by the same sentence context as the next word.
    std::vector<std::string> predict(const std::vector<std::string> &context, int topK,
                                     const std::string &prefix = {}) const;
    const std::vector<std::string> &vocabulary() const { return vocabulary_; }

    // For the hybrid with the neural model (nwp_predictor.cpp): the same ranking as predict(), but as
    // (log10 score, vocabulary index) pairs, `want` of them (max 256), plus the context state so the neural
    // side can score words the n-gram did not rank without recomputing it.
    struct Ranked {
        std::vector<std::pair<float, uint32_t>> top;
        NgramLM::Context state;
    };
    Ranked rank(const std::vector<std::string> &context, size_t want, const std::string &prefix = {}) const;
    float score(const NgramLM::Context &state, uint32_t vocabIndex) const;
    // The n-gram state after `context` (what rank() scores against) and a word's vocabulary index (-1 = not a vocabulary word).
    NgramLM::Context contextState(const std::vector<std::string> &context) const;
    // Exact display spelling first; on a miss, the same word in lower case with a curly apostrophe made straight.
    int32_t indexOf(const std::string &word) const;
    // Letters-only lower-case form of a vocabulary word (what typed letters are matched against).
    const std::string &alphaOf(uint32_t vocabIndex) const { return alpha_[vocabIndex]; }
    const NgramLM &model() const { return ngram_; }

private:
    NgramLM ngram_;
    std::vector<std::string> vocabulary_;
    std::vector<uint32_t> wordIds_;            // model number per vocabulary word; kUnk when unknown
    std::unordered_map<std::string, uint32_t> indexOfFolded_;   // lower-case, straight-apostrophe spelling -> index (first wins)
    std::unordered_map<std::string, uint32_t> indexOfWord_;   // display spelling -> index (hash lookup, not a scan of ~100k words)
    std::vector<std::string> alpha_;           // letters-only lower-case form of every vocabulary word
    // WHY these three: next-word prediction scores only the words seen after the context plus the most
    // frequent others, instead of all ~100-230k words (same ranking, many times faster; see predict()).
    std::vector<uint32_t> byUnigram_;          // known-word indices, most frequent first, ties by spelling
    std::vector<uint32_t> unknownBySpelling_;  // indices of words the model lacks, by spelling
    std::unordered_map<uint32_t, std::vector<uint32_t>> indicesOfId_;
};

}
