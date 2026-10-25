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

struct SwapParams {          // weights tuned on the tuning sets (Task 8); defaults are the tuned English values
    float wPath = 1.f, wBefore = 1.f, wAfter = 1.f, wNeural = 0.f;
    int pool = 24;           // beam candidates considered
    int minFrequency = 80;   // same junk rule as AlternativeParams
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
