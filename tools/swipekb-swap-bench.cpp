// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// swipekb-swap-bench — the swap scoreboard's engine (card #500). Replays recorded swipes and grades the swap options a
// person sees when the keyboard's first guess was wrong:
//   after-swipe      the guess bar's other places right after the swipe;
//   tapback-trace    the list shown when they tap back on the word while this swipe's guesses are still remembered;
//   tapback-notrace  the same list without the swipe's guesses (a word tapped later, or not typed by swiping);
//   junk             when the first guess was right, how many of the bar's other places are words nobody means;
//   junk-tapback     the same for the tap-back lists (with and without the swipe's guesses; new mode only);
//   speed            milliseconds to build one tap-back list (dictionary neighbours + sentence ranking + ordering).
// `--mode baseline` replays today's keyboard: it mirrors SwipeInputMethod::finishSwipe and observeWordSwap
// (plugin/swipeinputmethod.cpp) and calls the shipped WordSwap::build itself, so the baseline is the code that ships.
// `--mode new` grades the tap-to-swap path (card #500): the committed word is the same as baseline's, and every list
// is LanguageEngine::swapCandidates scored by the sentence model (Predictor::scoreWords) and ordered by rankSwap,
// then given the keyboard's own tidy-up (swapflow::tidySwapList, the very function both front ends call: swear-word rule,
// regional spelling order, the tapped word's capitals). It also reports how long the after-swipe list takes to build
// (the swap search and tidy-up the keyboard runs right after committing a swipe; the sentence scores run on a worker).
// `--params F`: SwapParams as JSON {wPath, wBefore, wAfter, wNeural, pool, minFrequency, beam, searchPrior,
// junkFilter, neighbours, neighbourFloor, syntheticTemperature,
// tapBackSentenceWords, tapBackMinFrequency, tapBackRareWithin, ncPath, ncBefore, ncNeural, ncMinFrequency} (missing keys keep the
// defaults), or an ARRAY of such objects: then every set is graded in one pass and each report is printed after a
// line "== params N" (bench/swap-tune.py uses this; WHY: the swipe and the swap searches are the slow part, and they
// do not depend on the weights, so one pass can grade many weight sets).
// `--diagnose` (new mode): for each wrong first guess, where the right word was lost (diagnose(), below):
// not gathered at all (and why: not a dictionary word, cut by the pool size, dropped as junk, pruned by the sentence
// prior or the search width during the search, a one-edit neighbour, or its path is far from the swipe), or gathered
// but ranked too low. Prints a table and 20 example misses after the report; `--diagnose-out FILE` also writes one JSON
// line per case and measure. WHY kept in the tool: it is how the next change to the swap lists should be chosen.
// HeliBoard input has no sentence. WHY that matters: tap-back lists are then ranked with no words before the word,
// the same as a word at the very start of a sentence. Every list is ranked as the keyboard ranks it: with no words
// before and none after, with the no-sentence weights (SwapParams::forContext).
// `--no-context` drops the sentence even when `--context` is given (the swipes are read without it). WHY: the
// no-sentence weights are tuned on the tuning sets with their sentences removed (bench/swap-tune.py trv8k-nocontext),
// which is how HeliBoard records and the first word of a message look.
//
// Usage:
//   swipekb-swap-bench --mode baseline|new --dict D --model M [--ngram N --vocab V --nwp W] [--context | --no-context] --language L
//                      (--layout F | --layouts-dir DIR) [--locale en_US] [--limit N] [--match ascii|lookup] [--params F]
//                      [--diagnose | --diagnose-out FILE] [--junk-out FILE]
//                      swipes.jsonl
// Output: Report::format() lines (bench/swap-score.sh reads them), then "setup: <engine mode> ngram=0|1 nwp=0|1 n=N".
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <tuple>
#include <string>
#include <vector>

