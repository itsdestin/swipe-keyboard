// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Scores the swipe engine on real swipes: first-guess / top-3 accuracy, per board, and milliseconds per swipe.
//   swipekb-swipenet-bench --model model.bin --words words.tsv --layout keys.tsv swipes.jsonl [options]
//   (no --model: geometric fallback only)    --layouts-dir DIR: per-swipe "layout" id -> DIR/<id>.tsv
// Timing is for one thread (--jobs 1, the default); more jobs only speed up parameter sweeps.
// Denominators printed: "all" = every swipe read; "in-dict" = swipes whose target is in the word list.
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <thread>

#include "swipekb/ngram.hpp"
#include "swipekb/swipe_engine.hpp"
#include "swipekb/swipe_lm.hpp"

using namespace swipekb;
using Clock = std::chrono::steady_clock;

namespace {
struct Swipe { std::string word, board; float aspect; std::vector<SwipePoint> pts; std::vector<std::string> ctx; };

// How a candidate is compared with the target: "ascii" = a-z only (the original English benchmark),
// "lookup" = lower-cased with accents kept, "exact" = byte for byte.
std::string norm(const std::string &s, const std::string &mode) {
    if (mode == "exact") return s;
    if (mode == "lookup") return lowerUtf8(s);
    std::string o;
    for (unsigned char c : s) if (std::isalpha(c)) o.push_back(char(std::tolower(c)));
    return o;
}
}

