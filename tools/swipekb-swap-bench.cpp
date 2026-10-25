// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// swipekb-swap-bench — the swap scoreboard's engine (card #500). Replays recorded swipes and grades the swap options a
// person sees when the keyboard's first guess was wrong:
//   after-swipe      the guess bar's other places right after the swipe;
//   tapback-trace    the list shown when they tap back on the word while this swipe's guesses are still remembered;
//   tapback-notrace  the same list without the swipe's guesses (a word tapped later, or not typed by swiping);
//   junk             when the first guess was right, how many of the bar's other places are words nobody means;
//   speed            milliseconds to build one tap-back list (dictionary neighbours + sentence ranking + ordering).
// `--mode baseline` replays today's keyboard: it mirrors SwipeInputMethod::finishSwipe and observeWordSwap
// (plugin/swipeinputmethod.cpp) and calls the shipped WordSwap::build itself, so the baseline is the code that ships.
// `--mode new` grades the tap-to-swap path (card #500): the committed word is the same as baseline's, and every list
// is LanguageEngine::swapCandidates scored by the sentence model (Predictor::scoreWords) and ordered by rankSwap,
// then given the same tidy-up as baseline (swear-word rule, regional spelling order).
// `--params F`: SwapParams as JSON {wPath, wBefore, wAfter, wNeural, pool, minFrequency} (missing keys keep the
// defaults), or an ARRAY of such objects: then every set is graded in one pass and each report is printed after a
// line "== params N" (bench/swap-tune.py uses this; WHY: the swipe and the swap searches are the slow part, and they
// do not depend on the weights, so one pass can grade many weight sets).
// HeliBoard input has no sentence. WHY that matters: tap-back lists are then ranked with no words before the word,
// the same as a word at the very start of a sentence.
//
// Usage:
//   swipekb-swap-bench --mode baseline|new --dict D --model M [--ngram N --vocab V --nwp W] [--context] --language L
//                      (--layout F | --layouts-dir DIR) [--locale en_US] [--limit N] [--match ascii|lookup] [--params F]
//                      swipes.jsonl
// Output: Report::format() lines (bench/swap-score.sh reads them), then "setup: <engine mode> ngram=0|1 nwp=0|1 n=N".
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "benchdata.h"
#include "languagetextrules.h"
#include "swapmeasures.h"
#include "swipekb/language_engine.hpp"
#include "swipekb/prediction.hpp"
#include "swipekb/swap.hpp"
#include "swipesession.h"
#include "wordswap.h"

using namespace swipekb;
using Clock = std::chrono::steady_clock;
using benchdata::BenchSwipe;
using swapbench::Measure;

namespace {
std::vector<std::string> toStd(const QStringList &l)
{
    std::vector<std::string> o;
    for (const auto &s : l) o.push_back(s.toStdString());
    return o;
}

// --params: one JSON object or an array of them; keys left out keep SwapParams' defaults. Empty on a bad file.
std::vector<SwapParams> readParams(const std::string &path, std::string &error)
{
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) { error = "cannot read " + path; return {}; }
    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError) { error = path + ": " + pe.errorString().toStdString(); return {}; }
    QJsonArray items;
    if (doc.isArray()) items = doc.array(); else if (doc.isObject()) items.append(doc.object());
    std::vector<SwapParams> out;
    for (const auto &v : items) {
        const auto o = v.toObject();
        SwapParams p;
        p.wPath = float(o.value("wPath").toDouble(p.wPath));
        p.wBefore = float(o.value("wBefore").toDouble(p.wBefore));
        p.wAfter = float(o.value("wAfter").toDouble(p.wAfter));
        p.wNeural = float(o.value("wNeural").toDouble(p.wNeural));
        p.pool = o.value("pool").toInt(p.pool);
        p.minFrequency = o.value("minFrequency").toInt(p.minFrequency);
        out.push_back(p);
    }
    if (out.empty()) error = path + ": no parameter sets";
    return out;
}
}

