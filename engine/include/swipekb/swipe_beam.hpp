// Lexicon trie + CTC prefix beam search: turns the network's per-step key probabilities into ranked words.
//
// WHY a trie-constrained search (the published standard for CTC gesture typing): only prefixes of real dictionary
// words are ever extended, so the search is cheap and can never output a non-word; a word prior (dictionary
// frequency, or a sentence-context language model) is added when a word is finished, and a cheap optimistic
// "best word below this prefix" bonus keeps likely words alive while the beam is pruned.
// Written without reference to FUTO code.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "swipekb/swipe_layout.hpp"

namespace swipekb {

class SwipeLexicon {
public:
    // Adds a word (UTF-8) with a 0..255 frequency score. Words are matched by their folded key sequence; a word that
    // maps to no key is ignored. Call finish() once after the last add().
    void add(std::string_view word, int freq, const SwipeLayout &layout);
    // "word<TAB>freq" lines. Returns the number of words added (0 = unreadable).
    size_t loadTsv(const std::string &path, const SwipeLayout &layout);
    void finish();

    size_t wordCount() const { return words_.size(); }
    const std::string &text(uint32_t w) const { return words_[w].text; }
    int freq(uint32_t w) const { return words_[w].freq; }
    const std::vector<uint8_t> &keys(uint32_t w) const { return words_[w].keys; }
    size_t nodeCount() const { return nodes_.size(); }

    struct Node {
        int32_t firstChild = -1, nextSibling = -1;
        uint32_t wordBegin = 0, wordEnd = 0;   // words ending exactly here (best frequency first)
        uint8_t key = 0;                       // key that leads into this node
        uint8_t bestFreq = 0;                  // best frequency of any word at or below this node
    };
    const Node &node(int32_t i) const { return nodes_[i]; }
    // Words in trie order, so a node's words are the contiguous range [wordBegin, wordEnd).
    uint32_t wordAt(uint32_t slot) const { return slot; }
    // Node reached by a key sequence, or -1.
    int32_t find(const std::vector<int> &keys) const;

private:
    struct Word { std::string text; std::vector<uint8_t> keys; int freq; };
    std::vector<Word> words_;
    std::vector<Node> nodes_;
    struct Pending { std::string lower; std::vector<uint8_t> keys; std::string text; int freq; };
    std::vector<Pending> pending_;
};

struct BeamParams {
    int beam = 48;           // prefixes kept per step
    int keysPerStep = 8;     // only the most likely keys at a step may start a new letter
    float lookahead = 0.03f; // nats per frequency point of the best word below a prefix (pruning only)
    float freqWeight = 0.03f;// nats per frequency point added when a word is finished (when no scorer is given)
    float hintBonus = 4.0f;  // extra pruning bonus for words the sentence context makes likely
};

struct SwipeCandidate {
    uint32_t word = 0;
    float score = 0;   // ctc + prior, nats, higher is better
    float ctc = 0;     // log-probability of the key sequence alone
};

// Thread-confined: owns reusable scratch arrays sized to the lexicon.
class SwipeBeamSearch {
public:
    explicit SwipeBeamSearch(const SwipeLexicon &lex) : lex_(lex) {}
    // logProbs: steps x (1 + keyCount), blank first. `prior` (optional) returns the natural-log prior of a lexicon word
    // (language model in context + frequency); without it dictionary frequency is used. `hints` are words the context
    // favours; they get a pruning bonus so the beam does not drop them early.
    std::vector<SwipeCandidate> decode(const float *logProbs, int steps, int keyCount, const BeamParams &p, int topN,
                                       const std::function<float(uint32_t)> &prior = {},
                                       const std::vector<uint32_t> &hints = {});

private:
    const SwipeLexicon &lex_;
    std::vector<uint32_t> stamp_;
    std::vector<int32_t> slot_;
    std::vector<uint32_t> bonusStamp_;
    std::vector<float> bonus_;
    uint32_t generation_ = 0;
};

}
