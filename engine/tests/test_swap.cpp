// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Tests for the swap ranking function, the synthetic finger path and LanguageEngine::swapCandidates.
#include <gtest/gtest.h>

#include <QString>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>

#include "swipekb/language_engine.hpp"
#include "swipekb/swap.hpp"
#include "testkeys.h"

using namespace swipekb;
using namespace swipekb::testkeys;

TEST(Swap, RankNeverReturnsTappedWordAndOrdersByScore) {
    std::vector<SwapCandidate> c{{"good", 0, -1}, {"food", -1, -1}, {"Good", 0, -1}, {"hood", -3, -1}};
    const auto r = rankSwap("good", c, SwapParams{}, 3);
    ASSERT_EQ(r, (std::vector<std::string>{"food", "hood"}));
}
TEST(Swap, AfterWordsChangeTheOrder) {
    // "their" vs "there" with the same path: the words after decide.
    std::vector<SwapCandidate> c{{"there", -1, -2, -4}, {"their", -1, -2, -1}};
    EXPECT_EQ(rankSwap("x", c, SwapParams{}, 1).front(), "their");
}
TEST(Swap, MissingNeuralIsNotZero) {
    SwapParams p; p.wNeural = 1;
    std::vector<SwapCandidate> c{{"a", -1, -1, 0, -3, true}, {"b", -1, -1, 0, 0, false}};
    EXPECT_EQ(rankSwap("x", c, p, 1).front(), "b");   // b is not penalised for having no neural score
}
// WHY: the list shown before the worker's neural scores arrive must still read the words before; the n-gram
// `before` stands in with the neural weight (the tuned wBefore is 0 because neural already holds that evidence).
TEST(Swap, WithoutNeuralTheNgramBeforeStandsIn) {
    SwapParams p; p.wBefore = 0; p.wNeural = 1; p.wAfter = 0;
    std::vector<SwapCandidate> none{{"rare", -1, -4}, {"usual", -1, -1}};
    EXPECT_EQ(rankSwap("x", none, p, 1).front(), "usual");
    // With neural scores, `before` keeps its own (zero) weight: only neural decides.
    std::vector<SwapCandidate> both{{"rare", -1, -4, 0, -1, true}, {"usual", -1, -1, 0, -2, true}};
    EXPECT_EQ(rankSwap("x", both, p, 1).front(), "rare");
}
// WHY pinned: these are the tuned weights (bench/swap-tune.py, 2026-10-24); changing them needs a new tuning run,
// not a quiet edit. Every language uses the English values (see swap.hpp).
TEST(Swap, ForLanguageGivesTheTunedWeights) {
    for (const char *lang : {"en", "fr", "de", "es", "xx"}) {
        const auto p = SwapParams::forLanguage(lang);
        EXPECT_FLOAT_EQ(p.wPath, 1.f); EXPECT_FLOAT_EQ(p.wBefore, 0.f);
        EXPECT_FLOAT_EQ(p.wAfter, 1.f); EXPECT_FLOAT_EQ(p.wNeural, 1.f);
        EXPECT_EQ(p.pool, 24); EXPECT_EQ(p.minFrequency, 80);
    }
}
TEST(Swap, SyntheticTraceVisitsKeyCentres) {
    const auto keys = qwertyRects(0, 0, 1);
    const auto t = syntheticTrace("see", keys);
    ASSERT_FALSE(t.empty());
    // 's' = row 1 (shift 50), index 1 -> left 150, centre 200; 'e' = row 0, index 2 -> centre 250.
    EXPECT_NEAR(t.front().x, 200.f, 1); EXPECT_NEAR(t.back().x, 250.f, 1);
    EXPECT_NEAR(t.front().y, 150.f, 1); EXPECT_NEAR(t.back().y, 50.f, 1);
    EXPECT_TRUE(syntheticTrace("s1", keys).empty());
    EXPECT_EQ(t.size(), 13u);   // "see" = s,e (doubled e once): one gap of 12 points + the end point
}
TEST(Swap, SyntheticTraceSkipsApostropheAndHandlesOneLetter) {
    const auto keys = qwertyRects(0, 0, 1);
    const auto a = syntheticTrace("it's", keys), b = syntheticTrace("its", keys);
    ASSERT_FALSE(a.empty());
    EXPECT_EQ(a.size(), b.size());
    EXPECT_GE(syntheticTrace("a", keys).size(), 2u);
    EXPECT_TRUE(syntheticTrace("", keys).empty());
}