#include "benchdata.h"
#include "languagetextrules.h"
#include "swapmeasures.h"
#include "swipekb/language_engine.hpp"
#include "swipekb/prediction.hpp"
#include "swipekb/swap.hpp"
#include "swapflow.h"
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
        p.beam = o.value("beam").toInt(p.beam);
        p.searchPrior = o.value("searchPrior").toInt(p.searchPrior);
        p.junkFilter = o.value("junkFilter").toBool(p.junkFilter);
        p.neighbours = o.value("neighbours").toInt(p.neighbours);
        p.neighbourFloor = float(o.value("neighbourFloor").toDouble(p.neighbourFloor));
        p.syntheticTemperature = float(o.value("syntheticTemperature").toDouble(p.syntheticTemperature));
        p.tapBackSentenceWords = o.value("tapBackSentenceWords").toInt(p.tapBackSentenceWords);
        p.tapBackMinFrequency = o.value("tapBackMinFrequency").toInt(p.tapBackMinFrequency);
        p.tapBackRareWithin = float(o.value("tapBackRareWithin").toDouble(p.tapBackRareWithin));
        p.ncPath = float(o.value("ncPath").toDouble(p.ncPath));
        p.ncBefore = float(o.value("ncBefore").toDouble(p.ncBefore));
        p.ncNeural = float(o.value("ncNeural").toDouble(p.ncNeural));
        p.ncMinFrequency = o.value("ncMinFrequency").toInt(p.ncMinFrequency);
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
    bool useContext = false, noContext = false, diagnose = false;
    std::string diagnoseFile, junkOut;
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
        else if (a == "--no-context") noContext = true;
        else if (a == "--limit") limit = size_t(std::atoll(val().c_str()));
        else if (a == "--match") match = val();
        else if (a == "--params") params = val();
        else if (a == "--diagnose") diagnose = true;
        else if (a == "--diagnose-out") { diagnose = true; diagnoseFile = val(); }
        else if (a == "--junk-out") junkOut = val();
        else if (a[0] != '-') input = a;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (mode.empty() || setup.dictionary.empty() || (layout.empty() && layoutsDir.empty()) || input.empty()) {
        std::fprintf(stderr, "usage: swipekb-swap-bench --mode baseline|new --dict D --model M [--ngram N --vocab V --nwp W]"
                             " [--context | --no-context] --language L (--layout F | --layouts-dir DIR) [--locale en_US] [--limit N]"
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
    if (diagnose && !newMode) { std::fprintf(stderr, "--diagnose needs --mode new\n"); return 2; }
    if (!newMode && paramSets.size() > 1) { std::fprintf(stderr, "several --params sets need --mode new\n"); return 2; }

    std::string readError;
    const std::vector<BenchSwipe> swipes = benchdata::readSwipes(input, limit, useContext && !noContext, &readError);
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
    // --junk-out: every option shown for a word whose first guess was right, one line each (list, word, frequency,
    // junk 0/1), first parameter set only. WHY: a junk definition can then be judged by reading the words it calls
    // junk, and an alternative definition re-scored from the file without replaying the swipes.
    std::FILE *junkLog = junkOut.empty() ? nullptr : std::fopen(junkOut.c_str(), "w");
    if (!junkOut.empty() && !junkLog) { std::fprintf(stderr, "cannot write %s\n", junkOut.c_str()); return 1; }
    auto junk = [&](const std::string &w, const char *list = nullptr) {
        const bool j = junkRule(engine, w);
        if (junkLog && list) std::fprintf(junkLog, "%s\t%s\t%d\t%d\n", list, w.c_str(), engine.frequency(w), j ? 1 : 0);
        return j;
    };
    const auto norm = [&](const std::string &s) { return benchdata::norm(s, match); };
    // Baseline mode's tidy-up of the dictionary neighbours, as the keyboard did before card #500: swear words are
    // dropped unless the tapped word is one itself (offeredUnprompted), then the regional spelling order
    // ("colour" before "color" in en_GB), cut to `limit`. New mode uses swapflow::tidySwapList (same two rules, plus
    // the tapped word's capitals), the function the keyboard itself calls.
    auto tidy = [&](const QString &tapped, QStringList list, int limit) {
        if (offeredUnprompted(tapped, language)) {
            QStringList clean;
            for (const auto &x : list) if (offeredUnprompted(x, language)) clean << x;
            list = clean;
        }
        return rankRegionalCandidates(list, qlocale, limit);
    };

    // New mode: the keyboard's tidy-up itself (no learned words in the bench, so no personal-words step).
    const swapflow::Rules swapRules{language, qlocale, {}};
    double afterMsTotal = 0, afterMsMax = 0;
    size_t afterLists = 0;
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
    // --diagnose tallies, per measure (index = Measure).
    constexpr std::array<int, 4> kPools{16, 24, 48, 96};
    enum Bucket { NotWord, PoolCut, Junk, Prior, Beam, Neighbour, PathFar, Other, kBuckets };
    const char *bucketName[kBuckets] = {"not a dictionary word", "found, cut by the pool size", "found, dropped as junk",
                                        "pruned by the sentence prior", "pruned by the search width",
                                        "one-edit neighbour (similar)", "path far from the swipe (>8 nats)", "other"};
    struct Diag {
        size_t cases = 0, inPool[4] = {}, gathered = 0, rank[6] = {};   // rank: 1, 2, 3, 4-6, 7+, tidied away
        size_t bucket[kBuckets] = {}, otherSpelling = 0, inSimilar = 0, notGathered = 0;
        std::vector<float> pathGap;   // target fit - tapped word fit, for words not gathered
    } diag[3];
    std::vector<std::string> examples[3];
    std::FILE *diagOut = diagnoseFile.empty() ? nullptr : std::fopen(diagnoseFile.c_str(), "w");
    const auto jsonStr = [](const std::string &x) {
        return QString::fromUtf8(QJsonDocument(QJsonArray{QString::fromStdString(x)}).toJson(QJsonDocument::Compact)).mid(1).chopped(1).toStdString();
    };

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
        QString otherSpelling;
        if (!candidates.isEmpty()) {
            const auto others = engine.otherSpellings(candidates.front().toStdString());
            if (!others.empty()) {
                otherSpelling = QString::fromStdString(others.front());
                candidates = SwipeSession::withOtherSpelling(candidates, otherSpelling);
            }
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
            // WHY swapflow::wordsAfter, the keyboard's own rule: the text after the tapped word as the keyboard reads
            // it (the punctuation recorded on the swiped word itself, then the next words) gives the same words after
            // (at most two, none past punctuation or a sentence end).
            std::vector<std::string> next;
            {
                const QString sw = QString::fromStdString(s.word);
                qsizetype tail = sw.size();
                while (tail > 0 && !sw.at(tail - 1).isLetter()) --tail;
                QString textAfter = sw.mid(tail);
                for (const auto &a : s.after) textAfter += u' ' + QString::fromStdString(a);
                next = toStd(swapflow::wordsAfter(textAfter));
            }
            // The candidates (and their neural scores) depend only on pool and minFrequency, not on the weights, so
            // they are built once per such pair and every weight set reuses them.
            struct Lists { std::vector<SwapCandidate> afterSwipe, trace, noTrace; };
            // WHY every gathering knob in the key: each changes which words are found; the weights do not.
            using GatherKey = std::tuple<int, int, int, int, bool, int, float, float, int, int, float, int>;
            std::map<GatherKey, Lists> cache;
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
            // WHY forContext per list, as the keyboard does (swapflow::afterSwipeList / tapBackParams): the bar after a
            // swipe never has words after; a tap-back list has the recorded words after. With none on either side the
            // list is ranked with the no-sentence weights, and gathered with the no-sentence rare-word floor (ncMinFrequency).
            const bool hasBefore = !context.empty();
            auto barParams = [&](const SwapParams &p) { return p.forContext(hasBefore, false); };
            auto tapParams = [&](const SwapParams &p) { return p.forTapBack().forContext(hasBefore, !next.empty()); };
            double traceMs = -1;   // the first tap-back-with-trace search and its sentence scores (speed, below)
            auto listsFor = [&](const SwapParams &p) -> const Lists & {
                const GatherKey key{p.pool, p.minFrequency, p.beam, p.searchPrior, p.junkFilter, p.neighbours,
                                    p.neighbourFloor, p.syntheticTemperature, p.tapBackSentenceWords,
                                    p.tapBackMinFrequency, p.tapBackRareWithin, p.ncMinFrequency};
                auto it = cache.find(key);
                if (it != cache.end()) return it->second;
                Lists l;
                const auto ta = Clock::now();
                auto afterCands = engine.swapCandidates(ws, keys, context, {}, s.trace, barParams(p));
                // Timed as the keyboard builds the after-swipe list on the input thread: the search and the tidy-up
                // (the first weight set's, once per swipe), without the sentence scores (a worker does those).
                if (cache.empty()) {
                    (void)swapflow::tidySwapList(w, afterCands, p, 2, swapRules, false);
                    const double ams = std::chrono::duration<double, std::milli>(Clock::now() - ta).count();
                    afterMsTotal += ams;
                    afterMsMax = std::max(afterMsMax, ams);
                    ++afterLists;
                }
                l.afterSwipe = withNeural(std::move(afterCands));
                const auto t0 = Clock::now();
                // WHY forTapBack: the keyboard builds tap-back lists with it (its rare-word rule), the bar after a
                // swipe without.
                const SwapParams tb = tapParams(p);
                l.trace = withNeural(engine.swapCandidates(ws, keys, context, next, s.trace, tb));
                if (traceMs < 0) traceMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                l.noTrace = withNeural(engine.swapCandidates(ws, keys, context, next, {}, tb));
                return cache.emplace(key, std::move(l)).first->second;
            };
            // WHY dress false for the after-swipe bar: the keyboard does not dress it in the first guess's capitals
            // (swapflow::afterSwipeList); tap-back lists are dressed like the tapped word.
            // WHY only the words the keyboard would have scored: the keyboard sends the list's best
            // swapflow::kMaxScored words (ranked without neural scores) to the worker and drops the rest
            // (swapflow::wordsToScore / applyScores). The cache holds neural scores for every word (they do not depend
            // on the weights), so the cut is redone here per weight set. Before 2026-10-25 the bench ranked every
            // gathered word, which only mattered once a list could hold more than 32 (pool 24 + 12 neighbours = 36).
            auto asKeyboard = [&](const std::vector<SwapCandidate> &c, const SwapParams &p) {
                if (c.empty() || !c.front().hasNeural) return c;
                auto bare = c;
                for (auto &x : bare) x.hasNeural = false;
                std::vector<SwapCandidate> kept;
                for (const auto &word : swapflow::wordsToScore(w, bare, p)) {
                    const std::string ww = word.toStdString();
                    for (const auto &x : c) if (x.word == ww) { kept.push_back(x); break; }
                }
                return kept;
            };
            auto shownQ = [&](const std::vector<SwapCandidate> &c, const SwapParams &p, int k, bool dress) {
                return swapflow::tidySwapList(w, asKeyboard(c, p), p, k, swapRules, dress);
            };
            auto shown = [&](const std::vector<SwapCandidate> &c, const SwapParams &p, int k, bool dress) {
                return toStd(shownQ(c, p, k, dress));
            };
            // Speed: one tap-back list with the first weight set, as the keyboard builds it (search + sentence
            // scores + order + tidy-up), not the other two lists built alongside it here. The neural scoring runs on
            // a worker in the keyboard, so this is an upper bound for the input thread.
            const Lists &firstLists = listsFor(paramSets.front());
            const auto t0 = Clock::now();
            const auto first = shown(firstLists.trace, tapParams(paramSets.front()), 3, true);
            const double ms = traceMs + std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            for (size_t i = 0; i < paramSets.size(); ++i) {
                const SwapParams &p = paramSets[i];
                const Lists &l = listsFor(p);
                auto &r = reports[i];
                // WHY through swapflow::afterSwipeCandidates: the keyboard's bar after a swipe is the first guess, its
                // other spelling ("it's" after "its") when there is one, then the swap list; the bench grades the
                // same other places (as baseline mode grades candidates.mid(1)).
                const auto bar = toStd(swapflow::afterSwipeCandidates(w, shownQ(l.afterSwipe, barParams(p), 2, false), otherSpelling).mid(1));
                r.add(Measure::AfterSwipe, wrongFirst, s.word, bar, norm);
                if (!wrongFirst) {
                    size_t j = 0;
                    for (const auto &a : bar) j += junk(a, i == 0 ? "bar" : nullptr);
                    r.addJunk(bar.size(), j);
                }
                const auto tapTrace = i == 0 ? first : shown(l.trace, tapParams(p), 3, true);
                const auto tapNoTrace = shown(l.noTrace, tapParams(p), 3, true);
                r.add(Measure::TapBackTrace, wrongFirst, s.word, tapTrace, norm);
                r.add(Measure::TapBackNoTrace, wrongFirst, s.word, tapNoTrace, norm);
                if (!wrongFirst) {
                    // Measure 4b: junk among the tap-back options of a word that was already right.
                    size_t jt = 0, jn = 0;
                    for (const auto &a : tapTrace) jt += junk(a, i == 0 ? "trace" : nullptr);
                    for (const auto &a : tapNoTrace) jn += junk(a, i == 0 ? "notrace" : nullptr);
                    r.addTapJunk(0, tapTrace.size(), jt);
                    r.addTapJunk(1, tapNoTrace.size(), jn);
                }
                r.msTotal += ms;
                ++r.lists;
            }
            if (diagnose && wrongFirst) {
                const SwapParams &p0 = paramSets.front();
                const std::string target = norm(s.word);
                const auto has = [&](const std::vector<SwapCandidate> &c) {
                    return std::any_of(c.begin(), c.end(), [&](const SwapCandidate &x) { return norm(x.word) == target; });
                };
                const Lists &l0 = listsFor(p0);
                const auto others = engine.otherSpellings(ws);
                const bool isOther = std::any_of(others.begin(), others.end(), [&](const std::string &o) { return norm(o) == target; });
                const auto sim = engine.similar(ws, 50);
                const bool inSim = std::any_of(sim.begin(), sim.end(), [&](const Candidate &c) { return norm(c.word) == target; });
                // WHY trimmed: recorded sentence words keep their punctuation ("slipping.", "(the"); the word itself
                // is what the dictionary is asked about.
                QString core = QString::fromStdString(s.word);
                while (!core.isEmpty() && !core.front().isLetter()) core.remove(0, 1);
                while (!core.isEmpty() && !core.back().isLetter()) core.chop(1);
                const bool isWord = !core.isEmpty() && engine.hasWord(core.toStdString());
                for (int m = 0; m < 3; ++m) {
                    const bool withTrace = m != int(Measure::TapBackNoTrace);
                    const auto &tr = withTrace ? s.trace : std::vector<TracePoint>{};
                    const auto &nx = m == int(Measure::AfterSwipe) ? std::vector<std::string>{} : next;
                    const auto &pool0 = m == 0 ? l0.afterSwipe : m == 1 ? l0.trace : l0.noTrace;
                    Diag &d = diag[m];
                    ++d.cases;
                    auto variant = [&](auto change) {
                        SwapParams q = m == int(Measure::AfterSwipe) ? p0 : p0.forTapBack();
                        change(q);
                        return has(engine.swapCandidates(ws, keys, context, nx, tr, q));
                    };
                    for (size_t k = 0; k < kPools.size(); ++k) d.inPool[k] += variant([&](SwapParams &q) { q.pool = kPools[k]; });
                    // The rank after ordering and tidy-up, in the full list.
                    const SwapParams pr = m == int(Measure::AfterSwipe) ? barParams(p0) : tapParams(p0);
                    const auto tidied = toStd(swapflow::tidySwapList(w, asKeyboard(pool0, pr), pr, int(pool0.size()), swapRules, m != int(Measure::AfterSwipe)));
                    int rank = -1;
                    for (size_t j = 0; j < tidied.size(); ++j) if (norm(tidied[j]) == target) { rank = int(j); break; }
                    const bool gathered = has(pool0);
                    int b = -1;
                    float gap = NAN;
                    if (gathered) {
                        ++d.gathered;
                        d.rank[rank < 0 ? 5 : rank < 3 ? rank : rank < 6 ? 3 : 4] += 1;
                    } else {
                        ++d.notGathered;
                        d.otherSpelling += isOther;
                        d.inSimilar += inSim;
                        const auto tt = tr.size() >= 2 ? tr : syntheticTrace(ws, keys);
                        const auto fits = engine.pathFits({core.toStdString(), ws}, keys, tt);
                        if (fits.size() == 2 && fits[0] > -1e29f && fits[1] > -1e29f) { gap = fits[0] - fits[1]; d.pathGap.push_back(gap); }
                        if (!isWord) b = NotWord;
                        else if (variant([](SwapParams &q) { q.pool = 96; })) b = PoolCut;
                        else if (variant([](SwapParams &q) { q.pool = 96; q.junkFilter = false; })) b = Junk;
                        else if (variant([](SwapParams &q) { q.pool = 96; q.searchPrior = 1; })
                                 || variant([](SwapParams &q) { q.pool = 96; q.searchPrior = 2; })) b = Prior;
                        else if (variant([](SwapParams &q) { q.pool = 96; q.beam = 256; })) b = Beam;
                        else if (inSim) b = Neighbour;
                        else if (!std::isnan(gap) && gap < -8.f) b = PathFar;
                        else b = Other;
                        ++d.bucket[b];
                    }
                    const bool miss = rank < 0 || rank >= (m == 0 ? 2 : 3);
                    std::string top;
                    for (size_t j = 0; j < tidied.size() && j < 5; ++j) top += (j ? ", " : "") + tidied[j];
                    if (miss && m != 0 && examples[m].size() < 10) {
                        char line[512];
                        std::snprintf(line, sizeof line, "  %s -> %s  [%s]  options: %s", ws.c_str(), s.word.c_str(),
                                      gathered ? ("gathered, rank " + std::to_string(rank + 1)).c_str() : bucketName[b], top.c_str());
                        examples[m].push_back(line);
                    }
                    if (diagOut) {
                        std::string ctx;
                        for (const auto &c : context) ctx += (ctx.empty() ? "" : " ") + c;
                        std::fprintf(diagOut, "{\"m\":%d,\"tapped\":%s,\"target\":%s,\"ctx\":%s,\"gathered\":%d,\"rank\":%d,"
                                              "\"bucket\":%d,\"gap\":%s,\"other\":%d,\"similar\":%d,\"top\":%s}\n",
                                     m, jsonStr(ws).c_str(), jsonStr(s.word).c_str(), jsonStr(ctx).c_str(), gathered ? 1 : 0,
                                     rank, b, std::isnan(gap) ? "null" : std::to_string(gap).c_str(), isOther ? 1 : 0,
                                     inSim ? 1 : 0, jsonStr(top).c_str());
                    }
                }
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
            for (const auto &a : after) j += junk(a, "bar");
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
        const QStringList listNoTrace = WordSwap::build(w, {}, neighbours, ranked, 3);
        report.add(Measure::TapBackNoTrace, wrongFirst, s.word, toStd(rankedOrder(listNoTrace)), norm);
        // Measure 4b for today's keyboard too, so new-mode tap-back junk has a baseline to be judged against.
        if (!wrongFirst) {
            size_t jt = 0, jn = 0;
            for (const auto &a : list) jt += junk(a.toStdString(), "trace");
            for (const auto &a : listNoTrace) jn += junk(a.toStdString(), "notrace");
            report.addTapJunk(0, size_t(list.size()), jt);
            report.addTapJunk(1, size_t(listNoTrace.size()), jn);
        }
    }
    if (reports.size() == 1) {
        std::printf("%s", report.format().c_str());
    } else {
        for (size_t i = 0; i < reports.size(); ++i) std::printf("== params %zu\n%s", i, reports[i].format().c_str());
    }
    if (diagnose) {
        const char *names[3] = {"after-swipe", "tapback-trace", "tapback-notrace"};
        const auto pct = [](size_t a, size_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
        std::printf("diagnose: wrong first guesses with a typed word; pool = gathered before ordering (weights set 0)\n");
        for (int m = 0; m < 3; ++m) {
            const Diag &d = diag[m];
            std::printf("diagnose %s: cases %zu | in pool 16 %.2f%% 24 %.2f%% 48 %.2f%% 96 %.2f%% | gathered %zu (%.2f%%)\n",
                        names[m], d.cases, pct(d.inPool[0], d.cases), pct(d.inPool[1], d.cases), pct(d.inPool[2], d.cases),
                        pct(d.inPool[3], d.cases), d.gathered, pct(d.gathered, d.cases));
            std::printf("  gathered, rank: 1st %zu, 2nd %zu, 3rd %zu, 4-6 %zu, 7+ %zu, tidied away %zu\n", d.rank[0], d.rank[1],
                        d.rank[2], d.rank[3], d.rank[4], d.rank[5]);
            std::vector<float> g = d.pathGap;
            std::sort(g.begin(), g.end());
            std::printf("  not gathered %zu: (other spelling %zu, in similar() %zu, path gap median %.1f nats)\n", d.notGathered,
                        d.otherSpelling, d.inSimilar, g.empty() ? 0.f : g[g.size() / 2]);
            for (int b = 0; b < kBuckets; ++b) std::printf("    %-36s %4zu (%.2f%% of cases)\n", bucketName[b], d.bucket[b], pct(d.bucket[b], d.cases));
        }
        for (int m = 1; m < 3; ++m) {
            std::printf("examples %s (missed the 3 options):\n", names[m]);
            for (const auto &e : examples[m]) std::printf("%s\n", e.c_str());
        }
    }
    if (diagOut) std::fclose(diagOut);
    if (junkLog) std::fclose(junkLog);
    // bench/swap-score.sh records the average as its "speed-after-swipe" row (budget 20 ms on the input thread).
    if (newMode && afterLists)
        std::printf("after-swipe list time: avg %.2f ms, max %.2f ms over %zu lists (swap search + tidy-up, input thread)\n",
                    afterMsTotal / double(afterLists), afterMsMax, afterLists);
    // WHY printed: the junk rule's frequency cut-offs come from a rank in this word list (benchdata.h).
    std::printf("junk rule: rare below frequency %d, names below %d\n", junkRule.rareBelow(), junkRule.nameBelow());
    std::printf("setup: %s ngram=%d nwp=%d n=%zu\n", engine.mode(), haveNgram ? 1 : 0, haveNgram && !nwp.empty() ? 1 : 0, n);
    if (noLayout) std::printf("(%zu swipes had no readable layout file; skipped)\n", noLayout);
    return 0;
}