int main(int argc, char **argv)
{
    LanguageEngineSetup setup;
    std::string layout, layoutsDir, input, match = "ascii", mode, vocab, nwp, params, locale = "en_US";
    bool useContext = false;
    size_t limit = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--mode") mode = val();
        else if (a == "--dict") setup.dictionary = val();
        else if (a == "--model") setup.swipeModel = val();
        else if (a == "--ngram") setup.ngram = val();
        else if (a == "--vocab") vocab = val();
        else if (a == "--nwp") nwp = val();
        else if (a == "--language") setup.language = val();
        else if (a == "--locale") locale = val();
        else if (a == "--layout") layout = val();
        else if (a == "--layouts-dir") layoutsDir = val();
        else if (a == "--context") useContext = true;
        else if (a == "--limit") limit = size_t(std::atoll(val().c_str()));
        else if (a == "--match") match = val();
        else if (a == "--params") params = val();
        else if (a[0] != '-') input = a;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (mode.empty() || setup.dictionary.empty() || (layout.empty() && layoutsDir.empty()) || input.empty()) {
        std::fprintf(stderr, "usage: swipekb-swap-bench --mode baseline|new --dict D --model M [--ngram N --vocab V --nwp W]"
                             " [--context] --language L (--layout F | --layouts-dir DIR) [--locale en_US] [--limit N]"
                             " [--match ascii|lookup] [--params F] swipes.jsonl\n");
        return 2;
    }
    if (mode != "baseline" && mode != "new") { std::fprintf(stderr, "unknown --mode %s (baseline|new)\n", mode.c_str()); return 2; }
    const bool newMode = mode == "new";
    // WHY the language's tuned weights by default: an unlabelled "new" run must grade what the keyboard will ship.
    std::vector<SwapParams> paramSets{SwapParams::forLanguage(setup.language)};
    if (!params.empty()) {
        std::string err;
        paramSets = readParams(params, err);
        if (paramSets.empty()) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    }
    if (!newMode && paramSets.size() > 1) { std::fprintf(stderr, "several --params sets need --mode new\n"); return 2; }

    std::string readError;
    const std::vector<BenchSwipe> swipes = benchdata::readSwipes(input, limit, useContext, &readError);
    if (!readError.empty()) { std::fprintf(stderr, "%s\n", readError.c_str()); return 1; }
    LanguageEngine engine;
    if (!engine.init(setup)) { std::fprintf(stderr, "cannot load the dictionary %s\n", setup.dictionary.c_str()); return 1; }
    const QString language = QString::fromStdString(setup.language);
    const QString qlocale = QString::fromStdString(locale);
    // WHY both, as SwipeInputMethod::syncLearnedWords does: texting words (lol, wyd) are swipeable on the keyboard,
    // and capitals ("I", "Paris") are how the keyboard spells the word it types, which the tap-back list is built from.
    // Learned words are left out: the bench has no person's history.
    engine.liftWords(textingWordPairs(language));
    engine.setCapitals(capitalsFor(language));

    // The sentence model that ranks tap-back lists (the keyboard's prediction worker, plugin/nextwordworker.cpp).
    // WHY siblingFallback=false: the keyboard only loads a neural file it was handed, never one lying next to the pack.
    Predictor predictor;
    const bool haveNgram = !setup.ngram.empty() && !vocab.empty()
        && predictor.init(setup.ngram, vocab, nwp, false) && predictor.ready();
    if (!setup.ngram.empty() && !haveNgram) { std::fprintf(stderr, "cannot load the sentence model %s\n", setup.ngram.c_str()); return 1; }

    // WHY the shared rule (tools/benchdata.h): measure 4 must mean the same as the swipe scoreboard's "junk".
    const benchdata::JunkRule junkRule(setup.dictionary, setup.language);
    auto junk = [&](const std::string &w) { return junkRule(engine, w); };
    const auto norm = [&](const std::string &s) { return benchdata::norm(s, match); };
    // The keyboard's tidy-up of a swap list, shared by both modes so they are tidied the same way: swear words are
    // dropped unless the tapped word is one itself (offeredUnprompted), then the regional spelling order
    // ("colour" before "color" in en_GB), cut to `limit`.
    auto tidy = [&](const QString &tapped, QStringList list, int limit) {
        if (offeredUnprompted(tapped, language)) {
            QStringList clean;
            for (const auto &x : list) if (offeredUnprompted(x, language)) clean << x;
            list = clean;
        }
        return rankRegionalCandidates(list, qlocale, limit);
    };

    // One report per parameter set (baseline: one).
    std::vector<swapbench::Report> reports(paramSets.size());
    swapbench::Report &report = reports.front();
    auto keysFor = [&](const BenchSwipe &s) {
        return benchdata::readLayout(layoutsDir.empty() ? layout : layoutsDir + "/" + s.board + ".tsv", s.w, s.h);
    };
    // The sentence as the keyboard reads it from the text (swipekb::textContextWords, at most 20 words).
    auto contextFor = [](const BenchSwipe &s) {
        QStringList beforeWords;
        for (const auto &w : s.before) beforeWords << QString::fromStdString(w);
        return toStd(beforeWords.isEmpty() ? QStringList() : textContextWords(beforeWords.join(u' ') + u' '));
    };
    // WHY a warm-up swipe: the first recognize() builds the word list for the layout (seconds), which is not a swipe. It gets the same cleaned context as the loop below.
    for (const auto &s : swipes) {
        const auto keys = keysFor(s);
        if (!keys.empty()) { engine.recognize(s.trace, keys, contextFor(s), 3); break; }
    }
    size_t n = 0, noLayout = 0;
    for (const auto &s : swipes) {
        ++n;
        const auto keys = keysFor(s);
        if (keys.empty()) { ++noLayout; continue; }
        const auto context = contextFor(s);

        // 1. The swipe, as finishSwipe: a 30-wide beam, regional spelling order cut to 3, then the other spelling.
        QStringList candidates;
        for (const auto &c : engine.recognize(s.trace, keys, context, 30))
            if (!c.word.empty()) candidates << QString::fromStdString(c.word);
        const QString rawShape = QString::fromStdString(engine.bestShapeWord());
        candidates = rankRegionalCandidates(candidates, qlocale, 3, rawShape);
        if (!candidates.isEmpty()) {
            const auto others = engine.otherSpellings(candidates.front().toStdString());
            if (!others.empty()) candidates = SwipeSession::withOtherSpelling(candidates, QString::fromStdString(others.front()));
        }
        // 2. A swipe with no guess types nothing: it counts as a wrong first guess with nothing to offer.
        const QString w = candidates.isEmpty() ? QString() : candidates.front();
        const bool wrongFirst = candidates.isEmpty() || norm(w.toStdString()) != norm(s.word);
        if (newMode) {
            if (w.isEmpty()) {
                for (auto &r : reports)
                    for (auto k : {Measure::AfterSwipe, Measure::TapBackTrace, Measure::TapBackNoTrace})
                        r.add(k, wrongFirst, s.word, {}, norm);
                continue;
            }
            const std::string ws = w.toStdString();
            // WHY at most the first two words after, stopping at a sentence end: swapCandidates reads two, and words
            // of the next sentence are no evidence for this one.
            std::vector<std::string> next;
            for (const auto &a : s.after) {
                if (next.size() >= 2) break;
                next.push_back(a);
                if (!a.empty() && (a.back() == '.' || a.back() == '!' || a.back() == '?')) break;
            }
            // The candidates (and their neural scores) depend only on pool and minFrequency, not on the weights, so
            // they are built once per such pair and every weight set reuses them.
            struct Lists { std::vector<SwapCandidate> afterSwipe, trace, noTrace; };
            std::map<std::pair<int, int>, Lists> cache;
            auto withNeural = [&](std::vector<SwapCandidate> c) {
                // WHY one call per list, all words at once: the keyboard's worker answers for the whole list, so
                // every candidate has a neural score or none does (rankSwap's "neural missing" rule never splits a list).
                if (haveNgram && !nwp.empty() && !c.empty()) {
                    std::vector<std::string> words;
                    for (const auto &x : c) words.push_back(x.word);
                    const auto sc = predictor.scoreWords(context, words);
                    if (sc.size() == c.size())
                        for (size_t i = 0; i < c.size(); ++i) { c[i].neural = sc[i]; c[i].hasNeural = true; }
                }
                return c;
            };
            double traceMs = -1;   // the first tap-back-with-trace search and its sentence scores (speed, below)
            auto listsFor = [&](const SwapParams &p) -> const Lists & {
                const auto key = std::make_pair(p.pool, p.minFrequency);
                auto it = cache.find(key);
                if (it != cache.end()) return it->second;
                Lists l;
                l.afterSwipe = withNeural(engine.swapCandidates(ws, keys, context, {}, s.trace, p));
                const auto t0 = Clock::now();
                l.trace = withNeural(engine.swapCandidates(ws, keys, context, next, s.trace, p));
                if (traceMs < 0) traceMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                l.noTrace = withNeural(engine.swapCandidates(ws, keys, context, next, {}, p));
                return cache.emplace(key, std::move(l)).first->second;
            };
            auto shown = [&](const std::vector<SwapCandidate> &c, const SwapParams &p, int k) {
                QStringList l;
                // WHY rank them all, then tidy: the tidy-up may drop some, and the list must still be k long.
                for (const auto &x : rankSwap(ws, c, p, int(c.size()))) l << QString::fromStdString(x);
                return toStd(tidy(w, l, k));
            };
            // Speed: one tap-back list with the first weight set, as the keyboard builds it (search + sentence
            // scores + order + tidy-up), not the other two lists built alongside it here. The neural scoring runs on
            // a worker in the keyboard, so this is an upper bound for the input thread.
            const Lists &firstLists = listsFor(paramSets.front());
            const auto t0 = Clock::now();
            const auto first = shown(firstLists.trace, paramSets.front(), 3);
            const double ms = traceMs + std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            for (size_t i = 0; i < paramSets.size(); ++i) {
                const SwapParams &p = paramSets[i];
                const Lists &l = listsFor(p);
                auto &r = reports[i];
                const auto bar = shown(l.afterSwipe, p, 2);
                r.add(Measure::AfterSwipe, wrongFirst, s.word, bar, norm);
                if (!wrongFirst) {
                    size_t j = 0;
                    for (const auto &a : bar) j += junk(a);
                    r.addJunk(bar.size(), j);
                }
                r.add(Measure::TapBackTrace, wrongFirst, s.word, i == 0 ? first : shown(l.trace, p, 3), norm);
                r.add(Measure::TapBackNoTrace, wrongFirst, s.word, shown(l.noTrace, p, 3), norm);
                r.msTotal += ms;
                ++r.lists;
            }
            continue;
        }
        // 3. The guess bar's other places, in their order.
        const std::vector<std::string> after = toStd(candidates.mid(1));
        // NOTE: the bench scores the dictionary spelling of the target (s.word); the keyboard's tap-back uses the word as
        // typed (capitalised at a sentence start).
        report.add(Measure::AfterSwipe, wrongFirst, s.word, after, norm);
        if (!wrongFirst) {
            size_t j = 0;
            for (const auto &a : after) j += junk(a);
            report.addJunk(after.size(), j);
        }
        if (w.isEmpty()) {
            report.add(Measure::TapBackTrace, wrongFirst, s.word, {}, norm);
            report.add(Measure::TapBackNoTrace, wrongFirst, s.word, {}, norm);
            continue;
        }

        // 4.-6. Tapping back on the word, as observeWordSwap (timed: this is the work done on the input thread
        // plus the worker's sentence ranking).
        const auto t0 = Clock::now();
        QStringList neighbours;
        for (const auto &o : engine.otherSpellings(w.toLower().toStdString())) neighbours << QString::fromStdString(o);
        for (const auto &c : engine.similar(w.toStdString(), 12)) neighbours << QString::fromStdString(c.word);
        // (The keyboard's personalize() step is skipped: with no learning and boost 0 it changes nothing.)
        neighbours = tidy(w, neighbours, 12);
        QStringList ranked;
        if (haveNgram)
            for (const auto &r : predictor.predict(context, 64, w.left(1).toLower().toStdString()))
                ranked << QString::fromStdString(r);
        QStringList list = WordSwap::build(w, candidates, neighbours, ranked, 3);
        report.msTotal += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        ++report.lists;
        // WHY swapped back: build() puts the strongest option in the middle of three (the bar's layout); scoring
        // needs the ranked order, best first.
        auto rankedOrder = [](QStringList l) { if (l.size() == 3) l.swapItemsAt(0, 1); return l; };
        report.add(Measure::TapBackTrace, wrongFirst, s.word, toStd(rankedOrder(list)), norm);
        // 7. The same word with no remembered swipe guesses.
        list = WordSwap::build(w, {}, neighbours, ranked, 3);
        report.add(Measure::TapBackNoTrace, wrongFirst, s.word, toStd(rankedOrder(list)), norm);
    }
    if (reports.size() == 1) {
        std::printf("%s", report.format().c_str());
    } else {
        for (size_t i = 0; i < reports.size(); ++i) std::printf("== params %zu\n%s", i, reports[i].format().c_str());
    }
    std::printf("setup: %s ngram=%d nwp=%d n=%zu\n", engine.mode(), haveNgram ? 1 : 0, haveNgram && !nwp.empty() ? 1 : 0, n);
    if (noLayout) std::printf("(%zu swipes had no readable layout file; skipped)\n", noLayout);
    return 0;
}
