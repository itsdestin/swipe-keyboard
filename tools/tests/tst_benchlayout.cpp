#include <QtTest>

#include "benchlayout.h"

// WHY: the scoreboard is only trustworthy if the key rectangles we build from FUTO's layout file come back
// out of our normalisation at exactly FUTO's key centres (otherwise the engine sees a different keyboard than
// FUTO's own tool does, and the "parity" number is wrong).
class TstBenchLayout : public QObject {
    Q_OBJECT
private slots:
    void normalisedCentresMatchLayoutFile()
    {
        std::vector<benchlayout::LayoutKey> layout;
        QVERIFY(benchlayout::loadLayout(QStringLiteral(SWIPEKB_LAYOUT_JSON), layout));
        QCOMPARE(layout.size(), size_t(26));
        const auto n = swipekb::normalize({{0, 0, 0}, {1, 1, 5}}, benchlayout::toRects(layout, 1000.f));
        QVERIFY(n.has_value());
        for (const auto &k : layout) {
            const int i = k.letter - 'a';
            QVERIFY2(std::fabs(n->key_cx[i] - k.cx) < 1e-3, qPrintable(QString("cx of %1").arg(k.letter)));
            QVERIFY2(std::fabs(n->key_cy[i] - k.cy) < 1e-3, qPrintable(QString("cy of %1").arg(k.letter)));
        }
        // And the trace itself is identity-scaled: a point at 500 px is 0.5 of the letter area.
        const auto m = swipekb::normalize({{500, 250, 0}, {1000, 1000, 5}}, benchlayout::toRects(layout, 1000.f));
        QVERIFY(m.has_value());
        QVERIFY(std::fabs(m->x[0] - 0.5) < 1e-3 && std::fabs(m->y[0] - 0.25) < 1e-3);
    }
};

QTEST_APPLESS_MAIN(TstBenchLayout)
#include "tst_benchlayout.moc"
