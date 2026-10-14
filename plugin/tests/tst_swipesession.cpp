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
        QTest::newRow("last three only") << QStringLiteral("so I am going to") << QStringList{"am", "going", "to"};
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
    }
    void needsSpaceBefore()
    {
        QFETCH(QString, before);
        QFETCH(bool, expected);
        QCOMPARE(SwipeSession::needsSpaceBefore(before), expected);
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
