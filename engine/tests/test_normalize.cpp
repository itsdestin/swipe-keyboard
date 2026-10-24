// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "swipekb/text.hpp"

using namespace swipekb;

// 100 px keys in three staggered rows (row 2 shifted half a key, row 3 one and a half). The expected centres below
// are worked out by hand from this geometry (they equal scripts/swipe/layouts/qwerty.tsv).
static std::vector<KeyRect> qwertyRects(float ox, float oy, float scale = 1.f)
{
    const char *rows[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    const float shift[3] = {0, 50, 150};
    std::vector<KeyRect> keys;
    for (int r = 0; r < 3; ++r)
        for (int i = 0; rows[r][i]; ++i)
            keys.push_back({rows[r][i], ox + (shift[r] + i * 100.f) * scale, oy + r * 100.f * scale, 100.f * scale, 100.f * scale});
    return keys;
}

static int idx(char c) { return c - 'a'; }

TEST(Normalize, KeyCentersFollowTheGeometry)
{
    auto n = normalize({{0, 0, 0}, {1, 1, 5}}, qwertyRects(30, 400));
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->letters, "abcdefghijklmnopqrstuvwxyz");
    ASSERT_EQ(n->key_cx.size(), 26u);
    EXPECT_NEAR(n->key_cx[idx('q')], 0.05f, 1e-5);
    EXPECT_NEAR(n->key_cy[idx('q')], 1.f / 6, 1e-5);
    EXPECT_NEAR(n->key_cx[idx('a')], 0.10f, 1e-5);
    EXPECT_NEAR(n->key_cy[idx('a')], 0.5f, 1e-5);
    EXPECT_NEAR(n->key_cx[idx('m')], 0.80f, 1e-5);
    EXPECT_NEAR(n->key_cy[idx('m')], 5.f / 6, 1e-5);
    EXPECT_NEAR(n->key_cx[idx('p')], 0.95f, 1e-5);
}

TEST(Normalize, TraceUsesTheSameLetterAreaAndKeepsTime)
{
    // (80,450) is the center of q when the letter area starts at (30,400).
    auto n = normalize({{80, 450, 0}, {1030, 700, 12.5f}}, qwertyRects(30, 400));
    ASSERT_TRUE(n.has_value());
    ASSERT_EQ(n->x.size(), 2u);
    EXPECT_NEAR(n->x[0], 0.05f, 1e-5);
    EXPECT_NEAR(n->y[0], 1.f / 6, 1e-5);
    EXPECT_NEAR(n->x[1], 1.0f, 1e-5);
    EXPECT_NEAR(n->y[1], 1.0f, 1e-5);
    EXPECT_FLOAT_EQ(n->t[1], 12.5f);
}

TEST(Normalize, ScaleDoesNotChangeTheResult)
{
    auto a = normalize({{0, 0, 0}, {1, 1, 1}}, qwertyRects(0, 0, 1.f));
    auto b = normalize({{0, 0, 0}, {1, 1, 1}}, qwertyRects(0, 0, 1.7f));
    ASSERT_TRUE(a && b);
    for (int i = 0; i < 26; ++i) {
        EXPECT_NEAR(a->key_cx[i], b->key_cx[i], 1e-5);
        EXPECT_NEAR(a->key_cy[i], b->key_cy[i], 1e-5);
    }
}

TEST(Normalize, UpperCaseLettersAccepted)
{
    auto keys = qwertyRects(0, 0);
    for (auto &k : keys) k.letter = static_cast<char>(k.letter - 'a' + 'A');
    EXPECT_TRUE(normalize({{0, 0, 0}, {1, 1, 1}}, keys).has_value());
}

TEST(Normalize, RejectsMissingLetterOrTooShortTrace)
{
    auto keys = qwertyRects(0, 0);
    EXPECT_FALSE(normalize({{0, 0, 0}}, keys).has_value());
    keys.pop_back(); // drop 'm'
    EXPECT_FALSE(normalize({{0, 0, 0}, {1, 1, 1}}, keys).has_value());
}
