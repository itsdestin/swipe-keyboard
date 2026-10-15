// Tests for the n-gram model file + lookups (swipekb::NgramLM) and the re-ranking (swipekb::rerankCandidates).
// WHY a hand-made model: every number in it is known, so each back-off step can be checked exactly, and the
// tests run without the 150 MB real model.
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "swipekb/ngram.hpp"
#include "swipekb/recognizer.hpp"

using namespace swipekb;

namespace {

// Words: 3 = "i", 4 = "am", 5 = "an", 6 = "here".
constexpr uint32_t I = 3, AM = 4, AN = 5, HERE = 6, BOS = NgramLM::kBos;

NgramModelData tinyModel()
{
    NgramModelData d;
    d.words = {"i", "am", "an", "here"};
    d.uni_lp = {-2.0f, -99.f, -1.5f, -1.0f, -1.2f, -1.4f, -1.3f};   // <unk> <s> </s> i am an here
    d.uni_bo = {0.f, -0.3f, 0.f, -0.2f, -0.25f, 0.f, 0.f};
    // Bigrams (sorted by key): "<s> i", "i am", "i an", "am here".
    d.bi_keys = {NgramLM::key(BOS, I), NgramLM::key(I, AM), NgramLM::key(I, AN), NgramLM::key(AM, HERE)};
    d.bi_lp = {-0.5f, -0.4f, -2.5f, -0.9f};
    d.bi_bo = {-0.1f, -0.15f, 0.f, 0.f};
    // Trigrams: "<s> i am", "i am here".
    d.tri_keys = {NgramLM::key(BOS, I, AM), NgramLM::key(I, AM, HERE)};
    d.tri_lp = {-0.2f, -0.3f};
    d.calib_a = -6.f;
    d.calib_b = 0.01f;
    d.floor = -8.f;
    return d;
}

std::string tempPath(const char *name)
{
    return (testing::TempDir() + "/") + name;
}

class Ngram : public testing::Test {
protected:
    void SetUp() override
    {
        path = tempPath("tiny.ngram");
        ASSERT_TRUE(NgramLM::write(path, tinyModel()));
        ASSERT_TRUE(lm.load(path));
    }
    std::string path;
    NgramLM lm;
};

} // namespace

TEST(NgramAlpha, LowerCaseLettersOnly)
{
    EXPECT_EQ(NgramLM::alphaForm("Don't"), "dont");
    EXPECT_EQ(NgramLM::alphaForm("I"), "i");
    EXPECT_EQ(NgramLM::alphaForm("well-known,"), "wellknown");
    EXPECT_EQ(NgramLM::alphaForm("don’t"), "dont");   // curly apostrophe is punctuation too
    EXPECT_EQ(NgramLM::alphaForm("café"), "café");
    EXPECT_EQ(NgramLM::alphaForm("1984"), "");
}

TEST_F(Ngram, LoadsWordList)
{
    EXPECT_EQ(lm.order(), 3);
    EXPECT_EQ(lm.id("i"), I);
    EXPECT_EQ(lm.id("here"), HERE);
    EXPECT_EQ(lm.id("zebra"), NgramLM::kUnk);
}

TEST_F(Ngram, TrigramHit)
{
    // "I am" + "here": the trigram is stored, so its own number comes back.
    EXPECT_NEAR(lm.logProb(lm.context({"I", "am"}), "here", 0), -0.3f, 1e-3);
    // Sentence start: "<s> i am".
    EXPECT_NEAR(lm.logProb(lm.context({"I"}), "am", 0), -0.2f, 1e-3);
}

TEST_F(Ngram, BacksOffToBigramThenUnigram)
{
    // "<s> i an": no trigram -> back-off of "<s> i" (-0.1) + bigram "i an" (-2.5).
    EXPECT_NEAR(lm.logProb(lm.context({"I"}), "an", 0), -0.1f - 2.5f, 1e-3);
    // "<s> i here": no trigram, no bigram "i here" -> bo("<s> i") + bo("i") + P(here).
    EXPECT_NEAR(lm.logProb(lm.context({"I"}), "here", 0), -0.1f - 0.2f - 1.3f, 1e-3);
    // Context pair never seen ("here am"): no pair back-off, then bigram "am"? none from "am" to "am" ->
    // bo("am") + P(am).
    EXPECT_NEAR(lm.logProb(lm.context({"here", "am"}), "am", 0), -0.25f - 1.2f, 1e-3);
}

TEST_F(Ngram, EmptyContextMeansSentenceStart)
{
    // First word of a sentence: the "<s> i" bigram.
    EXPECT_NEAR(lm.logProb(lm.context({}), "i", 0), -0.5f, 1e-3);
    // A word that never starts a sentence: bo(<s>) + unigram.
    EXPECT_NEAR(lm.logProb(lm.context({}), "am", 0), -0.3f - 1.2f, 1e-3);
}

TEST_F(Ngram, UnknownContextWordIsUnk)
{
    // "zebra" isn't in the model -> treated as <unk>, which has no bigrams: bo(<unk>) (0) + P(am).
    EXPECT_NEAR(lm.logProb(lm.context({"zebra"}), "am", 0), -1.2f, 1e-3);
}

