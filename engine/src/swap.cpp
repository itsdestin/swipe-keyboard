// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "swipekb/swap.hpp"

#include <QChar>
#include <QList>
#include <QString>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace swipekb {

std::vector<std::string> rankSwap(const std::string &tapped, const std::vector<SwapCandidate> &cands,
                                  const SwapParams &p, int topK)
{
    const QString tappedLow = QString::fromUtf8(tapped.c_str()).toLower();
    constexpr float kLn10 = 2.302585093f;
    struct Scored { const SwapCandidate *c; float score; };
    std::vector<Scored> s;
    std::unordered_set<QString> seen;
    seen.insert(tappedLow);   // WHY: a swap list must never offer the word that is already there
    for (const auto &c : cands) {
        // WHY insert doubles as the duplicate test: "Good" and "good" are one suggestion; the first (listed) wins.
        if (!seen.insert(QString::fromUtf8(c.word.c_str()).toLower()).second) continue;
        // WHY the neural term is skipped, not zeroed: log10 scores are negative, so 0 would make a word with no
        // neural answer yet look better than every word that has one.
        const float lm = p.wBefore * c.before + p.wAfter * c.after + (c.hasNeural ? p.wNeural * c.neural : 0.f);
        s.push_back({&c, p.wPath * c.path + kLn10 * lm});
    }
    // WHY stable: equal scores keep the caller's order (the beam's own order).
    std::stable_sort(s.begin(), s.end(), [](const Scored &a, const Scored &b) { return a.score > b.score; });
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size() && int(i) < topK; ++i) out.push_back(s[i].c->word);
    return out;
}

std::vector<TracePoint> syntheticTrace(const std::string &word, const std::vector<KeyRect> &keys)
{
    // Key centres for each letter, doubled letters once, apostrophes/hyphens skipped.
    std::vector<std::pair<float, float>> centres;
    char32_t prev = 0;
    for (const char32_t raw : QString::fromUtf8(word.c_str()).toUcs4()) {
        if (raw == U'\'' || raw == 0x2019 || raw == U'-') continue;
        const char32_t ch = QChar::toLower(raw);
        if (ch == prev) continue;
        const auto it = std::find_if(keys.begin(), keys.end(), [&](const KeyRect &k) { return k.letter == ch; });
        if (it == keys.end()) return {};   // a letter with no key: no honest path exists
        centres.push_back({it->x + it->w / 2, it->y + it->h / 2});
        prev = ch;
    }
    if (centres.empty()) return {};
    std::vector<TracePoint> out;
    float t = 0;
    for (size_t i = 0; i + 1 < centres.size(); ++i)
        for (int k = 0; k < 12; ++k, t += 20) {
            const float f = k / 12.f;
            out.push_back({centres[i].first + (centres[i + 1].first - centres[i].first) * f,
                           centres[i].second + (centres[i + 1].second - centres[i].second) * f, t});
        }
    out.push_back({centres.back().first, centres.back().second, t});
    // WHY a second point for one-key words: the swipe decoder refuses traces shorter than 2 points.
    if (out.size() < 2) out.push_back({centres.back().first, centres.back().second, t + 20});
    return out;
}

} // namespace swipekb
