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
        QCOMPARE(swapflow::tidySwapList("good", c, {}, 3, rules(), true), (QStringList{"food", "wood", "hood"}));
    }
    void aCleanWordIsNeverAnsweredWithASwearWord() {
        // The blocked spelling is assembled in pieces.
        const QString swear = QStringLiteral("f") + "uck";
        const std::vector<SwapCandidate> c{cand(qPrintable(swear), 0), cand("duck", -1), cand("luck", -2)};
        QCOMPARE(swapflow::tidySwapList("tuck", c, {}, 3, rules(), true), (QStringList{"duck", "luck"}));
        // The person's own swear word keeps its look-alikes, whatever they are.
        const std::vector<SwapCandidate> d{cand("tuck", 0), cand("duck", -1)};
        QCOMPARE(swapflow::tidySwapList(swear, d, {}, 3, rules(), true), (QStringList{"tuck", "duck"}));
        const std::vector<SwapCandidate> e{cand(qPrintable(QStringLiteral("sh") + "it"), 0), cand("duck", -1)};
        QCOMPARE(swapflow::tidySwapList(swear, e, {}, 3, rules(), true).size(), 2);
    }
    void theTappedWordsCapitalsAreKept() {
        const std::vector<SwapCandidate> c{cand("food", 0), cand("wood", -1)};
        QCOMPARE(swapflow::tidySwapList("Good", c, {}, 3, rules(), true), (QStringList{"Food", "Wood"}));
        QCOMPARE(swapflow::tidySwapList("GOOD", c, {}, 3, rules(), true), (QStringList{"FOOD", "WOOD"}));
    }
    void germanNounsKeepTheirCapital() {
        // WHY: "Maus" is how the dictionary spells it; a lower-case tapped word must not lower it.
        const std::vector<SwapCandidate> c{cand("Maus", 0), cand("aus", -1)};
        QCOMPARE(swapflow::tidySwapList("haus", c, {}, 3, rules("de", "de_DE"), true), (QStringList{"Maus", "aus"}));
    }
    void forgottenWordsAreDropped() {
        auto r = rules();
        r.personalize = [](const QStringList &l, const QString &, double) { QStringList o = l; o.removeAll("wood"); return o; };
        const std::vector<SwapCandidate> c{cand("food", 0), cand("wood", -1), cand("hood", -2)};
        QCOMPARE(swapflow::tidySwapList("good", c, {}, 3, r, true), (QStringList{"food", "hood"}));
    }
    // WHY (card #500 review): before card #500 the bar after a swipe was the swipe's runner-ups personalized at half
    // strength (boost 0.5 after the word before), so a learned word moved up there. The swap list must keep that
    // lift, including when the worker's scores re-rank it; a tap-back list keeps boost 0.
    void aLearnedWordKeepsItsLiftInTheBarAfterASwipe() {
        QList<QPair<QString, double>> calls;
        auto r = rules();
        // A stand-in for the front end's personalize(): "hood" is the person's word after "the" (lifted when boost > 0).
        r.personalize = [&calls](const QStringList &l, const QString &prev, double boost) {
            calls.append({prev, boost});
            QStringList o = l;
            if (boost > 0 && prev == "the" && o.removeAll("hood")) o.prepend("hood");
            return o;
        };
        r.prev = "the";   // what buildAfterSwipe sets from the words before
        const std::vector<SwapCandidate> c{cand("food", 0), cand("wood", -1), cand("hood", -2)};
        auto after = swapflow::afterSwipeList("good", c, {}, 1, r);
        QCOMPARE(after.candidates, (QStringList{"good", "hood", "food"}));
        QCOMPARE(calls.last(), (QPair<QString, double>{"the", swapflow::kAfterSwipeBoost}));
        // The worker's scores re-rank the bar: still lifted.
        QVERIFY(after.pending.arm({1, 0, 2}));
        const auto words = after.pending.scoreList();
        QVector<float> sc;
        for (const auto &w : words) sc << (w == "wood" ? -0.1f : -5.f);
        swapflow::PendingSwap tap;
        auto rr = rules();
        rr.personalize = r.personalize;   // as a front end's swapRules(): no boost, no word before
        const auto got = swapflow::handleScores(tap, after.pending, false, true, {1, 0, 2}, words, sc, rr);
        QCOMPARE(got.target, swapflow::ScoredList::AfterSwipe);
        QCOMPARE(got.list, (QStringList{"good", "hood", "wood"}));
        QCOMPARE(calls.last(), (QPair<QString, double>{"the", swapflow::kAfterSwipeBoost}));
        // A tap-back list on the same words: no lift.
        QCOMPARE(swapflow::tidySwapList("good", c, {}, 3, rr, true), (QStringList{"food", "wood", "hood"}));
        QCOMPARE(calls.last(), (QPair<QString, double>{QString(), swapflow::kTapBackBoost}));
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
    // WHY (card #500, 2026-10-25): only tap-back lists use the looser rare-word rule; the bar after a swipe keeps the
    // strict one.
    void onlyTapBackListsUseTheTapBackRule() {
        for (const char *lang : {"en", "de"}) {
            const auto tb = swapflow::tapBackParams(QString::fromLatin1(lang));
            QVERIFY(tb.tapBack);
            // WHY these: the tap-back rule must really be looser than the bar's (rarer words allowed) and add
            // sentence words; otherwise "only tap-back lists" would test nothing.
            QVERIFY(tb.tapBackMinFrequency < tb.minFrequency);
            QVERIFY(tb.tapBackSentenceWords > 0);
            QCOMPARE(tb.minFrequency, swipekb::SwapParams::forLanguage(lang).minFrequency);
        }
        QVERIFY(!swapflow::afterSwipeList("good", {cand("food", 0)}, {}, 5, rules()).pending.params.tapBack);
    }
    void thePostSwipeListIsNotDressedLikeTheFirstGuess() {
        // WHY: after a swipe the first guess is the dictionary's spelling ("I", German "Haus"); its capitals say
        // nothing about the alternatives, and the swipe session applies Shift/Caps itself.
        const auto i = swapflow::afterSwipeList("I", {cand("in", 0), cand("it", -1), cand("i", -2)}, {}, 5, rules());
        QCOMPARE(i.candidates, (QStringList{"I", "in", "it"}));
        const auto haus = swapflow::afterSwipeList("Haus", {cand("aus", 0), cand("Maus", -1)}, {}, 5, rules("de", "de_DE"));
        QCOMPARE(haus.candidates, (QStringList{"Haus", "aus", "Maus"}));
        QVERIFY(!haus.pending.dress);
        QCOMPARE(haus.pending.limit, 2);
        QCOMPARE(haus.pending.sessionSerial, quint64(5));
        QVERIFY(!haus.pending.waiting);   // armed only when a scoring job is sent
        // Nothing gathered: the bar keeps the swipe's own guesses.
        QVERIFY(swapflow::afterSwipeList("Haus", {}, {}, 5, rules("de", "de_DE")).candidates.isEmpty());
    }
    void scoresGoToTheListTheyWereAskedFor() {
        auto after = swapflow::afterSwipeList("its", {cand("ita", 0), cand("is", -1), cand("itd", -2)}, "it's", 9, rules());
        QVERIFY(after.pending.arm({1, 0, 4}));
        swapflow::PendingSwap tap;
        tap.tapped = "good";
        tap.cands = {cand("food", 0), cand("wood", -1)};
        QVERIFY(tap.arm({1, 0, 3}));
        const auto words = after.pending.scoreList();
        QVector<float> sc;
        for (int k = 0; k < words.size(); ++k) sc << (words[k] == "itd" ? -0.1f : -9.f);
        // The bar after the swipe, still open: re-ranked, "it's" still first.
        auto r = swapflow::handleScores(tap, after.pending, true, true, {1, 0, 4}, words, sc, rules());
        QCOMPARE(r.target, swapflow::ScoredList::AfterSwipe);
        QCOMPARE(r.list, (QStringList{"its", "it's", "itd", "ita"}));
        QVERIFY(tap.waiting);                     // the other list is untouched
        // The tap-back list is no longer open (finger down, or another word): its answer is dropped.
        r = swapflow::handleScores(tap, after.pending, false, true, {1, 0, 3}, {"food", "wood"}, {-1.f, -0.1f}, rules());
        QCOMPARE(r.target, swapflow::ScoredList::None);
        QVERIFY(!tap.waiting);
        // An unknown stamp changes nothing.
        QCOMPARE(swapflow::handleScores(tap, after.pending, true, true, {1, 0, 99}, {}, {}, rules()).target, swapflow::ScoredList::None);
        // Nothing to score: never armed.
        swapflow::PendingSwap empty;
        QVERIFY(!empty.arm({1, 0, 5}));
        QVERIFY(!empty.waiting);
    }
    void theBarAfterASwipeAsksForScoresOnlyWhenNoWordListIsWanted() {
        QVERIFY(swapflow::mayScoreAfterSwipe(true, true, false));
        QVERIFY(!swapflow::mayScoreAfterSwipe(true, true, true));    // a next-word/completion list is wanted
        QVERIFY(!swapflow::mayScoreAfterSwipe(false, true, false));  // not a prediction field
        QVERIFY(!swapflow::mayScoreAfterSwipe(true, false, false));  // no sentence model
    }
    void thePostSwipeListKeepsTheOtherSpellingFirst() {
        QCOMPARE(swapflow::afterSwipeCandidates("its", {"ita", "is"}, "it's"), (QStringList{"its", "it's", "ita", "is"}));
        QCOMPARE(swapflow::afterSwipeCandidates("its", {"it's", "is"}, "it's"), (QStringList{"its", "it's", "is"}));
        QCOMPARE(swapflow::afterSwipeCandidates("hello", {"hells", "Hello"}, {}), (QStringList{"hello", "hells"}));
    }
};
QTEST_GUILESS_MAIN(SwapFlowTest)
#include "tst_swapflow.moc"
