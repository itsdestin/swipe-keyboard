// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// swipekb-bench — scores the keyboard's swipe engine on recorded swipes, through swipekb::LanguageEngine (the class
// the keyboard and the Gaming Mode daemon use), so every number measures the code that ships: the dictionary
// reading, the key-rectangle handling, the sentence prior and the swipe engine itself.
//
// Usage:
//   swipekb-bench --dict LANG.combined (--layout keys.tsv | --layouts-dir DIR) [--model swipe-model.bin]
//                 [--ngram LANG.ngram] [--context] [--language en] [--limit N] [--match ascii|lookup|exact] swipes.jsonl
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

#include "swipekb/language_engine.hpp"

using namespace swipekb;
using Clock = std::chrono::steady_clock;

namespace {

struct Swipe {
    std::string word, board;
    float w = 1, h = 1;
    std::vector<TracePoint> trace;
    std::vector<std::string> context;
};

// Key rectangles in canvas pixels from a layout file (0..1 of the letter area), or empty when unreadable.
std::vector<KeyRect> readLayout(const std::string &path, float w, float h)
{
    std::ifstream in(path);
    std::vector<KeyRect> keys;
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
std::string norm(const std::string &s, const std::string &mode)
{
    if (mode == "exact") return s;
    if (mode == "lookup") return QString::fromStdString(s).toLower().remove(QLatin1Char('\'')).remove(QChar(0x2019)).remove(QLatin1Char('-')).toStdString();
    std::string o;
    for (unsigned char c : s) if (std::isalpha(c)) o.push_back(char(std::tolower(c)));
    return o;
}

} // namespace

int main(int argc, char **argv)
{
    LanguageEngineSetup setup;
    std::string layout, layoutsDir, input, match = "ascii";
    bool useContext = false;
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
        else if (a == "--limit") limit = size_t(std::atoll(val().c_str()));
        else if (a == "--match") match = val();
        else if (a[0] != '-') input = a;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (setup.dictionary.empty() || (layout.empty() && layoutsDir.empty()) || input.empty()) {
        std::fprintf(stderr, "usage: swipekb-bench --dict LANG.combined (--layout keys.tsv | --layouts-dir DIR) [--model m.bin]"
                             " [--ngram LANG.ngram] [--context] [--language en] [--limit N] [--match ascii|lookup|exact] swipes.jsonl\n");
        return 2;
    }
    std::vector<Swipe> swipes;
    QFile f(QString::fromStdString(input));
    if (!f.open(QIODevice::ReadOnly)) { std::fprintf(stderr, "cannot read %s\n", input.c_str()); return 1; }
    while (!f.atEnd() && (!limit || swipes.size() < limit)) {
        const auto o = QJsonDocument::fromJson(f.readLine()).object();
        if (o.isEmpty()) continue;
        Swipe s;
        s.word = o["word"].toString().toStdString();
        s.board = o["layout"].toString().toStdString();
        s.w = float(o["canvas_width"].toDouble(1));
        s.h = float(o["canvas_height"].toDouble(1));
        if (useContext && o.contains("sentence") && o.contains("word_idx")) {
            const auto toks = o["sentence"].toString().split(' ', Qt::SkipEmptyParts);
            for (int k = 0; k < std::min<int>(o["word_idx"].toInt(), int(toks.size())); ++k) s.context.push_back(toks[k].toStdString());
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
    LanguageEngine engine;
    if (!engine.init(setup)) { std::fprintf(stderr, "cannot load the dictionary %s\n", setup.dictionary.c_str()); return 1; }
    size_t n = 0, top1 = 0, top3 = 0, inDict = 0, dict1 = 0, dict3 = 0, noLayout = 0;
    double ms = 0;
    // WHY a warm-up swipe: the first recognize() builds the word list for the layout (seconds), which is not a swipe.
    for (const auto &s : swipes) {
        const auto keys = readLayout(layoutsDir.empty() ? layout : layoutsDir + "/" + s.board + ".tsv", s.w, s.h);
        if (!keys.empty()) { engine.recognize(s.trace, keys, s.context, 3); break; }
    }
    for (const auto &s : swipes) {
        ++n;
        const auto keys = readLayout(layoutsDir.empty() ? layout : layoutsDir + "/" + s.board + ".tsv", s.w, s.h);
        if (keys.empty()) { ++noLayout; continue; }
        const auto a = Clock::now();
        const auto c = engine.recognize(s.trace, keys, s.context, 3);
        ms += std::chrono::duration<double, std::milli>(Clock::now() - a).count();
        const std::string truth = norm(s.word, match);
        const bool h1 = !c.empty() && norm(c[0].word, match) == truth;
        bool h3 = false;
        for (const auto &x : c) h3 |= norm(x.word, match) == truth;
        const bool known = engine.hasWord(s.word);
        top1 += h1; top3 += h3;
        if (known) { ++inDict; dict1 += h1; dict3 += h3; }
    }
    auto pct = [](size_t a, size_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
    std::printf("Results: %zu samples\n  top-1: %zu / %zu = %.2f%%\n  top-3: %zu / %zu = %.2f%%\n", n, top1, n, pct(top1, n), top3, n, pct(top3, n));
    std::printf("all %zu: first %.2f%% top-3 %.2f%% | in-dict %zu (%.1f%%): first %.2f%% top-3 %.2f%% | %.2f ms/swipe | %s%s\n",
                n, pct(top1, n), pct(top3, n), inDict, pct(inDict, n), pct(dict1, inDict), pct(dict3, inDict),
                ms / double(std::max<size_t>(1, n - noLayout)), engine.mode(), engine.usingNgram() ? " + n-gram" : "");
    if (noLayout) std::printf("(%zu swipes had no readable layout file; counted as misses)\n", noLayout);
    return 0;
}