// ---- LanguageEngine::swapCandidates with the real English files (skipped when SWIPEKB_ASSETS is unset) ----

namespace {
bool hasWord(const std::vector<SwapCandidate> &c, const std::string &w)
{
    return std::any_of(c.begin(), c.end(), [&](const SwapCandidate &x) { return x.word == w; });
}
bool hasTapped(const std::vector<SwapCandidate> &c, const std::string &w)
{
    return std::any_of(c.begin(), c.end(), [&](const SwapCandidate &x) {
        return QString::fromStdString(x.word).toLower() == QString::fromStdString(w).toLower(); });
}
std::string list(const std::vector<SwapCandidate> &c)
{
    std::string s;
    for (const auto &x : c) s += x.word + " ";
    return s;
}
}

TEST(LanguageEngineSwap, TypedWordGetsPathLookalikes)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    LanguageEngine e;
    ASSERT_TRUE(e.init(english()));
    const auto keys = qwertyRects(0, 0, 1);
    const auto c = e.swapCandidates("good", keys, {"this", "is"}, {"news"}, {}, SwapParams{});
    // WHY these two: their swipe paths are "good"'s with the first key one row over; a one-letter-edit list ordered
    // by frequency would not have to put them anywhere.
    EXPECT_TRUE(hasWord(c, "food") || hasWord(c, "hood")) << list(c);
    EXPECT_FALSE(hasTapped(c, "good")) << list(c);
    EXPECT_LE(int(c.size()), SwapParams{}.pool);
    for (const auto &x : c) {
        EXPECT_TRUE(std::isfinite(x.path) && std::isfinite(x.before) && std::isfinite(x.after)) << x.word;
        EXPECT_FALSE(x.hasNeural) << x.word;   // the neural score is added later (Predictor::scoreWords)
    }
}

TEST(LanguageEngineSwap, SavedTraceGivesTheSwipesRunnerUps)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    LanguageEngine e;
    ASSERT_TRUE(e.init(english()));
    const auto keys = qwertyRects(0, 0, 1);
    const auto trace = traceThrough("were", keys);
    const auto c = e.swapCandidates("were", keys, {"we"}, {"here"}, trace, SwapParams{});
    ASSERT_FALSE(c.empty());
    EXPECT_TRUE(hasWord(c, "where") || hasWord(c, "we're")) << list(c);
    EXPECT_FALSE(hasTapped(c, "were")) << list(c);
    // The other spelling shares the swiped word's path, and a clean swipe through "were" fits "were" best: no
    // candidate's path may fit better than the other spelling's (which is "were"'s own).
    const auto it = std::find_if(c.begin(), c.end(), [](const SwapCandidate &x) { return x.word == "we're"; });
    ASSERT_NE(it, c.end()) << list(c);
    for (const auto &x : c) {
        EXPECT_LE(x.path, it->path + 1e-3f) << x.word;
        EXPECT_LE(x.path, 0.f) << x.word;   // a log-probability
    }
}

// WHY: a typed word without its apostrophe must be offered its dictionary spelling ("dont" -> "don't").
TEST(LanguageEngineSwap, TypedWordWithoutApostropheGetsItsSpelling)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    LanguageEngine e;
    ASSERT_TRUE(e.init(english()));
    const auto keys = qwertyRects(0, 0, 1);
    const auto dont = e.swapCandidates("dont", keys, {"i"}, {"know"}, {}, SwapParams{});
    EXPECT_TRUE(hasWord(dont, "don't")) << list(dont);
    EXPECT_FALSE(hasWord(dont, "dont")) << list(dont);
    const auto im = e.swapCandidates("im", keys, {}, {"going"}, {}, SwapParams{});
    EXPECT_TRUE(hasWord(im, "I'm") || hasWord(im, "i'm")) << list(im);
}

