// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// swipekb-bench — scores the keyboard's swipe engine on recorded swipes, through swipekb::LanguageEngine (the class
// the keyboard and the Gaming Mode daemon use), so every number measures the code that ships: the dictionary
// reading, the key-rectangle handling, the sentence prior and the swipe engine itself.
//
// Usage:
//   swipekb-bench --dict LANG.combined (--layout keys.tsv | --layouts-dir DIR) [--model swipe-model.bin]
//                 [--ngram LANG.ngram] [--context] [--language en] [--limit N] [--match ascii|lookup|exact] [--list]
//                 [--alt-pool N --alt-weight W --alt-min-freq F --alt-name-freq F --alt-margin M --alt-stretch 0|1 | --alt-off] swipes.jsonl
// Also prints how often a shown alternative is junk and how often the alternatives hold the word when the first guess missed.
// Swipes: one JSON object per line with "word", "data" [{x,y,t}] in 0..1 of the letter area, "canvas_width",
// "canvas_height" and (with --layouts-dir) "layout"; "sentence" + "word_idx" give the previous words (--context).
// Layouts: "letter<TAB>cx<TAB>cy<TAB>half_w<TAB>half_h" (scripts/swipe/layouts/qwerty.tsv).
// Output: "  top-1: a / n = x%" and "  top-3: ..." lines (bench/score.sh reads them), then one summary line.
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "benchdata.h"
#include "swipekb/language_engine.hpp"

using namespace swipekb;
using Clock = std::chrono::steady_clock;
using benchdata::BenchSwipe;
using benchdata::norm;
using benchdata::readLayout;
using benchdata::readSwipes;

