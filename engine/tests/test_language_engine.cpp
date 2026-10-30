// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Tests for swipekb::LanguageEngine: the one class the keyboard and the Gaming Mode daemon use for a language.
// Most cases build a tiny `.combined` dictionary, so they need no downloaded files. The swipe cases run on geometry
// alone (no model file) and, when SWIPEKB_ASSETS points at a full asset tree, also with the real English files.
#include <limits>
#include <gtest/gtest.h>

#include <QTemporaryDir>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <tuple>

#include "swipekb/language_engine.hpp"
#include "swipekb/ngram.hpp"
#include "testkeys.h"

using namespace swipekb;

namespace {

// qwertyRects / traceThrough live in testkeys.h (shared with test_swap).
using namespace swipekb::testkeys;

std::vector<std::string> words(const std::vector<Candidate> &c)
{
    std::vector<std::string> out;
    for (const auto &x : c) out.push_back(x.word);
    return out;
}

bool has(const std::vector<Candidate> &c, const std::string &w)
{
    const auto v = words(c);
    return std::find(v.begin(), v.end(), w) != v.end();
}

long at(const std::vector<std::string> &v, const char *w) { return std::find(v.begin(), v.end(), w) - v.begin(); }

// Writes a `.combined` file: (word, frequency, flags).
struct Dict {
    QTemporaryDir dir;
    std::string path;
    Dict(const std::vector<std::tuple<std::string, int, std::string>> &entries, const std::string &locale = "en")
    {
        path = dir.filePath("tiny.combined").toStdString();
        std::ofstream out(path);
        out << "dictionary=fixture,locale=" << locale << "\n";
        for (const auto &[w, f, flags] : entries) out << " word=" << w << ",f=" << f << ",flags=" << flags << ",originalFreq=" << f << "\n";
    }
};

LanguageEngine *open(LanguageEngine &e, const Dict &d, const std::string &language = "en")
{
    LanguageEngineSetup s;
    s.language = language;
    s.dictionary = d.path;
    return e.init(s) ? &e : nullptr;
}

const std::vector<std::tuple<std::string, int, std::string>> kEnglish = {
    {"hello", 120, ""}, {"help", 149, ""}, {"held", 158, ""}, {"hell", 108, ""}, {"helot", 52, ""}, {"hero", 126, ""},
    {"the", 222, ""}, {"teh", 0, "nonword"}, {"sea", 120, ""}, {"see", 150, ""}, {"sew", 90, ""}, {"set", 180, ""},
    {"seat", 130, ""}, {"search", 250, ""}, {"season", 250, ""}, {"sleep", 100, ""}, {"a", 200, ""},
    {"its", 163, ""}, {"it's", 162, ""}, {"let's", 145, ""}, {"lets", 106, ""}, {"don't", 185, ""},
    {"shit", 93, "offensive"}, {"world", 190, ""}, {"keyboard", 110, ""}, {"swipe", 100, ""},
    {"rn", 0, "nonword"}, {"pls", 0, "hand-added"}, {"us$", 50, ""}, {"The", 30, ""}};

} // namespace

TEST(LanguageEngine, NotReadyAnswersNothing)
{
    LanguageEngine e;
    EXPECT_FALSE(e.ready());
    const auto keys = qwertyRects(0, 0, 1);
    EXPECT_TRUE(e.recognize(traceThrough("hello", keys), keys, {}, 3).empty());
    EXPECT_TRUE(e.complete("hel").empty());
    EXPECT_TRUE(e.similar("sea").empty());
    EXPECT_FALSE(e.hasWord("hello"));
}

TEST(LanguageEngine, MissingOrEmptyDictionaryFailsInit)
{
    LanguageEngine e;
    LanguageEngineSetup s;
    s.dictionary = "/nonexistent/words.combined";
    EXPECT_FALSE(e.init(s));
    Dict onlyNonWords({{"rn", 0, "nonword"}});
    EXPECT_EQ(open(e, onlyNonWords), nullptr);
    EXPECT_FALSE(e.ready());
}

