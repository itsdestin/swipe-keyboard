// Next-word suggestions from a language's n-gram pack (context.ngram + vocab.tsv): the Spanish, French and
// German path of swipekb::Predictor.
//
// WHY its own file with no FUTO types: the offline scorer (tools/swipekb-nextword-eval) must rank words with
// exactly the code the keyboard runs, and must build without FUTO's library (card #179 removes it).
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

private:
    NgramLM ngram_;
    std::vector<std::string> vocabulary_;
    std::vector<uint32_t> wordIds_;            // model number per vocabulary word; kUnk when unknown
    std::vector<std::string> alpha_;           // letters-only lower-case form of every vocabulary word
    // WHY these three: next-word prediction scores only the words seen after the context plus the most
    // frequent others, instead of all ~100-230k words (same ranking, many times faster; see predict()).
    std::vector<uint32_t> byUnigram_;          // known-word indices, most frequent first, ties by spelling
    std::vector<uint32_t> unknownBySpelling_;  // indices of words the model lacks, by spelling
    std::unordered_map<uint32_t, std::vector<uint32_t>> indicesOfId_;
};

}
