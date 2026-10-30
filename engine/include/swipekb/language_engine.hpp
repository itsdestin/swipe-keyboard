// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Integrators, start here (see engine/README.md for a worked example, the files to download and their licences):
//   LanguageEngine e; LanguageEngineSetup s; s.language="en"; s.dictionary="en_swipe.combined";
//   e.init(s);  e.recognize(trace, keys, contextWords(textBeforeCursor), 3);  e.complete("hel");
// Only the dictionary is required; the n-gram pack and the swipe model are optional and each one that is missing
// or damaged only switches its own part off. Needs Qt 6 Core. MIT licence.
//
// LanguageEngine — everything the keyboard (and the Gaming Mode daemon) asks of one language, behind one class:
//   * swipe recognition: the project's own swipe engine (network + dictionary beam search + geometric cross-check,
//     with the geometry-only decoder as fallback) ranked by the language's n-gram sentence prior when it has one;
//   * the language's dictionary (`.combined` word lists): typed-word completion with one-edit corrections, similar
//     words for tap-to-swap, other spellings (its / it's), the dictionary's display spelling, offensive flags;
//   * words added at run time: the person's learned words (a set that is replaced, so forgotten words go away)
//     and the built-in texting words (which only ever raise a word).
//
// WHY one facade: the plugin and the daemon must give the same words for the same input, and neither should know
// which parts (model file, n-gram pack) are present. Nothing here throws to the caller: a missing or damaged
// model or n-gram pack only removes that part, a broken swipe gives no guesses.
//
// Threading: one instance per thread; call every member from the thread that owns the instance.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "swipekb/spellings.hpp"
#include "swipekb/swap.hpp"
#include "swipekb/swipe_rerank.hpp"
#include "swipekb/text.hpp"

namespace swipekb {

struct LanguageEngineSetup {
    std::string language = "en";   // "en", "es", "fr", "de"; German keeps the dictionary's capitals (nouns)
    std::string dictionary;        // the language's verified `.combined` word list (required)
    std::string ngram;             // optional n-gram pack ("SWKBNG2"): sentence prior for swipes; "" = word frequency
    std::string swipeModel;        // optional swipe network file ("SKNN"); "" or unusable = geometry only
};

// How the guesses after the first one are chosen (recognize()). WHY its own knobs: the first guess is what the
// finger path and the sentence say most strongly; the others are only worth showing when they are words the person
// could have meant. Without these, the next-closest finger paths came out as rare names, stretched words and rare
// words ("you" -> "youuuuu", "tou"; "way" -> "waaaay", "watt"). Defaults tuned on 2,235 recorded English swipes
// from sessions never trained on, tuned on or scored with (2026-10-24): shown alternatives that are rare,
// stretched or rare names fell from 43.7% to 5.5%; the right word in the top 3 went 93.38% -> 92.80% (those
// recordings are word prompts, rich in rare words and names); the first guess is unchanged by construction.
// Other languages keep only dropStretched (see init()): the frequency rules lowered their top 3.
struct AlternativeParams {
    int pool = 16;              // the swipe engine's best N words considered for the places after the first
    float sentenceWeight = 1.f; // extra weight on the sentence/frequency prior when ordering those places
    int minFrequency = 80;      // dictionary frequency (0..255) below which a word is never an alternative
    int nameMinFrequency = 100; // the same for words the dictionary writes with a capital (names), except German
    float margin = 1e9f;        // an alternative may score at most this far (nats) below the first guess (off)
    bool dropStretched = true;  // drop words with one letter three times in a row ("youuuuu") as alternatives
};

// Optional: the neural next-word model's opinion added to the swipe ranking (joint-model, steps 1-2).
// OFF unless `scorer` is set (the Gaming Mode daemon sets none). The swipe engine's best `pool`
// words are re-scored: score + (ngramWeight - 1) * their n-gram prior + neuralWeight * ln(10) * neural, where neural =
// scorer(previous words, words), log10 (Predictor::scoreWords); NaN = no opinion on that word, which then takes its own
// n-gram prior in the neural score's place. The search itself is not changed (its pruning still uses the n-gram prior),
// so only words it found can move up. The keyboard's two Plasma front ends set it (NextWordWorker::scoreForSwipe).
// WHY a function and not a Predictor: the engine stays free of the next-word model; the bench tools hand it
// Predictor::scoreWords, and a front end could hand it whatever worker owns the model.
struct ContextRescore {
    std::function<std::vector<float>(const std::vector<std::string> &context, const std::vector<std::string> &words)> scorer;
    float neuralWeight = 0.f;    // multiplies ln(10) * the neural log10 score (nats)
    float ngramWeight = 1.f;     // how much of the search's own n-gram prior is kept (1 = all, 0 = none)
    int pool = 30;               // the swipe engine's best N words that are re-scored
    // WHY only with words before (default): with none, the neural model only says how common a word is at a
    // sentence start, which the n-gram prior already says; HeliBoard sets (single words) are then unchanged.
    bool needWordsBefore = true;
    // Joint-model step 3 (optional): a small learned model (SwipeReranker) orders the pool instead of the two fixed
    // weights above. With it, a swipe with no words before is re-ordered too (its "no sentence" weights; the scorer is
    // not asked), and needWordsBefore is ignored. A swipe with words before whose scorer gives no usable answer keeps
    // the search's order, as without the model. nullptr (or a model lacking that weight set) = the fixed weights.
    std::shared_ptr<const SwipeReranker> reranker;
    // The weights tuned per language on tuning swipes only (tools/swipekb-rescore-bench, bench/rescore-tune.py); see
    // the .cpp. The scorer is left empty: the caller must still set it.
    static ContextRescore forLanguage(const std::string &language);
};

// One word the swipe search found, with the parts of its score (for measurement tools: swipekb-rescore-bench).
// One found word and the facts the learned re-ranker sees about it (measurement tools: swipekb-rescore-bench --dump).
struct RerankCandidate {
    std::string word;   // display spelling
    RerankRow features;
};

struct RawSwipeCandidate {
    std::string word;   // display spelling
    float score = 0;    // what recognize() ranks by before any re-scoring: path + prior - geometry (nats)
    float ctc = 0;      // path alone (nats)
    float prior = 0;    // the sentence (n-gram) or frequency prior inside `score` (nats)
};

class LanguageEngine {
public:
    LanguageEngine();
    ~LanguageEngine();
    LanguageEngine(const LanguageEngine &) = delete;
    LanguageEngine &operator=(const LanguageEngine &) = delete;

