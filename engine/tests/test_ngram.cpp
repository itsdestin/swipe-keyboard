// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Tests for the n-gram model file + lookups (swipekb::NgramLM).
// WHY a hand-made model: every number in it is known, so each back-off step can be checked exactly, and the
// tests run without the 150 MB real model.
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "swipekb/ngram.hpp"

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
    EXPECT_EQ(NgramLM::alphaForm("Straße"), "straße"); // Lowercase preserves ß as the model token, unlike full case folding.
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

TEST(NgramApostrophes, FormKeepsInnerApostrophesOnly)
{
    EXPECT_EQ(NgramLM::apostropheForm("We’re"), "we're");   // curly written straight
    EXPECT_EQ(NgramLM::apostropheForm("don't,"), "don't");
    EXPECT_EQ(NgramLM::apostropheForm("'tis"), "tis");
    EXPECT_EQ(NgramLM::apostropheForm("states'"), "states");
    EXPECT_EQ(NgramLM::apostropheForm("l’homme"), "l'homme");
    EXPECT_EQ(NgramLM::apostropheForm("well-known"), "wellknown");
}

// WHY: a pack built with --keep-apostrophes is told apart by its word list alone, so older packs load unchanged and
// keep matching letters only, while the new ones keep "were" and "we're" apart.
TEST(NgramApostrophes, DetectedFromTheWordListAndMatchedByForm)
{
    NgramModelData d;
    d.words = {"don't", "we're", "were", "yes", "they"};   // 3..7
    d.uni_lp = {-2.f, -99.f, -1.5f, -1.f, -1.6f, -1.2f, -1.3f, -1.4f};
    d.uni_bo = std::vector<float>(8, 0.f);
    d.bi_keys = {NgramLM::key(6, 4), NgramLM::key(7, 5)};   // "yes we're", "they were" (sorted by key)
    d.bi_lp = {-0.1f, -0.1f};
    d.bi_bo = {0.f, 0.f};
    const auto path = tempPath("apostrophes.ngram");
    ASSERT_TRUE(NgramLM::write(path, d));
    NgramLM lm;
    ASSERT_TRUE(lm.load(path));
    EXPECT_TRUE(lm.apostrophes());
    EXPECT_EQ(lm.form("We’re"), "we're");
    EXPECT_EQ(lm.id("we're"), 4u);
    EXPECT_EQ(lm.id("were"), 5u);
    EXPECT_EQ(lm.id("dont"), 3u);    // typed without the apostrophe, the model has no "dont": its "don't"
    EXPECT_EQ(lm.id("zzz"), NgramLM::kUnk);
    EXPECT_GT(lm.logProb(lm.context({"yes"}), 4), lm.logProb(lm.context({"yes"}), 5));
    EXPECT_GT(lm.logProb(lm.context({"They"}), 5), lm.logProb(lm.context({"They"}), 4));

    NgramLM plain;
    ASSERT_TRUE(NgramLM::write(path, tinyModel()));
    ASSERT_TRUE(plain.load(path));
    EXPECT_FALSE(plain.apostrophes());
    EXPECT_EQ(plain.form("I'm"), "im");
}
