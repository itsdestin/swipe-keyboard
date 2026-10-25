// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Pure rules behind "tap a word, see alternatives, swap it" (card #187): no keyboard, no Qt Virtual Keyboard.
#include <QtTest>
#include "wordswap.h"

class WordSwapTest : public QObject {
    Q_OBJECT
private slots:
    void wordUnderTheCursor_data() {
        QTest::addColumn<QString>("text"); QTest::addColumn<int>("cursor"); QTest::addColumn<QString>("word"); QTest::addColumn<int>("start");
        QTest::newRow("inside") << "the sea is blue" << 6 << "sea" << 4;
        QTest::newRow("just after") << "the sea is blue" << 7 << "sea" << 4;
        QTest::newRow("just before") << "the sea is blue" << 4 << "sea" << 4;
        QTest::newRow("start of text") << "sea is blue" << 0 << "sea" << 0;
        QTest::newRow("end of text") << "the sea" << 7 << "sea" << 4;
        QTest::newRow("before punctuation") << "the sea, is" << 7 << "sea" << 4;
        QTest::newRow("apostrophe") << "I don't know" << 5 << "don't" << 2;
        QTest::newRow("curly apostrophe") << QString::fromUtf8("I don\xe2\x80\x99t know") << 4 << QString::fromUtf8("don\xe2\x80\x99t") << 2;
        QTest::newRow("hyphen") << "a well-known fact" << 8 << "well-known" << 2;
        QTest::newRow("after wins over before") << "ab cd" << 2 << "ab" << 0;
        QTest::newRow("accented") << QString::fromUtf8("el niño va") << 6 << QString::fromUtf8("niño") << 3;
    }
    void wordUnderTheCursor() {
        QFETCH(QString, text); QFETCH(int, cursor); QFETCH(QString, word); QFETCH(int, start);
        const auto span = WordSwap::wordAt(text, cursor);
        QVERIFY(span.has_value());
        QCOMPARE(span->word, word); QCOMPARE(span->start, start); QCOMPARE(span->length, int(word.size()));
    }
    void noWordInSpaceOrPunctuation_data() {
        QTest::addColumn<QString>("text"); QTest::addColumn<int>("cursor");
        QTest::newRow("empty") << "" << 0;
        QTest::newRow("between spaces") << "a  b" << 2;
        QTest::newRow("punctuation both sides") << "a , b" << 3;
        QTest::newRow("lone apostrophe") << "a ' b" << 3;
        QTest::newRow("digits") << "ab 123 cd" << 5;
        QTest::newRow("cursor past end") << "abc" << 9;
        QTest::newRow("negative") << "abc" << -1;
    }
    void noWordInSpaceOrPunctuation() {
        QFETCH(QString, text); QFETCH(int, cursor);
        QVERIFY(!WordSwap::wordAt(text, cursor).has_value());
    }
    void veryLongWordIsIgnored() { QVERIFY(!WordSwap::wordAt(QString(80, u'a'), 10).has_value()); }

    void capitalizationPatternIsKept() {
        QCOMPARE(WordSwap::matchCase("Sea", "see"), QString("See"));
        QCOMPARE(WordSwap::matchCase("SEA", "see"), QString("SEE"));
        QCOMPARE(WordSwap::matchCase("sea", "see"), QString("see"));
        QCOMPARE(WordSwap::matchCase("A", "an"), QString("An"));
        QCOMPARE(WordSwap::matchCase("sea", "Haus"), QString("Haus")); // dictionary spelling (German nouns) is kept
        QCOMPARE(WordSwap::matchCase("McDonald", "mcdonalds"), QString("mcdonalds")); // mixed case: as spelled
    }