    // Reads the dictionary (required) and the optional parts. False (and not ready) when the dictionary cannot be
    // read or holds no usable word.
    bool init(const LanguageEngineSetup &setup);
    bool ready() const;
    bool hasModel() const;      // false = swipes are decoded by geometry only
    bool usingNgram() const;    // true = the n-gram pack loaded and ranks swipes by the sentence
    const char *mode() const;   // "own" (network + search) or "own-geometry"

    // Best-first guesses (display spellings), at most topK. `keys` are the live letter-key rectangles; `context` is
    // the previous words of the sentence, oldest first (contextWords()). Empty when not ready or the input is
    // unusable. The first call after a new letter set or new words rebuilds the word list once (a short pause).
    std::vector<Candidate> recognize(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &keys,
                                     const std::vector<std::string> &context, int topK);
    // The guess of the last recognize() whose path fits the swipe best, before the sentence prior ("" if none).
    std::string bestShapeWord() const;

    // Candidates to swap `word` with (tap-to-swap), scored on path and the n-gram (before/after), not yet ranked
    // (rankSwap()). They are the swipe search's words for this path (rare, stretched and name junk dropped as for
    // alternatives, SwapParams::minFrequency as the rare cutoff) plus the word's other spellings, at most
    // SwapParams::pool, then up to SwapParams::neighbours one-edit dictionary words (similar()) the search did not
    // find, with their own fit to the path; never `word` itself. `path` = the path's natural-log fit (ctc), other spellings get the tapped
    // word's; `before`/`after` = log10 n-gram evidence from the words before and up to two words after (without a
    // pack: before = the frequency prior on the same scale, after = 0). `trace`: the swipe that produced the word,
    // empty for a typed word or a forgotten swipe (a made-up path through the word's keys is used). `after` may be
    // empty (no words follow, or the field cannot be read past the word). Empty when not ready or unusable,
    // and for a typed word with a character that is on no key (a digit): it has no path.
    // WHY not const: like recognize(), it sets the search's sentence prior and uses its scratch space.
    std::vector<SwapCandidate> swapCandidates(const std::string &word, const std::vector<KeyRect> &keys,
                                              const std::vector<std::string> &before,
                                              const std::vector<std::string> &after,
                                              const std::vector<TracePoint> &trace, const SwapParams &p);

