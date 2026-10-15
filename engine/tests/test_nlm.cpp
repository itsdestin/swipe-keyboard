// The neural re-ranking (step 1b): the ranking rule with made-up scores, the fallbacks when the model or its
// library is missing, and (when the model file is present) that the real model prefers sensible words.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>

#include "swipekb/nlm.hpp"
#include "swipekb/recognizer.hpp"

using namespace swipekb;

namespace {
WordScore ws(float lp, float end = 0, int tokens = 1) { return {lp, end, tokens}; }
NlmWeights plain(float beta, int n = 5)
{
    NlmWeights w;
    w.n = n;
    w.beta = beta;
    w.per_token = false;   // plain sums keep the expected numbers easy to check by hand
    return w;
}
} // namespace

TEST(NlmOrder, ZeroWeightKeepsFirstPass)
{
    const auto o = nlmOrder({-1, -2, -3}, {3, 3, 3}, {ws(-9), ws(-1), ws(-1)}, plain(0));
    EXPECT_EQ(o, (std::vector<size_t>{0, 1, 2}));
}

TEST(NlmOrder, SentenceModelCanPromoteTheSecondGuess)
{
    // base: back -1, bach -1.5; neural: back -8, bach -4 -> totals -9 vs -5.5 with weight 1.
    const auto o = nlmOrder({-1, -1.5f}, {4, 4}, {ws(-8), ws(-4)}, plain(1));
    EXPECT_EQ(o, (std::vector<size_t>{1, 0}));
}

TEST(NlmOrder, OnlyTheFirstNAreReordered)
{
    // The third guess would win, but n = 2 keeps it out of reach (and out of the returned order).
    const auto o = nlmOrder({-1, -2, -3}, {3, 3, 3}, {ws(-5), ws(-5), ws(0)}, plain(1, 2));
    EXPECT_EQ(o, (std::vector<size_t>{0, 1}));
}

TEST(NlmOrder, TiesKeepTheFirstPassOrder)
{
    const auto o = nlmOrder({-1, -1, -1}, {3, 3, 3}, {ws(-2), ws(-2), ws(-2)}, plain(1));
    EXPECT_EQ(o, (std::vector<size_t>{0, 1, 2}));
}

TEST(NlmOrder, WordEndChanceCountsUnlessSwitchedOff)
{
    // "the" vs "they": the pieces favour "the" (-1 vs -2), but "the" rarely ends there in this context (-3).
    NlmWeights w = plain(1);
    EXPECT_EQ(nlmOrder({0, 0}, {3, 4}, {ws(-1, -3), ws(-2, -0.1f)}, w), (std::vector<size_t>{1, 0}));
    w.word_end = false;
    EXPECT_EQ(nlmOrder({0, 0}, {3, 4}, {ws(-1, -3), ws(-2, -0.1f)}, w), (std::vector<size_t>{0, 1}));
}

TEST(NlmOrder, PerTokenDividesByPieceCount)
{
    // -6 over 3 pieces = -2 per piece beats -3 over 1 piece only when normalised.
    NlmWeights w = plain(1);
    w.word_end = false;
    EXPECT_EQ(nlmOrder({0, 0}, {5, 5}, {ws(-3, 0, 1), ws(-6, 0, 3)}, w), (std::vector<size_t>{0, 1}));
    w.per_token = true;
    EXPECT_EQ(nlmOrder({0, 0}, {5, 5}, {ws(-3, 0, 1), ws(-6, 0, 3)}, w), (std::vector<size_t>{1, 0}));
}

TEST(NlmOrder, ShortWordsUseTheirOwnWeight)
{
    // "in" (2 letters) first, "on" second; the model prefers "on". With the short-word weight at 0 the
    // first pass stands; with the normal weight it flips.
    NlmWeights w = plain(1);
    EXPECT_EQ(nlmOrder({-1, -1.5f}, {2, 2}, {ws(-5), ws(-2)}, w), (std::vector<size_t>{1, 0}));
    w.beta_short = 0;
    EXPECT_EQ(nlmOrder({-1, -1.5f}, {2, 2}, {ws(-5), ws(-2)}, w), (std::vector<size_t>{0, 1}));
}