TEST_F(Ngram, UnknownCandidateUsesDictionaryFrequencyWithFloor)
{
    const auto c = lm.context({"I"});
    EXPECT_NEAR(lm.logProb(c, "zebra", 100.f), -6.f + 0.01f * 100.f, 1e-4);
    EXPECT_NEAR(lm.logProb(c, "zebra", -1000.f), -8.f, 1e-4);   // never below the floor
}

TEST_F(Ngram, ContextUsesOnlyTheLastTwoWords)
{
    EXPECT_NEAR(lm.logProb(lm.context({"zebra", "zebra", "I", "am"}), "here", 0), -0.3f, 1e-3);
}

TEST(NgramFile, MissingOrDamagedFilesFailToLoad)
{
    NgramLM lm;
    EXPECT_FALSE(lm.load("/nonexistent/en_ngram.bin"));
    EXPECT_FALSE(lm.loaded());

    const std::string junk = tempPath("junk.ngram");
    std::ofstream(junk) << "this is not a model file, just some text that is long enough to have a header";
    EXPECT_FALSE(lm.load(junk));

    // A good file cut short (an interrupted copy) must be rejected, not read past its end.
    const std::string good = tempPath("good.ngram"), cut = tempPath("cut.ngram");
    ASSERT_TRUE(NgramLM::write(good, tinyModel()));
    std::ifstream in(good, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    std::ofstream(cut, std::ios::binary) << bytes.substr(0, bytes.size() - 16);
    EXPECT_FALSE(lm.load(cut));
    EXPECT_FALSE(lm.loaded());
    // Lookups on an unloaded model are safe and return the floor.
    EXPECT_LT(lm.logProb(lm.context({"I"}), "am", 0), -5.f);
}

TEST(NgramFile, WriteRejectsInconsistentData)
{
    NgramModelData d = tinyModel();
    std::swap(d.bi_keys[0], d.bi_keys[1]);   // not sorted
    EXPECT_FALSE(NgramLM::write(tempPath("bad1.ngram"), d));

    d = tinyModel();
    d.tri_keys = {NgramLM::key(AN, AN, AN)};   // its pair "an an" is not a stored bigram
    d.tri_lp = {-1.f};
    EXPECT_FALSE(NgramLM::write(tempPath("bad2.ngram"), d));
}

// ---- re-ranking ----

namespace {
RawCandidate cand(const char *w, float ctc, float lm, float len = 3, float freq = 100)
{
    return {w, ctc, len, freq, lm, 0};
}
// Only the path score and the n-gram score count (no length or frequency terms), to keep the arithmetic plain.
RerankWeights plain(float alpha) { return {1.0f, 0.f, 0.f, alpha}; }
// shape / len^1 with len 3: the path score is ctc / 3.
} // namespace

TEST(Rerank, OrdersByPathScoreWithoutTheWordModel)
{
    const auto out = rerankCandidates({cand("an", -3, -1), cand("am", -1.5, -4), cand("as", -6, -2)}, plain(0), 3);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].word, "am");
    EXPECT_EQ(out[1].word, "an");
    EXPECT_EQ(out[2].word, "as");
}

TEST(Rerank, WordModelCanReorder)
{
    // "am" fits the path slightly better (-0.5 vs -1.0) but "an" is far likelier in context: with alpha 1 the
    // totals are am -0.5-4 = -4.5, an -1-1 = -2, as -2-2 = -4.
    const auto out = rerankCandidates({cand("am", -1.5, -4), cand("an", -3, -1), cand("as", -6, -2)}, plain(1), 3);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].word, "an");
    EXPECT_EQ(out[1].word, "as");
    EXPECT_EQ(out[2].word, "am");
    EXPECT_NEAR(out[0].score, -2.f, 1e-5);
}

TEST(Rerank, KeepsTopKAndFutosOrderOnTies)
{
    const auto out = rerankCandidates({cand("aa", -3, -1), cand("bb", -3, -1), cand("cc", -3, -1)}, plain(1), 2);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].word, "aa");
    EXPECT_EQ(out[1].word, "bb");
}

TEST(Rerank, BestRawShapeWordAlwaysStaysVisible)
{
    // "qi" matches the path best (ctc -0.3) but the word model hates it: it would rank last, yet it must take
    // the last visible slot so a clearly swiped rare word can still be picked.
    const auto out = rerankCandidates(
        {cand("qi", -0.3, -9), cand("so", -3, -1), cand("to", -3.3, -1), cand("go", -3.6, -1)}, plain(1), 3);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].word, "so");
    EXPECT_EQ(out[1].word, "to");
    EXPECT_EQ(out[2].word, "qi");
}

TEST(Rerank, GuardChangesNothingWhenAlreadyShown)
{
    const auto out = rerankCandidates({cand("so", -0.3, -1), cand("to", -3, -1), cand("go", -3.3, -1),
                                       cand("qi", -3.6, -1)},
                                      plain(1), 3);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].word, "so");
    EXPECT_EQ(out[1].word, "to");
    EXPECT_EQ(out[2].word, "go");
}

TEST(Rerank, EmptyPoolOrZeroTopK)
{
    EXPECT_TRUE(rerankCandidates({}, plain(1), 3).empty());
    EXPECT_TRUE(rerankCandidates({cand("so", -1, -1)}, plain(1), 0).empty());
}