int main(int argc, char **argv)
{
    LanguageEngineSetup setup;
    std::string layout, layoutsDir, input, match = "ascii";
    bool useContext = false, list = false;
    AlternativeParams alt;
    bool altSet = false;
    size_t limit = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--dict") setup.dictionary = val();
        else if (a == "--model") setup.swipeModel = val();
        else if (a == "--ngram") setup.ngram = val();
        else if (a == "--language") setup.language = val();
        else if (a == "--layout") layout = val();
        else if (a == "--layouts-dir") layoutsDir = val();
        else if (a == "--context") useContext = true;
        else if (a == "--list") list = true;
        else if (a == "--limit") limit = size_t(std::atoll(val().c_str()));
        else if (a == "--match") match = val();
        // WHY these: tuning and checking the rules for the guesses after the first (AlternativeParams).
        else if (a == "--alt-pool") { alt.pool = std::atoi(val().c_str()); altSet = true; }
        else if (a == "--alt-weight") { alt.sentenceWeight = float(std::atof(val().c_str())); altSet = true; }
        else if (a == "--alt-min-freq") { alt.minFrequency = std::atoi(val().c_str()); altSet = true; }
        else if (a == "--alt-name-freq") { alt.nameMinFrequency = std::atoi(val().c_str()); altSet = true; }
        else if (a == "--alt-margin") { alt.margin = float(std::atof(val().c_str())); altSet = true; }
        else if (a == "--alt-stretch") { alt.dropStretched = val() == "1"; altSet = true; }
        else if (a == "--alt-off") { alt = AlternativeParams{16, 0.f, 0, 0, 1e9f, false}; altSet = true; }
        else if (a[0] != '-') input = a;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (setup.dictionary.empty() || (layout.empty() && layoutsDir.empty()) || input.empty()) {
        std::fprintf(stderr, "usage: swipekb-bench --dict LANG.combined (--layout keys.tsv | --layouts-dir DIR) [--model m.bin]"
                             " [--ngram LANG.ngram] [--context] [--language en] [--limit N] [--match ascii|lookup|exact] swipes.jsonl\n");
        return 2;
    }
    // WHY shared reader: the swap benchmark reads swipes through the same code (tools/benchdata.h).
    std::string readError;
    const std::vector<BenchSwipe> swipes = readSwipes(input, limit, useContext, &readError);
    if (!readError.empty()) { std::fprintf(stderr, "%s\n", readError.c_str()); return 1; }
    LanguageEngine engine;
    if (!engine.init(setup)) { std::fprintf(stderr, "cannot load the dictionary %s\n", setup.dictionary.c_str()); return 1; }
    if (altSet) engine.setAlternativeParams(alt);
    // WHY the dictionary read again here: the "junk alternative" measure needs the dictionary's own capitals, which
    // the engine shows in lower case (English, Spanish, French).
    std::map<std::string, bool> capitalised;
    {
        std::ifstream d(setup.dictionary);
        std::map<std::string, int> best;
        for (std::string line; std::getline(d, line);) {
            if (line.rfind(" word=", 0) != 0) continue;
            const auto c = line.find(',', 6), fp = line.find(",f=");
            if (c == std::string::npos || fp == std::string::npos) continue;
            const QString w = QString::fromStdString(line.substr(6, c - 6));
            const int fr = std::atoi(line.c_str() + fp + 3);
            const std::string low = w.toLower().toStdString();
            if (!best.count(low) || fr > best[low]) { best[low] = fr; capitalised[low] = !w.isEmpty() && w.front().isUpper(); }
        }
    }
    auto stretchedWord = [](const std::string &w) {
        const auto u = QString::fromStdString(w).toLower().toUcs4();
        int run = 1;
        for (qsizetype i = 1; i < u.size(); ++i) { run = u[i] == u[i - 1] ? run + 1 : 1; if (run >= 3) return true; }
        return false;
    };
    // A shown alternative is "junk" when the person would hardly ever mean it: stretched, rare (dictionary
    // frequency under 80 of 255) or a rarely used name (written with a capital, under 120). Fixed before tuning.
    auto junk = [&](const std::string &w) {
        const int f = engine.frequency(w);
        const std::string low = QString::fromStdString(w).toLower().toStdString();
        const bool name = setup.language.rfind("de", 0) != 0 && capitalised.count(low) && capitalised[low];
        return stretchedWord(w) || f < 80 || (name && f < 120);
    };
    size_t shownAlts = 0, junkAlts = 0, firstWrong = 0, rescued = 0;
    size_t n = 0, top1 = 0, top3 = 0, inDict = 0, dict1 = 0, dict3 = 0, noLayout = 0;
    double ms = 0;
    // WHY a warm-up swipe: the first recognize() builds the word list for the layout (seconds), which is not a swipe.
    for (const auto &s : swipes) {
        const auto keys = readLayout(layoutsDir.empty() ? layout : layoutsDir + "/" + s.board + ".tsv", s.w, s.h);
        if (!keys.empty()) { engine.recognize(s.trace, keys, s.before, 3); break; }
    }
    for (const auto &s : swipes) {
        ++n;
        const auto keys = readLayout(layoutsDir.empty() ? layout : layoutsDir + "/" + s.board + ".tsv", s.w, s.h);
        if (keys.empty()) { ++noLayout; continue; }
        const auto a = Clock::now();
        const auto c = engine.recognize(s.trace, keys, s.before, 3);
        ms += std::chrono::duration<double, std::milli>(Clock::now() - a).count();
        const std::string truth = norm(s.word, match);
        const bool h1 = !c.empty() && norm(c[0].word, match) == truth;
        bool h3 = false;
        for (const auto &x : c) h3 |= norm(x.word, match) == truth;
        const bool known = engine.hasWord(s.word);
        top1 += h1; top3 += h3;
        for (size_t k = 1; k < c.size(); ++k) { ++shownAlts; junkAlts += junk(c[k].word); }
        if (!h1) { ++firstWrong; rescued += h3; }
        if (known) { ++inDict; dict1 += h1; dict3 += h3; }
        // WHY a per-swipe list: comparing two runs swipe by swipe shows which words a change won or lost.
        // WHY every guess, not only the first: comparing runs of the alternatives needs to see which word was lost.
        if (list) {
            std::printf("swipe\t%zu\t%s", n, s.word.c_str());
            for (const auto &x : c) std::printf("\t%s", x.word.c_str());
            std::printf("\n");
        }
    }
    auto pct = [](size_t a, size_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
    std::printf("Results: %zu samples\n  top-1: %zu / %zu = %.2f%%\n  top-3: %zu / %zu = %.2f%%\n", n, top1, n, pct(top1, n), top3, n, pct(top3, n));
    std::printf("all %zu: first %.2f%% top-3 %.2f%% | in-dict %zu (%.1f%%): first %.2f%% top-3 %.2f%% | %.2f ms/swipe | %s%s\n",
                n, pct(top1, n), pct(top3, n), inDict, pct(inDict, n), pct(dict1, inDict), pct(dict3, inDict),
                ms / double(std::max<size_t>(1, n - noLayout)), engine.mode(), engine.usingNgram() ? " + n-gram" : "");
    std::printf("alternatives: %zu shown, junk %.2f%% | first guess wrong %zu, right word among the alternatives %.2f%%\n",
                shownAlts, pct(junkAlts, shownAlts), firstWrong, pct(rescued, firstWrong));
    if (noLayout) std::printf("(%zu swipes had no readable layout file; counted as misses)\n", noLayout);
    return 0;
}