TEST(NlmOrder, ClearLeadIsNotOverruled)
{
    NlmWeights w = plain(1);
    w.margin = 2;
    // Lead 3 > margin 2: unchanged even though the model strongly prefers the second guess.
    EXPECT_EQ(nlmOrder({-1, -4}, {4, 4}, {ws(-20), ws(-1)}, w), (std::vector<size_t>{0, 1}));
    // Lead 1 <= margin: the model may re-order.
    EXPECT_EQ(nlmOrder({-1, -2}, {4, 4}, {ws(-20), ws(-1)}, w), (std::vector<size_t>{1, 0}));
}

TEST(NlmOrder, MissingScoresMeanNoChange)
{
    EXPECT_EQ(nlmOrder({-1, -2}, {3, 3}, {}, plain(1)), (std::vector<size_t>{}));
    EXPECT_EQ(nlmOrder({}, {}, {}, plain(1)), (std::vector<size_t>{}));
}

TEST(NeuralLMLoad, MissingOrDamagedModelGivesNothing)
{
    EXPECT_EQ(loadNeuralLM("/nonexistent/model.gguf"), nullptr);
    EXPECT_EQ(loadNeuralLM(""), nullptr);
    const std::string junk = ::testing::TempDir() + "/junk.gguf";
    std::ofstream(junk) << "this is not a model";
    EXPECT_EQ(loadNeuralLM(junk), nullptr);
    std::remove(junk.c_str());
}

// ---- inside the Recognizer (needs the downloaded swipe models) ----

