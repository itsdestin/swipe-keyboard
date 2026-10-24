// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// The no-network fallback decoder, on traces drawn through key centres (with and without sloppiness).
#include <QtTest>
#include <cmath>
#include <random>

#include "swipekb/swipe_geo.hpp"

using namespace swipekb;

class SwipeGeoTest : public QObject {
    Q_OBJECT
    SwipeLayout layout;
    SwipeLexicon lex;
    const float aspect = 0.4f;

    // Straight segments between the key centres of `word`, sampled every ~0.1 key width, `noise` key widths of jitter
    // on every waypoint (a sloppy swiper), 16 ms per sample.
    std::vector<SwipePoint> trace(const char *word, float noise = 0.f, unsigned seed = 1) {
        std::mt19937 rng(seed);
        std::normal_distribution<float> g(0.f, 1.f);
        const auto keys = layout.wordKeys(word);
        std::vector<std::pair<float, float>> way;
        for (int k : keys) way.push_back({layout.keys()[k].cx + noise * g(rng) / layout.unit(),
                                          layout.keys()[k].cy + noise * g(rng) / (layout.unit() * aspect)});
        std::vector<SwipePoint> out;
        double t = 0;
        if (way.size() == 1) { out.push_back({way[0].first, way[0].second, t}); return out; }
        for (size_t i = 1; i < way.size(); ++i) {
            const float dx = (way[i].first - way[i - 1].first) * layout.unit(), dy = (way[i].second - way[i - 1].second) * layout.unit() * aspect;
            const int n = std::max(2, int(std::hypot(dx, dy) / 0.1f));
            for (int j = (i == 1 ? 0 : 1); j <= n; ++j) {
                const float f = float(j) / n;
                out.push_back({way[i - 1].first + f * (way[i].first - way[i - 1].first), way[i - 1].second + f * (way[i].second - way[i - 1].second), t});
                t += 16;
            }
        }
        return out;
    }
    QString best(const std::vector<SwipePoint> &t, size_t rank = 0) {
        GeoDecoder d(lex, layout);
        const auto r = d.decode(t, aspect, 5);
        return rank < r.size() ? QString::fromStdString(lex.text(r[rank].word)) : QString();
    }
private slots:
    void initTestCase() {
        QVERIFY(layout.loadTsv(SWIPEKB_QWERTY_TSV));
        for (const auto &[w, f] : std::vector<std::pair<const char *, int>>{
                 {"hello", 200}, {"help", 180}, {"hell", 150}, {"world", 190}, {"word", 200}, {"would", 210}, {"the", 250},
                 {"then", 220}, {"there", 230}, {"see", 150}, {"se", 90}, {"caf\xC3\xA9", 100}, {"a", 240}, {"i", 245},
                 {"quick", 150}, {"brown", 140}, {"fox", 120}, {"jumps", 100}})
            lex.add(w, f, layout);
        lex.finish();
    }
    void exactTracesFindTheirWord() {
        for (const char *w : {"hello", "world", "the", "quick", "brown", "fox", "jumps"})
            QCOMPARE(best(trace(w)), QString(w));
    }
    void sloppyTracesStillFindTheirWord() {
        int ok = 0, n = 0;
        for (const char *w : {"hello", "world", "there", "quick", "brown", "jumps", "would"})
            for (unsigned s = 1; s <= 5; ++s, ++n) ok += best(trace(w, 0.12f, s)) == QString(w);
        QVERIFY2(ok >= n * 0.85, qPrintable(QString("only %1 of %2").arg(ok).arg(n)));
    }
    void similarPathsAreToldApart() {
        QCOMPARE(best(trace("help")), QString("help"));
        QCOMPARE(best(trace("hell")), QString("hell"));
        QCOMPARE(best(trace("word")), QString("word"));
    }
    void frequencyBreaksTiesBetweenIdenticalPaths() {
        QCOMPARE(best(trace("see")), QString("see"));   // same key path as "se"; more frequent
        QCOMPARE(best(trace("see"), 1), QString("se"));
    }
    void accentedWordIsMatchedOnItsBaseLetters() { QCOMPARE(best(trace("cafe")), QString("caf\xC3\xA9")); }
    void singleTouchSelectsThatKeysWord() {
        QCOMPARE(best(trace("a")), QString("a"));
        QCOMPARE(best(trace("i")), QString("i"));
    }
    void emptyAndDegenerateInputsAreSafe() {
        GeoDecoder d(lex, layout);
        QVERIFY(d.decode({}, aspect, 3).empty());
        QVERIFY(d.decode(trace("the"), aspect, 0).empty());
        SwipeLexicon none; none.finish();
        GeoDecoder e(none, layout);
        QVERIFY(e.decode(trace("the"), aspect, 3).empty());
    }
    void scoresAreOrderedBestFirst() {
        GeoDecoder d(lex, layout);
        const auto r = d.decode(trace("hello", 0.1f), aspect, 5);
        QVERIFY(r.size() >= 2);
        for (size_t i = 1; i < r.size(); ++i) QVERIFY(r[i - 1].score >= r[i].score);
    }
};

QTEST_APPLESS_MAIN(SwipeGeoTest)
#include "tst_swipegeo.moc"
