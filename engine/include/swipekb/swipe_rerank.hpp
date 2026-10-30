// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// SwipeReranker — joint-model experiment, step 3: a tiny learned model that re-orders the swipe search's best words.
//
// The search (network + dictionary + n-gram prior + geometric cross-check) hands back its best ~30 words for a swipe.
// When its first word is wrong, the right one is usually already among them (2nd or 3rd). The fixed re-scoring of
// steps 1-2 (ContextRescore weights) adds the neural sentence score with one weight; this model instead weighs a
// handful of facts about each word that the engine already has (how well the finger path fits, the sentence and the
// geometric cross-check separately, the neural sentence score, frequency, length against the length of the swipe,
// how far the swipe starts and ends from the word's first and last key, ...) and was trained on recorded swipes the
// swipe model never saw (scripts/swipe/train_rerank.py). Two weight sets: one for swipes with words before (the neural
// score is known) and one for a swipe with none (the first word of a message, single-word input).
//
// Model: per word, s = lin . z + out . relu(W z + b), z = (features - mean) / scale. The highest s comes first (only
// the order matters, so it is listwise: no threshold, no probability). With no hidden layer it is a linear model.
// WHY a plain text file format and not code: the training script writes it, the bench tools read it to try weights
// without a rebuild, and the chosen weights are compiled into the engine from the same text (builtIn()).
// Deterministic, no allocation per word beyond a scratch row, about 1 microsecond per word.
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace swipekb {

// The facts about one found word. Every value is finite by construction (clipped; a word without a neural score takes
// its n-gram probability in its place, see kRrNeural; no sentence = 0). WHY relative values (to the list's best): a swipe's scores move together with its length; only
// the differences between its words say which one was meant.
enum RerankFeature : int {
    kRrScore = 0,     // the search's own score minus the first word's (nats; <= 0)
    kRrPath,          // path fit (ctc) minus the best path fit in the list (nats; <= 0)
    kRrPrior,         // sentence / frequency prior minus the best prior in the list (nats; <= 0)
    kRrGeo,           // geometric cross-check cost the search subtracted (nats; >= 0)
    kRrNeural,        // ln(10) x neural sentence score minus the best in the list (nats; <= 0); see below
    kRrFirst,         // 1 for the search's first word
    kRrRankLog,       // ln(1 + place in the search's list)
    kRrFreq,          // dictionary frequency / 255 (learned and texting words included)
    kRrLetters,       // letters in the word / 10
    kRrLengthFit,     // |ln(length of the swipe / length of the word's ideal path)|, key widths, at most 3
    kRrStart,         // key widths from where the swipe starts to the word's first key, at most 4
    kRrEnd,           // key widths from where the swipe ends to the word's last key, at most 4
    kRrApostrophe,    // 1 when the word has an apostrophe
    kRrFirstGap,      // first word only: how far ahead of the second it was in the search (nats, at most 10)
    kRrPathRank,      // ln(1 + place in the list by path fit alone)
    kRrNeuralRank,    // ln(1 + place in the list by neural sentence score alone)
    kRrSwipeLength,   // length of the swipe in key widths / 10, at most 3 (the same for every word of a swipe)
    kRrCount
};
// A word the neural model has no score for (the next-word vocabulary lacks it, e.g. a word the person taught) takes
// its n-gram probability in the sentence in the neural score's place (the same log scale), as the fixed re-scoring does;
// without an n-gram pack, the list's average neural score. WHY: a missing score must not read as "very unlikely", or a
// learned word would be pushed below every other guess after a sentence.
// WHY no "is capitalised" or "is a learned word" feature: the training swipes hold neither (no learned words; the
// tools that replay them show words in lower case), so their weights could not be learned, only guessed.
using RerankRow = std::array<float, kRrCount>;
const char *rerankFeatureName(int feature);

class SwipeReranker {
public:
    // Which weight set: a swipe with words before the cursor (neural score known), or one without.
    enum Set { WithSentence = 0, NoSentence = 1 };

    // Reads the text format written by scripts/swipe/train_rerank.py. False (and not ready) when unreadable, when the
    // feature count differs from kRrCount, or when any number is not finite.
    bool parse(const std::string &text);
    bool load(const std::string &path);
    bool ready() const { return ready_; }
    bool has(Set set) const { return ready_ && sets_[set].present; }
    // The model's score for one word (higher = more likely meant). A non-finite feature counts as the training mean.
    float score(Set set, const RerankRow &f) const;
    // The weights compiled into the engine for this language ("en", "fr", ...), or nullptr when there are none.
    static std::shared_ptr<const SwipeReranker> builtIn(const std::string &language);

private:
    struct Model {
        bool present = false;
        int hidden = 0;
        std::array<float, kRrCount> mean{}, scale{}, lin{};
        std::vector<float> w1, b1, out;   // hidden x kRrCount, hidden, hidden
    };
    std::array<Model, 2> sets_;
    bool ready_ = false;
};

} // namespace swipekb