TEST(LanguageEngine, CompletesTypedLettersAndCorrectsOneSlip)
{
    Dict d(kEnglish);
    LanguageEngine e;
    ASSERT_TRUE(open(e, d));
    const auto hel = e.complete("hel", 12);
    EXPECT_EQ(words(hel).front(), "held");   // most frequent completion first
    EXPECT_TRUE(has(hel, "hello"));
    // Never a word shorter than what was typed (found on SteamOS: "he" once outranked every completion of "hel").
    for (const auto &c : hel) EXPECT_GE(c.word.size(), 3u) << c.word;
    // A forgotten double letter beats an exact but rare completion and other one-letter-off words.
    EXPECT_EQ(e.complete("helo", 12).front().word, "hello");
    // Swapped letters: "teh" is marked not-a-word, "the" is the correction.
    EXPECT_EQ(e.complete("teh", 3).front().word, "the");
    // Apostrophes are not typed: "dont" completes to the dictionary spelling.
    EXPECT_EQ(e.complete("dont", 3).front().word, "don't");
    EXPECT_TRUE(e.complete("", 3).empty());
    EXPECT_TRUE(e.complete(std::string(49, 'a'), 3).empty());
    // Ill-formed dictionary lines never appear; capitals collapse onto the more frequent spelling.
    EXPECT_FALSE(e.hasWord("us"));
    EXPECT_FALSE(has(e.complete("th", 12), "The"));
}

TEST(LanguageEngine, SimilarGivesWholeWordsOneEditAway)
{
    Dict d(kEnglish);
    LanguageEngine e;
    ASSERT_TRUE(open(e, d));
    const auto sea = words(e.similar("sea", 12));
    for (const char *want : {"see", "sew", "set", "seat"}) EXPECT_NE(std::find(sea.begin(), sea.end(), want), sea.end()) << want;
    for (const char *never : {"sea", "search", "season", "sleep"}) EXPECT_EQ(std::find(sea.begin(), sea.end(), never), sea.end()) << never;
    EXPECT_EQ(sea.front(), "set");   // most frequent neighbour first
    const auto swapped = e.similar("teh", 5);
    ASSERT_FALSE(swapped.empty());
    EXPECT_EQ(swapped.front().word, "the");
    // The word's own other spelling is not a "similar" word (otherSpellings offers it).
    EXPECT_FALSE(has(e.similar("its", 12), "it's"));
    EXPECT_FALSE(e.similar("SEA", 5).empty());
    EXPECT_TRUE(e.similar("a", 5).empty());
    EXPECT_TRUE(e.similar("", 5).empty());
    EXPECT_TRUE(e.similar(std::string(49, 'a'), 5).empty());
}

TEST(LanguageEngine, OtherSpellingsDisplayAndOffensiveFlags)
{
    Dict d(kEnglish);
    LanguageEngine e;
    ASSERT_TRUE(open(e, d));
    EXPECT_EQ(e.otherSpellings("its"), std::vector<std::string>{"it's"});
    EXPECT_EQ(e.otherSpellings("let's"), std::vector<std::string>{"lets"});
    EXPECT_EQ(e.displaySpelling("dont"), "don't");
    EXPECT_EQ(e.displaySpelling("unknownword"), "unknownword");
    EXPECT_TRUE(e.isOffensive("shit"));
    EXPECT_TRUE(e.isOffensive("Shit"));
    EXPECT_FALSE(e.isOffensive("set"));
}

TEST(LanguageEngine, LiftWordsBringsInTextingWordsAndOnlyEverRaises)
{
    Dict d(kEnglish);
    LanguageEngine e;
    ASSERT_TRUE(open(e, d));
    EXPECT_FALSE(has(e.complete("lm", 12), "lmao"));
    EXPECT_FALSE(e.hasWord("rn"));
    e.liftWords({{"lmao", 100}, {"rn", 90}, {"pls", 80}, {"wyd", 85}, {"bad1", 50}, {"don't", 50}});   // digits/apostrophes skipped
    EXPECT_TRUE(has(e.complete("lma", 12), "lmao"));
    EXPECT_TRUE(has(e.complete("wy", 12), "wyd"));
    EXPECT_TRUE(e.hasWord("rn"));
    EXPECT_TRUE(e.hasWord("pls"));
    EXPECT_FALSE(e.hasWord("bad1"));
    // Only raises: see 150 stays ahead of sew 90 and set 180 ahead of both.
    e.liftWords({{"see", 10}, {"set", 20}, {"the", 5}});
    const auto order = words(e.complete("se", 12));
    EXPECT_LT(at(order, "set"), at(order, "see"));
    EXPECT_LT(at(order, "see"), at(order, "sew"));
    // A weaker lift on an already lifted word changes nothing.
    e.liftWords({{"wyd", 5}});
    EXPECT_TRUE(has(e.complete("wy", 12), "wyd"));
}

