// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// swipekb-rescore-bench — joint-model experiment, step 1 (no training): how much could a better context-aware ranking
// of a swipe gain, and what does the existing neural next-word model (LANG.nwp.bin) give when it re-scores swipes?
//   --headroom   every swipe whose first guess is wrong goes in one bucket: the word is not in the word list; it is,
//                but the search did not find it; or it was found at rank 2-3, 4-10, 11-30 (the 30 words the keyboard
//                asks for, which a re-ranker sees) or further down. Once with the shipped search width and once with
//                a search 256 wide. Buckets 2-3 .. 11-30 are the ceiling of any re-ranking of today's search.
//   --dump FILE  one JSON line per swipe for tuning (bench/rescore-tune.py): whether it has words before, and for each
//                of the 30 found words its score, n-gram prior, neural score and whether it is the right word. Counts
//                and numbers only, no words.
//   --speed      milliseconds per swipe for recognize() without re-scoring, with it and the neural model's sentence
//                state cold (read from scratch), and warm (the sentence read up to the word before, as while typing
//                on), plus the memory the next-word model takes.
// The first guess is the engine's (LanguageEngine::recognize asked for 30 words, as the keyboard's finishSwipe),
// without the keyboard's regional-spelling step. The words before are read as the keyboard reads them from the text
// (textContextWords, as tools/swipekb-swap-bench), so the numbers match the UX scoreboard's runs.
//
// Usage:
//   swipekb-rescore-bench --dict D --model M --ngram N --vocab V --nwp W --language L (--layout F | --layouts-dir DIR)
//                         [--context] [--limit N] [--match ascii|lookup] [--headroom] [--dump FILE] [--speed]
//                         [--rescore-weights NEURAL,NGRAM] swipes.jsonl
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "benchdata.h"
#include "languagetextrules.h"
#include "swipekb/language_engine.hpp"
#include "swipekb/prediction.hpp"

using namespace swipekb;
using Clock = std::chrono::steady_clock;
using benchdata::BenchSwipe;

