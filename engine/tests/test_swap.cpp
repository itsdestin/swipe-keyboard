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
// WHY (2026-10-25): with no words before and none after, `before` and `neural` only say how common a word is; at the
// sentence weights they outvoted the finger path (HeliBoard sets, no sentences, worse than today's keyboard). Such a
// list uses the no-sentence weights; a list with any words around it keeps the sentence weights.
TEST(Swap, NoContextUsesTheNoSentenceWeights) {
    SwapParams p; p.wPath = 1; p.wBefore = 0; p.wAfter = 1; p.wNeural = 1;
    p.ncPath = 1; p.ncBefore = 0; p.ncNeural = 0;
    // "common" fits the path worse but is the more common word; "swiped" fits better.
    const std::vector<SwapCandidate> scored{{"common", -3, -1, 0, -1, true}, {"swiped", -1, -4, 0, -4, true}};
    EXPECT_EQ(rankSwap("x", scored, p, 1).front(), "common");                          // default: sentence weights
    EXPECT_EQ(rankSwap("x", scored, p.forContext(true, false), 1).front(), "common");  // words before
    EXPECT_EQ(rankSwap("x", scored, p.forContext(false, true), 1).front(), "common");  // words after
    EXPECT_EQ(rankSwap("x", scored, p.forContext(false, false), 1).front(), "swiped"); // none: the path decides
    // Before the worker answers, `before` stands in with ncBefore + ncNeural (here 0: the path decides too).
    const std::vector<SwapCandidate> bare{{"common", -3, -1}, {"swiped", -1, -4}};
    EXPECT_EQ(rankSwap("x", bare, p.forContext(true, false), 1).front(), "common");
    EXPECT_EQ(rankSwap("x", bare, p.forContext(false, false), 1).front(), "swiped");
    SwapParams q = p; q.ncBefore = 1;
    EXPECT_EQ(rankSwap("x", bare, q.forContext(false, false), 1).front(), "common");
    q = p; q.ncNeural = 1;
    EXPECT_EQ(rankSwap("x", bare, q.forContext(false, false), 1).front(), "common");
}
// WHY pinned (2026-10-25, swap.hpp): with no sentence the tuned no-sentence weights are all 0, so a list keeps the
// swap search's own order (the recognizer's score), whatever the n-gram and neural scores say.
TEST(Swap, NoContextKeepsTheSearchOrderByDefault) {
    const SwapParams p = SwapParams::forLanguage("en").forContext(false, false);
    const std::vector<SwapCandidate> c{{"first", -9, -6, 0, -6, true}, {"second", -1, -1, 0, -1, true},
                                       {"third", -2, -2}};
    EXPECT_EQ(rankSwap("x", c, p, 3), (std::vector<std::string>{"first", "second", "third"}));
    // With words before, the same list is ranked by its scores.
    EXPECT_EQ(rankSwap("x", c, SwapParams::forLanguage("en").forContext(true, false), 1).front(), "second");
}
TEST(Swap, ForContextOnlyMarksListsWithNoWordsAround) {
    const SwapParams p = SwapParams::forLanguage("en");
    EXPECT_FALSE(p.noContext);
    EXPECT_TRUE(p.forContext(false, false).noContext);
    EXPECT_FALSE(p.forContext(true, false).noContext);
    EXPECT_FALSE(p.forContext(false, true).noContext);
    EXPECT_FALSE(p.forContext(false, false).forContext(true, true).noContext);
    // Nothing else changes, and the tap-back rules survive.
    const SwapParams t = p.forTapBack().forContext(false, false);
    EXPECT_TRUE(t.tapBack);
    EXPECT_FLOAT_EQ(t.wNeural, p.wNeural); EXPECT_FLOAT_EQ(t.ncNeural, p.ncNeural); EXPECT_EQ(t.pool, p.pool);
}
// WHY pinned: these are the tuned weights (bench/swap-tune.py, 2026-10-24); changing them needs a new tuning run,
// not a quiet edit. Every language uses the English values (see swap.hpp).
TEST(Swap, ForLanguageGivesTheTunedWeights) {
    for (const char *lang : {"en", "fr", "de", "es", "xx"}) {
        const auto p = SwapParams::forLanguage(lang);
        EXPECT_FLOAT_EQ(p.wPath, 1.f); EXPECT_FLOAT_EQ(p.wBefore, 0.f);
        EXPECT_FLOAT_EQ(p.wAfter, 1.f); EXPECT_FLOAT_EQ(p.wNeural, 1.f);
        EXPECT_EQ(p.pool, 24); EXPECT_EQ(p.minFrequency, 80);
        EXPECT_EQ(p.beam, 256); EXPECT_EQ(p.searchPrior, 0); EXPECT_TRUE(p.junkFilter);
        EXPECT_EQ(p.neighbours, 12); EXPECT_FLOAT_EQ(p.neighbourFloor, 15.f);
        EXPECT_FLOAT_EQ(p.syntheticTemperature, 2.f);
        EXPECT_FALSE(p.tapBack);
        EXPECT_EQ(p.tapBackSentenceWords, 16); EXPECT_EQ(p.tapBackMinFrequency, 0);
        EXPECT_FLOAT_EQ(p.tapBackRareWithin, 1.f);
        EXPECT_FLOAT_EQ(p.ncPath, 0.f); EXPECT_FLOAT_EQ(p.ncBefore, 0.f); EXPECT_FLOAT_EQ(p.ncNeural, 0.f);
        EXPECT_FALSE(p.noContext);
    }
    // The no-sentence floor is the one per-language setting (tuned 2026-10-25, swap.hpp).
    EXPECT_EQ(SwapParams::forLanguage("en").ncMinFrequency, 0);
    EXPECT_EQ(SwapParams::forLanguage("xx").ncMinFrequency, 0);
    EXPECT_EQ(SwapParams::forLanguage("fr").ncMinFrequency, 80);
    EXPECT_EQ(SwapParams::forLanguage("es").ncMinFrequency, 80);
    EXPECT_EQ(SwapParams::forLanguage("de").ncMinFrequency, 66);
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
    EXPECT_LE(int(c.size()), SwapParams{}.pool + SwapParams{}.neighbours);   // search words + one-edit neighbours
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

// WHY (card #500 review): one-letter words ("a", "I") have a made-up path of a single key (syntheticTrace pads it to
// two points); tapping one must still give real, finite look-alikes and never the word itself, on both kinds of list.
TEST(LanguageEngineSwap, OneLetterWordsGetLookalikes)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    LanguageEngine e;
    ASSERT_TRUE(e.init(english()));
    const auto keys = qwertyRects(0, 0, 1);
    for (const char *w : {"a", "I"}) {
        ASSERT_GE(syntheticTrace(w, keys).size(), 2u) << w;
        for (const SwapParams &p : {SwapParams{}, SwapParams{}.forTapBack()}) {
            const auto c = e.swapCandidates(w, keys, {"this", "is"}, {"good"}, {}, p);
            EXPECT_FALSE(c.empty()) << w;
            EXPECT_FALSE(hasTapped(c, w)) << w << ": " << list(c);
            for (const auto &x : c) EXPECT_TRUE(std::isfinite(x.path) && std::isfinite(x.before)) << w << " " << x.word;
        }
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
    // The tap-back list (sentence words and the looser rare-word rule on top), after the common word "the" too: it
    // has the most continuations the sentence words look through.
    const auto t1 = std::chrono::steady_clock::now();
    size_t m = 0;
    const auto tapBack = SwapParams{}.forTapBack();
    for (const auto &w : words) m += e.swapCandidates(w, keys, {"i", "think", "the"}, {"is", "fine"}, {}, tapBack).size();
    const double tms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count() / words.size();
    std::printf("swapCandidates, tap-back: %.2f ms per word over %zu words (%zu candidates)\n", tms, words.size(), m);
    // WHY 60 and not the 20 ms budget: shared CI machines are slow and noisy; the measured value is reported above.
    EXPECT_LE(ms, 60.0);
    EXPECT_LE(tms, 60.0);
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
    off.neighbours = 0;
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
    SwapParams flat;
    flat.syntheticTemperature = 2;   // a typed word: the made-up path is searched flattened
    EXPECT_FALSE(e.swapCandidates("hello", keys, {"say"}, {}, {}, flat).empty());
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
    small.beam = 0;
    small.pool = 96;
    std::set<std::string> wideWords;
    for (const auto &x : e.swapCandidates("hello", keys, {"say"}, {}, trace, big)) wideWords.insert(x.word);
    size_t kept = 0, n = 0;
    for (const auto &x : e.swapCandidates("hello", keys, {"say"}, {}, trace, small)) { ++n; kept += wideWords.count(x.word); }
    EXPECT_GE(kept * 10, n * 8) << "the wide search lost most of the narrow one's words";
}

// WHY: a flatter network output on a typed word's made-up path lets more look-alikes through (the clean path makes
// the network over-sure), and a real swipe is never flattened.
TEST(LanguageEngineSwap, SyntheticTemperatureOnlyTouchesTypedWords)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    LanguageEngine e;
    ASSERT_TRUE(e.init(english()));
    const auto keys = qwertyRects(0, 0, 1);
    SwapParams plain, flat;
    plain.syntheticTemperature = 1;
    plain.beam = flat.beam = 256;
    plain.pool = flat.pool = 96;
    flat.syntheticTemperature = 2;
    const auto a = e.swapCandidates("stole", keys, {"the"}, {}, {}, plain);
    const auto b = e.swapCandidates("stole", keys, {"the"}, {}, {}, flat);
    EXPECT_GE(b.size(), a.size()) << list(a) << "| " << list(b);
    const auto trace = traceThrough("stole", keys);
    const auto c = e.swapCandidates("stole", keys, {"the"}, {}, trace, plain);
    const auto d = e.swapCandidates("stole", keys, {"the"}, {}, trace, flat);
    ASSERT_EQ(c.size(), d.size());
    for (size_t i = 0; i < c.size(); ++i) { EXPECT_EQ(c[i].word, d[i].word); EXPECT_FLOAT_EQ(c[i].path, d[i].path); }
}