TEST(LanguageEngine, LearnedWordsAreReplacedNotAccumulated)
{
    Dict d(kEnglish);
    LanguageEngine e;
    ASSERT_TRUE(open(e, d));
    e.setLearnedWords({{"destin", 120}, {"see", 10}});
    EXPECT_TRUE(e.hasWord("destin"));
    EXPECT_TRUE(has(e.complete("dest", 3), "destin"));
    // A learned frequency overrides the dictionary's (see now trails sew) ...
    auto order = words(e.complete("se", 12));
    EXPECT_GT(at(order, "see"), at(order, "sew"));
    // ... and a new set replaces the old one: forgotten words are gone, "see" gets its own frequency back.
    e.setLearnedWords({});
    EXPECT_FALSE(e.hasWord("destin"));
    order = words(e.complete("se", 12));
    EXPECT_LT(at(order, "see"), at(order, "sew"));
}

TEST(LanguageEngine, CompletionKeepsEachLanguagesDisplayForms)
{
    const std::vector<std::tuple<std::string, std::string, std::string, std::string>> cases = {
        {"en", "hello", "hel", "hello"}, {"es", "niño", "ni", "niño"}, {"fr", "cœur", "cœ", "cœur"}, {"de", "Straße", "stra", "Straße"}};
    for (const auto &[language, word, prefix, display] : cases) {
        Dict d({{word, 200, ""}}, language);
        LanguageEngine e;
        ASSERT_TRUE(open(e, d, language)) << language;
        const auto c = e.complete(prefix, 3);
        ASSERT_FALSE(c.empty()) << language;
        EXPECT_EQ(c.front().word, display);
    }
    // German keeps noun capitals in swipes and completion; other languages are lower-cased.
    Dict en({{"Paris", 150, ""}});
    LanguageEngine e;
    ASSERT_TRUE(open(e, en, "en"));
    EXPECT_EQ(e.complete("par", 1).front().word, "paris");
}

// WHY: the capitals list (owner decision 2026-10-23) shows "I", "I'll", "Paris" with their capital in swipes,
// completion, other spellings and tap-to-swap words, while a word the list leaves out ("Hall" is the dictionary's most
// frequent form, but "the hall" is lower case) stays lower case. German ignores it and keeps its own capitals.
TEST(LanguageEngine, CapitalsListGivesNamesAndITheirCapital)
{
    const auto caps = Capitals::fromTsv("i\tI\ni'll\tI'll\nparis\tParis\nhaus\tHAUS\n");
    Dict d({{"I", 196, ""}, {"I'll", 164, ""}, {"ill", 120, ""}, {"Paris", 150, ""}, {"Hall", 134, ""}, {"hall", 133, ""},
            {"halls", 100, ""}, {"hello", 120, ""}, {"park", 140, ""}});
    LanguageEngine e;
    ASSERT_TRUE(open(e, d));
    EXPECT_EQ(e.complete("par", 3).front().word, "paris");   // without the list: all lower case, as before
    e.setCapitals(caps);
    EXPECT_TRUE(has(e.complete("par", 3), "Paris"));
    EXPECT_EQ(e.complete("hal", 1).front().word, "hall");
    EXPECT_EQ(e.displaySpelling("i"), "I");
    EXPECT_EQ(e.otherSpellings("ill"), std::vector<std::string>{"I'll"});
    EXPECT_TRUE(has(e.similar("hell", 12), "hall"));
    const auto keys = qwertyRects(0, 0, 1);
    EXPECT_TRUE(has(e.recognize(traceThrough("paris", keys), keys, {}, 3), "Paris"));
    e.setCapitals(nullptr);
    EXPECT_EQ(e.displaySpelling("paris"), "paris");

    Dict de({{"Haus", 200, ""}, {"haus", 10, ""}}, "de");
    LanguageEngine g;
    ASSERT_TRUE(open(g, de, "de"));
    g.setCapitals(caps);
    EXPECT_EQ(g.displaySpelling("haus"), "Haus");
}

