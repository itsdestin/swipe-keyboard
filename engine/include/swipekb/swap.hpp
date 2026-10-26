// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Swap lists: the look-alike words offered for a word the user typed or swiped. Every swap list goes through
// rankSwap so the ordering rule lives in exactly one place.
#pragma once

#include <string>
#include <vector>

#include "swipekb/text.hpp"

namespace swipekb {

struct SwapCandidate {
    std::string word;        // display spelling
    float path = 0;          // beam ctc (nats); the tapped word's own path = the best
    float before = 0;        // n-gram log10 P(word | words before)
    float after = 0;         // n-gram log10 P(next 1-2 words | ..., word); 0 when none follow
    float neural = 0;        // Predictor::scoreWords (log10); 0 until the worker answers
    bool hasNeural = false;
};

// WHY these values (tuned 2026-10-24 by bench/swap-tune.py, coordinate search over {0, .25, .5, 1, 2, 4} per weight,
// on TUNING data only: FUTO English trv8k, 7,198 swipes, 514 with a wrong first guess). Objective = mean of the three
// "right word among the options" rates for wrong first guesses (after the swipe, tap-back with the swipe's guesses,
// tap-back without them). Today's keyboard 34.70 (junk 5.81 %); first tuning (weights only) 38.85; with the
// gathering below 42.54 (junk 4.34 %). wBefore = 0 because the neural score (Predictor::scoreWords) already blends
// the same n-gram evidence about the words before; counting it twice made lists worse. The weights came out the
// same before and after the gathering changes.
// The gathering (swap bench --diagnose showed the right word was mostly never gathered, rarely ranked too low; when
// gathered it was among the 3 options 96 % of the time):
//   beam 256: the swipe recognizer's 48 dropped look-alikes during the search (38.85 -> 40.47);
//   neighbours 12, floor 15: one-letter-off dictionary words the search cannot reach, e.g. a plural the swipe did
//     not finish (+0.9);
//   syntheticTemperature 2: a typed word's made-up path is perfectly clean, so the network is over-sure and the
//     look-alikes a real swipe could give fall out (tap-back without the swipe 34.82 -> 38.33).
//   Pool 48 graded 42.61 (one swipe better on tap-back with the swipe, one worse without); 24 is kept, it is less work.
// Neural missing: the keyboard's worker scores a whole list at once, so a list has neural scores for all words or
// for none. With none (the list shown before the worker answers), rankSwap gives `before` the neural weight
// (2026-10-24): on trv8k that list went from 34.63 (junk 6.96 %, over the junk limit) to 37.03 (junk 4.36 %).
struct SwapParams {
    float wPath = 1.f, wBefore = 0.f, wAfter = 1.f, wNeural = 1.f;
    // A list for a word with no words before it and none after it (`noContext`, set by forContext()): the first word
    // of a message or field, or a field the keyboard may not read. Then `before` and `neural` hold nothing but how
    // common each word is (at a sentence start), so they get their own weights: rankSwap uses ncPath / ncBefore /
    // ncNeural instead of wPath / wBefore / wNeural (there is no `after` to weigh).
    // WHY all three 0 = the swap search's own order (rankSwap's sort is stable, so equal scores keep the gathered
    // order: other spellings, then the searched words best first by the search's score, then neighbours). That score
    // is the swipe recognizer's own (path + its word prior), which is exactly how recognize() orders the guess bar's
    // alternatives when there are no words before (it adds no sentence weight then). Tuned 2026-10-25 by
    // bench/swap-tune.py on TUNING data only, the sets with their sentence removed (trv8k-nocontext: 7,198 swipes, 674
    // wrong first guesses; tune-fr/de-nocontext: 95). first / in-3 (%), after-swipe | tap-back with | without the swipe:
    //   today's keyboard        trv8k 38.87/49.26 | 38.87/50.89 |  9.50/21.51 (objective 40.55); FR+DE 44.21
    //   sentence weights (1/0/1) trv8k 34.27/48.07 | 36.05/53.12 | 19.29/36.50 (45.90); FR+DE 41.75
    //   best weights (path 1, before 0, neural .5): trv8k 47.63, FR+DE 40.35; path alone: 44.51, FR+DE 37.89
    //   search order (0/0/0)    trv8k 38.72/50.15 | 39.02/54.90 | 24.78/41.10 (48.72); FR+DE 50.18
    //     (FR+DE 40.00/57.90 | 40.00/60.00 | 22.11/32.63, against 38.95/52.63 | 38.95/56.84 | 8.42/23.16).
    // A weighted mix of path, `before` and `neural` cannot reach it: the search's word prior and `before` disagree
    // (French "données" after a swipe: -32.4 against -10.0 nats), so `before` is no stand-in for it.
    // Re-tuned per language on 2026-10-25 (grid ncPath {0 .. 2} x ncBefore x ncNeural {0 .. 1}, each language on its
    // own no-sentence slice, junk limits per language): 0/0/0 stayed best for French and German too; every weighted
    // mix that kept their junk within the limits lost "right word in 3" (French at best 46.5 against 52.1).
    float ncPath = 0.f, ncBefore = 0.f, ncNeural = 0.f;
    // No-sentence lists only: words rarer than this frequency (0..255) are not gathered (learned and texting words
    // always are); 0 = no floor beyond the usual rare-word rule. WHY a floor: outside English the swap search has no
    // rare-word cutoff (as recognize(), where one cost 2-5 points), and with no sentence to say otherwise the search
    // order offers rare words that fit the path well. Tuned values and why they are all 0: see forLanguage().
    int ncMinFrequency = 0;
    bool noContext = false;
    int pool = 24;           // beam candidates considered
    int minFrequency = 80;   // same junk rule as AlternativeParams
    // How the swap search gathers its words (card #500 hill-climb, 2026-10-24; see above). searchPrior and
    // junkFilter exist so tools/swipekb-swap-bench --diagnose can ask "would the word have been found if...".
    int beam = 256;          // search width (word beginnings kept per step); 0 = the swipe recognizer's (48)
    int searchPrior = 0;     // during the search: 0 = sentence prior, 1 = word frequency only, 2 = none (path only)
    bool junkFilter = true;  // false = keep rare, stretched and rare-name words too (diagnosis only)
    int neighbours = 12;     // dictionary words one edit from the tapped word (similar()) added to the pool
    // A neighbour's path = the better of its own fit to the swipe and (the tapped word's fit - neighbourFloor); with
    // no network, the latter. WHY a floor: a one-letter-off word ("paintings") can fit a swipe that skipped that
    // letter very badly and still be the word meant. < 0 = no floor (and without a network, no neighbours).
    float neighbourFloor = 15.f;
    // Flattening of the network's output for the made-up path of a typed word (see EngineParams::temperature);
    // 1 = none. A real swipe is always searched as is.
    float syntheticTemperature = 2.f;
    // Tap-back lists only (`tapBack`, set by forTapBack(); never the bar right after a swipe):
    //   tapBackSentenceWords: up to this many words the n-gram expects in the tapped word's place (from the words
    //     before it and, when known, the words after it: fill in the blank), added with their own fit to the path;
    //     0 = none.
    //   tapBackMinFrequency / tapBackRareWithin: a looser rare-word rule (the person asked for options). Words down to
    //     tapBackMinFrequency may be offered; one below minFrequency only when its path is within tapBackRareWithin
    //     nats of the tapped word's (negative = only when it fits better by that much; 1e9 = no path condition).
    // WHY these values (2026-10-25, trv8k tuning set, 514 wrong first guesses; first / in-3, %): tap-back with the
    // swipe 37.55 / 47.08 -> 39.30 / 49.03, without it 31.13 / 38.33 -> 31.91 / 38.72; objective 42.54 -> 43.32. The
    // bar after a swipe is unchanged (42.22, junk 4.34 %). Junk among tap-back options of a word that was already
    // right: 5.13 -> 7.50 % with the swipe, 4.61 -> 4.84 % without (limit set beforehand: +3 points). Tried:
    //   rare words with no path condition (minimum 40 / 20 / 0): +0.4 to +0.6, but tap-back junk 16-17.5 %: dropped;
    //   within -2 / 0 / 1 / 2 nats (minimum 0): 42.87 / 43.06 / 43.19 / 43.19, junk 5.4 / 7.3 / 7.6 / 8.1 %; 1 kept
    //     (2 is no better and at the junk limit);
    //   sentence words 8 / 16 / 32 / 64: alone 42.74 for 16-64 (8 one swipe less); on top of the rare-word rule
    //     +0.20 (two tap-back swipes); 16 kept, the least work. They cost about 2.5 ms per tap-back list (7.0 -> 9.4 ms).
    //     WHY not after a swipe: there they found one more word in 514 for about 4.5 ms on every swipe.
    //   The weights stayed the same when re-tuned on top (swap-tune.py, pool 24).
    // French and German (FUTO validation slices): unchanged on every measure (their rare-word cutoff is already 0).
    bool tapBack = false;
    int tapBackSentenceWords = 16;
    int tapBackMinFrequency = 0;
    float tapBackRareWithin = 1.f;
    // The same settings for a tap-back list.
    SwapParams forTapBack() const { SwapParams q = *this; q.tapBack = true; return q; }
    // The same settings for a word with (or without) words before and after it: no words on either side = the
    // no-sentence weights (ncPath, ncBefore, ncNeural). WHY both sides: words after are real sentence evidence
    // (`after`), and the with-sentence weights were tuned with them; the bar right after a swipe never has words after.
    SwapParams forContext(bool wordsBefore, bool wordsAfter) const
    {
        SwapParams q = *this;
        q.noContext = !wordsBefore && !wordsAfter;
        return q;
    }
    // The tuned settings for a language ("en", "fr", ...).
    // WHY one set for every language: French and German were tuned too (FUTO swipe-5 validation sessions, AZERTY
    // 262 swipes + German layout 386, only 75 wrong first guesses together). Their own best weights (wBefore .25,
    // pool 16) scored 46.22 there against 44.89 for the English weights: 1.3 points, about one swipe per measure, below
    // the 3-point margin set before looking. So the English weights are used everywhere (Spanish has no tuning data
    // and shares the French/German choice). Today's keyboard scores 43.11 on those French/German swipes.
    // The no-sentence floor (ncMinFrequency) is per language (2026-10-25, each language's no-sentence tuning slice,
    // 48 French / 47 German wrong first guesses, so one swipe is about 2 points; objective = mean in-3 of the three
    // lists; junk = the scoreboards' rule, tools/benchdata.h):
    //   French 80: objective 54.86 -> 52.08 (today's keyboard 47.22), every first and in-3 at or above today's
    //     keyboard; tap-back junk 37.5 / 26.3 % -> 16.2 / 10.1 % (today 27.7 / 8.1, limits +3). 76 and lower break
    //     the tap-back junk limit; 78 ties 80, 85 loses 2.8 points.
    //   German 66: objective 45.39 unchanged (today 41.13), first guesses better (bar 36.17 -> 38.30 %); tap-back junk
    //     12.2 / 8.8 % -> 4.6 / 3.3 % (today 10.0 / 4.1). Every floor from 60 to 66 scores the same, 68 loses 4.3.
    //     Tap-back with the swipe's guesses stays one swipe below today's keyboard in in-3 (48.94 against 51.06 %),
    //     as without the floor.
    //   Spanish has no tuning slice: it would take French's 80 (its word list matches French's frequency by rank).
    // WHY 0 in every language anyway (owner's decision, 2026-10-25): tuned on only 48 / 47 sentence cases, the floor
    // did not hold on the HeliBoard test sets (single words, many of them rarer): the right word was found less often
    // than by today's keyboard in Spanish and French (after a swipe es 26.73/35.72 -> 23.48/30.47 %, fr 29.88/42.51 ->
    // 26.47/37.21) and less than without the floor in German (23.19/29.48 -> 21.03/26.59). The knob stays so it can be
    // re-tuned on a bigger non-English single-word tuning set.
    static SwapParams forLanguage(const std::string &lang) { (void)lang; return SwapParams{}; }
};

// Best-first, at most topK, never `tapped` itself (case-insensitive). Score = wPath*path + ln(10)*(wBefore*before +
// wAfter*after + wNeural*neural); with hasNeural false, `before` stands in for the neural score (weight
// wBefore + wNeural), never 0. With p.noContext, ncPath / ncBefore / ncNeural take the places of wPath / wBefore /
// wNeural (same rule, same stand-in).
std::vector<std::string> rankSwap(const std::string &tapped, const std::vector<SwapCandidate> &cands,
                                  const SwapParams &p, int topK = 3);

// A finger path through the centres of `word`'s keys (doubled letters once), 12 points per key gap, 20 ms apart,
// so the beam can find words whose path looks like this word's. Empty when a letter has no key.
// Apostrophes and hyphens are skipped (they are not on letter keys).
std::vector<TracePoint> syntheticTrace(const std::string &word, const std::vector<KeyRect> &keys);

} // namespace swipekb