    void alternativesNeverIncludeTheWordItselfAndFillTheMiddleWithTheBest() {
        const auto out = WordSwap::build("Sea", {"sea", "see", "sew"}, {"sea", "SEE", "set", "say"}, {});
        QVERIFY(!out.contains("Sea", Qt::CaseInsensitive));
        QCOMPARE(out.size(), 3);
        // swipe candidates take at most two places, then the best neighbour; strongest goes in the middle
        QCOMPARE(out, (QStringList{"Sew", "See", "Set"}));
    }
    void swipeCandidatesLeaveRoomForOneNeighbour() {
        const auto out = WordSwap::build("sea", {"a", "b", "c", "d"}, {"x"}, {});
        QCOMPARE(out, (QStringList{"b", "a", "x"}));
    }
    void sentenceRankingOnlyReordersNeighbours() {
        const QStringList neighbours{"set", "see", "sew", "say"};
        QCOMPARE(WordSwap::build("sea", {}, neighbours, {}), (QStringList{"see", "set", "sew"}));
        QCOMPARE(WordSwap::build("sea", {}, neighbours, {"see", "say", "unrelated", "sew"}), (QStringList{"say", "see", "sew"}));
        // a ranked word that is not a neighbour is never introduced
        QVERIFY(!WordSwap::build("sea", {}, neighbours, {"unrelated"}).contains("unrelated"));
    }
    void fewerThanThreeStaysShortAndNothingGivesNothing() {
        QCOMPARE(WordSwap::build("sea", {}, {"see"}, {}), (QStringList{"see"}));
        QCOMPARE(WordSwap::build("sea", {}, {"sea", "SEA"}, {}), QStringList());
        QCOMPARE(WordSwap::build("sea", {}, {}, {}), QStringList());
    }

    void tapIsACursorMoveWithUnchangedText() {
        CursorMoveTracker t;
        QVERIFY(!t.observe("hello world", 11, 0));        // first look is only a baseline
        QVERIFY(!t.observe("hello world", 11, 10));       // nothing moved
        QVERIFY(t.observe("hello world", 3, 20));         // cursor moved, text same: a tap
        QVERIFY(!t.observe("hello world!", 3, 30));       // text changed: not a tap
        t.clear();
        QVERIFY(!t.observe("hello world", 3, 40));        // new field: baseline again
    }
    void ownTypingIsNeverATap() {
        CursorMoveTracker t;
        t.observe("hello ", 6, 0);
        t.ownEdit(100);
        QVERIFY(!t.observe("hello ", 6, 110));
        QVERIFY(!t.observe("hello a", 6, 120));           // text arrives first ...
        QVERIFY(!t.observe("hello a", 7, 130));           // ... cursor after it: still the same edit
        QVERIFY(t.observe("hello a", 2, 400));            // 250 ms after the text: a later move is a tap
    }
    void ownEditWhoseTextNeverArrivesStopsBlockingAfterASecond() {
        CursorMoveTracker t;
        t.observe("hello", 5, 0);
        t.ownEdit(100);
        QVERIFY(!t.observe("hello", 2, 600));
        QVERIFY(t.observe("hello", 1, 1200));
    }

