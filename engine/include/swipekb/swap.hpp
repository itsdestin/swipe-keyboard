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

// WHY these values (tuned 2026-10-24 by bench/swap-tune.py, coordinate search over {0, .25, .5, 1, 2, 4} per weight
// and pool {16, 24, 32}, on TUNING data only: FUTO English trv8k, 7,198 swipes, 514 with a wrong first guess).
// Objective = mean of the three "right word among the options" rates for wrong first guesses (after the swipe,
// tap-back with the swipe's guesses, tap-back without them): today's keyboard 34.70, these weights 38.85; junk
// 5.81 % -> 4.57 %. wBefore = 0 because the neural score (Predictor::scoreWords) already blends the same n-gram
// evidence about the words before; counting it twice made lists worse. Pool 24 and 32 graded the same; 24 is faster.
// Neural missing: the keyboard's worker scores a whole list at once, so a list has neural scores for all words or
// for none; rankSwap stays as it is (with none, the first list is ordered by path and following words only, then
// re-ordered when the scores arrive). Cost measured on trv8k for a list with no neural scores: 34.63 with these
// weights (junk 6.96 %) against 37.03 with wBefore = 1 (junk 4.36 %), so the list shown before the worker answers
// is weaker than the one that replaces it.
struct SwapParams {
    float wPath = 1.f, wBefore = 0.f, wAfter = 1.f, wNeural = 1.f;
    int pool = 24;           // beam candidates considered
    int minFrequency = 80;   // same junk rule as AlternativeParams
    // The tuned weights for a language ("en", "fr", ...).
    // WHY one set for every language: French and German were tuned too (FUTO swipe-5 validation sessions, AZERTY
    // 262 swipes + German layout 386, only 75 wrong first guesses together). Their own best weights (wBefore .25,
    // pool 16) scored 46.22 there against 44.89 for the English weights: 1.3 points, about one swipe per measure, below
    // the 3-point margin set before looking. So the English weights are used everywhere (Spanish has no tuning data
    // and shares the French/German choice). Today's keyboard scores 43.11 on those French/German swipes.
    static SwapParams forLanguage(const std::string &lang) { (void)lang; return SwapParams{}; }
};

// Best-first, at most topK, never `tapped` itself (case-insensitive). Score = wPath*path + ln(10)*(wBefore*before +
// wAfter*after + wNeural*neural); with hasNeural false, the neural term is left out (not scored as 0).
std::vector<std::string> rankSwap(const std::string &tapped, const std::vector<SwapCandidate> &cands,
                                  const SwapParams &p, int topK = 3);

// A finger path through the centres of `word`'s keys (doubled letters once), 12 points per key gap, 20 ms apart,
// so the beam can find words whose path looks like this word's. Empty when a letter has no key.
// Apostrophes and hyphens are skipped (they are not on letter keys).
std::vector<TracePoint> syntheticTrace(const std::string &word, const std::vector<KeyRect> &keys);

} // namespace swipekb
