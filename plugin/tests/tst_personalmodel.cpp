// The personal-learning model on its own: counting, decay, forgetting, limits and the storage format.
#include <QtTest>
#include "swipekb/learning.hpp"

using swipekb::PersonalModel;

namespace {
constexpr long long kDay = 86400;
std::vector<std::string> keys(std::initializer_list<const char *> words)
{
    std::vector<std::string> out;
    for (auto w : words) out.push_back(w);
    return out;
}
std::vector<std::string> order(const std::vector<PersonalModel::Ranked> &ranked)
{
    std::vector<std::string> out;
    for (const auto &r : ranked) out.push_back(r.key);
    return out;
}
bool contains(const std::vector<std::string> &list, const std::string &word)
{
    return std::find(list.begin(), list.end(), word) != list.end();
}
}

class PersonalModelTest : public QObject {
    Q_OBJECT
private slots:
    void aNewWordIsSuggestibleOnlyAfterTwoUses()
    {
        PersonalModel m;
        m.learn("with", "destin", false, true, 1000);
        QVERIFY(!m.learnedNewWord("destin", 1000));
        QVERIFY(!contains(order(m.rerank({}, "", "des", 1000, 5, true)), "destin"));
        QVERIFY(m.swipeWords(1000).empty());
        m.learn("with", "destin", false, true, 2000);
        QVERIFY(m.learnedNewWord("destin", 2000));
        QVERIFY(contains(order(m.rerank({}, "", "des", 2000, 5, true)), "destin"));
        QCOMPARE(m.swipeWords(2000).size(), size_t(1));
        QVERIFY(m.wantsCapital("destin"));   // typed with a capital in the middle of a sentence
    }
    void aKnownWordIsBoostedAfterOneUse()
    {
        PersonalModel m;
        const auto base = keys({"hello", "help", "held", "hell"});
        QCOMPARE(order(m.rerank(base, "", "hel", 0, 4, true)), base);
        m.learn("", "hell", true, false, 100);
        m.learn("", "hell", true, false, 200);
        // Two uses lift "hell" from last place, but the predictor's own order still leads.
        const auto got = order(m.rerank(base, "", "hel", 300, 4, true));
        QVERIFY(std::find(got.begin(), got.end(), std::string("hell")) - got.begin() < 3);
        QCOMPARE(got.front(), std::string("hello"));
        // WHY: with nothing learned the order is exactly the predictor's.
        PersonalModel empty;
        QCOMPARE(order(empty.rerank(base, "x", "hel", 0, 4, true)), base);
    }
    void aPairLiftsTheWordThatUsuallyFollows()
    {
        PersonalModel m;
        for (int i = 0; i < 3; ++i) m.learn("good", "morning", true, false, 100 + i);
        const auto base = keys({"night", "luck", "idea", "morning"});
        QCOMPARE(order(m.rerank(base, "good", "", 200, 4, true)).front(), std::string("morning"));
        // Another previous word gets no help from the pair.
        QVERIFY(order(m.rerank(base, "bad", "", 200, 4, true)).front() != std::string("morning"));
    }
    void learnedFollowersJoinTheListEvenIfThePredictorMissedThem()
    {
        PersonalModel m;
        for (int i = 0; i < 3; ++i) m.learn("dear", "zeynep", false, true, 100 + i);
        const auto got = order(m.rerank(keys({"sir", "madam", "friend"}), "dear", "", 200, 3, true));
        QVERIFY(contains(got, "zeynep"));
    }
    void oneUseNeverDisplacesTheTopChoice()
    {
        // Never fully overriding: the n-gram's first choice survives a single personal use of a lower word.
        PersonalModel m;
        m.learn("the", "second", true, false, 100);
        const auto got = order(m.rerank(keys({"first", "x", "second"}), "the", "", 150, 3, true));
        QCOMPARE(got.front(), std::string("first"));
    }
    void anExtraLearnedWordCannotJumpTheWholeListOnWeakEvidence()
    {
        PersonalModel m;
        m.learn("", "zorp", false, false, 100);
        m.learn("", "zorp", false, false, 110);
        const auto got = order(m.rerank(keys({"zoo", "zone", "zero"}), "", "zo", 150, 3, true));
        QCOMPARE(got.front(), std::string("zoo"));
    }
    void forgetErasesWordAndPairsAndKeepsItGone()
    {
        PersonalModel m;
        for (int i = 0; i < 3; ++i) m.learn("my", "xyzzy", false, false, 100 + i);
        QVERIFY(m.learnedNewWord("xyzzy", 200));
        m.forget("xyzzy");
        QVERIFY(!m.learnedNewWord("xyzzy", 200));
        QVERIFY(m.swipeWords(200).empty());
        QCOMPARE(m.pairCount(), size_t(0));
        QVERIFY(m.isForgotten("xyzzy"));
        // Never suggested again, even when the predictor lists it...
        QVERIFY(!contains(order(m.rerank(keys({"xyzzy", "other"}), "my", "", 200, 5, true)), "xyzzy"));
        // ...until the person types it again, and then it needs its two uses once more.
        m.learn("my", "xyzzy", false, false, 300);
        QVERIFY(!m.isForgotten("xyzzy"));
        QVERIFY(!m.learnedNewWord("xyzzy", 300));
        m.learn("my", "xyzzy", false, false, 301);
        QVERIFY(m.learnedNewWord("xyzzy", 301));
    }
    void clearForgetsEverythingIncludingTheForgottenList()
    {
        PersonalModel m;
        m.learn("a", "b", true, false, 1);
        m.forget("c");
        QVERIFY(!m.empty());
        m.clear();
        QVERIFY(m.empty());
        QCOMPARE(m.serialize(), PersonalModel().serialize());
    }
    void usesFadeWithAHalfLifeAndEventuallyVanish()
    {
        PersonalModel m;
        m.learn("", "bloop", true, false, 0);
        const auto base = keys({"a", "b", "c", "d", "e", "bloop"});
        auto place = [&](long long now) {
            const auto got = order(m.rerank(base, "", "", now, 6, true));
            return std::find(got.begin(), got.end(), std::string("bloop")) - got.begin();
        };
        const auto fresh = place(0);
        QVERIFY(fresh < 5);                 // lifted
        QCOMPARE(place(900 * kDay), 5);     // 20 half-lives later: back to its own place
        // A word the language does not know disappears altogether once it has faded.
        PersonalModel n;
        n.learn("", "zibble", false, false, 0);
        n.learn("", "zibble", false, false, 10);
        QVERIFY(n.learnedNewWord("zibble", 20));
        QVERIFY(!n.learnedNewWord("zibble", 500 * kDay));
        // Half-life: after one half-life the pair boost is clearly smaller.
        PersonalModel p;
        for (int i = 0; i < 4; ++i) p.learn("a", "b", true, false, 0);
        const auto near = p.rerank(keys({"x", "y", "b"}), "a", "", 0, 3, false);
        const auto far = p.rerank(keys({"x", "y", "b"}), "a", "", 365 * kDay, 3, false);
        QCOMPARE(order(near).front(), std::string("b"));
        QCOMPARE(order(far).front(), std::string("x"));
    }
    void sizeStaysBounded()
    {
        swipekb::LearningLimits limits;
        limits.maxWords = 50; limits.maxPairs = 80;
        PersonalModel m(limits);
        for (int i = 0; i < 400; ++i)
            m.learn("w" + std::to_string(i), "w" + std::to_string(i + 1), true, false, 1000 + i);
        QVERIFY(m.wordCount() <= 50);
        QVERIFY(m.pairCount() <= 80);
        // The most recent words survive.
        QVERIFY(m.usedByPerson("w400", 1));
    }
    void refusesAnythingThatIsNotAPlainWord()
    {
        PersonalModel m;
        QVERIFY(!m.learn("a", "two words", true, false, 1).valid == false);   // a space is allowed in the model, callers screen words
        QVERIFY(!m.learn("a", "tab\tbed", true, false, 1).valid);
        QVERIFY(!m.learn("a", "new\nline", true, false, 1).valid);
        QVERIFY(!m.learn("a", "", true, false, 1).valid);
        QVERIFY(!m.learn("a", std::string(65, 'x'), true, false, 1).valid);
    }
    void undoTakesBackExactlyOneLearn()
    {
        PersonalModel m;
        m.learn("a", "b", true, false, 10);
        const auto before = m.serialize();
        const auto undo = m.learn("c", "b", true, false, 20);
        QVERIFY(m.pairCount() == 2);
        m.undo(undo);
        QCOMPARE(m.serialize(), before);
        const auto fresh = m.learn("a", "newword", false, false, 30);
        m.undo(fresh);
        QCOMPARE(m.serialize(), before);
    }
    void swipeWordsVersionChangesOnlyWhenTheSetChanges()
    {
        PersonalModel m;
        const auto v0 = m.swipeWordsVersion();
        m.learn("", "common", true, false, 1);   // a known word never enters the swipe list
        QCOMPARE(m.swipeWordsVersion(), v0);
        m.learn("", "nova", false, false, 2);
        QCOMPARE(m.swipeWordsVersion(), v0);     // one use: not yet
        m.learn("", "nova", false, false, 3);
        QVERIFY(m.swipeWordsVersion() != v0);
    }
    void storageRoundTripsAndRefusesDamage()
    {
        PersonalModel m;
        for (int i = 0; i < 3; ++i) m.learn("dear", "zeynep", false, true, 100 + i);
        m.learn("the", "cat", true, false, 500);
        m.forget("gone");
        const auto text = m.serialize();
        PersonalModel back;
        QVERIFY(back.parse(text));
        QCOMPARE(back.serialize(), text);
        QVERIFY(back.learnedNewWord("zeynep", 600));
        QVERIFY(back.isForgotten("gone"));
        // Every kind of damage: empty, wrong version, cut short, garbage, bad numbers, trailing junk, bad counts.
        const std::vector<std::string> bad = {
            "", "SWKBLEARN 2\nE\t0\t0\t0\n", text.substr(0, text.size() / 2), "not a store at all",
            "SWKBLEARN 1\nW\tx\tNaN\t1\t1\t0\t0\nE\t1\t0\t0\n", "SWKBLEARN 1\nW\tx\t1\t1\t1\t0\t0\n",
            text + "W\textra\t1\t1\t1\t0\t0\n", "SWKBLEARN 1\nW\tx\t1\t1\t1\t5\t0\nE\t1\t0\t0\n",
            "SWKBLEARN 1\nW\tx\t-1\t1\t1\t0\t0\nE\t1\t0\t0\n", "SWKBLEARN 1\nE\t1\t0\t0\n",
            std::string(5 << 20, 'x')};
        for (const auto &b : bad) {
            PersonalModel x;
            x.learn("a", "b", true, false, 1);
            QVERIFY2(!x.parse(b), qPrintable(QString::fromStdString(b.substr(0, 40))));
            QVERIFY(x.empty());   // a damaged file leaves it empty, never half-filled
        }
    }
};

QTEST_GUILESS_MAIN(PersonalModelTest)
#include "tst_personalmodel.moc"