TEST(LanguageEngineSwap, AfterIsZeroWithoutFollowingWords)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    LanguageEngine e;
    ASSERT_TRUE(e.init(english()));
    const auto keys = qwertyRects(0, 0, 1);
    const auto none = e.swapCandidates("good", keys, {"this", "is"}, {}, {}, SwapParams{});
    ASSERT_FALSE(none.empty());
    for (const auto &x : none) EXPECT_EQ(x.after, 0.f) << x.word;
    // With a following word the after evidence is there (log10, so below zero).
    const auto some = e.swapCandidates("good", keys, {"this", "is"}, {"news"}, {}, SwapParams{});
    ASSERT_FALSE(some.empty());
    EXPECT_TRUE(std::any_of(some.begin(), some.end(), [](const SwapCandidate &x) { return x.after < 0.f; }));
}

// WHY: without the swipe model the decoder runs on geometry alone; swap lists must still work.
TEST(LanguageEngineSwap, GeometryOnlyStillGivesCandidates)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    auto s = english();
    s.swipeModel.clear();
    s.ngram.clear();   // also no n-gram: before falls back to the word's frequency, after to 0
    LanguageEngine e;
    ASSERT_TRUE(e.init(s));
    ASSERT_FALSE(e.hasModel());
    const auto keys = qwertyRects(0, 0, 1);
    const auto c = e.swapCandidates("good", keys, {"this", "is"}, {"news"}, {}, SwapParams{});
    ASSERT_FALSE(c.empty());
    EXPECT_FALSE(hasTapped(c, "good")) << list(c);
    for (const auto &x : c) {
        EXPECT_EQ(x.after, 0.f) << x.word;
        EXPECT_GT(x.before, 0.f) << x.word;   // frequency-based, positive
    }
}

TEST(LanguageEngineSwap, FastEnoughForTheInputThread)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    LanguageEngine e;
    ASSERT_TRUE(e.init(english()));
    const auto keys = qwertyRects(0, 0, 1);
    // 1,000 common words: the most frequent completions of each two-letter start.
    std::vector<std::string> words;
    std::set<std::string> seen;
    for (const char *pre : {"th", "an", "wh", "wa", "be", "ha", "co", "pr", "st", "re", "mo", "fo", "go", "ca", "ma",
                            "sa", "lo", "pe", "de", "in", "to", "ab", "ev", "li", "ne", "ti", "wo", "yo", "se"})
        for (const auto &c : e.complete(pre, 60))
            if (words.size() < 1000 && seen.insert(c.word).second) words.push_back(c.word);
    ASSERT_GE(words.size(), 900u);
    (void)e.swapCandidates("hello", keys, {}, {}, {}, SwapParams{});   // WHY: the first call builds the word list
    const auto t0 = std::chrono::steady_clock::now();
    size_t n = 0;
    for (const auto &w : words) n += e.swapCandidates(w, keys, {"i", "think"}, {"is", "fine"}, {}, SwapParams{}).size();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / words.size();
    std::printf("swapCandidates: %.2f ms per word over %zu words (%zu candidates)\n", ms, words.size(), n);
    // WHY 60 and not the 20 ms budget: shared CI machines are slow and noisy; the measured value is reported above.
    EXPECT_LE(ms, 60.0);
}