namespace {
// Resident memory of this process in MB (/proc/self/statm), for the model's memory cost.
double residentMb()
{
    std::ifstream f("/proc/self/statm");
    long pages = 0, resident = 0;
    f >> pages >> resident;
    return double(resident) * double(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
}

// The headroom buckets for one search width.
struct Buckets {
    enum { NotWord, NotFound, Rank2to3, Rank4to10, Rank11to30, RankBeyond30, Rank1Other, kCount };
    size_t n[kCount] = {};
    static constexpr const char *names[kCount] = {"not in the word list", "in the list, not found by the search",
                                                  "found, rank 2-3", "found, rank 4-10", "found, rank 11-30",
                                                  // WHY this last bucket: with the shipped width it is empty by
                                                  // construction (a check); with the wider search it is the swipes
                                                  // that search alone would put right.
                                                  "found, rank > 30", "found first (only the wider search)"};
};
}

int main(int argc, char **argv)
{
    LanguageEngineSetup setup;
    std::string layout, layoutsDir, input, match = "ascii", vocab, nwp, dumpFile, weights;
    bool useContext = false, headroom = false, speed = false;
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
        else if (a == "--vocab") vocab = val();
        else if (a == "--nwp") nwp = val();
        else if (a == "--language") setup.language = val();
        else if (a == "--layout") layout = val();
        else if (a == "--layouts-dir") layoutsDir = val();
        else if (a == "--context") useContext = true;
        else if (a == "--limit") limit = size_t(std::atoll(val().c_str()));
        else if (a == "--match") match = val();
        else if (a == "--headroom") headroom = true;
        else if (a == "--dump") dumpFile = val();
        else if (a == "--speed") speed = true;
        else if (a == "--rescore-weights") weights = val();
        else if (a[0] != '-') input = a;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (setup.dictionary.empty() || (layout.empty() && layoutsDir.empty()) || input.empty() || (!headroom && dumpFile.empty() && !speed)) {
        std::fprintf(stderr, "usage: swipekb-rescore-bench --dict D --model M --ngram N --vocab V --nwp W --language L"
                             " (--layout F | --layouts-dir DIR) [--context] [--limit N] [--match ascii|lookup]"
                             " [--headroom] [--dump FILE] [--speed] [--rescore-weights NEURAL,NGRAM] swipes.jsonl\n");
        return 2;
    }
    std::string readError;
    const auto swipes = benchdata::readSwipes(input, limit, useContext, &readError);
    if (!readError.empty()) { std::fprintf(stderr, "%s\n", readError.c_str()); return 1; }
    LanguageEngine engine;
    if (!engine.init(setup)) { std::fprintf(stderr, "cannot load the dictionary %s\n", setup.dictionary.c_str()); return 1; }
    // WHY the next-word model is loaded after the engine: the memory it adds is measured on its own.
    const double mbBefore = residentMb();
    Predictor predictor;
    const bool haveNwp = !setup.ngram.empty() && !vocab.empty() && !nwp.empty() && predictor.init(setup.ngram, vocab, nwp, false);
    const double mbAfter = residentMb();
    if ((speed || !dumpFile.empty()) && !haveNwp) { std::fprintf(stderr, "--dump and --speed need --ngram, --vocab and --nwp\n"); return 1; }

    ContextRescore rescore = ContextRescore::forLanguage(setup.language);
    if (!weights.empty()) {
        const auto parts = QString::fromStdString(weights).split(u',');
        if (parts.size() != 2) { std::fprintf(stderr, "--rescore-weights NEURAL,NGRAM\n"); return 2; }
        rescore.neuralWeight = parts[0].toFloat();
        rescore.ngramWeight = parts[1].toFloat();
    }
    rescore.scorer = [&predictor](const std::vector<std::string> &c, const std::vector<std::string> &w) { return predictor.scoreWords(c, w); };

    const auto norm = [&](const std::string &s) { return benchdata::norm(s, match); };
    auto keysFor = [&](const BenchSwipe &s) {
        return benchdata::readLayout(layoutsDir.empty() ? layout : layoutsDir + "/" + s.board + ".tsv", s.w, s.h);
    };
    // WHY a warm-up swipe: the first call builds the word list for the layout (seconds), which is not a swipe.
    for (const auto &s : swipes) {
        const auto keys = keysFor(s);
        if (!keys.empty()) { engine.recognize(s.trace, keys, s.before, 30); break; }
    }
    // The sentence as the keyboard reads it from the text (at most 20 words), as tools/swipekb-swap-bench.
    auto contextFor = [](const BenchSwipe &s) {
        QStringList w;
        for (const auto &x : s.before) w << QString::fromStdString(x);
        std::vector<std::string> out;
        if (!w.isEmpty()) for (const auto &x : textContextWords(w.join(u' ') + u' ')) out.push_back(x.toStdString());
        return out;
    };
    std::FILE *dump = dumpFile.empty() ? nullptr : std::fopen(dumpFile.c_str(), "w");
    if (!dumpFile.empty() && !dump) { std::fprintf(stderr, "cannot write %s\n", dumpFile.c_str()); return 1; }

    Buckets shipped, wide;
    size_t n = 0, noLayout = 0, firstRight = 0, withBefore = 0, wrongWithBefore = 0;
    size_t reRankableWithBefore = 0;   // wrong first guess, sentence known, word among the 30 (buckets 2-3 .. 11-30)
    double msOff = 0, msCold = 0, msWarm = 0;
    size_t timed = 0;
    for (const auto &s : swipes) {
        ++n;
        const auto keys = keysFor(s);
        if (keys.empty()) { ++noLayout; continue; }
        const std::string target = norm(s.word);
        const auto context = contextFor(s);
        const bool before = !context.empty();
        withBefore += before;
        engine.setContextRescore({});
        const auto rec = engine.recognize(s.trace, keys, context, 30);
        const bool right = !rec.empty() && norm(rec.front().word) == target;
        firstRight += right;
        if (headroom && !right) {
            wrongWithBefore += before;
            QString core = QString::fromStdString(s.word);
            while (!core.isEmpty() && !core.front().isLetter()) core.remove(0, 1);
            while (!core.isEmpty() && !core.back().isLetter()) core.chop(1);
            const bool isWord = !core.isEmpty() && engine.hasWord(core.toStdString());
            for (int pass = 0; pass < 2; ++pass) {
                Buckets &b = pass == 0 ? shipped : wide;
                const int beam = pass == 0 ? 0 : 256;
                // WHY two lists: the 30 the keyboard asks for (what a re-ranker sees, ordered as the keyboard orders
                // them), then up to 256 to tell "found further down" from "not found at all".
                const auto top30 = engine.rawCandidates(s.trace, keys, context, 30, beam);
                int rank = -1;
                for (size_t k = 0; k < top30.size(); ++k) if (norm(top30[k].word) == target) { rank = int(k); break; }
                int bucket;
                if (!isWord) bucket = Buckets::NotWord;
                else if (rank == 0) bucket = Buckets::Rank1Other;
                else if (rank >= 1 && rank <= 2) bucket = Buckets::Rank2to3;
                else if (rank >= 3 && rank <= 9) bucket = Buckets::Rank4to10;
                else if (rank >= 10) bucket = Buckets::Rank11to30;
                else {
                    const auto all = engine.rawCandidates(s.trace, keys, context, 256, beam);
                    const bool found = std::any_of(all.begin(), all.end(), [&](const RawSwipeCandidate &c) { return norm(c.word) == target; });
                    bucket = found ? Buckets::RankBeyond30 : Buckets::NotFound;
                }
                ++b.n[bucket];
                if (pass == 0 && before && bucket >= Buckets::Rank2to3 && bucket <= Buckets::Rank11to30) ++reRankableWithBefore;
            }
        }
        if (dump) {
            // The 30 words recognize() re-scores (the same search, the same order) with their parts.
            const auto raw = engine.rawCandidates(s.trace, keys, context, 30);
            std::vector<std::string> words;
            for (const auto &c : raw) words.push_back(c.word);
            const auto neural = predictor.scoreWords(context, words);
            std::fprintf(dump, "{\"before\":%d,\"right\":%d,\"c\":[", before ? 1 : 0, right ? 1 : 0);
            for (size_t k = 0; k < raw.size(); ++k)
                std::fprintf(dump, "%s[%.4f,%.4f,%.4f,%d]", k ? "," : "", raw[k].score, raw[k].prior,
                             k < neural.size() ? neural[k] : -99.f, norm(raw[k].word) == target ? 1 : 0);
            std::fprintf(dump, "]}\n");
        }
        if (speed && before) {
            // Off, then on with the sentence read from scratch, then on with the sentence read up to the word before
            // (the model only reads the newest word, as while typing a sentence on).
            const auto t0 = Clock::now();
            engine.setContextRescore({});
            (void)engine.recognize(s.trace, keys, context, 30);
            const auto t1 = Clock::now();
            engine.setContextRescore(rescore);
            (void)predictor.scoreWords({"\x01"}, {});   // WHY: a sentence no real one extends, so the next read is cold
            const auto t2 = Clock::now();
            (void)engine.recognize(s.trace, keys, context, 30);
            const auto t3 = Clock::now();
            (void)predictor.scoreWords(std::vector<std::string>(context.begin(), context.end() - 1), {});
            const auto t4 = Clock::now();
            (void)engine.recognize(s.trace, keys, context, 30);
            const auto t5 = Clock::now();
            const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            msOff += ms(t0, t1); msCold += ms(t2, t3); msWarm += ms(t4, t5);
            ++timed;
            engine.setContextRescore({});
        }
    }
    if (dump) std::fclose(dump);
    const size_t counted = n - noLayout;
    const auto pct = [](size_t a, size_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
    std::printf("swipes %zu (with words before %zu), first guess right %zu (%.2f%%), wrong %zu\n", counted, withBefore,
                firstRight, pct(firstRight, counted), counted - firstRight);
    if (headroom) {
        const size_t wrong = counted - firstRight;
        std::printf("headroom: wrong first guesses %zu (with words before %zu); percent of wrong / of all swipes\n", wrong, wrongWithBefore);
        for (int k = 0; k < Buckets::kCount; ++k)
            std::printf("  %-40s beam48 %5zu (%5.2f%% / %5.2f%%)   beam256 %5zu (%5.2f%% / %5.2f%%)\n", Buckets::names[k],
                        shipped.n[k], pct(shipped.n[k], wrong), pct(shipped.n[k], counted), wide.n[k], pct(wide.n[k], wrong),
                        pct(wide.n[k], counted));
        const size_t reRank = shipped.n[Buckets::Rank2to3] + shipped.n[Buckets::Rank4to10] + shipped.n[Buckets::Rank11to30];
        std::printf("headroom: a perfect re-ranker of the 30 found words: first guess %.2f%% -> %.2f%% (+%.2f points); "
                    "only swipes with words before: +%.2f points\n", pct(firstRight, counted), pct(firstRight + reRank, counted),
                    pct(reRank, counted), pct(reRankableWithBefore, counted));
        std::printf("headroom: words 2-3 alone (a top-3 re-ranker): +%.2f points\n", pct(shipped.n[Buckets::Rank2to3], counted));
    }
    if (speed && timed)
        std::printf("speed: %zu swipes with words before: recognize %.2f ms, re-scored cold %.2f ms, warm %.2f ms per swipe\n",
                    timed, msOff / double(timed), msCold / double(timed), msWarm / double(timed));
    if (haveNwp) {
        struct stat st {};
        const double fileMb = ::stat(nwp.c_str(), &st) == 0 ? double(st.st_size) / (1024.0 * 1024.0) : 0;
        std::printf("memory: next-word model (n-gram pack + vocabulary + neural file) +%.1f MB resident; neural file %.1f MB\n",
                    mbAfter - mbBefore, fileMb);
    }
    std::printf("setup: %s n=%zu weights neural=%.3f ngram=%.3f pool=%d\n", engine.mode(), n, rescore.neuralWeight,
                rescore.ngramWeight, rescore.pool);
    if (noLayout) std::printf("(%zu swipes had no readable layout file; skipped)\n", noLayout);
    return 0;
}
