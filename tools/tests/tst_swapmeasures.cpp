// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Swap scoreboard (card #500): the counting behind the swap measures, tested without any engine.
#include <QTest>
#include "../swapmeasures.h"
using namespace swapbench;
class TstSwapMeasures : public QObject { Q_OBJECT
private slots:
    void countsOnlyWrongFirstGuesses() {
        Report r; auto id = [](const std::string &s) { return s; };
        r.add(Measure::AfterSwipe, true, "good", {"food", "good", "hood"}, id);   // in 3, not first
        r.add(Measure::AfterSwipe, true, "good", {"good"}, id);                   // first
        r.add(Measure::AfterSwipe, false, "good", {"x"}, id);                     // first guess was right: not a case
        r.add(Measure::AfterSwipe, true, "good", {"a", "b", "c", "good"}, id);    // 4th: a miss
        QCOMPARE(r.m[0].cases, size_t(3)); QCOMPARE(r.m[0].first, size_t(1)); QCOMPARE(r.m[0].inThree, size_t(2));
    }
    void formatsZeroCases() {
        Report r;
        QVERIFY(QString::fromStdString(r.format()).contains("tapback-notrace: cases 0 first 0.00% in3 0.00%"));
    }
    void junkShare() {
        Report r; r.addJunk(3, 1); r.addJunk(1, 0);
        QVERIFY(QString::fromStdString(r.format()).contains("junk: right 2 shown 4 junk 25.00% count 1\n"));
    }
    void tapBackJunkHasItsOwnLine() {
        Report r; r.addTapJunk(0, 3, 1); r.addTapJunk(1, 4, 0); r.addTapJunk(1, 4, 2);
        const auto out = QString::fromStdString(r.format());
        QVERIFY(out.contains("junk-tapback: trace shown 3 junk 33.33% notrace shown 8 junk 25.00% counts 1 2\n"));
        QVERIFY(out.contains("junk: right 0 shown 0 junk 0.00% count 0\n"));   // measure 4 is left alone
    }
};
QTEST_APPLESS_MAIN(TstSwapMeasures)
#include "tst_swapmeasures.moc"