// WHY: without the swipe network a neighbour has no fit of its own; the tapped word's fit minus the floor stands in.
TEST(LanguageEngineSwap, NeighboursWithoutNetworkUseTheFloor)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    auto s = english();
    s.swipeModel.clear();
    LanguageEngine e;
    ASSERT_TRUE(e.init(s));
    const auto keys = qwertyRects(0, 0, 1);
    SwapParams p;
    p.neighbours = 12;
    p.neighbourFloor = 15;
    const auto c = e.swapCandidates("painting", keys, {"a"}, {}, {}, p);
    std::set<std::string> sim;
    for (const auto &x : e.similar("painting", 12)) sim.insert(x.word);
    EXPECT_TRUE(std::any_of(c.begin(), c.end(), [&](const SwapCandidate &x) { return sim.count(x.word) > 0; })) << list(c);
    SwapParams none, noNeighbours;
    none.neighbourFloor = -1;   // no floor and no network: a neighbour has no path at all and stays out
    noNeighbours.neighbours = 0;
    std::set<std::string> plain;
    for (const auto &x : e.swapCandidates("painting", keys, {"a"}, {}, {}, noNeighbours)) plain.insert(x.word);
    for (const auto &x : e.swapCandidates("painting", keys, {"a"}, {}, {}, none)) EXPECT_TRUE(plain.count(x.word)) << x.word;
}

