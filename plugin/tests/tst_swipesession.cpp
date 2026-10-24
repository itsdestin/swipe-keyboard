// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QtTest>

#include "swipesession.h"

class TstSwipeSession : public QObject {
    Q_OBJECT
private slots:
    void hiddenTextFallback_data()
    {
        QTest::addColumn<QString>("surrounding");
        QTest::addColumn<int>("cursor");
        QTest::addColumn<int>("anchor");
        QTest::addColumn<bool>("expected");
        QTest::newRow("hidden buffer before echo") << QString() << 0 << 0 << true;
        QTest::newRow("hidden buffer after word") << QString() << 3 << 3 << true;
        QTest::newRow("hidden cursor moved") << QString() << 1 << 1 << false;
        QTest::newRow("known text after cursor") << QStringLiteral("tee") << 0 << 0 << false;
        QTest::newRow("select all before deleting") << QStringLiteral("tee") << 0 << 3 << false;
        QTest::newRow("selection even with hidden buffer") << QString() << 0 << 3 << false;
        QTest::newRow("known text before cursor") << QStringLiteral("tee") << 3 << 3 << false;
        QTest::newRow("missing context") << QString() << -1 << -1 << false;
    }
    void hiddenTextFallback()
    {
        QFETCH(QString, surrounding);
        QFETCH(int, cursor);
        QFETCH(int, anchor);
        QFETCH(bool, expected);
        const bool keep = SwipeSession::keepHiddenTextSpaceFallback(surrounding, cursor, anchor, 0, 3);
        QCOMPARE(keep, expected);
        SwipeSession session;
        QCOMPARE(session.commitSwipe(QString(), {"the"}, CaseMode::AsIs, {.afterSwipe = keep}).text,
                 expected ? QStringLiteral(" the") : QStringLiteral("the"));
    }

    void contextWords_data()
    {
        QTest::addColumn<QString>("before");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("empty") << QString() << QStringList();
        QTest::newRow("three") << QStringLiteral("I am going") << QStringList{"I", "am", "going"};
        // WHY: the whole sentence so far is context (the sentence models use more than 3 words).
        QTest::newRow("whole sentence") << QStringLiteral("so I am going to the shop with my sister and her two dogs")
                                        << QString("so I am going to the shop with my sister and her two dogs").split(' ');
        QTest::newRow("last twenty only") << QStringLiteral("w1 w2 w3 w4 w5 w6 w7 w8 w9 w10 w11 w12 w13 w14 w15 w16 w17 w18 w19 w20 w21 w22")
                                          << QString("w3 w4 w5 w6 w7 w8 w9 w10 w11 w12 w13 w14 w15 w16 w17 w18 w19 w20 w21 w22").split(' ');
        QTest::newRow("trailing space") << QStringLiteral("going to ") << QStringList{"going", "to"};
        QTest::newRow("punctuation trimmed") << QStringLiteral("well, \"really\"") << QStringList{"well", "really"};
        QTest::newRow("new sentence resets") << QStringLiteral("Done. I am") << QStringList{"I", "am"};
        QTest::newRow("question resets") << QStringLiteral("Why? Because") << QStringList{"Because"};
        QTest::newRow("just ended") << QStringLiteral("Done.") << QStringList();
    }
    void contextWords()
    {
        QFETCH(QString, before);
        QFETCH(QStringList, expected);
        QCOMPARE(SwipeSession::contextWords(before), expected);
    }

    void needsSpaceBefore_data()
    {
        QTest::addColumn<QString>("before");
        QTest::addColumn<bool>("expected");
        QTest::newRow("start of field") << QString() << false;
        QTest::newRow("after word") << QStringLiteral("I said") << true;
        QTest::newRow("after space") << QStringLiteral("I said ") << false;
        QTest::newRow("after newline") << QStringLiteral("hi\n") << false;
        QTest::newRow("after period") << QStringLiteral("end.") << true;
        QTest::newRow("after comma") << QStringLiteral("so,") << true;
        QTest::newRow("after open paren") << QStringLiteral("(") << false;
        QTest::newRow("after open quote") << QStringLiteral("say \"") << false;
        QTest::newRow("after hyphen") << QStringLiteral("well-") << false;
        QTest::newRow("after slash") << QStringLiteral("and/") << false;
        QTest::newRow("after at") << QStringLiteral("@") << false;
        // Straight quotes: opening (glue) only at the start, after a space or an opening bracket.
        QTest::newRow("after closing double quote") << QStringLiteral("say \"hello\"") << true;
        QTest::newRow("after closing apostrophe") << QStringLiteral("students'") << true;
        QTest::newRow("after bracket then quote") << QStringLiteral("('") << false;
        QTest::newRow("quote at start") << QStringLiteral("\"") << false;
        QTest::newRow("after curly open quote") << QStringLiteral("say \u201c") << false;
        QTest::newRow("after curly open single") << QStringLiteral("say \u2018") << false;
    }
    void needsSpaceBefore()
    {
        QFETCH(QString, before);
        QFETCH(bool, expected);
        QCOMPARE(SwipeSession::needsSpaceBefore(before), expected);
    }