    // Path fit (ctc, the scale of SwapCandidate::path) of each word against this swipe, found by the search or not.
    // Empty without a network (geometry only) or for an unusable path. For measurement tools (swap bench --diagnose).
    std::vector<float> pathFits(const std::vector<std::string> &words, const std::vector<KeyRect> &keys,
                                const std::vector<TracePoint> &trace);

    // Dictionary words that start with the typed letters, most frequent first, followed by close corrections
    // (one letter replaced, missing, extra or two swapped; a missing double letter costs least). Never a word
    // shorter than what was typed. Empty for "" or more than 48 letters.
    std::vector<Candidate> complete(const std::string &typed, int topK = 3) const;
    // Whole dictionary words exactly one edit from `word`, most frequent first; never `word` itself, its other
    // spellings or words that merely extend it. Case-insensitive. Empty for words under 2 or over 48 letters.
    std::vector<Candidate> similar(const std::string &word, int topK = 12) const;
    // The same swipe's other dictionary spellings, most frequent first ("its" -> {"it's"}).
    std::vector<std::string> otherSpellings(const std::string &word) const;
    // The dictionary's usual spelling of a word typed in any case or without its apostrophe ("dont" -> "don't");
    // the word itself when the dictionary does not know it.
    std::string displaySpelling(const std::string &word) const;
    // True when the dictionary (or a learned/texting word) has this word; case, apostrophes and hyphens ignored.
    bool hasWord(const std::string &word) const;
    // True when the dictionary marks the word offensive (flags=offensive / possibly_offensive=true).
    bool isOffensive(const std::string &word) const;

    // The person's learned words (0..255 frequency). Replaces the previous set: words left out are gone again
    // (a dictionary word gets its own frequency back). A learned frequency overrides the dictionary's.
    // Words must be letters only (no digits, apostrophes or spaces); others are skipped.
    void setLearnedWords(const std::vector<std::pair<std::string, int>> &words);
    // Built-in texting words (lol, wyd, jaja...): only RAISE a word. A word the dictionary has at this frequency or
    // higher keeps its own; a word it marks "not a word" comes in at this frequency. Same letters-only rule.
    void liftWords(const std::vector<std::pair<std::string, int>> &words);
    // Words shown with a capital letter (Capitals). Lower case stays the rule for every other word. Ignored in
    // German, which keeps the dictionary's own capitals; nullptr = all lower case (as before).
    void setCapitals(std::shared_ptr<const Capitals> capitals);
    // Replaces the alternative-guess rules (see AlternativeParams); for measurement tools.
    void setAlternativeParams(const AlternativeParams &params);
    // Turns the neural re-scoring of swipes on (a scorer set) or off (no scorer, the default). See ContextRescore.
    void setContextRescore(ContextRescore rescore);
    // The swipe search's own list for this swipe, best first, at most topN, before re-scoring or the alternative
    // rules. `beamWidth` > 0 searches that wide for this call only (the recognizer's own width is put back).
    // For measurement tools (how far down the right word is, and whether a wider search finds it).
    std::vector<RawSwipeCandidate> rawCandidates(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &keys,
                                                 const std::vector<std::string> &context, int topN, int beamWidth = 0);
    // The words recognize() would re-order for this swipe (the search's best ContextRescore::pool, in its order) with
    // the re-ranker's features, exactly as recognize() computes them; the neural score comes from the ContextRescore
    // scorer that is set (asked only with words before). Empty when not ready or unusable. For the training script.
    std::vector<RerankCandidate> rerankCandidates(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &keys,
                                                  const std::vector<std::string> &context);
    // The dictionary frequency (0..255) of this spelling (learned and texting words included); 0 = unknown.
    int frequency(const std::string &word) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace swipekb
