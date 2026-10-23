// Layout folding, lexicon trie and the CTC prefix beam search, on hand-made probability tables (no network needed).
#include <QtTest>
#include <cmath>

#include "swipekb/ngram.hpp"
#include "swipekb/swipe_beam.hpp"
#include "swipekb/swipe_layout.hpp"
#include "swipekb/swipe_lm.hpp"

using namespace swipekb;

namespace {
// Probability table that "spells" the given key indices: each key held for `hold` steps, then a blank step.
// The chosen symbol gets `p`, everything else shares the rest.
std::vector<float> spell(const std::vector<int> &keys, int K, int hold = 2, float p = 0.9f, int trailing = 2) {
    std::vector<int> sym;   // 0 = blank, k+1 = key k
    for (int k : keys) { for (int i = 0; i < hold; ++i) sym.push_back(k + 1); sym.push_back(0); }
    for (int i = 0; i < trailing; ++i) sym.push_back(0);
    std::vector<float> lp(sym.size() * (K + 1));
    for (size_t t = 0; t < sym.size(); ++t)
        for (int c = 0; c <= K; ++c) lp[t * (K + 1) + c] = std::log(c == sym[t] ? p : (1.f - p) / K);
    return lp;
}
}

class SwipeBeamTest : public QObject {
    Q_OBJECT
    SwipeLayout layout;
    std::vector<int> keysOf(const char *w) { return layout.wordKeys(w); }
    SwipeLexicon makeLexicon(std::initializer_list<std::pair<const char *, int>> words) {
        SwipeLexicon lex;
        for (const auto &[w, f] : words) lex.add(w, f, layout);
        lex.finish();
        return lex;
    }
    QString top(const SwipeLexicon &lex, const std::vector<SwipeCandidate> &c, size_t i = 0) {
        return i < c.size() ? QString::fromStdString(lex.text(c[i].word)) : QString();
    }
private slots:
    void initTestCase() { QVERIFY(layout.loadTsv(SWIPEKB_QWERTY_TSV)); QCOMPARE(layout.size(), size_t(26)); }

