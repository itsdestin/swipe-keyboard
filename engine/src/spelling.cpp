// Spellings — puts apostrophes and hyphens back into FUTO's letters-only guesses (see recognizer.hpp).

#include "swipekb/recognizer.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <QString>
#include <QRegularExpression>

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
    // WHY Unicode letters and interior punctuation only: retain display forms without accepting control/markup entries.
    static const QRegularExpression valid(QString::fromUtf8(R"(^\p{L}+(?:['’\-]\p{L}+)*$)"));
    return valid.match(QString::fromStdString(w).normalized(QString::NormalizationForm_C)).hasMatch();
}

std::string lower(const std::string &w)
{
    return QString::fromStdString(w).normalized(QString::NormalizationForm_C).toLower().toStdString();
}

std::string lettersOf(const std::string &w)
{
    std::string out;
    for (char32_t c : QString::fromStdString(lower(w)).toUcs4())
        if (QChar::isLetter(c)) out += QString::fromUcs4(&c,1).toStdString();
    return out;
}

} // namespace

std::vector<std::string> Spellings::recognitionAliases(const std::string &word, const std::string &language)
{
    // WHY: the physical œ/æ/ß popup shares a base key, while many users draw the
    // conventional oe/ae/ss digraph. Neither should erase the dictionary's display form.
    const QString source=QString::fromStdString(lettersOf(word));
    QString base=source, digraph=source;
    if (language=="fr") {
        base.replace(QStringLiteral("œ"),QStringLiteral("o")).replace(QStringLiteral("æ"),QStringLiteral("a"));
        digraph.replace(QStringLiteral("œ"),QStringLiteral("oe")).replace(QStringLiteral("æ"),QStringLiteral("ae"));
    } else if (language=="de") {
        base.replace(QStringLiteral("ß"),QStringLiteral("s"));
        digraph.replace(QStringLiteral("ß"),QStringLiteral("ss"));
    }
    std::vector<std::string> out;
    if (base!=source) out.push_back(base.toStdString());
    if (digraph!=source && digraph!=base) out.push_back(digraph.toStdString());
    return out;
}

Spellings Spellings::fromCombinedText(const std::string &text, const std::string &language)
{
    // letters -> (lower-case spelling -> best frequency)
    std::unordered_map<std::string, std::vector<std::pair<std::string, int>>> groups;
    Spellings out;
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
        const std::string spelled = language=="de" ? QString::fromStdString(word).normalized(QString::NormalizationForm_C).toStdString() : lower(word);
        const auto add=[&](const std::string &key) {
            auto &g=groups[key];
            auto it=std::find_if(g.begin(),g.end(),[&](const auto &x) { return x.first==spelled; });
            if (it==g.end()) g.push_back({spelled,freq});
            else it->second=std::max(it->second,freq);
        };
        add(lettersOf(word));
        const auto aliases=recognitionAliases(word,language);
        for (const auto &key:aliases) add(key);
        if (!aliases.empty()) out.otherKeys_[lower(word)]=aliases.front();
    }
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

Spellings Spellings::fromCombinedFile(const std::string &path, const std::string &language)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return fromCombinedText(ss.str(), language);
}

std::string Spellings::spell(const std::string &letters) const
{
    const auto it = byLetters_.find(letters);
    return it == byLetters_.end() ? letters : it->second.front();
}

std::vector<std::string> Spellings::others(const std::string &word) const
{
    const std::string me = lower(word);
    const auto alias=otherKeys_.find(me);
    const auto it = byLetters_.find(alias==otherKeys_.end() ? lettersOf(word) : alias->second);
    if (it == byLetters_.end())
        return {};
    std::vector<std::string> out;
    for (const std::string &w : it->second)
        if (lower(w) != me)
            out.push_back(w);
    return out;
}

} // namespace swipekb
