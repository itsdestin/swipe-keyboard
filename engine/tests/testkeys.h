// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Test helpers shared by the engine tests: a QWERTY key layout and a clean swipe through a word.
// WHY a header: test_language_engine and test_swap both need the same layout; one copy cannot drift.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "swipekb/text.hpp"

namespace swipekb::testkeys {

inline std::vector<KeyRect> qwertyRects(float ox, float oy, float scale)
{
    const char *rows[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    const float shift[3] = {0, 50, 150};
    std::vector<KeyRect> keys;
    for (int r = 0; r < 3; ++r)
        for (int i = 0; rows[r][i]; ++i)
            keys.push_back({char32_t(rows[r][i]), ox + (shift[r] + i * 100.f) * scale, oy + r * 100.f * scale, 100.f * scale, 100.f * scale});
    return keys;
}

// A clean swipe: straight lines between the centres of the word's letters, 12 points per segment, 8 ms apart.
inline std::vector<TracePoint> traceThrough(const std::string &word, const std::vector<KeyRect> &keys)
{
    auto centre = [&](char c) {
        for (const auto &k : keys)
            if (k.letter == char32_t(c)) return std::pair{k.x + k.w / 2, k.y + k.h / 2};
        return std::pair{0.f, 0.f};
    };
    std::vector<TracePoint> out;
    float t = 0;
    for (size_t i = 0; i + 1 < word.size(); ++i) {
        auto [x0, y0] = centre(word[i]);
        auto [x1, y1] = centre(word[i + 1]);
        for (int s = 0; s < 12; ++s, t += 8) {
            const float f = s / 12.f;
            out.push_back({x0 + (x1 - x0) * f, y0 + (y1 - y0) * f, t});
        }
    }
    auto [xe, ye] = centre(word.back());
    out.push_back({xe, ye, t});
    return out;
}

} // namespace swipekb::testkeys
