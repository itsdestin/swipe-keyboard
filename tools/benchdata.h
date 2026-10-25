// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// benchdata.h — reads recorded swipes (jsonl: "word", "data" [{x,y,t}] 0..1 of the letter area, "canvas_width",
// "canvas_height", optional "layout", "sentence" + "word_idx") and layout files for every bench tool, so the swipe
// and swap scoreboards read the same swipes the same way.
#pragma once

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "swipekb/language_engine.hpp"

namespace benchdata {

struct BenchSwipe {
    std::string word, board;
    float w = 1, h = 1;
    std::vector<swipekb::TracePoint> trace;
    // Sentence words before and after the swiped word (empty unless the sentence was asked for).
    std::vector<std::string> before, after;
};

// Key rectangles in canvas pixels from a layout file (0..1 of the letter area), or empty when unreadable.
inline std::vector<swipekb::KeyRect> readLayout(const std::string &path, float w, float h)
{
    std::ifstream in(path);
    std::vector<swipekb::KeyRect> keys;
    std::string line;
    if (!in || !std::getline(in, line)) return {};
    while (std::getline(in, line)) {
        std::istringstream f(line);
        std::string letter;
        float cx, cy, hw, hh;
        if (!(f >> letter >> cx >> cy >> hw >> hh)) continue;
        const auto u = QString::fromStdString(letter).toUcs4();
        if (u.size() != 1) continue;
        keys.push_back({char32_t(u[0]), (cx - hw) * w, (cy - hh) * h, 2 * hw * w, 2 * hh * h});
    }
    return keys;
}

// How a guess is compared with the target: "ascii" = a-z only (the original English benchmark),
// "lookup" = lower-cased with accents kept (apostrophes/hyphens dropped), "exact" = byte for byte.
inline std::string norm(const std::string &s, const std::string &mode)
{
    if (mode == "exact") return s;
    if (mode == "lookup") return QString::fromStdString(s).toLower().remove(QLatin1Char('\'')).remove(QChar(0x2019)).remove(QLatin1Char('-')).toStdString();
    std::string o;
    for (unsigned char c : s) if (std::isalpha(c)) o.push_back(char(std::tolower(c)));
    return o;
}

// Reads up to `limit` swipes (0 = all) from a jsonl file. With `withSentence`, fills `before` (the words ahead of
// word_idx) and `after` (the next words). On a missing file returns empty and sets *error (when given).
inline std::vector<BenchSwipe> readSwipes(const std::string &jsonl, size_t limit, bool withSentence, std::string *error)
{
    std::vector<BenchSwipe> swipes;
    QFile f(QString::fromStdString(jsonl));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "cannot read " + jsonl;
        return swipes;
    }
    while (!f.atEnd() && (!limit || swipes.size() < limit)) {
        const auto o = QJsonDocument::fromJson(f.readLine()).object();
        if (o.isEmpty()) continue;
        BenchSwipe s;
        s.word = o["word"].toString().toStdString();
        s.board = o["layout"].toString().toStdString();
        s.w = float(o["canvas_width"].toDouble(1));
        s.h = float(o["canvas_height"].toDouble(1));
        if (withSentence && o.contains("sentence") && o.contains("word_idx")) {
            const auto toks = o["sentence"].toString().split(' ', Qt::SkipEmptyParts);
            const int idx = o["word_idx"].toInt();
            for (int k = 0; k < std::min<int>(idx, int(toks.size())); ++k) s.before.push_back(toks[k].toStdString());
            // WHY 2: the "after" term of the swap ranking uses the next two words only.
            for (int k = idx + 1; k >= 0 && k < int(toks.size()) && k <= idx + 2; ++k) s.after.push_back(toks[k].toStdString());
        }
        double t0 = -1;
        for (const auto &p : o["data"].toArray()) {
            const auto po = p.toObject();
            const double t = po["t"].toDouble();
            if (t0 < 0) t0 = t;
            s.trace.push_back({float(po["x"].toDouble()) * s.w, float(po["y"].toDouble()) * s.h, float(t - t0)});
        }
        if (!s.trace.empty()) swipes.push_back(std::move(s));
    }
    return swipes;
}

// WHY one shared junk rule: the swipe scoreboard and the swap scoreboard both report "junk", and the two numbers must
// mean the same thing, so the rule lives here once. A shown alternative is "junk" when the person would hardly ever
// mean it: stretched (a letter three times in a row), rare (dictionary frequency under 80 of 255) or a rarely used
// name (written with a capital in the dictionary, under 120; not in German, where all nouns are capitalised).
// Fixed before tuning.
class JunkRule
{
public:
    // Reads the dictionary file again for its own capitals, which the engine shows in lower case (en, es, fr).
    JunkRule(const std::string &dictPath, const std::string &language)
        : m_germanLike(language.rfind("de", 0) == 0)
    {
        std::ifstream d(dictPath);
        std::map<std::string, int> best;
        for (std::string line; std::getline(d, line);) {
            if (line.rfind(" word=", 0) != 0) continue;
            const auto c = line.find(',', 6), fp = line.find(",f=");
            if (c == std::string::npos || fp == std::string::npos) continue;
            const QString w = QString::fromStdString(line.substr(6, c - 6));
            const int fr = std::atoi(line.c_str() + fp + 3);
            const std::string low = w.toLower().toStdString();
            if (!best.count(low) || fr > best[low]) { best[low] = fr; m_capitalised[low] = !w.isEmpty() && w.front().isUpper(); }
        }
    }
    bool operator()(const swipekb::LanguageEngine &engine, const std::string &word) const
    {
        const int f = engine.frequency(word);
        const std::string low = QString::fromStdString(word).toLower().toStdString();
        const auto it = m_capitalised.find(low);
        const bool name = !m_germanLike && it != m_capitalised.end() && it->second;
        return stretched(word) || f < 80 || (name && f < 120);
    }

private:
    static bool stretched(const std::string &w)
    {
        const auto u = QString::fromStdString(w).toLower().toUcs4();
        int run = 1;
        for (qsizetype i = 1; i < u.size(); ++i) { run = u[i] == u[i - 1] ? run + 1 : 1; if (run >= 3) return true; }
        return false;
    }
    bool m_germanLike;
    std::map<std::string, bool> m_capitalised;
};

} // namespace benchdata
