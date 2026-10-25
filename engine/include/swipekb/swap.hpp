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
    // The tuned weights for a language ("en", "fr", ...).
    // WHY one set for every language: French and German were tuned too (FUTO swipe-5 validation sessions, AZERTY
    // 262 swipes + German layout 386, only 75 wrong first guesses together). Their own best weights (wBefore .25,
    // pool 16) scored 46.22 there against 44.89 for the English weights: 1.3 points, about one swipe per measure, below
    // the 3-point margin set before looking. So the English weights are used everywhere (Spanish has no tuning data
    // and shares the French/German choice). Today's keyboard scores 43.11 on those French/German swipes.
    static SwapParams forLanguage(const std::string &lang) { (void)lang; return SwapParams{}; }
};

// Best-first, at most topK, never `tapped` itself (case-insensitive). Score = wPath*path + ln(10)*(wBefore*before +
// wAfter*after + wNeural*neural); with hasNeural false, `before` stands in for the neural score (weight
// wBefore + wNeural), never 0.
std::vector<std::string> rankSwap(const std::string &tapped, const std::vector<SwapCandidate> &cands,
                                  const SwapParams &p, int topK = 3);

// A finger path through the centres of `word`'s keys (doubled letters once), 12 points per key gap, 20 ms apart,
// so the beam can find words whose path looks like this word's. Empty when a letter has no key.
// Apostrophes and hyphens are skipped (they are not on letter keys).
std::vector<TracePoint> syntheticTrace(const std::string &word, const std::vector<KeyRect> &keys);

} // namespace swipekb
