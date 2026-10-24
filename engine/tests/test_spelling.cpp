// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "swipekb/spellings.hpp"

using swipekb::Capitals;
using swipekb::Spellings;
using V = std::vector<std::string>;

// A tiny dictionary in AOSP's .combined layout, with the cases that matter (frequencies as in the shipped one).
static const char *kDict =
    "dictionary=main:en_us,locale=en_US,description=test,date=1,version=54\n"
    " word=I'm,f=116,flags=,originalFreq=116\n"
    " word=im,f=0,not_a_word=true\n"                       // AOSP's autocorrect-only entry: not a spelling
    " word=let's,f=145,flags=\n"
    " word=lets,f=106,flags=\n"
    " word=its,f=163,flags=\n"
    " word=it's,f=162,flags=\n"
    " word=was,f=201,flags=\n"
    " word=wa's,f=50,flags=\n"                             // far rarer: never offered
    " word=whats,f=53,flags=nonword\n"
    " word=what's,f=154,flags=\n"
    " word=don't,f=185,flags=\n"
    " word=US$,f=97,flags=\n"                              // not letters: never typed
    " word=us,f=160,flags=\n"
    " word=states',f=200,flags=\n"                         // trailing apostrophe: never typed
    " word=states,f=151,flags=\n"
    " word=London,f=150,flags=\n"
    " word=full-time,f=113,flags=\n"
    " word=fulltime,f=80,flags=\n"
    "  shortcut=let's,f=whitelist\n";

TEST(Spellings, PutsApostrophesBackWithoutCapitals)
{
    const Spellings s = Spellings::fromCombinedText(kDict);
    EXPECT_EQ(s.spell("im"), "i'm");      // lower case here; Capitals (below) adds "I'm" (owner decision 2026-10-23)
    EXPECT_EQ(s.spell("lets"), "let's");
    EXPECT_EQ(s.spell("dont"), "don't");
    EXPECT_EQ(s.spell("whats"), "what's");
    EXPECT_EQ(s.spell("fulltime"), "full-time");
}

TEST(Spellings, RestoresVerifiedLigaturesAndSharpSFromPhysicalAliases)
{
    const auto fr=Spellings::fromCombinedText(" word=cœur,f=152\n word=cour,f=149\n word=œuvre,f=147\n", "fr");
    EXPECT_EQ(fr.spell("cour"), "cœur");
    EXPECT_EQ(fr.spell("coeur"), "cœur");
    EXPECT_EQ(fr.spell("oeuvre"), "œuvre");
    EXPECT_EQ(fr.others("cœur"), V{"cour"}); // ambiguity remains selectable, not erased
    const auto de=Spellings::fromCombinedText(" word=Straße,f=156\n word=Fußball,f=140\n", "de");
    EXPECT_EQ(de.spell("strasse"), "Straße");
    EXPECT_EQ(de.spell("strase"), "Straße");
    EXPECT_EQ(de.spell("fussball"), "Fußball");
    EXPECT_EQ(Spellings::fromCombinedText(" word=masse,f=140\n", "de").spell("masse"), "masse");
}

TEST(Spellings, PlainWordsStayPlain)
{
    const Spellings s = Spellings::fromCombinedText(kDict);
    EXPECT_EQ(s.spell("its"), "its");
    EXPECT_EQ(s.spell("was"), "was");
    EXPECT_EQ(s.spell("us"), "us");
    EXPECT_EQ(s.spell("states"), "states");
    EXPECT_EQ(s.spell("london"), "london");   // capitals are the keyboard's business, not ours
    EXPECT_EQ(s.spell("hello"), "hello");     // not in the dictionary at all: unchanged
}

TEST(Spellings, OtherSpellingsForTheGuessBar)
{
    const Spellings s = Spellings::fromCombinedText(kDict);
    EXPECT_EQ(s.others("its"), V{"it's"});
    EXPECT_EQ(s.others("it's"), V{"its"});
    EXPECT_EQ(s.others("let's"), V{"lets"});
    EXPECT_EQ(s.others("Let's"), V{"lets"});   // capitalized at a sentence start: still the same word
    EXPECT_EQ(s.others("i'm"), V{});           // "im" is not a word
    EXPECT_EQ(s.others("was"), V{});           // "wa's" too rare to offer
    EXPECT_EQ(s.others("states"), V{});
    EXPECT_EQ(s.others("hello"), V{});
}

TEST(Spellings, MissingFileIsEmpty)
{
    EXPECT_TRUE(Spellings::fromCombinedFile("/nonexistent.combined").empty());
}

// WHY: the list decides the shown capitals of en/es/fr words; anything not listed is lower case, a malformed line can
// never turn one word into another, and an empty list means "leave words as they are" (German).
TEST(Capitals, ListedWordsGetTheirFormOthersLowerCase)
{
    const auto caps = Capitals::fromTsv("# capitals language=en\ni\tI\t0.96\ni'm\tI'm\nparis\tParis\niphone\tiPhone\n"
                                        "nasa\tNASA\nbank\tBanco\n\nbad line\n");
    ASSERT_NE(caps, nullptr);
    EXPECT_EQ(caps->size(), 5u);   // "bank -> Banco" is not the same word: skipped
    EXPECT_EQ(caps->apply("i"), "I");
    EXPECT_EQ(caps->apply("I'M"), "I'm");
    EXPECT_EQ(caps->apply("i\u2019m"), "I\u2019m");   // keeps the word's own apostrophe
    EXPECT_EQ(caps->apply("Paris"), "Paris");
    EXPECT_EQ(caps->apply("IPHONE"), "iPhone");
    EXPECT_EQ(caps->apply("nasa"), "NASA");
    EXPECT_EQ(caps->apply("Hall"), "hall");
    EXPECT_EQ(caps->apply("bank"), "bank");
    EXPECT_EQ(Capitals::fromTsv("# nothing\n"), nullptr);
}
