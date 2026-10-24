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

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "swipekb/spellings.hpp"
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
struct AlternativeParams {
    int pool = 16;              // the swipe engine's best N words considered for the places after the first
    float sentenceWeight = 1.f; // extra weight on the sentence/frequency prior when ordering those places
    int minFrequency = 80;      // dictionary frequency (0..255) below which a word is never an alternative
    int nameMinFrequency = 100; // the same for words the dictionary writes with a capital (names), except German
    float margin = 1e9f;        // an alternative may score at most this far (nats) below the first guess (off)
    bool dropStretched = true;  // drop words with one letter three times in a row ("youuuuu") as alternatives
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
    // The dictionary frequency (0..255) of this spelling (learned and texting words included); 0 = unknown.
    int frequency(const std::string &word) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace swipekb
