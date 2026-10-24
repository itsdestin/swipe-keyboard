// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <QString>

#include "swipekb/text.hpp"

namespace swipekb {

namespace {

// Decode UTF-8 to code points. WHY lenient: text before the cursor comes from arbitrary apps; a bad byte
// becomes U+FFFD (not punctuation, not space) instead of failing the swipe.
std::vector<char32_t> decode(const std::string &s)
{
    std::vector<char32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + len > s.size()) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        char32_t cp = len == 1 ? c : c & (0xFF >> (len + 1));
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            const unsigned char cc = s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

void encode(char32_t cp, std::string &out)
{
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
}

// WHY a hand-made table: the plugin used Qt's QChar::isSpace / isPunct. Plain C++ has no Unicode tables, so
// this lists the same classes for the characters that actually show up in typed text.
bool isSpace(char32_t c)
{
    return (c >= 9 && c <= 13) || c == 32 || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

// Unicode "punctuation" (categories Pc Pd Ps Pe Pi Pf Po), NOT symbols: $ + < = > ^ ` | ~ stay part of a word,
// exactly like QChar::isPunct.
bool isPunct(char32_t c)
{
    if (c < 0x80) {
        static const std::string ascii = "!\"#%&'()*,-./:;?@[\\]_{}";
        return ascii.find(char(c)) != std::string::npos;
    }
    return c == 0xA1 || c == 0xA7 || c == 0xAB || c == 0xB6 || c == 0xB7 || c == 0xBB || c == 0xBF ||
           (c >= 0x2010 && c <= 0x2027) || (c >= 0x2030 && c <= 0x2043) || (c >= 0x2045 && c <= 0x2051) ||
           (c >= 0x2053 && c <= 0x205E) || (c >= 0x3001 && c <= 0x3003) || (c >= 0x3008 && c <= 0x3011) ||
           (c >= 0x3014 && c <= 0x301F) || c == 0xFF01 || c == 0xFF1F || c == 0xFF0C || c == 0xFF0E;
}

} // namespace

std::vector<std::string> contextWords(const std::string &textBeforeCursor, int max)
{
    // WHY canonical equivalents must reach swipe and prediction context models with the same spelling.
    std::vector<char32_t> text = decode(QString::fromStdString(textBeforeCursor).normalized(QString::NormalizationForm_C).toStdString());

    // Only the current sentence counts: after . ! ? the next word starts fresh, like Gboard.
    size_t start = 0;
    for (size_t i = text.size(); i-- > 0;)
        if (text[i] == U'.' || text[i] == U'!' || text[i] == U'?') {
            start = i + 1;
            break;
        }

    std::vector<std::string> words;
    size_t i = start;
    while (i < text.size()) {
        while (i < text.size() && isSpace(text[i])) ++i;
        size_t b = i;
        while (i < text.size() && !isSpace(text[i])) ++i;
        size_t e = i;
        while (b < e && isPunct(text[b])) ++b;
        while (e > b && isPunct(text[e - 1])) --e;
        if (e > b) {
            std::string w;
            for (size_t k = b; k < e; ++k) encode(text[k], w);
            words.push_back(std::move(w));
        }
    }
    const size_t keep = max < 0 ? 0 : static_cast<size_t>(max);
    if (words.size() > keep)
        words.erase(words.begin(), words.end() - keep);
    return words;
}

} // namespace swipekb
