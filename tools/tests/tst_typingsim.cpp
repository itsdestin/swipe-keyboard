// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// The typing simulator behind the user-experience scoreboard, with made-up bars (no engine, no model).
#include <QTest>
#include "../typingsim.h"
using namespace typingsim;
class TstTypingSim : public QObject { Q_OBJECT
private slots:
    // No bar at all: every letter plus a space, nothing saved.
    void noBarTypesEverything() {
        Tally t; typeLine(QStringLiteral("I see you."), Bar{}, t);
        QCOMPARE(t.words, size_t(3)); QCOMPARE(t.baseline, size_t(2 + 4 + 4)); QCOMPARE(t.taps, t.baseline);
        QCOMPARE(t.typedOut, size_t(3));
    }
    // The next-word bar offers "you" after "I see ": one tap instead of four.
    void nextWordTap() {
        Bar b; b.nextWord = [](const QString &text) { return text == QLatin1String("I see ") ? QStringList{"them", "you"} : QStringList{}; };
        Tally t; typeLine(QStringLiteral("I see you"), b, t);
        QCOMPARE(t.taps, size_t(2 + 4 + 1)); QCOMPARE(t.fromBar[0], size_t(1)); QCOMPARE(t.typedOut, size_t(2));
    }
    // Completion after two letters ("ho" -> "House", case folded): 2 letters + 1 tap = 3 of 6.
    void completionAfterTwoLetters() {
        Bar b; b.completion = [](const QString &text, const QString &typed) {
            Q_UNUSED(text); return typed == QLatin1String("ho") ? QStringList{"hot", "House", "ho"} : QStringList{typed}; };
        Tally t; typeLine(QStringLiteral("house"), b, t);
        QCOMPARE(t.baseline, size_t(6)); QCOMPARE(t.taps, size_t(3)); QCOMPARE(t.fromBar[2], size_t(1));
    }
    // Four letters typed before the bar has it: counted under "3 or more".
    void lateCompletionCountsAsThreePlus() {
        Bar b; b.completion = [](const QString &, const QString &typed) { return typed.size() == 4 ? QStringList{"thinking"} : QStringList{}; };
        Tally t; typeLine(QStringLiteral("thinking"), b, t);
        QCOMPARE(t.taps, size_t(5)); QCOMPARE(t.fromBar[3], size_t(1));
    }
    // The bar shows the whole word only once it is written: no saving (the word's own slot is the same tap as Space).
    void fullWordOnBarSavesNothing() {
        Bar b; b.completion = [](const QString &, const QString &typed) { return QStringList{typed}; };
        Tally t; typeLine(QStringLiteral("cat"), b, t);
        QCOMPARE(t.taps, size_t(4)); QCOMPARE(t.typedOut, size_t(1));
    }
    // Numbers and lone symbols are typed the same either way: in neither total. The text the bar sees keeps them.
    void punctuationAndNumbersLeftOut() {
        QString seen;
        Bar b; b.nextWord = [&](const QString &text) { seen = text; return QStringList{}; };
        Tally t; typeLine(QStringLiteral("at 5 - go!"), b, t);
        QCOMPARE(t.words, size_t(2)); QCOMPARE(t.baseline, size_t(3 + 3)); QCOMPARE(seen, QStringLiteral("at 5 - "));
        QCOMPARE(targetOf(QStringLiteral("aquí.")), QStringLiteral("aquí"));
    }
    void formatLine() {
        Tally t; typeLine(QStringLiteral("hi"), Bar{}, t);
        QCOMPARE(QString::fromStdString(t.format()),
                 QStringLiteral("typing: lines=1 words=1 baseline=3 taps=3 bar0=0 bar1=0 bar2=0 bar3plus=0 typedout=1\n"));
    }
};
QTEST_APPLESS_MAIN(TstTypingSim)
#include "tst_typingsim.moc"