    // WHY a trace: tap-to-swap re-scores the remembered finger path against each alternative (one brain), so the
    // memory keeps the path and the key layout it was drawn on, not only the guesses of the moment.
    static std::vector<swipekb::TracePoint> trace3() { return {{10, 20, 0}, {30, 22, 40}, {55, 25, 90}}; }
    static std::vector<swipekb::KeyRect> keys2() { return {{U's', 0, 0, 20, 30}, {U'e', 20, 0, 20, 30}}; }
    void recentSwipesAreAnchoredToTheirText() {
        RecentSwipes m;
        m.remember("I can ", "see", trace3(), keys2(), {"see", "sea", "sew"});
        const auto got = m.traceFor("I can ", "see");
        QVERIFY(got.has_value());
        QCOMPARE(got->first.size(), size_t(3));
        QCOMPARE(got->first[1].x, 30.f);
        QCOMPARE(got->first[2].t_ms, 90.f);
        QCOMPARE(got->second.size(), size_t(2));
        QCOMPARE(got->second[1].letter, U'e');
        QCOMPARE(m.candidatesFor("I can ", "see"), (QStringList{"see", "sea", "sew"}));
        QVERIFY(!m.traceFor("You can ", "see").has_value());   // same word, different place
        QVERIFY(!m.traceFor("I can ", "sea").has_value());     // different word
        QVERIFY(m.candidatesFor("You can ", "see").isEmpty());
        m.forget("I can ", "see");
        QVERIFY(!m.traceFor("I can ", "see").has_value());
        QVERIFY(m.candidatesFor("I can ", "see").isEmpty());
        // A word typed key by key has no finger path to re-score.
        m.remember("I can ", "go", {}, {});
        QVERIFY(!m.traceFor("I can ", "go").has_value());
    }
    void longTextAnchorsOnItsLastCharacters() {
        RecentSwipes m;
        const QString before = QString("lots of earlier text, ").repeated(5) + "and then ";
        m.remember(before, "go", trace3(), keys2(), {"go", "so"});
        QVERIFY(m.traceFor("EDITED " + before.mid(7), "go").has_value()); // far-away edit tolerated
        QCOMPARE(m.candidatesFor("EDITED " + before.mid(7), "go"), (QStringList{"go", "so"}));
        QVERIFY(!m.traceFor(before + "x", "go").has_value());
    }
    void ownWordsAreFoundByComparingNotReading() {
        // Tap-to-swap in stock Plasma fields offers only the keyboard's own words (card #187).
        RecentSwipes m;
        m.remember("I can ", "see", trace3(), keys2(), {"see", "sea"}, {"i", "can"});
        m.remember("I can see ", "it", {}, {}, {}, {"i", "can", "see"});   // typed key by key: no swipe guesses
        const QString text = "I can see it now";
        for (int cursor : {6, 7, 9}) {
            const auto o = m.ownedAt(text, cursor);
            QVERIFY2(o.has_value(), qPrintable(QString::number(cursor)));
            QCOMPARE(o->span, (WordSpan{6, 3, "see"}));
            QCOMPARE(o->trace.size(), size_t(3));
            QCOMPARE(o->keys.size(), size_t(2));
            QCOMPARE(o->candidates, (QStringList{"see", "sea"}));
            QCOMPARE(o->context, (QStringList{"i", "can"}));
        }
        QCOMPARE(m.ownedAt(text, 11)->span, (WordSpan{10, 2, "it"}));
        QVERIFY(m.ownedAt(text, 11)->trace.empty());
        QVERIFY(m.ownedAt(text, 11)->candidates.isEmpty());
        QVERIFY(!m.ownedAt(text, 14).has_value());              // "now" was not typed by the keyboard
        QVERIFY(!m.ownedAt(text, 2).has_value());               // nor "can"
        QVERIFY(!m.ownedAt("I can seed it", 8).has_value());    // never part of a longer word
        QVERIFY(!m.ownedAt("We can see it", 9).has_value());    // same word, different text before it
        QVERIFY(!m.ownedAt(text, -1).has_value());
        QVERIFY(!m.ownedAt(text, 99).has_value());
    }
    void anOutsideEditIsToldApartFromOwnTyping() {
        CursorMoveTracker t;
        t.observe("hello", 5, 0);
        QVERIFY(!t.outsideEdit());
        t.ownEdit(100);
        t.observe("hello ", 6, 120);                            // our own key's echo
        QVERIFY(!t.outsideEdit());
        t.observe("hello there", 11, 2000);                     // nobody on this keyboard typed that
        QVERIFY(t.outsideEdit());
        t.observe("hello there", 3, 2100);                      // a tap: text unchanged
        QVERIFY(!t.outsideEdit());
    }
    void aLateReportOfOwnTypingIsNotAnOutsideEdit() {
        // Chromium on a busy machine: the app reports the keyboard's own word well after the usual settle time.
        CursorMoveTracker t;
        t.observe("ok ", 3, 0);
        t.ownEdit(100);
        t.observe("ok ", 3, 600);
        t.observe("ok ", 3, 1500);                              // nothing yet; taps already count again
        t.observe("ok hello ", 9, 2500);                        // the late report of our own edit
        QVERIFY(!t.outsideEdit());
        t.observe("ok hello there", 14, 2800);                  // a second change: someone else's
        QVERIFY(t.outsideEdit());
        t.ownEdit(5000);
        t.observe("ok hello there", 14, 5100);
        t.observe("ok hello there!", 15, 9000);                 // far too late to be ours
        QVERIFY(t.outsideEdit());
    }
    void memoryIsBounded() {
        RecentSwipes m;
        for (int i = 0; i < 40; ++i) m.remember(QString("t%1 ").arg(i), "w", trace3(), keys2(), {"w", "x"});
        QCOMPARE(m.size(), RecentSwipes::kMax);
        QVERIFY(!m.traceFor("t0 ", "w").has_value());
        QVERIFY(m.traceFor("t39 ", "w").has_value());
        m.clear();
        QCOMPARE(m.size(), 0);
    }

