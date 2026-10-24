// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QtTest>

#include "swipeoutcome.h"

// Each swipe must end with at most one outcome: erase, keep, or nothing (pick is written elsewhere).
class TstSwipeOutcome : public QObject {
    Q_OBJECT
    using T = SwipeOutcomeTracker;
private slots:
    void firstBackspaceIsEraseOnlyOnce()
    {
        T t;
        t.beginSwipe(100);
        const auto o = t.keyTapped(true, "");
        QCOMPARE(o.kind, T::Kind::Erase);
        QCOMPARE(o.swipeTs, qint64(100));
        QVERIFY(!t.keyTapped(true, ""));      // second backspace: nothing
        QVERIFY(!t.keyTapped(false, "a"));    // typing after an erase: no keep either
    }
    void typedTextIsKeep()
    {
        for (const QString &k : {QStringLiteral(" "), QStringLiteral("."), QStringLiteral("x")}) {
            T t;
            t.beginSwipe(5);
            const auto o = t.keyTapped(false, k);
            QCOMPARE(o.kind, T::Kind::Keep);
            QCOMPARE(o.swipeTs, qint64(5));
            QVERIFY(!t.keyTapped(true, ""));  // erase after keep: nothing (one outcome only)
        }
    }
    void keysWithoutTextAreIgnored()
    {
        T t;
        t.beginSwipe(7);
        QVERIFY(!t.keyTapped(false, ""));
        QCOMPARE(t.keyTapped(true, "").kind, T::Kind::Erase);   // still pending
    }
    void swipingAgainKeepsPrevious()
    {
        T t;
        QVERIFY(!t.beginSwipe(1));            // nothing before
        const auto o = t.beginSwipe(2);
        QCOMPARE(o.kind, T::Kind::Keep);
        QCOMPARE(o.swipeTs, qint64(1));
        QCOMPARE(t.keyTapped(true, "").swipeTs, qint64(2));   // new swipe is the pending one
    }
    void pickAbandonAndUnrecordedGiveNothing()
    {
        T t;
        t.beginSwipe(1);
        t.picked();
        QVERIFY(!t.keyTapped(false, " "));
        QVERIFY(!t.beginSwipe(2));            // picked swipe is not "kept" by the next swipe
        t.abandon();
        QVERIFY(!t.beginSwipe(0));            // abandoned: no keep
        QVERIFY(!t.keyTapped(false, "a"));    // unrecorded swipe (ts 0): nothing to point at
    }
};

QTEST_APPLESS_MAIN(TstSwipeOutcome)
#include "tst_swipeoutcome.moc"