// The swipe network's fit for any word (SwipeEngine::pathScores): the scale the swap list's paths are on.
TEST(LanguageEngineSwap, PathFitsMatchTheSearch)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    LanguageEngine e;
    ASSERT_TRUE(e.init(english()));
    const auto keys = qwertyRects(0, 0, 1);
    const auto trace = traceThrough("were", keys);
    const auto c = e.swapCandidates("were", keys, {"we"}, {}, trace, SwapParams{});
    const auto it = std::find_if(c.begin(), c.end(), [](const SwapCandidate &x) { return x.word == "we're"; });
    ASSERT_NE(it, c.end()) << list(c);   // its path is "were"'s own fit as the search found it
    const auto fit = e.pathFits({"were", "zzzz", "1"}, keys, trace);
    ASSERT_EQ(fit.size(), 3u);
    // WHY >= and close: the exact sum over alignments is never below the search's pruned sum, and the search keeps
    // a clean swipe's own word nearly whole.
    EXPECT_GE(fit[0], it->path - 1e-3f);
    EXPECT_LE(fit[0], it->path + 1.f);
    EXPECT_LE(fit[0], 0.f);
    EXPECT_LT(fit[1], fit[0] - 5.f);   // a path through z only fits far worse
    EXPECT_LE(fit[2], -1e29f);         // no key: no fit
    auto s = english();
    s.swipeModel.clear();
    LanguageEngine g;
    ASSERT_TRUE(g.init(s));
    EXPECT_TRUE(g.pathFits({"were"}, keys, trace).empty());   // geometry only: no network to score on
}

// WHY: one-edit neighbours the search did not reach join the pool, scored on the swipe (or floored).
TEST(LanguageEngineSwap, NeighboursJoinThePool)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    LanguageEngine e;
    ASSERT_TRUE(e.init(english()));
    const auto keys = qwertyRects(0, 0, 1);
    SwapParams off, on;
    on.neighbours = 12;
    on.neighbourFloor = 15;
    const auto a = e.swapCandidates("painting", keys, {"a"}, {}, {}, off);
    const auto b = e.swapCandidates("painting", keys, {"a"}, {}, {}, on);
    std::set<std::string> before;
    for (const auto &x : a) before.insert(x.word);
    std::set<std::string> sim;
    for (const auto &x : e.similar("painting", 12)) sim.insert(x.word);
    float ref = -1e30f;
    for (const auto &x : a) ref = std::max(ref, x.path);
    size_t added = 0;
    for (const auto &x : b) {
        if (before.count(x.word)) continue;
        ++added;
        EXPECT_TRUE(sim.count(x.word)) << x.word;   // only neighbours are new
        EXPECT_FALSE(hasTapped({x}, "painting"));
        EXPECT_TRUE(std::isfinite(x.path)) << x.word;
    }
    EXPECT_GT(added, 0u) << list(b);
    EXPECT_GE(b.size(), a.size());
}

// WHY: the swap search may run wider or without the sentence prior, but the next swipe must be recognised exactly
// as before (the recognizer's search settings are restored).
TEST(LanguageEngineSwap, WiderSwapSearchLeavesRecognitionAlone)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    LanguageEngine e;
    ASSERT_TRUE(e.init(english()));
    const auto keys = qwertyRects(0, 0, 1);
    const auto trace = traceThrough("hello", keys);
    const auto first = e.recognize(trace, keys, {"say"}, 5);
    SwapParams wide;
    wide.beam = 256;
    wide.searchPrior = 2;
    const auto w = e.swapCandidates("hello", keys, {"say"}, {}, trace, wide);
    EXPECT_FALSE(w.empty());
    const auto again = e.recognize(trace, keys, {"say"}, 5);
    ASSERT_EQ(first.size(), again.size());
    for (size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i].word, again[i].word);
        EXPECT_FLOAT_EQ(first[i].score, again[i].score);
    }
    // A wider search finds at least the words the default one does (same pool, junk rules).
    SwapParams big;
    big.beam = 256;
    big.pool = 96;
    SwapParams small;
    small.pool = 96;
    std::set<std::string> wideWords;
    for (const auto &x : e.swapCandidates("hello", keys, {"say"}, {}, trace, big)) wideWords.insert(x.word);
    size_t kept = 0, n = 0;
    for (const auto &x : e.swapCandidates("hello", keys, {"say"}, {}, trace, small)) { ++n; kept += wideWords.count(x.word); }
    EXPECT_GE(kept * 10, n * 8) << "the wide search lost most of the narrow one's words";
}