    void takeReplacesExactlyTheShownWordAndAddsNoSpace_data() {
        QTest::addColumn<int>("cursor");
        QTest::newRow("cursor at end") << 7; QTest::newRow("cursor inside") << 5; QTest::newRow("cursor at start") << 4;
    }
    void takeReplacesExactlyTheShownWordAndAddsNoSpace() {
        QFETCH(int, cursor);
        const QString text = "the Sea is";
        WordSwapSession s;
        QVERIFY(s.begin(text, cursor, {}, {"see", "sew", "set"}));
        QCOMPARE(s.candidates().size(), 3);
        const int index = s.candidates().indexOf("See");
        QVERIFY(index >= 0);
        const auto plan = s.take(index, s.token(), text, cursor);
        QCOMPARE(plan.text, QString("See"));
        QCOMPARE(plan.replaceFrom, 4 - cursor);      // relative to the cursor, so it begins at the word start
        QCOMPARE(plan.replaceLength, 3);
        QVERIFY(!s.active());
    }
    void takeIsRefusedWhenAnythingNoLongerMatches() {
        const QString text = "the sea is";
        auto fresh = [&] { WordSwapSession s; [&] { QVERIFY(s.begin(text, 5, {}, {"see", "sew"})); }(); return s; };
        { auto s = fresh(); QVERIFY(s.take(0, QString(), text, 5).isEmpty()); }                       // finger never pressed
        { auto s = fresh(); QVERIFY(s.take(0, "swap:0", text, 5).isEmpty()); }                        // another list's press
        { auto s = fresh(); const auto t = s.token(); QVERIFY(s.take(0, t, "the sea is.", 5).isEmpty()); }   // text changed
        { auto s = fresh(); const auto t = s.token(); QVERIFY(s.take(0, t, text, 9).isEmpty()); }     // cursor moved away
        { auto s = fresh(); const auto t = s.token(); QVERIFY(s.take(7, t, text, 5).isEmpty()); }     // no such alternative
        { auto s = fresh(); const auto t = s.token(); QVERIFY(!s.take(0, t, text, 5).isEmpty()); }    // control: this one works
    }
    void sentenceAnswerChangesTheListAndTheToken() {
        WordSwapSession s;
        QVERIFY(s.begin("the sea is", 5, {}, {"set", "see", "sew"}));
        const auto before = s.token();
        QVERIFY(!s.setRanked({}));                 // nothing new
        QVERIFY(s.setRanked({"sew", "see"}));
        QVERIFY(s.token() != before);              // a press on the old list must not swap the new one
        QVERIFY(s.take(0, before, "the sea is", 5).isEmpty());
    }
    void noAlternativesMeansNotActive() {
        WordSwapSession s;
        QVERIFY(!s.begin("the sea is", 5, {}, {}));
        QVERIFY(!s.active());
        QVERIFY(!s.begin("the  is", 4, {}, {"x"}));  // no word under the cursor
    }
    void stillValidFollowsTheWord() {
        WordSwapSession s;
        QVERIFY(s.begin("the sea is", 5, {}, {"see"}));
        QVERIFY(s.stillValid("the sea is", 4));
        QVERIFY(s.stillValid("the sea is", 7));
        QVERIFY(!s.stillValid("the sea is", 8));
        QVERIFY(!s.stillValid("the sea is", 3));
        QVERIFY(!s.stillValid("the seas is", 5));
    }
};
QTEST_GUILESS_MAIN(WordSwapTest)
#include "tst_wordswap.moc"
