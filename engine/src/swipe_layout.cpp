// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "swipekb/swipe_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace swipekb {

std::u32string decodeUtf8(std::string_view s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        char32_t cp; int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1f; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0f; n = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
        else { ++i; continue; }   // stray byte: skip rather than guess
        if (i + n > s.size()) break;
        bool ok = true;
        for (int k = 1; k < n; ++k) {
            const unsigned char cc = s[i + k];
            if ((cc >> 6) != 2) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3f);
        }
        if (ok) out.push_back(cp);
        i += ok ? n : 1;
    }
    return out;
}

char32_t foldChar(char32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c < 0x80) return c;
    // Latin-1 upper case -> lower case (U+00C0..U+00DE except the multiplication sign).
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) c += 32;
    switch (c) {
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: return 'a';
    case 0xE7: return 'c';
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: return 'e';
    case 0xEC: case 0xED: case 0xEE: case 0xEF: return 'i';
    case 0xF1: return 'n';
    case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: case 0x153: return 'o';
    case 0xF9: case 0xFA: case 0xFB: case 0xFC: return 'u';
    case 0xFD: case 0xFF: return 'y';
    case 0xDF: return 's';
    default: return c;
    }
}

std::string lowerUtf8(std::string_view s) {
    std::string out;
    for (char32_t c : decodeUtf8(s)) {
        if (c >= 'A' && c <= 'Z') c += 32;
        else if (c >= 0xC0 && c <= 0xDE && c != 0xD7) c += 32;
        if (c < 0x80) out.push_back(char(c));
        else if (c < 0x800) { out.push_back(char(0xC0 | (c >> 6))); out.push_back(char(0x80 | (c & 0x3f))); }
        else if (c < 0x10000) { out.push_back(char(0xE0 | (c >> 12))); out.push_back(char(0x80 | ((c >> 6) & 0x3f))); out.push_back(char(0x80 | (c & 0x3f))); }
        else { out.push_back(char(0xF0 | (c >> 18))); out.push_back(char(0x80 | ((c >> 12) & 0x3f))); out.push_back(char(0x80 | ((c >> 6) & 0x3f))); out.push_back(char(0x80 | (c & 0x3f))); }
    }
    return out;
}

SwipeLayout::SwipeLayout(std::vector<SwipeKey> keys) : keys_(std::move(keys)) {
    // WHY the key pitch (distance between neighbouring key centres), not the key's drawn width: real boards leave
    // gaps between keys, so half the drawn width understates the spacing a finger actually travels (measured: 20%
    // on a common HeliBoard layout). Median distance to the nearest other key centre is 0.1 on a 10-key row.
    std::vector<float> nearest;
    for (size_t i = 0; i < keys_.size(); ++i) {
        float best = 1e9f;
        for (size_t j = 0; j < keys_.size(); ++j)
            if (i != j) best = std::min(best, std::hypot(keys_[i].cx - keys_[j].cx, keys_[i].cy - keys_[j].cy));
        if (best < 1e8f) nearest.push_back(best);
    }
    if (!nearest.empty()) {
        std::nth_element(nearest.begin(), nearest.begin() + nearest.size() / 2, nearest.end());
        unit_ = 1.f / std::max(1e-3f, nearest[nearest.size() / 2]);
    }
    byCodepoint_.assign(0x180, -1);
    for (size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i].label < byCodepoint_.size()) byCodepoint_[keys_[i].label] = int(i);
}

bool SwipeLayout::loadTsv(const std::string &path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    std::getline(in, line);   // header
    std::vector<SwipeKey> keys;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string label; float cx, cy, hw, hh;
        if (!(std::getline(ss, label, '\t') && (ss >> cx >> cy >> hw >> hh))) continue;
        const auto cps = decodeUtf8(label);
        if (cps.size() != 1 || hw <= 0 || hh <= 0) continue;
        keys.push_back({cps[0] >= 'A' && cps[0] <= 'Z' ? char32_t(cps[0] + 32) : cps[0], cx, cy, hw, hh});
    }
    if (keys.empty()) return false;
    *this = SwipeLayout(std::move(keys));
    return true;
}

int SwipeLayout::keyOf(char32_t c) const {
    auto at = [&](char32_t x) { return x < byCodepoint_.size() ? byCodepoint_[x] : -1; };
    // Exact label first (a layout with its own n-tilde key keeps it), then the folded base letter.
    char32_t lower = c >= 'A' && c <= 'Z' ? c + 32 : (c >= 0xC0 && c <= 0xDE && c != 0xD7 ? c + 32 : c);
    if (int k = at(lower); k >= 0) return k;
    return at(foldChar(c));
}

std::vector<int> SwipeLayout::wordKeys(std::string_view utf8, int *rawLen) const {
    std::vector<int> out;
    int raw = 0;
    for (char32_t c : decodeUtf8(utf8)) {
        const int k = keyOf(c);
        if (k < 0) continue;
        ++raw;
        if (out.empty() || out.back() != k) out.push_back(k);
    }
    if (rawLen) *rawLen = raw;
    return out;
}

}