// WHY: a swap list changes the search's width, frequency weight and temperature for its own search only; the next swipe
// must be recognised exactly as if no swap list had been built (the restore is a guard object, so it also runs if the
// search throws; that path has no hook to force a throw, so only the normal path is checked here).
TEST(LanguageEngineSwap, ASwapListNeverChangesTheNextSwipe)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    LanguageEngine e;
    ASSERT_TRUE(e.init(english()));
    const auto keys = qwertyRects(0, 0, 1);
    const auto trace = traceThrough("hello", keys);
    const auto words = [](const std::vector<Candidate> &c) {
        std::vector<std::string> o;
        for (const auto &x : c) o.push_back(x.word);
        return o;
    };
    const auto before = words(e.recognize(trace, keys, {"i", "said"}, 10));
    ASSERT_FALSE(before.empty());
    SwapParams odd;
    odd.beam = 512;
    odd.searchPrior = 2;            // also clears the lookahead and the frequency weight during the search
    odd.syntheticTemperature = 4.f; // a typed word: the made-up path is searched flattened
    odd.tapBack = true;
    odd.tapBackSentenceWords = 64;  // also moves the sentence prior's context to the tapped word's place
    (void)e.swapCandidates("world", keys, {"hello"}, {}, {}, odd);
    (void)e.swapCandidates("hello", keys, {"i"}, {}, trace, odd);
    EXPECT_EQ(words(e.recognize(trace, keys, {"i", "said"}, 10)), before);
}

