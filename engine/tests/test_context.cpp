#include <gtest/gtest.h>

#include "swipekb/recognizer.hpp"

using swipekb::contextWords;
using V = std::vector<std::string>;

// Mirrors plugin/tests/tst_swipesession.cpp (the same rules, now shared) plus the cases Qt used to handle for us.
TEST(ContextWords, BasicRules)
{
    EXPECT_EQ(contextWords(""), V{});
    EXPECT_EQ(contextWords("I am going"), (V{"I", "am", "going"}));
    EXPECT_EQ(contextWords("going to "), (V{"going", "to"}));
    EXPECT_EQ(contextWords("well, \"really\""), (V{"well", "really"}));
    EXPECT_EQ(contextWords("Done. I am"), (V{"I", "am"}));
    EXPECT_EQ(contextWords("Why? Because"), (V{"Because"}));
    EXPECT_EQ(contextWords("Done."), V{});
}

TEST(ContextWords, KeepsLastMaxOnly)
{
    EXPECT_EQ(contextWords("w1 w2 w3 w4 w5", 3), (V{"w3", "w4", "w5"}));
    EXPECT_EQ(contextWords("w1 w2", 0), V{});
}

TEST(ContextWords, InnerPunctuationAndSymbolsStay)
{
    EXPECT_EQ(contextWords("don't (stop) $5 a+b"), (V{"don't", "stop", "$5", "a+b"}));
    EXPECT_EQ(contextWords("-- ... !"), V{});
}

TEST(ContextWords, Utf8AndUnicodeSpace)
{
    EXPECT_EQ(contextWords("caf\xC3\xA9 \xE2\x80\x9Cna\xC3\xAFve\xE2\x80\x9D"), (V{"caf\xC3\xA9", "na\xC3\xAFve"}));
    EXPECT_EQ(contextWords("a\xC2\xA0" "b\n\tc"), (V{"a", "b", "c"}));
    EXPECT_EQ(contextWords("bad \xFF byte"), (V{"bad", "\xEF\xBF\xBD", "byte"}));
}
