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

// The user-experience scoreboard (bench/ux-score.sh): what a person feels for EVERY swipe, not only the wrong ones.
// Each swipe lands in exactly one bucket, checked in this order:
//   Right0          the first guess was the word (no tap);
//   Right1          the word was on the bar shown right after the swipe (one tap on it);
//   Fixed2          the word was in the list shown on tapping the word later, with the swipe still remembered
//                   (one tap on the word, one on the option);
//   RetypeNotInList the word is not in the word list, so no list could hold it: deleted and typed letter by letter;
//   RetypeInList    the same for a word the list has.
enum class Ux { Right0, Right1, Fixed2, RetypeNotInList, RetypeInList };
struct UxTally {
    size_t n[5] = {};   // indexed by Ux
    size_t taps = 0;    // taps after the swipe itself, summed (tapsFor())
    // WHY retype = letters + 1: one tap to delete the wrong word (Backspace right after a swipe takes the whole word),
    // then every letter; the space was already there. A plain rule, stated on the scoreboard; it ignores the time a
    // person spends noticing the mistake.
    static size_t tapsFor(Ux u, size_t letters) { return u == Ux::Right0 ? 0 : u == Ux::Right1 ? 1 : u == Ux::Fixed2 ? 2 : letters + 1; }
    // `bar` = every option shown on the bar after the swipe (not the first guess); `tapBack` = the tap-back list.
    // Compared with `norm` (the scoreboard's match rule). `letters` = the word's length as typed.
    Ux add(bool firstRight, const std::string &target, const std::vector<std::string> &bar,
           const std::vector<std::string> &tapBack, bool inWordList, size_t letters,
           const std::function<std::string(const std::string &)> &norm);
    size_t swipes() const { return n[0] + n[1] + n[2] + n[3] + n[4]; }
    // One line, "ux: swipes=N right0=.. right1=.. fixed2=.. retype_notinlist=.. retype_inlist=.. taps=..", which
    // bench/ux-score.sh reads (counts only: percentages are worked out there, from these counts).
    std::string format() const;
};
}
