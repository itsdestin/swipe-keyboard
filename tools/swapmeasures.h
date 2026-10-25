// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Swap scoreboard (card #500): plain counting for the five swap measures. No engine here, so it is
// tested on its own and the scoreboard tool only has to feed it.
#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace swapbench {
enum class Measure { AfterSwipe, TapBackTrace, TapBackNoTrace };
struct Tally { size_t cases = 0, first = 0, inThree = 0; };
struct Report {
    Tally m[3];                      // indexed by Measure
    size_t rightFirst = 0, shownWhenRight = 0, junkWhenRight = 0;   // measure 4
    double msTotal = 0; size_t lists = 0;                           // measure 5
    // A case counts only when the first guess was wrong (`wrongFirst`); `shown` is the bar's options in order.
    void add(Measure k, bool wrongFirst, const std::string &target, const std::vector<std::string> &shown,
             const std::function<std::string(const std::string &)> &norm);
    void addJunk(size_t shownCount, size_t junkCount) { ++rightFirst; shownWhenRight += shownCount; junkWhenRight += junkCount; }
    // Measure 4b: the same count for the tap-back lists (0 = with the swipe's guesses, 1 = without) when the first
    // guess was right. WHY its own line: a looser rare-word rule for tap-back lists only (card #500, 2026-10-25)
    // must show what it costs there; measure 4 covers only the bar right after the swipe.
    size_t tapShown[2] = {}, tapJunk[2] = {};
    void addTapJunk(int which, size_t shownCount, size_t junkCount) { tapShown[which] += shownCount; tapJunk[which] += junkCount; }
    std::string format() const;      // the lines bench/swap-score.sh reads
};
}
