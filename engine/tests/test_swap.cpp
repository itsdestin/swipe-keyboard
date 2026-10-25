// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Tests for the swap ranking function and the synthetic finger path.
#include <gtest/gtest.h>

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