TEST(LanguageEngine, GeometryOnlySwipesFindCleanWordsAtAnySize)
{
    Dict d(kEnglish);
    LanguageEngine e;
    ASSERT_TRUE(open(e, d));
    EXPECT_FALSE(e.hasModel());
    EXPECT_STREQ(e.mode(), "own-geometry");
    EXPECT_FALSE(e.usingNgram());
    for (const auto &keys : {qwertyRects(0, 0, 1), qwertyRects(200, 900, 1.6f)})
        for (const std::string w : {"hello", "world", "keyboard", "swipe"}) {
            const auto c = e.recognize(traceThrough(w, keys), keys, {}, 3);
            ASSERT_FALSE(c.empty()) << w;
            EXPECT_LE(c.size(), 3u);
            EXPECT_TRUE(has(c, w)) << w << " -> first guess " << c[0].word;
        }
    EXPECT_FALSE(e.bestShapeWord().empty());
    // Taught words become swipeable at the next swipe.
    const auto keys = qwertyRects(0, 0, 1);
    EXPECT_FALSE(has(e.recognize(traceThrough("qwerty", keys), keys, {}, 3), "qwerty"));
    e.setLearnedWords({{"qwerty", 150}});
    EXPECT_TRUE(has(e.recognize(traceThrough("qwerty", keys), keys, {}, 3), "qwerty"));
}

TEST(LanguageEngine, BadInputAndDamagedPartsNeverBreakSwiping)
{
    Dict d(kEnglish);
    QTemporaryDir dir;
    const std::string junk = dir.filePath("junk.bin").toStdString();
    std::ofstream(junk) << std::string(4096, 'x');
    LanguageEngine e;
    LanguageEngineSetup s;
    s.dictionary = d.path;
    s.swipeModel = junk;
    s.ngram = junk;
    ASSERT_TRUE(e.init(s));
    EXPECT_FALSE(e.hasModel());
    EXPECT_FALSE(e.usingNgram());
    auto keys = qwertyRects(0, 0, 1);
    EXPECT_TRUE(has(e.recognize(traceThrough("hello", keys), keys, {}, 3), "hello"));
    EXPECT_TRUE(e.recognize({{1, 1, 0}}, keys, {}, 3).empty());                       // one point
    EXPECT_TRUE(e.recognize(traceThrough("hello", keys), {}, {}, 3).empty());         // no keys
    auto nan = traceThrough("hello", keys);
    nan[3].x = std::nanf("");
    EXPECT_TRUE(e.recognize(nan, keys, {}, 3).empty());
    keys[0].w = -5;
    EXPECT_TRUE(e.recognize(traceThrough("hello", qwertyRects(0, 0, 1)), keys, {}, 3).empty());
}

// ---- with the real English files (SWIPEKB_ASSETS = an asset tree from scripts/fetch-assets.py) ----

// assets() / haveEnglish() / english() live in testkeys.h (shared with test_swap).