// WHY (card #500, 2026-10-25): words the n-gram expects in the tapped word's place join the pool with their own path
// fit, so a word meant there is gathered even when its path is not a look-alike of the tapped word's.
TEST(LanguageEngineSwap, SentenceWordsJoinThePool)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    LanguageEngine e;
    ASSERT_TRUE(e.init(english()));
    const auto keys = qwertyRects(0, 0, 1);
    SwapParams off, on;
    off.tapBack = on.tapBack = true;
    off.tapBackSentenceWords = 0;
    on.tapBackSentenceWords = 16;
    const auto a = e.swapCandidates("cat", keys, {"in", "the"}, {"states"}, {}, off);
    const auto b = e.swapCandidates("cat", keys, {"in", "the"}, {"states"}, {}, on);
    std::set<std::string> plain;
    for (const auto &x : a) plain.insert(x.word);
    size_t added = 0;
    for (const auto &x : b) {
        if (plain.count(x.word)) continue;
        ++added;
        EXPECT_FALSE(hasTapped({x}, "cat"));
        EXPECT_TRUE(std::isfinite(x.path) && x.path > -1e29f) << x.word;   // its own fit, never a placeholder
        EXPECT_GE(e.frequency(x.word), 80) << x.word;                       // the junk rule still applies
    }
    for (const auto &x : a) EXPECT_TRUE(hasWord(b, x.word)) << x.word;  // nothing gathered before is lost
    EXPECT_GT(added, 0u) << list(b);
    EXPECT_LE(added, 16u) << list(b);
    // Fill in the blank: "in the ___ states" is "united", a path nothing like "cat"'s; the words after decide it.
    EXPECT_TRUE(hasWord(b, "united")) << list(b);
    EXPECT_FALSE(hasWord(a, "united")) << list(a);
    // The bar right after a swipe never gets them (it is built after every swipe; see swap.hpp).
    SwapParams bar = on;
    bar.tapBack = false;
    const auto c = e.swapCandidates("cat", keys, {"in", "the"}, {"states"}, {}, bar);
    ASSERT_EQ(c.size(), a.size()) << list(c);
    for (size_t i = 0; i < a.size(); ++i) EXPECT_EQ(a[i].word, c[i].word);
}

// WHY: the sentence words come from the n-gram pack; without one (word frequency only) the list is as before.
TEST(LanguageEngineSwap, SentenceWordsNeedTheNgramPack)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    auto s = english();
    s.ngram.clear();
    LanguageEngine e;
    ASSERT_TRUE(e.init(s));
    const auto keys = qwertyRects(0, 0, 1);
    SwapParams off, on;
    off.tapBack = on.tapBack = true;
    off.tapBackSentenceWords = 0;
    on.tapBackSentenceWords = 16;
    const auto a = e.swapCandidates("cat", keys, {"in", "the"}, {"states"}, {}, off);
    const auto b = e.swapCandidates("cat", keys, {"in", "the"}, {"states"}, {}, on);
    ASSERT_EQ(a.size(), b.size()) << list(b);
    for (size_t i = 0; i < a.size(); ++i) EXPECT_EQ(a[i].word, b[i].word);
}

