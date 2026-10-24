// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// The engine as the keyboard will use it: loads pieces, copes with missing/damaged model files, and always answers.
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <thread>

#include "swipekb/swipe_engine.hpp"

using namespace swipekb;

class SwipeEngineTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString words;
    std::vector<SwipePoint> trace(const SwipeEngine &e, const char *word) {
        std::vector<SwipePoint> out;
        double t = 0;
        const auto keys = e.layout().wordKeys(word);
        for (size_t i = 0; i < keys.size(); ++i) {
            const auto &k = e.layout().keys()[keys[i]];
            if (i) {
                const auto &p = e.layout().keys()[keys[i - 1]];
                for (int j = 1; j < 6; ++j) { out.push_back({p.cx + (k.cx - p.cx) * j / 6, p.cy + (k.cy - p.cy) * j / 6, t}); t += 16; }
            }
            out.push_back({k.cx, k.cy, t}); t += 16;
        }
        return out;
    }
private slots:
    void initTestCase() {
        QVERIFY(dir.isValid());
        words = dir.filePath("words.tsv");
        QFile f(words);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hello\t200\nhelp\t150\nworld\t190\nthe\t250\nquick\t120\nbrown\t100\nfox\t90\n");
    }
    void notReadyUntilLoaded() {
        SwipeEngine e;
        QVERIFY(!e.ready());
        QVERIFY(e.decode({{0.5f, 0.5f, 0}}, 0.4f, 3).empty());
        QVERIFY(e.loadLayout(SWIPEKB_QWERTY_TSV));
        QVERIFY(!e.ready());
        QCOMPARE(e.loadWords(words.toStdString()), size_t(7));
        QVERIFY(e.ready());
        QVERIFY(!e.hasModel());
    }
    void withoutModelTheGeometricDecoderAnswers() {
        SwipeEngine e;
        QVERIFY(e.loadLayout(SWIPEKB_QWERTY_TSV)); e.loadWords(words.toStdString());
        const auto r = e.decode(trace(e, "world"), 0.4f, 3);
        QVERIFY(!r.empty());
        QCOMPARE(QString::fromStdString(e.lexicon().text(r[0].word)), QString("world"));
    }
    void damagedOrMissingModelKeepsSwipingWorking() {
        SwipeEngine e;
        QVERIFY(e.loadLayout(SWIPEKB_QWERTY_TSV)); e.loadWords(words.toStdString());
        QVERIFY(!e.loadModel("/nonexistent/model.bin"));
        QFile bad(dir.filePath("bad.bin")); QVERIFY(bad.open(QIODevice::WriteOnly)); bad.write("SKNN garbage"); bad.close();
        QVERIFY(!e.loadModel(bad.fileName().toStdString()));
        QVERIFY(!e.hasModel());
        const auto r = e.decode(trace(e, "quick"), 0.4f, 1);
        QCOMPARE(QString::fromStdString(e.lexicon().text(r.at(0).word)), QString("quick"));
    }
    void modelPathAlwaysYieldsWordsFromTheLexicon() {
        SwipeEngine e;
        QVERIFY(e.loadLayout(SWIPEKB_QWERTY_TSV)); e.loadWords(words.toStdString());
        QVERIFY(e.loadModel(QString(SWIPEKB_TEST_DATA "/swipe_net/model_f32.bin").toStdString()));
        QVERIFY(e.hasModel());
        // The fixture network is random, so we cannot ask for a particular word: only for a sane answer.
        const auto r = e.decode(trace(e, "hello"), 0.4f, 3);
        QVERIFY(!r.empty() && r.size() <= 3);
        for (const auto &c : r) QVERIFY(c.word < e.lexicon().wordCount());
        for (size_t i = 1; i < r.size(); ++i) QVERIFY(r[i - 1].score >= r[i].score);
        e.params.geoWeight = 0;   // the network-only path must work too
        QVERIFY(!e.decode(trace(e, "the"), 0.4f, 3).empty());
    }
    void copiesAreIndependentAndThreadSafeToUse() {
        SwipeEngine e;
        QVERIFY(e.loadLayout(SWIPEKB_QWERTY_TSV)); e.loadWords(words.toStdString());
        QVERIFY(e.loadModel(QString(SWIPEKB_TEST_DATA "/swipe_net/model_f32.bin").toStdString()));
        const auto t = trace(e, "brown");
        std::vector<SwipeCandidate> a, b;
        SwipeEngine e1(e), e2(e);
        std::thread th1([&] { for (int i = 0; i < 20; ++i) a = e1.decode(t, 0.4f, 3); });
        std::thread th2([&] { for (int i = 0; i < 20; ++i) b = e2.decode(t, 0.4f, 3); });
        th1.join(); th2.join();
        QVERIFY(!a.empty());
        QCOMPARE(a.size(), b.size());
        for (size_t i = 0; i < a.size(); ++i) QCOMPARE(a[i].word, b[i].word);
    }
    void priorChangesTheNetworkRanking() {
        SwipeEngine e;
        QVERIFY(e.loadLayout(SWIPEKB_QWERTY_TSV)); e.loadWords(words.toStdString());
        QVERIFY(e.loadModel(QString(SWIPEKB_TEST_DATA "/swipe_net/model_f32.bin").toStdString()));
        e.params.geoWeight = 0;
        const auto t = trace(e, "hello");
        const auto base = e.decode(t, 0.4f, 5);
        QVERIFY(base.size() >= 2);
        const uint32_t last = base.back().word;
        e.setPrior([&](uint32_t w) { return w == last ? 1000.f : 0.f; });
        QCOMPARE(e.decode(t, 0.4f, 5).at(0).word, last);   // a strong enough prior wins
    }
};

QTEST_APPLESS_MAIN(SwipeEngineTest)
#include "tst_swipeengine.moc"
