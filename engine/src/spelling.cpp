// Spellings — puts apostrophes and hyphens back into FUTO's letters-only guesses (see recognizer.hpp).

#include "swipekb/recognizer.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace swipekb {

namespace {

// WHY 80 points (AOSP's 0-255 scale, ~27.5 points per step of ten in how often a word is used): close enough to
// offer "we're" when "were" is typed (186 vs 106) or "we'll" for "well" (168 vs 94), far enough to never offer
// junk like "wa's" for "was" (201 vs 50) or "doe's" for "does" (151 vs 27).
constexpr int kOfferWithin = 80;

// Letters with single ' or - between them ("don't", "o'clock", "full-time"). WHY so strict: the guess is typed
// as-is, so "us$", "states'" or "'tis" style entries must never replace a clean word.
bool wellFormed(const std::string &w)
{
    if (w.empty())
        return false;
    auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (!letter(w.front()) || !letter(w.back()))
        return false;
    for (size_t i = 1; i < w.size(); ++i) {
        const char c = w[i];
        if (letter(c))
            continue;
        if ((c != '\'' && c != '-') || !letter(w[i - 1]))
            return false;
    }
    return true;
}

std::string lower(const std::string &w)
{
    std::string out = w;
    for (char &c : out)
        if (c >= 'A' && c <= 'Z')
            c = char(c | 0x20);
    return out;
}

std::string lettersOf(const std::string &w)
{
    std::string out;
    for (char c : lower(w))
        if (c >= 'a' && c <= 'z')
            out += c;
    return out;
}

} // namespace

Spellings Spellings::fromCombinedText(const std::string &text)
{
    // letters -> (lower-case spelling -> best frequency)
    std::unordered_map<std::string, std::vector<std::pair<std::string, int>>> groups;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t p = line.find(" word=");
        if (p != 0)
            continue;   // header, shortcut lines
        const size_t e = line.find(',', p + 6);
        if (e == std::string::npos)
            continue;
        const std::string word = line.substr(p + 6, e - p - 6);
        const size_t fp = line.find(",f=", e);
        if (fp == std::string::npos)
            continue;
        const int freq = std::atoi(line.c_str() + fp + 3);
        // WHY skip these: AOSP lists "im", "alot", "whats" only so its autocorrect can replace them; they are
        // not spellings anyone means.
        if (freq <= 0 || line.find("not_a_word=true") != std::string::npos
            || line.find("nonword") != std::string::npos || !wellFormed(word))
            continue;
        const std::string spelled = lower(word);
        auto &g = groups[lettersOf(word)];
        auto it = std::find_if(g.begin(), g.end(), [&](const auto &x) { return x.first == spelled; });
        if (it == g.end())
            g.push_back({spelled, freq});
        else
            it->second = std::max(it->second, freq);
    }
    Spellings out;
    for (auto &[letters, g] : groups) {
        // Most frequent first; on a tie the plain letters-only spelling first (it is what FUTO returned).
        std::stable_sort(g.begin(), g.end(), [&](const auto &a, const auto &b) {
            if (a.second != b.second)
                return a.second > b.second;
            return a.first == letters && b.first != letters;
        });
        // WHY skip: most words have one spelling, the letters themselves; storing them would only cost memory.
        if (g.size() == 1 && g.front().first == letters)
            continue;
        std::vector<std::string> list;
        for (const auto &[w, f] : g)
            if (f >= g.front().second - kOfferWithin)
                list.push_back(w);
        out.byLetters_.emplace(letters, std::move(list));
    }
    return out;
}

Spellings Spellings::fromCombinedFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return fromCombinedText(ss.str());
}

std::string Spellings::spell(const std::string &letters) const
{
    const auto it = byLetters_.find(letters);
    return it == byLetters_.end() ? letters : it->second.front();
}

std::vector<std::string> Spellings::others(const std::string &word) const
{
    const auto it = byLetters_.find(lettersOf(word));
    if (it == byLetters_.end())
        return {};
    const std::string me = lower(word);
    std::vector<std::string> out;
    for (const std::string &w : it->second)
        if (w != me)
            out.push_back(w);
    return out;
}

} // namespace swipekb
