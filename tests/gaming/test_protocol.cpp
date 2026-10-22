// Unit tests for the daemon's request validation (gaming/daemon/protocol.hpp), no sockets or models.
#include <gtest/gtest.h>
#include <QJsonArray>
#include <QJsonDocument>
#include "protocol.hpp"
using namespace gamingd;

static QJsonObject obj(const char *json) { return QJsonDocument::fromJson(json).object(); }

TEST(Protocol, BodyNeedsObjectVersionOneAndStringOp)
{
    EXPECT_STREQ(parseBody("nope").error, kErrBadJson);
    EXPECT_STREQ(parseBody("[1]").error, kErrBadJson);
    EXPECT_STREQ(parseBody(R"({"v":2,"op":"ping"})").error, kErrVersion);
    EXPECT_STREQ(parseBody(R"({"v":1,"op":5})").error, kErrBadRequest);
    EXPECT_TRUE(parseBody(R"({"v":1,"op":"ping"})").value);
}

TEST(Protocol, FramesRoundTripAndOversizeIsRefused)
{
    const auto f = encodeFrame("abc");
    ASSERT_EQ(f.size(), 7);
    EXPECT_EQ(uchar(f[3]), 3);
    EXPECT_TRUE(encodeFrame(QByteArray(kMaxFrame + 1, 'x')).isEmpty());
}

TEST(Protocol, SecretHeuristicHitsPasswordsNotWords)
{
    EXPECT_TRUE(looksLikeSecret("Xk9#mQ2$vLp"));
    EXPECT_TRUE(looksLikeSecret("Passw0rd!"));
    EXPECT_TRUE(looksLikeSecret("bcdfghjklmnpqrstvw"));
    for (const char *w : {"hello", "don't", "Beautiful", "well-known", "naïve", "I'm"}) EXPECT_FALSE(looksLikeSecret(w)) << w;
}

