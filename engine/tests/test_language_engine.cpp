// Tests for swipekb::LanguageEngine: the one class the keyboard and the Gaming Mode daemon use for a language.
// Most cases build a tiny `.combined` dictionary, so they need no downloaded files. The swipe cases run on geometry
// alone (no model file) and, when SWIPEKB_ASSETS points at a full asset tree, also with the real English files.
#include <gtest/gtest.h>

#include <QTemporaryDir>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <tuple>

#include "swipekb/language_engine.hpp"

using namespace swipekb;

namespace {

std::vector<KeyRect> qwertyRects(float ox, float oy, float scale)
{
    const char *rows[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    const float shift[3] = {0, 50, 150};
    std::vector<KeyRect> keys;
    for (int r = 0; r < 3; ++r)
        for (int i = 0; rows[r][i]; ++i)
            keys.push_back({char32_t(rows[r][i]), ox + (shift[r] + i * 100.f) * scale, oy + r * 100.f * scale, 100.f * scale, 100.f * scale});
    return keys;
}

// A clean swipe: straight lines between the centres of the word's letters, 12 points per segment, 8 ms apart.
std::vector<TracePoint> traceThrough(const std::string &word, const std::vector<KeyRect> &keys)
{
    auto centre = [&](char c) {
        for (const auto &k : keys)
            if (k.letter == char32_t(c)) return std::pair{k.x + k.w / 2, k.y + k.h / 2};
        return std::pair{0.f, 0.f};
    };
    std::vector<TracePoint> out;
    float t = 0;
    for (size_t i = 0; i + 1 < word.size(); ++i) {
        auto [x0, y0] = centre(word[i]);
        auto [x1, y1] = centre(word[i + 1]);
        for (int s = 0; s < 12; ++s, t += 8) {
            const float f = s / 12.f;
            out.push_back({x0 + (x1 - x0) * f, y0 + (y1 - y0) * f, t});
        }
    }
    auto [xe, ye] = centre(word.back());
    out.push_back({xe, ye, t});
    return out;
}

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

namespace {
std::string assets() { const char *a = std::getenv("SWIPEKB_ASSETS"); return a ? a : ""; }
bool haveEnglish() { return std::ifstream(assets() + "/dict/en_swipe.combined").good(); }
LanguageEngineSetup english()
{
    LanguageEngineSetup s;
    s.dictionary = assets() + "/dict/en_swipe.combined";
    for (const auto *p : {"/language-packs/swipe-model.bin", "/language-files/swipe-model.bin"})
        if (std::ifstream(assets() + p).good()) { s.swipeModel = assets() + p; break; }
    for (const auto *p : {"/language-packs/en.ngram", "/language-files/en.ngram"})
        if (std::ifstream(assets() + p).good()) { s.ngram = assets() + p; break; }
    return s;
}
}

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
    EXPECT_EQ(e.complete("helo", 3).front().word, "hello");
}