    // SpaceContext: facts the text alone can't show (app hides its text; web-address/email field).
    void spaceContext_data()
    {
        QTest::addColumn<QString>("before");
        QTest::addColumn<bool>("afterSwipe");
        QTest::addColumn<bool>("urlField");
        QTest::addColumn<bool>("expected");
        QTest::newRow("hidden text after swipe") << QString() << true << false << true;
        QTest::newRow("hidden text, no swipe") << QString() << false << false << false;
        QTest::newRow("known text wins over flag") << QStringLiteral("I said ") << true << false << false;
        QTest::newRow("url: after dot") << QStringLiteral("www.") << false << true << false;
        QTest::newRow("url: after at") << QStringLiteral("me@") << false << true << false;
        QTest::newRow("url: after slash") << QStringLiteral("example.com/") << false << true << false;
        QTest::newRow("url: after letter") << QStringLiteral("www") << false << true << true;
        QTest::newRow("normal field: after dot") << QStringLiteral("end.") << false << false << true;
    }
    void spaceContext()
    {
        QFETCH(QString, before);
        QFETCH(bool, afterSwipe);
        QFETCH(bool, urlField);
        QFETCH(bool, expected);
        QCOMPARE(SwipeSession::needsSpaceBefore(before, {.afterSwipe = afterSwipe, .urlOrEmailField = urlField}),
                 expected);
    }
    void commitPassesSpaceContext()
    {
        SwipeSession s;
        // App reports no text, but we just swiped "hello": the next word still gets its space.
        QCOMPARE(s.commitSwipe(QString(), {"world"}, CaseMode::AsIs, {.afterSwipe = true}).text,
                 QStringLiteral(" world"));
        QCOMPARE(s.commitSwipe(QStringLiteral("www."), {"example"}, CaseMode::AsIs, {.urlOrEmailField = true}).text,
                 QStringLiteral("example"));
    }