TEST(Protocol, UnknownModeDropsSecretContextAndCapsLength)
{
    auto a = parseSwipe(obj(R"({"points":[[0,0,0],[1,1,5]],"context":["Xk9#mQ2$vLp","the"]})"), Mode::Unknown);
    ASSERT_TRUE(a.value);
    EXPECT_TRUE(a.value->contextDropped);
    EXPECT_TRUE(a.value->context.empty());
    auto b = parseSwipe(obj(R"({"points":[[0,0,0],[1,1,5]],"context":["Xk9#mQ2$vLp"]})"), Mode::Plain);
    EXPECT_FALSE(b.value->contextDropped);
    QJsonArray many;
    for (int i = 0; i < 13; ++i) many.append("a");
    QJsonObject r{{"points", QJsonArray{QJsonArray{0, 0, 0}, QJsonArray{1, 1, 1}}}, {"context", many}};
    EXPECT_FALSE(parseSwipe(r, Mode::Unknown).value);
    EXPECT_TRUE(parseSwipe(r, Mode::Plain).value);
}

TEST(Protocol, LayoutRejectsDuplicatesBadSizesAndNonLetters)
{
    EXPECT_TRUE(parseLayout(obj(R"({"keys":[{"c":"Q","x":0,"y":0,"w":10,"h":10}]})")).value);
    EXPECT_EQ(parseLayout(obj(R"({"keys":[{"c":"Q","x":0,"y":0,"w":10,"h":10}]})")).value->at(0).letter, U'q');
    EXPECT_FALSE(parseLayout(obj(R"({"keys":[{"c":"q","x":0,"y":0,"w":10,"h":10},{"c":"Q","x":5,"y":0,"w":10,"h":10}]})")).value);
    EXPECT_FALSE(parseLayout(obj(R"({"keys":[{"c":"1","x":0,"y":0,"w":10,"h":10}]})")).value);
    EXPECT_FALSE(parseLayout(obj(R"({"keys":[{"c":"q","x":0,"y":0,"w":-1,"h":10}]})")).value);
}

// ---- Milestone M7 (multilingual) -------------------------------------------------------------------------------

TEST(ProtocolM7, LayoutRequestNamesSteamsLayoutAndDefaultsToEnglish)
{
    EXPECT_EQ(*parseSteamLayout(obj(R"({"keys":[]})")).value, "qwerty");   // older callers never sent one
    EXPECT_EQ(*parseSteamLayout(obj(R"({"steam_layout":"french"})")).value, "french");
    EXPECT_EQ(*parseSteamLayout(obj(R"({"steam_layout":"tchinese_bopomofo"})")).value, "tchinese_bopomofo");
    for (const char *bad : {R"({"steam_layout":5})", R"({"steam_layout":""})", R"({"steam_layout":"Fr ench"})",
                            R"({"steam_layout":"../x"})", R"({"steam_layout":"french\n"})", R"({"steam_layout":null})"})
        EXPECT_STREQ(parseSteamLayout(obj(bad)).error, kErrBadRequest) << bad;
    EXPECT_STREQ(parseSteamLayout(obj(("{\"steam_layout\":\"" + std::string(49, 'a') + "\"}").c_str())).error, kErrBadRequest);
}

TEST(ProtocolM7, WordsAreTakenAsComposedUnicodeSoDecomposedLettersRoundTrip)
{
    // n + combining tilde, u + combining diaeresis: some keyboards send these. The engine knows only composed letters.
    auto a = parseNextWords(obj("{\"words\":[\"ni\\u006e\\u0303o\",\"u\\u0308ber\"],\"partial\":\"se\\u0301\"}"), Mode::Plain);
    ASSERT_TRUE(a.value);
    EXPECT_EQ(a.value->words[0], "ni\xC3\xB1o");   // niño
    EXPECT_EQ(a.value->words[1], "\xC3\xBC" "ber"); // über
    EXPECT_EQ(a.value->partial, "s\xC3\xA9");       // sé
}

TEST(ProtocolM7, LayoutAcceptsAccentedKeysEvenWhenSentDecomposed)
{
    auto l = parseLayout(obj("{\"keys\":[{\"c\":\"\\u00D1\",\"x\":0,\"y\":0,\"w\":10,\"h\":10},"
                             "{\"c\":\"n\\u0303\",\"x\":20,\"y\":0,\"w\":10,\"h\":10}]}"));
    EXPECT_FALSE(l.value);   // Ñ and ñ (decomposed) are the same key: a duplicate
    auto one = parseLayout(obj("{\"keys\":[{\"c\":\"n\\u0303\",\"x\":0,\"y\":0,\"w\":10,\"h\":10},"
                               "{\"c\":\"\\u00DF\",\"x\":20,\"y\":0,\"w\":10,\"h\":10}]}"));
    ASSERT_TRUE(one.value);
    EXPECT_EQ(one.value->at(0).letter, U'\u00F1');
    EXPECT_EQ(one.value->at(1).letter, U'\u00DF');
}

TEST(ProtocolM7, RealWordsInEverySupportedLanguageAreNotMistakenForSecrets)
{
    for (const char *w : {"Überraschung", "Straße", "Größenwahn", "pingüino", "niño", "Cómo", "français", "cœur", "l\xE2\x80\x99" "école",
                          "Krankenhäuser", "Ärgernis", "naïveté", "Schmetterlingsflügel", "murciélago"})
        EXPECT_FALSE(looksLikeSecret(w)) << w;
    // 17 letters with only accented vowels is still language, not a token (the old check counted ASCII vowels only).
    EXPECT_FALSE(looksLikeSecret("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"));
    EXPECT_TRUE(looksLikeSecret("Xk9#mQ2$vLp"));   // and real secrets are still caught
}

TEST(ProtocolM7, NewErrorCodesAreStableWireStrings)
{
    EXPECT_STREQ(kErrUnsupported, "unsupported");
    EXPECT_STREQ(kErrLanguageOff, "language_not_enabled");
    EXPECT_STREQ(kErrLanguageUnavailable, "language_unavailable");
}
