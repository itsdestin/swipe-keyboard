// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// The swap flow both front ends share (card #500): the tidy-up of a swap list, the words read after a tapped word,
// the worker's scores re-ranking a list, and the post-swipe list. No keyboard and no engine files needed.
#include <QtTest>

#include "swapflow.h"

using swipekb::SwapCandidate;

namespace {
SwapCandidate cand(const char *word, float path, float before = 0)
{
    SwapCandidate c;
    c.word = word;
    c.path = path;
    c.before = before;
    return c;
}
swapflow::Rules rules(const QString &language = "en", const QString &locale = "en_US")
{
    return {language, locale, {}};
}
}

class SwapFlowTest : public QObject {
    Q_OBJECT
private slots:
    void bestFirstAndNeverTheWordItself() {
        const std::vector<SwapCandidate> c{cand("hood", -3), cand("good", 0), cand("food", -1), cand("wood", -2), cand("mood", -5)};
        QCOMPARE(swapflow::tidySwapList("good", c, {}, 3, rules()), (QStringList{"food", "wood", "hood"}));
    }
    void aCleanWordIsNeverAnsweredWithASwearWord() {
        // The blocked spelling is assembled in pieces.
        const QString swear = QStringLiteral("f") + "uck";
        const std::vector<SwapCandidate> c{cand(qPrintable(swear), 0), cand("duck", -1), cand("luck", -2)};
        QCOMPARE(swapflow::tidySwapList("tuck", c, {}, 3, rules()), (QStringList{"duck", "luck"}));
        // The person's own swear word keeps its look-alikes, whatever they are.
        const std::vector<SwapCandidate> d{cand("tuck", 0), cand("duck", -1)};
        QCOMPARE(swapflow::tidySwapList(swear, d, {}, 3, rules()), (QStringList{"tuck", "duck"}));
        const std::vector<SwapCandidate> e{cand(qPrintable(QStringLiteral("sh") + "it"), 0), cand("duck", -1)};
        QCOMPARE(swapflow::tidySwapList(swear, e, {}, 3, rules()).size(), 2);
    }
    void theTappedWordsCapitalsAreKept() {
        const std::vector<SwapCandidate> c{cand("food", 0), cand("wood", -1)};
        QCOMPARE(swapflow::tidySwapList("Good", c, {}, 3, rules()), (QStringList{"Food", "Wood"}));
        QCOMPARE(swapflow::tidySwapList("GOOD", c, {}, 3, rules()), (QStringList{"FOOD", "WOOD"}));
    }
    void germanNounsKeepTheirCapital() {
        // WHY: "Maus" is how the dictionary spells it; a lower-case tapped word must not lower it.
        const std::vector<SwapCandidate> c{cand("Maus", 0), cand("aus", -1)};
        QCOMPARE(swapflow::tidySwapList("haus", c, {}, 3, rules("de", "de_DE")), (QStringList{"Maus", "aus"}));
    }
    void forgottenWordsAreDropped() {
        auto r = rules();
        r.personalize = [](const QStringList &l) { QStringList o = l; o.removeAll("wood"); return o; };
        const std::vector<SwapCandidate> c{cand("food", 0), cand("wood", -1), cand("hood", -2)};
        QCOMPARE(swapflow::tidySwapList("good", c, {}, 3, r), (QStringList{"food", "hood"}));
    }
    void theWorkersScoresReRankTheList() {
        swapflow::PendingSwap p;
        p.stamp = {1, 0, 7};
        p.tapped = "good";
        p.cands = {cand("food", 0), cand("wood", -1), cand("hood", -2)};
        p.limit = 3;
        p.waiting = true;
        const auto asked = p.scoreList();
        QCOMPARE(asked, (QStringList{"food", "wood", "hood"}));
        // Another list's answer changes nothing and keeps waiting.
        QVERIFY(!p.answer({1, 0, 6}, asked, {-5.f, -1.f, -2.f}, rules()).has_value());
        QVERIFY(p.waiting);
        const auto got = p.answer({1, 0, 7}, asked, {-5.f, -1.f, -2.f}, rules());
        QVERIFY(got.has_value());
        QCOMPARE(*got, (QStringList{"wood", "hood", "food"}));
        QVERIFY(!p.waiting);
        // A second answer for the same list is ignored.
        QVERIFY(!p.answer({1, 0, 7}, asked, {-5.f, -1.f, -2.f}, rules()).has_value());
    }
    void anAnswerThatDoesNotLineUpIsIgnored() {
        std::vector<SwapCandidate> c{cand("food", 0), cand("wood", -1)};
        QVERIFY(!swapflow::applyScores(c, {"food", "wood"}, {-1.f}));
        QVERIFY(!swapflow::applyScores(c, {"food", "wood"}, {}));
        QVERIFY(!c[0].hasNeural);
        // Words not asked about are dropped, so the list is ranked on one scale.
        QVERIFY(swapflow::applyScores(c, {"wood"}, {-1.f}));
        QCOMPARE(c.size(), size_t(1));
        QVERIFY(c[0].hasNeural);
    }
    void atMostThirtyTwoWordsGoToTheWorker() {
        std::vector<SwapCandidate> c;
        std::vector<std::string> words;
        for (int i = 0; i < 40; ++i) words.push_back("w" + std::string(1, char('a' + i % 26)) + std::string(1, char('a' + i / 26)));
        for (int i = 0; i < 40; ++i) c.push_back(cand(words[size_t(i)].c_str(), -float(i)));
        const auto asked = swapflow::wordsToScore("x", c, {});
        QCOMPARE(int(asked.size()), swapflow::kMaxScored);
        QCOMPARE(asked.first(), QString::fromStdString(words[0]));   // the best ones
    }
    void wordsAfterTheTappedWord_data() {
        QTest::addColumn<QString>("after"); QTest::addColumn<QStringList>("words");
        QTest::newRow("two words") << " is good today" << QStringList{"is", "good"};
        QTest::newRow("sentence end") << " now. Then" << QStringList{"now"};
        QTest::newRow("punctuation right after") << ", and then" << QStringList{};
        QTest::newRow("full stop right after") << ". And" << QStringList{};
        QTest::newRow("nothing") << "" << QStringList{};
        QTest::newRow("spaces only") << "   " << QStringList{};
        QTest::newRow("a number stops it") << " 12 cats" << QStringList{};
        QTest::newRow("apostrophe") << " don't go" << QStringList{"don't", "go"};
        QTest::newRow("comma after first") << " now, then" << QStringList{"now"};
    }
    void wordsAfterTheTappedWord() {
        QFETCH(QString, after); QFETCH(QStringList, words);
        QCOMPARE(swapflow::wordsAfter(after), words);
    }
    void thePostSwipeListKeepsTheOtherSpellingFirst() {
        QCOMPARE(swapflow::afterSwipeCandidates("its", {"ita", "is"}, "it's"), (QStringList{"its", "it's", "ita", "is"}));
        QCOMPARE(swapflow::afterSwipeCandidates("its", {"it's", "is"}, "it's"), (QStringList{"its", "it's", "is"}));
        QCOMPARE(swapflow::afterSwipeCandidates("hello", {"hells", "Hello"}, {}), (QStringList{"hello", "hells"}));
    }
};
QTEST_GUILESS_MAIN(SwapFlowTest)
#include "tst_swapflow.moc"
