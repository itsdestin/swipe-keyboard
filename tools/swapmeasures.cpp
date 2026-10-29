// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "swapmeasures.h"
#include <cstdio>

namespace swapbench {
namespace {
// WHY: a zero denominator must print 0.00%, not nan, so the scoreboard script can always parse the line.
double pct(size_t n, size_t d) { return d ? 100.0 * double(n) / double(d) : 0.0; }
}

void Report::add(Measure k, bool wrongFirst, const std::string &target, const std::vector<std::string> &shown,
                 const std::function<std::string(const std::string &)> &norm) {
    if (!wrongFirst) return;   // WHY: only swipes the keyboard got wrong are cases for swap options
    Tally &t = m[int(k)];
    ++t.cases;
    const std::string want = norm(target);
    // WHY: "first" and "in3" use the ranked order (caller passes ranked, not display order).
    for (size_t i = 0; i < shown.size() && i < 3; ++i)
        if (norm(shown[i]) == want) {
            if (i == 0) ++t.first;
            ++t.inThree;
            break;
        }
}

std::string Report::format() const {
    static const char *names[3] = {"after-swipe", "tapback-trace", "tapback-notrace"};
    std::string out;
    char buf[160];
    for (int i = 0; i < 3; ++i) {
        std::snprintf(buf, sizeof buf, "%s: cases %zu first %.2f%% in3 %.2f%%\n", names[i], m[i].cases,
                      pct(m[i].first, m[i].cases), pct(m[i].inThree, m[i].cases));
        out += buf;
    }
    // WHY the raw count at the end ("count N"): bench/swap-tune.py adds counts up over several sets, and a count
    // worked back from a rounded percentage can be off by one or two once thousands of options are shown.
    std::snprintf(buf, sizeof buf, "junk: right %zu shown %zu junk %.2f%% count %zu\n", rightFirst, shownWhenRight,
                  pct(junkWhenRight, shownWhenRight), junkWhenRight);
    out += buf;
    // WHY "junk-tapback" and not "junk ...": bench/swap-score.sh and swap-tune.py read the "junk:" line above as measure 4.
    std::snprintf(buf, sizeof buf, "junk-tapback: trace shown %zu junk %.2f%% notrace shown %zu junk %.2f%% counts %zu %zu\n",
                  tapShown[0], pct(tapJunk[0], tapShown[0]), tapShown[1], pct(tapJunk[1], tapShown[1]), tapJunk[0],
                  tapJunk[1]);
    out += buf;
    std::snprintf(buf, sizeof buf, "speed: %.2f ms/list\n", lists ? msTotal / double(lists) : 0.0);
    out += buf;
    return out;
}

Ux UxTally::add(bool firstRight, const std::string &target, const std::vector<std::string> &bar,
                const std::vector<std::string> &tapBack, bool inWordList, size_t letters,
                const std::function<std::string(const std::string &)> &norm)
{
    const std::string want = norm(target);
    auto holds = [&](const std::vector<std::string> &list) {
        for (const auto &w : list) if (norm(w) == want) return true;
        return false;
    };
    // WHY this order: a person takes the cheapest fix there is; the first guess beats the bar, the bar beats tapping
    // back. Retyping is all that is left.
    const Ux u = firstRight ? Ux::Right0 : holds(bar) ? Ux::Right1 : holds(tapBack) ? Ux::Fixed2
                 : inWordList ? Ux::RetypeInList : Ux::RetypeNotInList;
    ++n[int(u)];
    taps += tapsFor(u, letters);
    return u;
}

std::string UxTally::format() const
{
    char buf[200];
    std::snprintf(buf, sizeof buf, "ux: swipes=%zu right0=%zu right1=%zu fixed2=%zu retype_notinlist=%zu retype_inlist=%zu taps=%zu\n",
                  swipes(), n[0], n[1], n[2], n[3], n[4], taps);
    return buf;
}
}