TEST(LanguageEngineEnglish, CleanSwipesOfCommonWordsLandInTopThree)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    LanguageEngine e;
    ASSERT_TRUE(e.init(english()));
    const auto keys = qwertyRects(0, 0, 1), big = qwertyRects(200, 900, 1.6f);
    for (const std::string w : {"hello", "the", "keyboard", "swipe", "world"}) {
        EXPECT_TRUE(has(e.recognize(traceThrough(w, keys), keys, {"this", "is", "a"}, 3), w)) << w;
        EXPECT_TRUE(has(e.recognize(traceThrough(w, big), big, {}, 3), w)) << w;
    }
    // Swipes carry no apostrophes; the dictionary's spelling is what comes out.
    EXPECT_TRUE(has(e.recognize(traceThrough("dont", keys), keys, {}, 3), "don't"));
    // WHY exact first guesses: "I's", "wa's" and "year's" share these paths, and once won every tie (a check on the
    // letters alone could not see it). The apostrophe spelling is still offered as the other spelling.
    for (const std::string w : {"is", "in", "it", "was", "as", "years", "does", "students"})
        EXPECT_EQ(e.recognize(traceThrough(w, keys), keys, {}, 3).front().word, w) << w;
    // WHY with a sentence: with an apostrophe-aware pack, at the very start of a sentence "We're"/"We'll" are the
    // likelier words (by design); mid-sentence the plain word must still come out.
    for (const auto &[w, before] : std::vector<std::pair<std::string, std::vector<std::string>>>{
             {"were", {"they"}}, {"well", {"as"}}, {"its", {"the", "dog", "wagged"}}})
        EXPECT_EQ(e.recognize(traceThrough(w, keys), keys, before, 3).front().word, w) << w;
    EXPECT_EQ(e.recognize(traceThrough("dont", keys), keys, {}, 3).front().word, "don't");
    EXPECT_EQ(e.otherSpellings("were").front(), "we're");
    EXPECT_EQ(e.complete("helo", 3).front().word, "hello");
}

// WHY: with a pack that keeps apostrophes, the sentence decides between spellings of one finger path ("yes, we're" /
// "they were"), while junk far rarer than the common word ("I's" for "is") still never wins.
TEST(LanguageEngineEnglish, ApostrophePackLetsTheSentenceChooseTheSpelling)
{
    if (!haveEnglish()) GTEST_SKIP() << "set SWIPEKB_ASSETS to a fetched asset tree";
    NgramModelData d;
    d.words = {"i's", "is", "it's", "its", "they", "we're", "were", "yes"};   // 3..10, sorted like the pack builder
    d.uni_lp = {-2.f, -99.f, -1.5f, -3.f, -1.3f, -1.4f, -1.6f, -1.5f, -1.4f, -1.6f, -1.6f};
    d.uni_bo = std::vector<float>(11, 0.f);
    // "they were", "yes I's" (made likely on purpose: "I's" is 121 points rarer than "is" in the dictionary, so it
    // must not be a swipe word at all), "yes we're".
    d.bi_keys = {NgramLM::key(7, 9), NgramLM::key(10, 3), NgramLM::key(10, 8)};
    d.bi_lp = {-0.1f, -0.05f, -0.1f};
    d.bi_bo = {0.f, 0.f, 0.f};
    d.calib_a = -6.f;
    d.calib_b = 0.01f;
    QTemporaryDir dir;
    auto s = english();
    s.ngram = dir.filePath("en.ngram").toStdString();
    ASSERT_TRUE(NgramLM::write(s.ngram, d));
    if (s.swipeModel.empty()) GTEST_SKIP() << "the swipe model is not in SWIPEKB_ASSETS";
    LanguageEngine e;
    ASSERT_TRUE(e.init(s));
    const auto keys = qwertyRects(0, 0, 1);
    EXPECT_EQ(e.recognize(traceThrough("were", keys), keys, {"yes"}, 3).front().word, "we're");
    EXPECT_EQ(e.recognize(traceThrough("were", keys), keys, {"they"}, 3).front().word, "were");
    EXPECT_EQ(e.recognize(traceThrough("is", keys), keys, {"yes"}, 3).front().word, "is");
}