namespace {
std::string assets()
{
    const char *d = std::getenv("SWIPEKB_ASSETS");
    return d ? d : "";
}
bool haveAssets() { return std::ifstream(ModelPaths::fromDir(assets()).encoder).good(); }

std::vector<KeyRect> qwerty()
{
    const char *rows[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    const float shift[3] = {0, 50, 150};
    std::vector<KeyRect> keys;
    for (int r = 0; r < 3; ++r)
        for (int i = 0; rows[r][i]; ++i)
            keys.push_back({rows[r][i], shift[r] + i * 100.f, r * 100.f, 100.f, 100.f});
    return keys;
}

std::vector<TracePoint> through(const std::string &word, const std::vector<KeyRect> &keys)
{
    auto center = [&](char c) {
        for (const auto &k : keys)
            if (k.letter == c) return std::pair{k.x + k.w / 2, k.y + k.h / 2};
        return std::pair{0.f, 0.f};
    };
    std::vector<TracePoint> out;
    float t = 0;
    for (size_t i = 0; i + 1 < word.size(); ++i) {
        auto [x0, y0] = center(word[i]);
        auto [x1, y1] = center(word[i + 1]);
        for (int s = 0; s < 12; ++s, t += 8)
            out.push_back({x0 + (x1 - x0) * s / 12.f, y0 + (y1 - y0) * s / 12.f, t});
    }
    auto [xe, ye] = center(word.back());
    out.push_back({xe, ye, t});
    return out;
}

// Gives a fixed score per word (default very unlikely); counts calls.
struct StubScorer : WordScorer {
    std::map<std::string, float> lp;
    bool fail = false;
    int *calls;
    explicit StubScorer(int *c) : calls(c) {}
    std::vector<WordScore> score(const std::vector<std::string> &, const std::vector<std::string> &words) override
    {
        ++*calls;
        if (fail) return {};
        std::vector<WordScore> out;
        for (const auto &w : words)
            out.push_back({lp.count(w) ? lp[w] : -30.f, 0, 1});
        return out;
    }
};
} // namespace

TEST(RecognizerNlm, OffByDefault)
{
    EXPECT_FALSE(RecognizerOptions{}.nlm);
}

TEST(RecognizerNlm, MissingModelKeepsTheOldRanking)
{
    if (!haveAssets()) GTEST_SKIP() << "run scripts/fetch-assets.sh";
    ModelPaths p = ModelPaths::fromDir(assets());
    p.nlm = "/nonexistent/en_nlm.gguf";
    RecognizerOptions on;
    on.nlm = true;
    Recognizer withFlag, plainR;
    ASSERT_TRUE(withFlag.init(p, on));
    ASSERT_TRUE(plainR.init(p));
    EXPECT_FALSE(withFlag.usingNlm());
    const auto keys = qwerty();
    for (const std::string w : {"hello", "world", "of"}) {
        const auto a = withFlag.recognize(through(w, keys), keys, {"the"}, 3);
        const auto b = plainR.recognize(through(w, keys), keys, {"the"}, 3);
        ASSERT_EQ(a.size(), b.size());
        for (size_t i = 0; i < a.size(); ++i)
            EXPECT_EQ(a[i].word, b[i].word);
    }
}

TEST(RecognizerNlm, StubScorerReordersOnlyTheTopAndKeepsTheShapeGuard)
{
    if (!haveAssets()) GTEST_SKIP() << "run scripts/fetch-assets.sh";
    Recognizer r;
    ASSERT_TRUE(r.init(ModelPaths::fromDir(assets())));
    const auto keys = qwerty();
    const auto trace = through("hello", keys);
    const auto before = r.recognize(trace, keys, {"say"}, 5);
    ASSERT_GE(before.size(), 3u);
    int calls = 0;
    auto stub = std::make_unique<StubScorer>(&calls);
    stub->lp[before[2].word] = 0;   // the stub loves the third guess
    r.setWordScorerForTesting(std::move(stub));
    ASSERT_TRUE(r.usingNlm());
    const auto after = r.recognize(trace, keys, {"say"}, 3);
    EXPECT_EQ(calls, 1);
    ASSERT_EQ(after.size(), 3u);
    EXPECT_EQ(after[0].word, before[2].word);
    // The best raw shape ("hello" for a clean swipe) stays visible.
    bool shown = false;
    for (const auto &c : after)
        shown = shown || c.word == "hello";
    EXPECT_TRUE(shown);
}

TEST(RecognizerNlm, ScorerFailureKeepsTheFirstPass)
{
    if (!haveAssets()) GTEST_SKIP() << "run scripts/fetch-assets.sh";
    Recognizer r;
    ASSERT_TRUE(r.init(ModelPaths::fromDir(assets())));
    const auto keys = qwerty();
    const auto trace = through("world", keys);
    const auto before = r.recognize(trace, keys, {"hello"}, 3);
    int calls = 0;
    auto stub = std::make_unique<StubScorer>(&calls);
    stub->fail = true;
    r.setWordScorerForTesting(std::move(stub));
    const auto after = r.recognize(trace, keys, {"hello"}, 3);
    ASSERT_EQ(after.size(), before.size());
    for (size_t i = 0; i < after.size(); ++i)
        EXPECT_EQ(after[i].word, before[i].word);
}

TEST(RealNeuralModel, PrefersTheWordThatFitsTheSentence)
{
    const std::string path = ModelPaths::fromDir(assets()).nlm;
    if (!std::ifstream(path).good()) GTEST_SKIP() << "no neural model at " << path;
    auto m = loadNeuralLM(path, {.threads = 2});
    ASSERT_NE(m, nullptr);
    const auto s = m->score({"I", "would", "like", "a", "cup", "of"}, {"tea", "tee", "toe"});
    ASSERT_EQ(s.size(), 3u);
    EXPECT_GT(s[0].lp, s[1].lp);
    EXPECT_GT(s[0].lp, s[2].lp);
    // Same context again (kept in memory) must give the same numbers as a fresh run.
    const auto again = m->score({"I", "would", "like", "a", "cup", "of"}, {"tea", "tee", "toe"});
    ASSERT_EQ(again.size(), 3u);
    EXPECT_NEAR(again[0].lp, s[0].lp, 1e-3);
    // Sentence start: scored capitalised, after "beginning of text".
    const auto first = m->score({}, {"the", "thx"});
    ASSERT_EQ(first.size(), 2u);
    EXPECT_GT(first[0].lp + first[0].word_end, first[1].lp + first[1].word_end);
}
