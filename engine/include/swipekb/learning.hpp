// PersonalModel — what the keyboard learns from the person's own typing (Phase 2 of the open-engine roadmap).
//
// It remembers three small things, all local and all per language:
//   * words the person uses (new words such as names and slang, plus a use count for ordinary words),
//   * word pairs ("previous word" -> "word"), and
//   * words the person asked to forget (never suggested again until they type the word themselves).
// It never sees the text of a field: the keyboard hands it single words that were typed or swiped, after the
// same privacy gates that guard next-word prediction.
//
// WHY plain C++ with no Qt and no keyboard types: the offline personal test (tools/swipekb-personal-eval) must
// run the exact code the keyboard runs, and must build without FUTO's library. Callers lower-case words before
// handing them in ("keys"), so the unicode rules stay in one place (languagetextrules in the plugin).
// WHY single-threaded: only the keyboard's GUI thread touches it.
//
// WHY it re-ranks lists instead of changing the n-gram predictor: the predictors only return an ordered list
// of words, and the personal layer sits beside them. A different predictor (e.g. a neural next-word model)
// drops in underneath without touching this file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace swipekb {

struct LearningLimits {
    // Caps keep the file small and the lookups fast; the least-recently/least-often used entries go first.
    std::size_t maxWords = 4000, maxPairs = 12000, maxForgotten = 300;
    // A use counts half as much after this many days without another use (the owner asked for 30-60 days).
    double halfLifeDays = 45;
    // A word the language does not know becomes suggestible after this many uses (owner decision: 2).
    unsigned newWordUses = 2;
};

// How a personal boost is mixed with the n-gram's own order. The n-gram order gives each word a score that
// falls with its place in the list; the personal part adds on top, capped, so it can lift a word a few places
// but never throws the n-gram's choices away. Tuned on the Tatoeba personal test (see the eval tool).
struct LearningWeights {
    double rankDecay = 0.35;   // base score of place r is 1 / (1 + rankDecay * r)
    double outsidePlace = 12;  // a learned word the predictor did not list scores as if it stood in this place
    double pair = 0.55;        // largest boost for "this word usually follows the previous word"
    double word = 0.35;        // largest boost for "this person uses this word a lot"
    double pairSpeed = 0.7;    // how fast repeated pair uses approach the largest boost
    double wordSpeed = 0.25;   // the same for single words
};

class PersonalModel {
public:
    explicit PersonalModel(LearningLimits limits = {}, LearningWeights weights = {})
        : limits_(limits), weights_(weights) {}

    // --- learning -------------------------------------------------------------------------------------------
    // What learn() changed, so a swipe the person immediately corrects or erases can be taken back.
    struct Undo {
        bool valid = false;
        std::string prev, word;
        bool hadWord = false, hadPair = false, wasForgotten = false;
        double wordScore = 0, pairScore = 0;
        std::int64_t wordLast = 0, pairLast = 0;
        std::uint32_t wordUses = 0, wordCapUses = 0, pairUses = 0;
        bool wordKnown = false;
    };
    // Counts one use of `word` (lower case) after `prev` (lower case, "" at a sentence start). `known` = the
    // language's dictionary already has the word. `capitalized` = typed with a capital in the middle of a
    // sentence (a name). `now` = seconds. Refuses (valid=false) words that are not plain single words.
    Undo learn(const std::string &prev, const std::string &word, bool known, bool capitalized, std::int64_t now);
    void undo(const Undo &undo);

    // --- using what was learned ----------------------------------------------------------------------------
    struct Ranked {
        std::string key;      // lower-case word
        int baseIndex = -1;   // place in the list given to rerank(), or -1 for a learned word added to it
    };
    // Re-orders `base` (lower-case keys, best first; may be empty) and returns at most `topK`. `prefix` = the
    // letters typed so far of the current word ("" = next word). When `inject` is true, learned words that fit
    // (follow `prev`, or start with `prefix`) join the list even if the predictor did not list them.
    // Forgotten words are dropped from the result.
    // `boostScale` scales the personal part (1 = full; swipes use less, because the finger path is strong evidence).
    std::vector<Ranked> rerank(const std::vector<std::string> &base, const std::string &prev, const std::string &prefix,
                               std::int64_t now, std::size_t topK, bool inject, double boostScale = 1.0) const;

    bool isForgotten(const std::string &word) const { return forgotten_.count(word) != 0; }
    // True when this person has used the word at least `minUses` times (a swear word they typed themselves
    // may then be offered unprompted).
    bool usedByPerson(const std::string &word, unsigned minUses = 2) const;
    // True for words the language did not know and that now have enough uses to be suggested and swiped.
    bool learnedNewWord(const std::string &word, std::int64_t now) const;
    // True when the word has personal data worth offering "remove" for.
    bool removable(const std::string &word) const;
    // Name-like words (capital in the middle of sentences) are shown with a capital.
    bool wantsCapital(const std::string &word) const;
    // New words to add to the swipe dictionaries, with a 0..255 frequency. Changes only when the set changes.
    std::vector<std::pair<std::string, int>> swipeWords(std::int64_t now) const;
    std::uint64_t swipeWordsVersion() const { return swipeVersion_; }

    // --- forgetting -----------------------------------------------------------------------------------------
    void forget(const std::string &word);   // erases the word and every pair with it; never suggested again until typed again
    void clear();                           // forgets everything, including the forgotten list

    std::size_t wordCount() const { return words_.size(); }
    std::size_t pairCount() const { return pairs_.size(); }
    bool empty() const { return words_.empty() && pairs_.empty() && forgotten_.empty(); }
    bool dirty() const { return dirty_; }
    void markClean() { dirty_ = false; }

    // --- storage ---------------------------------------------------------------------------------------------
    // Text with a version line and an end line. parse() returns false (and leaves the model empty) for anything
    // damaged, truncated, from a newer version or oversized: a bad file must never break the keyboard.
    std::string serialize() const;
    bool parse(const std::string &text);

    const LearningLimits &limits() const { return limits_; }
    LearningWeights &weights() { return weights_; }

private:
    struct Word {
        double score = 0;
        std::int64_t last = 0;
        std::uint32_t uses = 0, capUses = 0;
        bool known = false;
    };
    struct Pair {
        double score = 0;
        std::int64_t last = 0;
        std::uint32_t uses = 0;
    };
    double decayed(double score, std::int64_t last, std::int64_t now) const;
    bool suggestible(const Word &w, std::int64_t now) const;
    double boost(const std::string &prev, const std::string &key, std::int64_t now) const;
    void prune(std::int64_t now);
    void rebuildFollowers();
    static std::string pairKey(const std::string &prev, const std::string &word) { return prev + '\x1f' + word; }
    static bool plain(const std::string &s);

    LearningLimits limits_;
    LearningWeights weights_;
    std::unordered_map<std::string, Word> words_;
    std::unordered_map<std::string, Pair> pairs_;
    std::unordered_map<std::string, std::vector<std::string>> followers_;   // previous word -> words seen after it
    std::unordered_set<std::string> forgotten_;
    std::uint64_t swipeVersion_ = 1;
    bool dirty_ = false;
};

}