    void layoutFoldsAccentsAndDropsPunctuation() {
        QCOMPARE(layout.keyOf(U'a'), layout.keyOf(U'A'));
        QCOMPARE(layout.keyOf(0xE9), layout.keyOf(U'e'));   // e-acute
        QCOMPARE(layout.keyOf(0xC9), layout.keyOf(U'e'));   // E-acute
        QCOMPARE(layout.keyOf(0xF1), layout.keyOf(U'n'));   // n-tilde (this layout has no own key)
        QCOMPARE(layout.keyOf(0xFC), layout.keyOf(U'u'));
        QCOMPARE(layout.keyOf(U'\''), -1);
        int raw = 0;
        QCOMPARE(layout.wordKeys("see", &raw).size(), size_t(2)); QCOMPARE(raw, 3);       // repeats merge
        QCOMPARE(layout.wordKeys("can't"), layout.wordKeys("cant"));
        QCOMPARE(layout.wordKeys("caf\xC3\xA9"), layout.wordKeys("cafe"));
        QVERIFY(layout.wordKeys("123").empty());
        QVERIFY(layout.unit() > 9.9f && layout.unit() < 10.1f);
    }
    void layoutWithOwnAccentKeyKeepsIt() {
        SwipeLayout es({{U'n', 0.7f, 0.8f, 0.05f, 0.16f}, {char32_t(0xF1), 0.9f, 0.5f, 0.05f, 0.16f}});
        QVERIFY(es.keyOf(0xF1) != es.keyOf(U'n'));
        QCOMPARE(es.keyOf(0xD1), es.keyOf(0xF1));   // capital N-tilde
    }
    void lexiconDedupesAndSharesNodes() {
        auto lex = makeLexicon({{"cat", 100}, {"car", 120}, {"cart", 90}, {"see", 200}, {"se", 50}, {"The", 250},
                                {"the", 240}, {"can't", 150}, {"cant", 10}, {"1234", 5}});
        // "the" folds into "The" (same word, different capitals); "can't" and "cant" stay apart (same key path);
        // "1234" has no letters.
        QCOMPARE(lex.wordCount(), size_t(8));
        const int32_t n = lex.find(keysOf("see"));
        QVERIFY(n > 0);
        QCOMPARE(lex.node(n).wordEnd - lex.node(n).wordBegin, 2u);   // see + se share one path, kept apart by spelling
        QCOMPARE(int(lex.node(lex.find(keysOf("ca"))).bestFreq), 150);
        QCOMPARE(lex.find(keysOf("cz")), -1);
    }
    void accentTwinsAreKeptApart() {
        auto lex = makeLexicon({{"que", 250}, {"qu\xC3\xA9", 120}, {"o\xC3\xB9", 80}, {"ou", 200}, {"Que", 100}});
        QCOMPARE(lex.wordCount(), size_t(4));   // "Que" is "que" with a capital
        const int32_t n = lex.find(keysOf("que"));
        QCOMPARE(lex.node(n).wordEnd - lex.node(n).wordBegin, 2u);
    }
    void beamFindsTheSpelledWord() {
        auto lex = makeLexicon({{"cat", 100}, {"car", 100}, {"cart", 100}, {"dog", 100}, {"at", 100}});
        SwipeBeamSearch search(lex);
        const int K = int(layout.size());
        auto lp = spell(keysOf("cat"), K);
        auto r = search.decode(lp.data(), int(lp.size() / (K + 1)), K, BeamParams{}, 3);
        QCOMPARE(top(lex, r), QString("cat"));
        lp = spell(keysOf("dog"), K);
        r = search.decode(lp.data(), int(lp.size() / (K + 1)), K, BeamParams{}, 3);
        QCOMPARE(top(lex, r), QString("dog"));
        QVERIFY(r[0].score >= r.back().score);
    }
    void beamDoesNotNeedBlanksBetweenDifferentLetters() {
        auto lex = makeLexicon({{"cat", 100}, {"cut", 100}});
        SwipeBeamSearch search(lex);
        const int K = int(layout.size());
        // c c a a t t with no blank in between (a finger sliding straight from key to key)
        std::vector<int> sym{keysOf("cat")[0] + 1, keysOf("cat")[0] + 1, keysOf("cat")[1] + 1, keysOf("cat")[1] + 1,
                             keysOf("cat")[2] + 1, keysOf("cat")[2] + 1};
        std::vector<float> lp(sym.size() * (K + 1));
        for (size_t t = 0; t < sym.size(); ++t)
            for (int c = 0; c <= K; ++c) lp[t * (K + 1) + c] = std::log(c == sym[t] ? 0.9f : 0.1f / K);
        auto r = search.decode(lp.data(), int(sym.size()), K, BeamParams{}, 2);
        QCOMPARE(top(lex, r), QString("cat"));
    }
    void frequencyBreaksTiesAndSameShapeWordsBothAppear() {
        auto lex = makeLexicon({{"see", 100}, {"se", 180}, {"sea", 60}});
        SwipeBeamSearch search(lex);
        const int K = int(layout.size());
        auto lp = spell(keysOf("se"), K);
        auto r = search.decode(lp.data(), int(lp.size() / (K + 1)), K, BeamParams{}, 5);
        QCOMPARE(top(lex, r, 0), QString("se"));    // higher frequency
        QCOMPARE(top(lex, r, 1), QString("see"));   // same key path, a double letter the shape cannot show
        QCOMPARE(top(lex, r, 2), QString("sea"));   // a different path: only a far-fetched extra letter
    }
    void priorOverridesFrequency() {
        auto lex = makeLexicon({{"see", 100}, {"se", 180}});
        SwipeBeamSearch search(lex);
        const int K = int(layout.size());
        auto lp = spell(keysOf("se"), K);
        auto r = search.decode(lp.data(), int(lp.size() / (K + 1)), K, BeamParams{}, 2,
                               [&](uint32_t w) { return lex.text(w) == "see" ? 0.f : -5.f; });
        QCOMPARE(top(lex, r), QString("see"));
    }
    void neverReturnsAWordOutsideTheLexicon() {
        auto lex = makeLexicon({{"cat", 100}, {"dog", 100}});
        SwipeBeamSearch search(lex);
        const int K = int(layout.size());
        auto lp = spell(keysOf("zzq"), K);
        auto r = search.decode(lp.data(), int(lp.size() / (K + 1)), K, BeamParams{}, 3);
        for (const auto &c : r) QVERIFY(lex.text(c.word) == "cat" || lex.text(c.word) == "dog");
    }
    void emptyInputsAreSafe() {
        SwipeLexicon empty; empty.finish();
        SwipeBeamSearch s0(empty);
        QVERIFY(s0.decode(nullptr, 0, 26, BeamParams{}, 3).empty());
        auto lex = makeLexicon({{"cat", 100}});
        SwipeBeamSearch s1(lex);
        QVERIFY(s1.decode(nullptr, 0, 26, BeamParams{}, 3).empty());
    }
    void sentenceContextChoosesBetweenEquallyShapedWords() {
        // A tiny n-gram model: after "the", "cat" is likely and "car" is not.
        NgramModelData d;
        d.words = {"the", "cat", "car"};   // ids 3,4,5
        d.uni_lp = {-9.f, -9.f, -9.f, -2.f, -3.f, -3.f};
        d.uni_bo = {0, 0, 0, 0, 0, 0};
        d.bi_keys = {NgramLM::key(3, 4)};  // the -> cat
        d.bi_lp = {-0.3f}; d.bi_bo = {0.f};
        const QString path = QDir(QDir::tempPath()).filePath("swipebeam-test.ngram");
        QVERIFY(NgramLM::write(path.toStdString(), d));
        NgramLM lm;
        QVERIFY(lm.load(path.toStdString()));
        auto lex = makeLexicon({{"cat", 100}, {"car", 100}, {"the", 200}});
        NgramWordPrior prior(lm, lex);
        prior.setContext({"the"});
        uint32_t cat = 0, car = 0;
        for (uint32_t w = 0; w < lex.wordCount(); ++w) { if (lex.text(w) == "cat") cat = w; if (lex.text(w) == "car") car = w; }
        QVERIFY(prior(cat) > prior(car));
        const auto hints = prior.hints();
        QVERIFY(std::find(hints.begin(), hints.end(), cat) != hints.end());
        // swipe halfway between cat and car (t and r keys equally likely at the last position)
        SwipeBeamSearch search(lex);
        const int K = int(layout.size());
        auto lp = spell(keysOf("cat"), K, 2, 0.5f);
        const int r = keysOf("car")[2] + 1, t = keysOf("cat")[2] + 1;
        const int steps = int(lp.size() / (K + 1));
        for (int s = 0; s < steps; ++s) {   // make r and t exactly equally likely wherever t was the favourite
            float &a = lp[size_t(s) * (K + 1) + t]; float &b = lp[size_t(s) * (K + 1) + r];
            if (a > b) b = a;
        }
        auto res = search.decode(lp.data(), steps, K, BeamParams{}, 2, [&](uint32_t w) { return prior(w); }, hints);
        QCOMPARE(top(lex, res), QString("cat"));
        prior.setContext({});
        QVERIFY(prior(car) >= -9.f * 2.31f);
        QFile::remove(path);
    }
};

QTEST_APPLESS_MAIN(SwipeBeamTest)
#include "tst_swipebeam.moc"
