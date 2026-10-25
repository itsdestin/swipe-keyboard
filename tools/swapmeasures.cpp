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
    std::snprintf(buf, sizeof buf, "junk: right %zu shown %zu junk %.2f%%\n", rightFirst, shownWhenRight,
                  pct(junkWhenRight, shownWhenRight));
    out += buf;
    std::snprintf(buf, sizeof buf, "speed: %.2f ms/list\n", lists ? msTotal / double(lists) : 0.0);
    out += buf;
    return out;
}
}
