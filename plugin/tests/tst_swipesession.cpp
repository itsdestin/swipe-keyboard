#include <QtTest>

#include "swipesession.h"

class TstSwipeSession : public QObject {
    Q_OBJECT
private slots:
    void contextWords_data()
    {
        QTest::addColumn<QString>("before");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("empty") << QString() << QStringList();
        QTest::newRow("three") << QStringLiteral("I am going") << QStringList{"I", "am", "going"};
        // WHY: the whole sentence so far is context (FUTO's model was tested that way), not just 3 words.
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
};

QTEST_GUILESS_MAIN(TstSwipeSession)
#include "tst_swipesession.moc"