    // Tapping a key right after a swiped word: "write" + tap "a" must give "write a", not "writea".
    void spaceBeforeTappedKey_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<bool>("afterSwipedWord");
        QTest::addColumn<bool>("urlField");
        QTest::addColumn<bool>("expected");
        QTest::newRow("letter") << QStringLiteral("a") << true << false << true;
        QTest::newRow("capital letter") << QStringLiteral("A") << true << false << true;
        QTest::newRow("digit") << QStringLiteral("5") << true << false << true;
        QTest::newRow("accented letter") << QStringLiteral("\u00e9") << true << false << true;
        QTest::newRow("not after a swipe") << QStringLiteral("a") << false << false << false;
        QTest::newRow("url field") << QStringLiteral("a") << true << true << false;
        for (const char *p : {".", ",", "!", "?", ";", ":", "'", "\"", "\u201d", "\u2019", ")", "-"})
            QTest::newRow(qPrintable(QStringLiteral("punctuation ") + QString::fromUtf8(p)))
                << QString::fromUtf8(p) << true << false << false;
        QTest::newRow("space") << QStringLiteral(" ") << true << false << false;
        QTest::newRow("enter") << QStringLiteral("\n") << true << false << false;
        QTest::newRow("backspace / shift (no text)") << QString() << true << false << false;
        // WHY: the spacebar slide sends Left/Right (empty text) right after a swiped word; they must never type a space.
        QTest::newRow("left/right arrow from the spacebar slide (no text)") << QString() << true << false << false;
    }
    void spaceBeforeTappedKey()
    {
        QFETCH(QString, key);
        QFETCH(bool, afterSwipedWord);
        QFETCH(bool, urlField);
        QFETCH(bool, expected);
        QCOMPARE(SwipeSession::spaceBeforeTappedKey(key, afterSwipedWord, urlField), expected);
    }

    void otherSpellingGoesFirstInTheGuessBar()
    {
        QCOMPARE(SwipeSession::withOtherSpelling({"its", "ita", "itd"}, "it's"), (QStringList{"its", "it's", "ita", "itd"}));
        QCOMPARE(SwipeSession::withOtherSpelling({"its"}, "it's"), (QStringList{"its", "it's"}));
        QCOMPARE(SwipeSession::withOtherSpelling({"its", "it's"}, "it's"), (QStringList{"its", "it's"})); // no duplicate
        QCOMPARE(SwipeSession::withOtherSpelling({"hello", "hells"}, QString()), (QStringList{"hello", "hells"}));
        QCOMPARE(SwipeSession::withOtherSpelling({}, "it's"), QStringList());
        // The typed word stays the best guess; the other spelling is the first alternative.
        SwipeSession s;
        QCOMPARE(s.commitSwipe(QString(), SwipeSession::withOtherSpelling({"its", "ita"}, "it's"), CaseMode::AsIs).text,
                 QStringLiteral("its"));
        QCOMPARE(s.alternatives(), (QStringList{"it's", "ita"}));
    }

    void commitAtStartHasNoSpace()
    {
        SwipeSession s;
        const CommitPlan p = s.commitSwipe(QString(), {"hello", "hells", "hollow"}, CaseMode::AsIs);
        QCOMPARE(p.text, QStringLiteral("hello"));
        QCOMPARE(p.replaceFrom, 0);
        QCOMPARE(p.replaceLength, 0);
        QVERIFY(s.isActive());
        QCOMPARE(s.alternatives(), (QStringList{"hells", "hollow"}));
    }
    void commitAfterWordAddsSpace()
    {
        SwipeSession s;
        QCOMPARE(s.commitSwipe(QStringLiteral("I said"), {"hello"}, CaseMode::AsIs).text, QStringLiteral(" hello"));
    }
    void casing()
    {
        SwipeSession s;
        QCOMPARE(s.commitSwipe(QString(), {"hello", "hells"}, CaseMode::Capitalize).text, QStringLiteral("Hello"));
        QCOMPARE(s.alternatives(), QStringList{"Hells"});
        QCOMPARE(s.commitSwipe(QString(), {"hello"}, CaseMode::AllCaps).text, QStringLiteral("HELLO"));
        QCOMPARE(s.commitSwipe(QString(), {"I"}, CaseMode::AsIs).text, QStringLiteral("I")); // dictionary casing kept
    }
    void swapReplacesOnlyTheWord()
    {
        SwipeSession s;
        s.commitSwipe(QStringLiteral("I said"), {"hello", "hells", "hollow"}, CaseMode::AsIs); // typed " hello"
        const CommitPlan p = s.swapToAlternative(1);                                           // "hollow"
        QCOMPARE(p.text, QStringLiteral("hollow"));
        QCOMPARE(p.replaceFrom, -5);
        QCOMPARE(p.replaceLength, 5);
        QCOMPARE(s.alternatives(), (QStringList{"hello", "hells"}));
        const CommitPlan back = s.swapToAlternative(0); // "hello" again
        QCOMPARE(back.text, QStringLiteral("hello"));
        QCOMPARE(back.replaceFrom, -6);
        QCOMPARE(back.replaceLength, 6);
    }
    void backspaceUndoesLatestSwipeOnlyOnce()
    {
        SwipeSession s;
        s.commitSwipe(QString(), {"hello", "yellow"}, CaseMode::AsIs);
        const auto plan = s.takeBackspace(QStringLiteral("hello"), false);
        QVERIFY(plan.text.isEmpty());
        QCOMPARE(plan.replaceFrom, -5);
        QCOMPARE(plan.replaceLength, 5);
        QVERIFY(!s.isActive());
        QVERIFY(s.takeBackspace(QStringLiteral("hello"), false).isEmpty());
    }
    void backspaceUsesChosenAlternativeAndOnlyItsOwnSeparator()
    {
        SwipeSession s;
        s.commitSwipe(QStringLiteral("say"), {"hello", "yellow"}, CaseMode::AsIs);
        s.swapToAlternative(0);
        auto plan = s.takeBackspace(QStringLiteral("say yellow"), false);
        QCOMPARE(plan.replaceFrom, -7);
        QCOMPARE(plan.replaceLength, 7); // undo the space introduced by this swipe too
        s.commitSwipe(QStringLiteral("say "), {"hello"}, CaseMode::Capitalize);
        plan = s.takeBackspace(QStringLiteral("say Hello"), false);
        QCOMPARE(plan.replaceLength, 5); // preserve an existing space the user typed
    }
    void backspaceFallsBackOnUncertainOrChangedContext_data()
    {
        QTest::addColumn<QString>("before");
        QTest::addColumn<bool>("selection");
        QTest::newRow("hidden") << QString() << false;
        QTest::newRow("cursor moved") << QStringLiteral("say he") << false;
        QTest::newRow("extra typed character") << QStringLiteral("say hellox") << false;
        QTest::newRow("same suffix different context") << QStringLiteral("other hello") << false;
        QTest::newRow("selected word") << QStringLiteral("say hello") << true;
    }
    void backspaceFallsBackOnUncertainOrChangedContext()
    {
        QFETCH(QString, before);
        QFETCH(bool, selection);
        SwipeSession s;
        s.commitSwipe(QStringLiteral("say"), {"hello"}, CaseMode::AsIs);
        QVERIFY(s.takeBackspace(before, selection).isEmpty());
        QVERIFY(!s.isActive());
    }
    void backspaceAfterSessionEndedStaysNormal()
    {
        SwipeSession s;
        s.commitSwipe(QString(), {"hello"}, CaseMode::AsIs);
        s.clear(); // tapped punctuation/letter/space, reset, or reported cursor move
        QVERIFY(s.takeBackspace(QStringLiteral("hello"), false).isEmpty());
    }

    void swapKeepsCasing()
    {
        SwipeSession s;
        s.commitSwipe(QString(), {"hello", "hells"}, CaseMode::Capitalize);
        QCOMPARE(s.swapToAlternative(0).text, QStringLiteral("Hells"));
    }
    void invalidSwapsDoNothing()
    {
        SwipeSession s;
        QVERIFY(s.swapToAlternative(0).isEmpty());
        s.commitSwipe(QString(), {"hello", "hells"}, CaseMode::AsIs);
        QVERIFY(s.swapToAlternative(1).isEmpty());
        QVERIFY(s.swapToAlternative(-1).isEmpty());
    }
    void noCandidatesMeansNothing()
    {
        SwipeSession s;
        s.commitSwipe(QString(), {"hello"}, CaseMode::AsIs);
        QVERIFY(s.commitSwipe(QString(), {}, CaseMode::AsIs).isEmpty());
        QVERIFY(!s.isActive());
        QVERIFY(s.alternatives().isEmpty());
    }
    void cursorMoveToEarlierIdenticalWordEndsUndo()
    {
        SwipeSession s;
        s.commitSwipe(QStringLiteral("hello"), {"hello"}, CaseMode::AsIs);
        QVERIFY(s.cursorStillAfterWord(QStringLiteral("hello hello")));
        // Moving to the earlier word must invalidate the session even though its suffix matches.
        QVERIFY(!s.cursorStillAfterWord(QStringLiteral("hello")));
        s.clear(); // the input method applies this invalidation on the reported cursor update
        QVERIFY(s.takeBackspace(QStringLiteral("hello hello"), false).isEmpty());
    }

    void cursorTracking()
    {
        SwipeSession s;
        QVERIFY(!s.cursorStillAfterWord(QStringLiteral("x")));
        s.commitSwipe(QStringLiteral("I said"), {"hello"}, CaseMode::AsIs);
        QVERIFY(s.cursorStillAfterWord(QStringLiteral("I said hello")));
        QVERIFY(!s.cursorStillAfterWord(QStringLiteral("I said hello!")));
        QVERIFY(!s.cursorStillAfterWord(QStringLiteral("I sa")));
        s.clear();
        QVERIFY(!s.isActive());
        QVERIFY(!s.cursorStillAfterWord(QStringLiteral("I said hello")));
    }
    // WHY: with capitals shown ("iPhone", "YouTube"), Shift at a sentence start must not make "IPhone"; plain words
    // and caps lock behave as before.
    void capitalizeLeavesWordsWithInnerCapitalsAlone()
    {
        QCOMPARE(SwipeSession::applyCase("iPhone", CaseMode::Capitalize), QString("iPhone"));
        QCOMPARE(SwipeSession::applyCase("YouTube", CaseMode::Capitalize), QString("YouTube"));
        QCOMPARE(SwipeSession::applyCase("hello", CaseMode::Capitalize), QString("Hello"));
        QCOMPARE(SwipeSession::applyCase("I", CaseMode::Capitalize), QString("I"));
        QCOMPARE(SwipeSession::applyCase("iPhone", CaseMode::AllCaps), QString("IPHONE"));
        QCOMPARE(SwipeSession::applyCase("Paris", CaseMode::AsIs), QString("Paris"));
    }
};

QTEST_GUILESS_MAIN(TstSwipeSession)
#include "tst_swipesession.moc"
