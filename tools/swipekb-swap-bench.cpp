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
// HeliBoard input has no sentence. WHY that matters: tap-back lists are then ranked with no words before the word,
// the same as a word at the very start of a sentence.
//
// Usage:
//   swipekb-swap-bench --mode baseline|new --dict D --model M [--ngram N --vocab V --nwp W] [--context] --language L
//                      (--layout F | --layouts-dir DIR) [--locale en_US] [--limit N] [--match ascii|lookup] [--params F]
//                      swipes.jsonl
// Output: Report::format() lines (bench/swap-score.sh reads them), then "setup: <engine mode> ngram=0|1 nwp=0|1 n=N".
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
    // WHY refused for now: the improved swap order arrives in a later step of card #500; until then only today's
    // keyboard can be measured, and a silent fallback would record baseline numbers under a "new" label.
    if (mode != "baseline") { std::fprintf(stderr, "--mode %s is not available yet (only baseline)\n", mode.c_str()); return 2; }

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

    // Junk rule copied from tools/swipekb-bench.cpp so measure 4 means the same as the swipe scoreboard's "junk".
    // WHY the dictionary read again here: the rule needs the dictionary's own capitals.
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
    auto junk = [&](const std::string &w) {
        const int f = engine.frequency(w);
        const std::string low = QString::fromStdString(w).toLower().toStdString();
        const bool name = setup.language.rfind("de", 0) != 0 && capitalised.count(low) && capitalised[low];
        return stretchedWord(w) || f < 80 || (name && f < 120);
    };
    const auto norm = [&](const std::string &s) { return benchdata::norm(s, match); };

    swapbench::Report report;
    auto keysFor = [&](const BenchSwipe &s) {
        return benchdata::readLayout(layoutsDir.empty() ? layout : layoutsDir + "/" + s.board + ".tsv", s.w, s.h);
    };
    // WHY a warm-up swipe: the first recognize() builds the word list for the layout (seconds), which is not a swipe.
    for (const auto &s : swipes) {
        const auto keys = keysFor(s);
        if (!keys.empty()) { engine.recognize(s.trace, keys, s.before, 3); break; }
    }
    size_t n = 0, noLayout = 0;
    for (const auto &s : swipes) {
        ++n;
        const auto keys = keysFor(s);
        if (keys.empty()) { ++noLayout; continue; }
        // The sentence as the keyboard reads it from the text (swipekb::textContextWords, at most 20 words).
        QStringList beforeWords;
        for (const auto &w : s.before) beforeWords << QString::fromStdString(w);
        const QStringList contextWords = beforeWords.isEmpty() ? QStringList()
                                                               : textContextWords(beforeWords.join(u' ') + u' ');
        const auto context = toStd(contextWords);

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
        // 3. The guess bar's other places, in their order.
        const std::vector<std::string> after = toStd(candidates.mid(1));
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
        if (offeredUnprompted(w, language)) {
            QStringList clean;
            for (const auto &x : neighbours) if (offeredUnprompted(x, language)) clean << x;
            neighbours = clean;
        }
        // (The keyboard's personalize() step is skipped: with no learning and boost 0 it changes nothing.)
        neighbours = rankRegionalCandidates(neighbours, qlocale, 12);
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
    std::printf("%s", report.format().c_str());
    std::printf("setup: %s ngram=%d nwp=%d n=%zu\n", engine.mode(), haveNgram ? 1 : 0, haveNgram && !nwp.empty() ? 1 : 0, n);
    if (noLayout) std::printf("(%zu swipes had no readable layout file; skipped)\n", noLayout);
    return 0;
}
