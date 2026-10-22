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