int main(int argc, char **argv) {
    std::string model, words, layoutPath, layoutsDir, input, ngramPath, match = "ascii";
    EngineParams ep;
    float lmWeight = 1.0f; bool useContext = false;
    int jobs = 1, topN = 5; size_t limit = 0, perBoard = 0, misses = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--model") model = val(); else if (a == "--words") words = val(); else if (a == "--layout") layoutPath = val();
        else if (a == "--layouts-dir") layoutsDir = val();
        else if (a == "--beam") ep.beam.beam = std::atoi(val()); else if (a == "--keys-per-step") ep.beam.keysPerStep = std::atoi(val());
        else if (a == "--lookahead") ep.beam.lookahead = float(std::atof(val())); else if (a == "--freq-weight") ep.freqWeight = float(std::atof(val()));
        else if (a == "--geo-weight") ep.geoWeight = float(std::atof(val())); else if (a == "--rerank") ep.rerank = std::atoi(val());
        else if (a == "--jobs") jobs = std::atoi(val()); else if (a == "--limit") limit = size_t(std::atoll(val()));
        else if (a == "--top") topN = std::atoi(val()); else if (a == "--match") match = val();
        else if (a == "--per-board") perBoard = size_t(std::atoll(val()));
        else if (a == "--misses") misses = size_t(std::atoll(val()));
        else if (a == "--context") useContext = true;
        else if (a == "--ngram") ngramPath = val(); else if (a == "--lm-weight") lmWeight = float(std::atof(val()));
        else if (a[0] != '-') input = a; else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (words.empty() || (layoutPath.empty() && layoutsDir.empty()) || input.empty()) {
        std::fprintf(stderr, "usage: %s [--model m.bin] --words words.tsv (--layout keys.tsv | --layouts-dir DIR) [--freq-weight x --geo-weight x --beam N --keys-per-step N --jobs N --limit N --match ascii|lookup|exact --per-board N] swipes.jsonl\n", argv[0]);
        return 2;
    }
    std::vector<Swipe> swipes;
    QFile f(QString::fromStdString(input));
    if (!f.open(QIODevice::ReadOnly)) { std::fprintf(stderr, "cannot read swipes\n"); return 1; }
    while (!f.atEnd() && (!limit || swipes.size() < limit)) {
        const auto o = QJsonDocument::fromJson(f.readLine()).object();
        Swipe s;
        s.word = o["word"].toString().toStdString();
        s.board = o["layout"].toString().toStdString();
        s.aspect = float(o["canvas_height"].toDouble() / o["canvas_width"].toDouble());
        // Previous words of the recorded sentence (the collections store the sentence and the word's position).
        if (o.contains("sentence") && o.contains("word_idx")) {
            const auto toks = o["sentence"].toString().split(' ', Qt::SkipEmptyParts);
            for (int k = 0; k < std::min<int>(o["word_idx"].toInt(), int(toks.size())); ++k) {
                const std::string a = NgramLM::alphaForm(toks[k].toStdString());
                if (!a.empty()) s.ctx.push_back(a);
            }
        }
        for (const auto &p : o["data"].toArray()) {
            const auto po = p.toObject();
            s.pts.push_back({float(po["x"].toDouble()), float(po["y"].toDouble()), po["t"].toDouble()});
        }
        if (!s.pts.empty()) swipes.push_back(std::move(s));
    }
    struct Out { std::vector<std::string> top; bool inDict = false; double ms = 0; };
    std::vector<Out> out(swipes.size());
    // Group swipes by board so each board's engine (its own layout, hence its own lexicon indices) is built once.
    std::map<std::string, std::vector<size_t>> groups;
    for (size_t i = 0; i < swipes.size(); ++i) groups[layoutsDir.empty() ? std::string() : swipes[i].board].push_back(i);
    NgramLM lm;
    if (!ngramPath.empty() && !lm.load(ngramPath)) { std::fprintf(stderr, "cannot load n-gram file\n"); return 1; }
    const auto start = Clock::now();
    size_t boardsSkipped = 0;
    for (const auto &[board, idxs] : groups) {
        SwipeEngine base;
        const std::string lp = layoutsDir.empty() ? layoutPath : layoutsDir + "/" + board + ".tsv";
        if (!base.loadLayout(lp)) { ++boardsSkipped; continue; }
        base.params = ep;
        if (!base.loadWords(words)) { std::fprintf(stderr, "cannot read words\n"); return 1; }
        if (!model.empty() && !base.loadModel(model)) { std::fprintf(stderr, "cannot load model\n"); return 1; }
        std::set<std::string> dict;
        for (uint32_t w = 0; w < base.lexicon().wordCount(); ++w) dict.insert(norm(base.lexicon().text(w), match));
        std::atomic<size_t> next{0};
        auto work = [&]() {
            SwipeEngine eng(base);
            std::unique_ptr<NgramWordPrior> prior;
            if (!ngramPath.empty()) {   // single-word swipes: plain word frequency from the model, no previous words
                prior = std::make_unique<NgramWordPrior>(lm, eng.lexicon());
                prior->unigramOnly = !useContext; prior->lmWeight = lmWeight;
                if (!useContext) eng.setPrior([&](uint32_t w) { return (*prior)(w); });
            }
            for (size_t k; (k = next++) < idxs.size();) {
                const size_t i = idxs[k];
                if (prior && useContext) {   // sentence-context prior: previous words of this swipe's sentence
                    prior->setContext(swipes[i].ctx);
                    eng.setPrior([&](uint32_t w) { return (*prior)(w); }, prior->hints());
                }
                const auto a = Clock::now();
                const auto res = eng.decode(swipes[i].pts, swipes[i].aspect, topN);
                out[i].ms = std::chrono::duration<double, std::milli>(Clock::now() - a).count();
                for (const auto &r : res) out[i].top.push_back(norm(eng.lexicon().text(r.word), match));
                out[i].inDict = dict.count(norm(swipes[i].word, match)) > 0;
            }
        };
        std::vector<std::thread> th;
        for (int j = 1; j < jobs; ++j) th.emplace_back(work);
        work();
        for (auto &t : th) t.join();
    }
    const double wall = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    struct Tally { size_t n = 0, t1 = 0, t3 = 0, d = 0, d1 = 0, d3 = 0; double ms = 0; };
    auto add = [&](Tally &t, size_t i) {
        const std::string truth = norm(swipes[i].word, match);
        const auto &top = out[i].top;
        const bool h1 = !top.empty() && top[0] == truth;
        bool h3 = false;
        for (size_t k = 0; k < top.size() && k < 3; ++k) h3 |= top[k] == truth;
        ++t.n; t.t1 += h1; t.t3 += h3; t.ms += out[i].ms;
        if (out[i].inDict) { ++t.d; t.d1 += h1; t.d3 += h3; }
    };
    for (size_t i = 0, shown = 0; i < swipes.size() && shown < misses; ++i) {   // first N first-guess misses: truth -> top 3
        const std::string truth = norm(swipes[i].word, match);
        if (!out[i].top.empty() && out[i].top[0] == truth) continue;
        std::printf("miss %-14s%s ->", truth.c_str(), out[i].inDict ? "" : "*");
        for (size_t k = 0; k < out[i].top.size() && k < 3; ++k) std::printf(" %s", out[i].top[k].c_str());
        std::printf("\n");
        ++shown;
    }
    Tally all; std::map<std::string, Tally> boards;
    for (size_t i = 0; i < swipes.size(); ++i) { add(all, i); if (!layoutsDir.empty()) add(boards[swipes[i].board], i); }
    auto pct = [](size_t a, size_t n) { return n ? 100.0 * double(a) / double(n) : 0.0; };
    std::printf("all %zu: first %.2f%% top-3 %.2f%% | in-dict %zu (%.1f%%): first %.2f%% top-3 %.2f%% | %.2f ms/swipe (jobs %d, wall %.1f s)\n",
                all.n, pct(all.t1, all.n), pct(all.t3, all.n), all.d, pct(all.d, all.n), pct(all.d1, all.d), pct(all.d3, all.d),
                all.ms / double(std::max<size_t>(1, all.n)), jobs, wall / 1000);
    if (perBoard) {
        std::vector<std::pair<std::string, Tally>> v(boards.begin(), boards.end());
        std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second.n > b.second.n; });
        for (size_t i = 0; i < v.size() && i < perBoard; ++i)
            std::printf("  board %-22s n=%-5zu first %.2f%% top-3 %.2f%% (in-dict %.2f%% / %.2f%%)\n", v[i].first.c_str(), v[i].second.n,
                        pct(v[i].second.t1, v[i].second.n), pct(v[i].second.t3, v[i].second.n), pct(v[i].second.d1, v[i].second.d), pct(v[i].second.d3, v[i].second.d));
    }
    if (boardsSkipped) std::printf("(%zu boards had no readable layout file; their swipes count as misses)\n", boardsSkipped);
    return 0;
}
