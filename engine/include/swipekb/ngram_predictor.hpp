// Next-word suggestions from a language's n-gram pack (context.ngram + vocab.tsv): the Spanish, French and
// German path of swipekb::Predictor.
//
// WHY its own file with no FUTO types: the offline scorer (tools/swipekb-nextword-eval) must rank words with
// exactly the code the keyboard runs, and must build without FUTO's library (card #179 removes it).
#pragma once

#include <string>
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
    std::vector<std::string> predict(const std::vector<std::string> &context, int topK) const;
    const std::vector<std::string> &vocabulary() const { return vocabulary_; }

private:
    NgramLM ngram_;
    std::vector<std::string> vocabulary_;
};

}