// ContextRescore (joint-model experiment): off unless a scorer is set; with one, the neural score can move a found
// word to first place; a broken scorer, or no words before (by default), leaves the guesses exactly as they were.
TEST(LanguageEngine, ContextRescoreIsOffByDefaultAndNeverBreaksASwipe)
{
    Dict d(kEnglish);
    LanguageEngine e;
    ASSERT_TRUE(open(e, d));
    const auto keys = qwertyRects(0, 0, 1);
    const auto trace = traceThrough("help", keys);
    const std::vector<std::string> before{"please"};
    const auto plain = words(e.recognize(trace, keys, before, 3));
    ASSERT_FALSE(plain.empty());
    ASSERT_EQ(plain.front(), "help");
    // The search's own list starts with the same first guess.
    const auto raw = e.rawCandidates(trace, keys, before, 30);
    ASSERT_FALSE(raw.empty());
    EXPECT_EQ(raw.front().word, "help");
    // Pick a found word other than the first and make the scorer favour it strongly.
    std::string other;
    for (const auto &c : raw) if (c.word != "help") { other = c.word; break; }
    ASSERT_FALSE(other.empty());
    ContextRescore r;
    r.neuralWeight = 100.f;
    r.scorer = [&](const std::vector<std::string> &, const std::vector<std::string> &w) {
        std::vector<float> s;
        for (const auto &x : w) s.push_back(x == other ? 0.f : -10.f);
        return s;
    };
    e.setContextRescore(r);
    EXPECT_EQ(e.recognize(trace, keys, before, 3).front().word, other);
    // No words before: not re-scored (needWordsBefore), the guesses are as without it.
    e.setContextRescore({});
    const auto plainNoContext = words(e.recognize(trace, keys, {}, 3));
    e.setContextRescore(r);
    EXPECT_EQ(words(e.recognize(trace, keys, {}, 3)), plainNoContext);
    // A scorer that throws or answers for the wrong number of words changes nothing.
    r.scorer = [](const std::vector<std::string> &, const std::vector<std::string> &) -> std::vector<float> { throw 1; };
    e.setContextRescore(r);
    EXPECT_EQ(words(e.recognize(trace, keys, before, 3)), plain);
    r.scorer = [](const std::vector<std::string> &, const std::vector<std::string> &) { return std::vector<float>{0.f}; };
    e.setContextRescore(r);
    EXPECT_EQ(words(e.recognize(trace, keys, before, 3)), plain);
    // Neutral weights (no neural, all of the n-gram prior) change nothing either.
    r.neuralWeight = 0.f;
    r.scorer = [](const std::vector<std::string> &, const std::vector<std::string> &w) { return std::vector<float>(w.size(), -3.f); };
    e.setContextRescore(r);
    EXPECT_EQ(words(e.recognize(trace, keys, before, 3)), plain);
}

// Joint-model step 2: a word the next-word vocabulary lacks (NaN from the scorer, e.g. a word the person taught) takes
// its own n-gram prior in the neural score's place. With the English weights (neural 1, n-gram 0) every such word keeps
// exactly the score the search gave it, so "no opinion on any word" changes nothing.
TEST(LanguageEngine, ContextRescoreKeepsWordsTheScorerDoesNotKnow)
{
    Dict d(kEnglish);
    LanguageEngine e;
    ASSERT_TRUE(open(e, d));
    const auto keys = qwertyRects(0, 0, 1);
    const auto trace = traceThrough("help", keys);
    const std::vector<std::string> before{"please"};
    const auto plain = words(e.recognize(trace, keys, before, 3));
    ASSERT_FALSE(plain.empty());
    auto r = ContextRescore::forLanguage("en");
    // WHY no learned model here: this checks the fixed weights' fallback (the learned one has its own test below).
    r.reranker = nullptr;
    r.scorer = [](const std::vector<std::string> &, const std::vector<std::string> &w) {
        return std::vector<float>(w.size(), std::numeric_limits<float>::quiet_NaN());
    };
    e.setContextRescore(r);
    EXPECT_EQ(words(e.recognize(trace, keys, before, 3)), plain);
    // Known words still move: one unknown word (NaN) beside a scorer that strongly favours another found word.
    const auto raw = e.rawCandidates(trace, keys, before, 30);
    std::string other;
    for (const auto &c : raw) if (c.word != "help") { other = c.word; break; }
    ASSERT_FALSE(other.empty());
    r.scorer = [&](const std::vector<std::string> &, const std::vector<std::string> &w) {
        std::vector<float> s;
        for (const auto &x : w) s.push_back(x == other ? 50.f : x == "help" ? std::numeric_limits<float>::quiet_NaN() : -10.f);
        return s;
    };
    r.neuralWeight = 100.f;
    e.setContextRescore(r);
    EXPECT_EQ(e.recognize(trace, keys, before, 3).front().word, other);
}

