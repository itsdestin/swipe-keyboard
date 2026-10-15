#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

#include "swipekb/recognizer.hpp"

using namespace swipekb;

static std::vector<KeyRect> qwertyRects(float ox, float oy, float scale)
{
    const char *rows[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    const float shift[3] = {0, 50, 150};
    std::vector<KeyRect> keys;
    for (int r = 0; r < 3; ++r)
        for (int i = 0; rows[r][i]; ++i)
            keys.push_back({rows[r][i], ox + (shift[r] + i * 100.f) * scale, oy + r * 100.f * scale, 100.f * scale, 100.f * scale});
    return keys;
}

// A clean swipe: straight lines between the centers of the word's letters, 12 points per segment, 8 ms apart.
static std::vector<TracePoint> traceThrough(const std::string &word, const std::vector<KeyRect> &keys)
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
        for (int s = 0; s < 12; ++s, t += 8) {
            const float f = s / 12.f;
            out.push_back({x0 + (x1 - x0) * f, y0 + (y1 - y0) * f, t});
        }
    }
    auto [xe, ye] = center(word.back());
    out.push_back({xe, ye, t});
    return out;
}

static std::string assets() { const char *d = std::getenv("SWIPEKB_ASSETS"); return d ? d : ""; }
static bool haveAssets() { return std::ifstream(ModelPaths::fromDir(assets()).encoder).good(); }

static bool inTop(const std::vector<Candidate> &c, const std::string &w)
{
    return std::any_of(c.begin(), c.end(), [&](const Candidate &x) {
        std::string lower = x.word;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return std::tolower(ch); });
        return lower == w;
    });
}

TEST(Recognizer, NotReadyReturnsNothing)
{
    Recognizer r;
    EXPECT_FALSE(r.ready());
    auto keys = qwertyRects(0, 0, 1);
    EXPECT_TRUE(r.recognize(traceThrough("hello", keys), keys, {}, 3).empty());
}

TEST(Recognizer, MissingEncoderFailsInit)
{
    if (!haveAssets()) GTEST_SKIP() << "run scripts/fetch-assets.sh";
    ModelPaths p = ModelPaths::fromDir(assets());
    p.encoder = "/nonexistent/model.pte";
    Recognizer r;
    EXPECT_FALSE(r.init(p));
    EXPECT_FALSE(r.ready());
}

TEST(Recognizer, CleanSwipesOfCommonWordsLandInTopThree)
{
    if (!haveAssets()) GTEST_SKIP() << "run scripts/fetch-assets.sh";
    Recognizer r;
    ASSERT_TRUE(r.init(ModelPaths::fromDir(assets())));
    auto keys = qwertyRects(0, 0, 1);
    for (const std::string w : {"hello", "the", "keyboard", "swipe", "world"}) {
        auto c = r.recognize(traceThrough(w, keys), keys, {}, 3);
        ASSERT_FALSE(c.empty()) << w;
        EXPECT_LE(c.size(), 3u);
        EXPECT_TRUE(inTop(c, w)) << w << " -> first guess " << c[0].word;
    }
}

TEST(Recognizer, DifferentKeyboardSizeAndPositionStillWorks)
{
    if (!haveAssets()) GTEST_SKIP() << "run scripts/fetch-assets.sh";
    Recognizer r;
    ASSERT_TRUE(r.init(ModelPaths::fromDir(assets())));
    auto small = qwertyRects(0, 0, 1);
    auto big = qwertyRects(200, 900, 1.6f);
    EXPECT_TRUE(inTop(r.recognize(traceThrough("hello", small), small, {}, 3), "hello"));
    EXPECT_TRUE(inTop(r.recognize(traceThrough("hello", big), big, {}, 3), "hello"));
}

// ---- the n-gram ranking and its fallbacks ----

static RecognizerOptions withNgram()
{
    RecognizerOptions o;
    o.ngram = true;
    return o;
}

TEST(Recognizer, NgramOffByDefault)
{
    // WHY a test: the ship decision (bench/history.tsv, step 1) left it off; flipping the default must be a
    // deliberate, visible change.
    EXPECT_FALSE(RecognizerOptions{}.ngram);
}

TEST(Recognizer, MissingNgramModelFallsBackToTheOldRanking)
{
    if (!haveAssets()) GTEST_SKIP() << "run scripts/fetch-assets.sh";
    ModelPaths p = ModelPaths::fromDir(assets());
    p.ngram = "/nonexistent/en_ngram.bin";
    Recognizer r;
    ASSERT_TRUE(r.init(p, withNgram()));
    EXPECT_FALSE(r.usingNgram());
    Recognizer old;
    ASSERT_TRUE(old.init(ModelPaths::fromDir(assets()), {}));
    auto keys = qwertyRects(0, 0, 1);
    const std::vector<std::string> ctx = {"I", "would", "like"};
    const auto a = r.recognize(traceThrough("hello", keys), keys, ctx, 3);
    const auto b = old.recognize(traceThrough("hello", keys), keys, ctx, 3);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i)
        EXPECT_EQ(a[i].word, b[i].word);
}

TEST(Recognizer, DamagedNgramModelFallsBackToTheOldRanking)
{
    if (!haveAssets()) GTEST_SKIP() << "run scripts/fetch-assets.sh";
    const std::string junk = testing::TempDir() + "/damaged.ngram";
    std::ofstream(junk) << std::string(4096, 'x');
    ModelPaths p = ModelPaths::fromDir(assets());
    p.ngram = junk;
    Recognizer r;
    ASSERT_TRUE(r.init(p, withNgram()));
    EXPECT_FALSE(r.usingNgram());
    auto keys = qwertyRects(0, 0, 1);
    EXPECT_TRUE(inTop(r.recognize(traceThrough("hello", keys), keys, {}, 3), "hello"));
}

TEST(Recognizer, NgramRankingFindsCleanSwipes)
{
    if (!haveAssets()) GTEST_SKIP() << "run scripts/fetch-assets.sh";
    const ModelPaths p = ModelPaths::fromDir(assets());
    if (!std::ifstream(p.ngram).good()) GTEST_SKIP() << "run scripts/lm/build-lm.sh";
    Recognizer r;
    ASSERT_TRUE(r.init(p, withNgram()));
    EXPECT_TRUE(r.usingNgram());
    auto keys = qwertyRects(0, 0, 1);
    for (const std::string w : {"hello", "the", "keyboard", "swipe", "world"}) {
        const auto c = r.recognize(traceThrough(w, keys), keys, {"this", "is", "a"}, 3);
        ASSERT_FALSE(c.empty()) << w;
        EXPECT_LE(c.size(), 3u);
        EXPECT_TRUE(inTop(c, w)) << w << " -> first guess " << c[0].word;
    }
    // The tuning aid returns the whole unranked pool with the model's scores filled in.
    const auto pool = r.recognizeDetailed(traceThrough("hello", keys), keys, {"say"});
    EXPECT_GT(pool.size(), 3u);
    EXPECT_TRUE(std::any_of(pool.begin(), pool.end(), [](const RawCandidate &c) { return c.lm < 0; }));
}