// WHY (card #500, 2026-10-25): a tap-back list may offer rarer words than the bar after a swipe (the person asked
// for options), but only with a path that fits the swipe about as well as the tapped word; the bar keeps its rule.
TEST(LanguageEngineSwap, TapBackListsMayOfferRareWordsThatFitThePath)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    LanguageEngine e;
    ASSERT_TRUE(e.init(english()));
    const auto keys = qwertyRects(0, 0, 1);
    const auto trace = traceThrough("were", keys);
    SwapParams bar;
    bar.tapBackSentenceWords = 0;
    bar.tapBackMinFrequency = 0;
    bar.tapBackRareWithin = 1e9f;   // no path condition: any rare word may come in
    const auto rare = [&](const std::vector<SwapCandidate> &c) {
        std::vector<SwapCandidate> o;
        for (const auto &x : c) if (e.frequency(x.word) < 80) o.push_back(x);
        return o;
    };
    // The bar after a swipe ignores the tap-back settings.
    EXPECT_TRUE(rare(e.swapCandidates("were", keys, {"we"}, {}, trace, bar)).empty());
    EXPECT_FALSE(bar.tapBack);
    const SwapParams loose = bar.forTapBack();
    EXPECT_TRUE(loose.tapBack);
    const auto all = rare(e.swapCandidates("were", keys, {"we"}, {}, trace, loose));
    ASSERT_FALSE(all.empty());
    // With the path condition, only the rare words whose path is within the margin of the tapped word's stay; the
    // other spelling "we're" carries the tapped word's own path.
    SwapParams strict = loose;
    strict.tapBackRareWithin = 0;
    const auto c = e.swapCandidates("were", keys, {"we"}, {}, trace, strict);
    const auto it = std::find_if(c.begin(), c.end(), [](const SwapCandidate &x) { return x.word == "we're"; });
    ASSERT_NE(it, c.end()) << list(c);
    const auto kept = rare(c);
    for (const auto &x : kept) EXPECT_GE(x.path, it->path - 1e-3f) << x.word;
    EXPECT_LT(kept.size(), all.size()) << list(c);
}

// WHY (2026-10-25): a list for a word with no sentence around it never gathers words under ncMinFrequency, and a list
// with a sentence is not touched by it (swap.hpp). English files, with the floor set by hand.
TEST(LanguageEngineSwap, NoSentenceFloorOnlyCutsListsWithNoWordsAround)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    LanguageEngine e;
    ASSERT_TRUE(e.init(english()));
    const auto keys = qwertyRects(0, 0, 1);
    const auto trace = traceThrough("were", keys);
    SwapParams p = SwapParams{}.forTapBack();
    p.tapBackSentenceWords = 0;
    p.tapBackRareWithin = 1e9f;   // rare words may come in, so a floor has something to cut
    p.ncMinFrequency = 120;
    const auto below = [&](const std::vector<SwapCandidate> &c) {
        size_t n = 0;
        for (const auto &x : c) n += e.frequency(x.word) < 120;
        return n;
    };
    const auto withSentence = e.swapCandidates("were", keys, {"we"}, {}, trace, p.forContext(true, false));
    EXPECT_GT(below(withSentence), 0u) << list(withSentence);
    const auto none = e.swapCandidates("were", keys, {}, {}, trace, p.forContext(false, false));
    ASSERT_FALSE(none.empty());
    // The other spelling ("we're") is never filtered; every other word is at or above the floor.
    for (const auto &x : none) if (x.word != "we're") EXPECT_GE(e.frequency(x.word), 120) << x.word;
}