// Joint-model step 3: the learned re-ranker (SwipeReranker). A hand-written model file: every feature has mean 0 and
// scale 1, and only the weights given are non-zero.
static std::string rerankerText(const std::vector<std::pair<int, float>> &sentence, const std::vector<std::pair<int, float>> &noSentence)
{
    std::string t = "swipekb-rerank 1\nfeatures " + std::to_string(kRrCount) + "\n";
    const auto set = [&](const char *name, const std::vector<std::pair<int, float>> &w) {
        std::vector<float> lin(kRrCount, 0.f);
        for (const auto &[i, v] : w) lin[size_t(i)] = v;
        t += std::string("set ") + name + " hidden 0\nmean";
        for (int i = 0; i < kRrCount; ++i) t += " 0";
        t += "\nscale";
        for (int i = 0; i < kRrCount; ++i) t += " 1";
        t += "\nlin";
        for (float v : lin) t += " " + std::to_string(v);
        t += "\n";
    };
    if (!sentence.empty()) set("sentence", sentence);
    if (!noSentence.empty()) set("nosentence", noSentence);
    return t + "end\n";
}

static std::shared_ptr<const SwipeReranker> reranker(const std::string &text)
{
    auto r = std::make_shared<SwipeReranker>();
    EXPECT_TRUE(r->parse(text));
    return r;
}

// The file format: a damaged file switches the model off; the score is lin . z + out . relu(W z + b).
TEST(SwipeReranker, ReadsOnlyCompleteFilesAndScoresAsWritten)
{
    SwipeReranker r;
    EXPECT_FALSE(r.parse(""));
    EXPECT_FALSE(r.ready());
    EXPECT_TRUE(r.parse(rerankerText({{kRrPath, 2.f}}, {})));
    EXPECT_TRUE(r.has(SwipeReranker::WithSentence));
    EXPECT_FALSE(r.has(SwipeReranker::NoSentence));
    RerankRow f{};
    f[kRrPath] = -1.5f;
    EXPECT_FLOAT_EQ(r.score(SwipeReranker::WithSentence, f), -3.f);
    // A value that is not a number counts as the mean: it says nothing.
    f[kRrPath] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(r.score(SwipeReranker::WithSentence, f), 0.f);
    // Wrong feature count, a NaN weight, a zero scale, a truncated file: refused.
    std::string bad = rerankerText({{kRrPath, 2.f}}, {});
    EXPECT_FALSE(r.parse(std::string(bad).replace(bad.find("features ") + 9, 2, "99")));
    EXPECT_FALSE(r.ready());
    EXPECT_FALSE(r.parse(std::string(bad).replace(bad.find(" 2.000000"), 9, " nan")));
    EXPECT_FALSE(r.parse(std::string(bad).replace(bad.find("scale 1"), 7, "scale 0")));
    EXPECT_FALSE(r.parse(bad.substr(0, bad.size() / 2)));
    // One hidden unit: relu(1 * z_path + 0.5) * 2, plus nothing linear.
    std::string h = "swipekb-rerank 1\nfeatures " + std::to_string(kRrCount) + "\nset nosentence hidden 1\nmean";
    for (int i = 0; i < kRrCount; ++i) h += " 0";
    h += "\nscale";
    for (int i = 0; i < kRrCount; ++i) h += " 1";
    h += "\nlin";
    for (int i = 0; i < kRrCount; ++i) h += " 0";
    h += "\nw1";
    for (int i = 0; i < kRrCount; ++i) h += i == kRrPath ? " 1" : " 0";
    h += "\nb1 0.5\nout 2\nend\n";
    ASSERT_TRUE(r.parse(h));
    RerankRow g{};
    g[kRrPath] = 1.f;
    EXPECT_FLOAT_EQ(r.score(SwipeReranker::NoSentence, g), 3.f);
    g[kRrPath] = -2.f;
    EXPECT_FLOAT_EQ(r.score(SwipeReranker::NoSentence, g), 0.f);
}

