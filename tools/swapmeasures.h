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
    std::string format() const;      // the lines bench/swap-score.sh reads
};
}