// With a re-ranker, a swipe with no words before is re-ordered by its "no sentence" weights (no scorer needed); one
// with words before uses the "sentence" weights only when the scorer answers, and otherwise keeps the search's order.
TEST(LanguageEngine, LearnedRerankerOrdersBothKindsOfSwipeAndNeverBreaksOne)
{
    Dict d(kEnglish);
    LanguageEngine e;
    ASSERT_TRUE(open(e, d));
    const auto keys = qwertyRects(0, 0, 1);
    const auto trace = traceThrough("help", keys);
    const std::vector<std::string> before{"please"};
    const auto plain = words(e.recognize(trace, keys, before, 3));
    const auto plainNoContext = words(e.recognize(trace, keys, {}, 3));
    const auto raw = e.rawCandidates(trace, keys, {}, 30);
    ASSERT_GE(raw.size(), 2u);
    // "No sentence" weights that prefer the search's LAST word: it comes first.
    ContextRescore r;
    r.reranker = reranker(rerankerText({}, {{kRrRankLog, 100.f}}));
    e.setContextRescore(r);
    EXPECT_EQ(e.recognize(trace, keys, {}, 3).front().word, raw.back().word);
    // Words before but no "sentence" weights and no scorer: as found.
    EXPECT_EQ(words(e.recognize(trace, keys, before, 3)), plain);
    // The features recognize() uses are the ones the training script reads.
    const auto cands = e.rerankCandidates(trace, keys, {});
    ASSERT_EQ(cands.size(), raw.size());
    EXPECT_EQ(cands.front().word, raw.front().word);
    EXPECT_EQ(cands.front().features[kRrFirst], 1.f);
    EXPECT_EQ(cands[1].features[kRrFirst], 0.f);
    for (const auto &c : cands)
        for (float v : c.features) EXPECT_TRUE(std::isfinite(v));
    // "Sentence" weights that follow the neural score: the word the scorer favours comes first.
    std::string other;
    for (const auto &c : e.rawCandidates(trace, keys, before, 30)) if (c.word != "help") { other = c.word; break; }
    ASSERT_FALSE(other.empty());
    r.reranker = reranker(rerankerText({{kRrNeural, 100.f}}, {}));
    r.scorer = [&](const std::vector<std::string> &, const std::vector<std::string> &w) {
        std::vector<float> s;
        for (const auto &x : w) s.push_back(x == other ? 0.f : -10.f);
        return s;
    };
    e.setContextRescore(r);
    EXPECT_EQ(e.recognize(trace, keys, before, 3).front().word, other);
    // No "no sentence" weights: a swipe without words before is as found (and the scorer is not asked).
    EXPECT_EQ(words(e.recognize(trace, keys, {}, 3)), plainNoContext);
    // A scorer that throws or answers short: as found.
    r.scorer = [](const std::vector<std::string> &, const std::vector<std::string> &) -> std::vector<float> { throw 1; };
    e.setContextRescore(r);
    EXPECT_EQ(words(e.recognize(trace, keys, before, 3)), plain);
    r.scorer = [](const std::vector<std::string> &, const std::vector<std::string> &) { return std::vector<float>{0.f}; };
    e.setContextRescore(r);
    EXPECT_EQ(words(e.recognize(trace, keys, before, 3)), plain);
    // A word the scorer has no opinion on (NaN, e.g. a learned word) is not read as "very unlikely": with every other
    // word scored very low it stays first.
    r.scorer = [](const std::vector<std::string> &, const std::vector<std::string> &w) {
        std::vector<float> s;
        for (const auto &x : w) s.push_back(x == "help" ? std::numeric_limits<float>::quiet_NaN() : -12.f);
        return s;
    };
    e.setContextRescore(r);
    EXPECT_EQ(e.recognize(trace, keys, before, 3).front().word, "help");
}

// The weights compiled into the engine, when there are any, read without error and hold both sets.
TEST(SwipeReranker, BuiltInWeightsAreComplete)
{
    for (const char *lang : {"en", "es", "fr", "de"}) {
        const auto r = SwipeReranker::builtIn(lang);
        if (!r) continue;
        EXPECT_TRUE(r->has(SwipeReranker::WithSentence)) << lang;
        EXPECT_TRUE(r->has(SwipeReranker::NoSentence)) << lang;
    }
}
