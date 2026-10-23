// The runner must reproduce what PyTorch computes for the same weights and trace (fixture written by
// scripts/swipe/make_fixture.py), in both weight formats, and must reject damaged files.
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>
#include <cmath>
#include <fstream>
#include <iterator>

#include "swipekb/swipe_net.hpp"

using namespace swipekb;

class SwipeNetTest : public QObject {
    Q_OBJECT
    QJsonObject c;
    SwipeLayout layout;
    std::vector<SwipePoint> trace;
    static std::vector<float> floats(const QJsonValue &v) {
        std::vector<float> out;
        for (const auto &x : v.toArray()) out.push_back(float(x.toDouble()));
        return out;
    }
    static std::vector<uint8_t> bytes(const QString &path) {
        std::ifstream in(path.toStdString(), std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    static float maxDiff(const std::vector<float> &a, const std::vector<float> &b) {
        if (a.size() != b.size()) return 1e9f;
        float m = 0;
        for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(a[i] - b[i]));
        return m;
    }
private slots:
    void initTestCase() {
        QFile f(QString(SWIPEKB_TEST_DATA) + "/swipe_net/case.json");
        QVERIFY(f.open(QIODevice::ReadOnly));
        c = QJsonDocument::fromJson(f.readAll()).object();
        QVERIFY(layout.loadTsv(SWIPEKB_QWERTY_TSV));
        for (const auto &p : c["points"].toArray()) {
            const auto o = p.toObject();
            trace.push_back({float(o["x"].toDouble()), float(o["y"].toDouble()), o["t"].toDouble()});
        }
    }
    void inputFeaturesMatchPython() {
        SwipeNet net;
        QVERIFY(net.loadBytes(bytes(QString(SWIPEKB_TEST_DATA) + "/swipe_net/model_f32.bin")));
        SwipeInput in;
        QVERIFY(net.buildInput(trace, layout, float(c["aspect"].toDouble()), in));
        QCOMPARE(in.steps, 64);
        QVERIFY2(maxDiff(in.feats, floats(c["feats"])) < 1e-4f, "features differ from the Python pipeline");
        QVERIFY2(maxDiff(in.pos, floats(c["pos"])) < 1e-4f, "positions differ");
        QVERIFY2(maxDiff(in.keys, floats(c["keys"])) < 1e-4f, "key geometry differs");
    }
    void forwardMatchesPyTorchF32() {
        SwipeNet net;
        QVERIFY(net.loadBytes(bytes(QString(SWIPEKB_TEST_DATA) + "/swipe_net/model_f32.bin")));
        SwipeInput in;
        QVERIFY(net.buildInput(trace, layout, float(c["aspect"].toDouble()), in));
        std::vector<float> lp;
        QVERIFY(net.forward(in, lp));
        const auto want = floats(c["logp_f32"]);
        QVERIFY2(maxDiff(lp, want) < 2e-4f, qPrintable(QString("max diff %1").arg(maxDiff(lp, want))));
        // every row is a probability distribution
        const int K = int(layout.size());
        for (int t = 0; t < in.steps; ++t) {
            double s = 0;
            for (int k = 0; k <= K; ++k) s += std::exp(double(lp[size_t(t) * (K + 1) + k]));
            QVERIFY(std::fabs(s - 1.0) < 1e-4);
        }
    }
    void forwardMatchesPyTorchF16Weights() {
        SwipeNet net;
        QVERIFY(net.loadBytes(bytes(QString(SWIPEKB_TEST_DATA) + "/swipe_net/model_f16.bin")));
        SwipeInput in;
        QVERIFY(net.buildInput(trace, layout, float(c["aspect"].toDouble()), in));
        std::vector<float> lp;
        QVERIFY(net.forward(in, lp));
        QVERIFY2(maxDiff(lp, floats(c["logp_f16"])) < 2e-4f, "half-precision weights differ");
    }
    void rejectsDamagedFiles() {
        auto good = bytes(QString(SWIPEKB_TEST_DATA) + "/swipe_net/model_f32.bin");
        SwipeNet net;
        auto cut = good; cut.resize(good.size() - 7);
        QVERIFY(!net.loadBytes(cut)); QVERIFY(!net.ready());
        auto extra = good; extra.push_back(0);
        QVERIFY(!net.loadBytes(extra));
        auto magic = good; magic[0] = 'X';
        QVERIFY(!net.loadBytes(magic));
        auto version = good; version[4] = 2;
        QVERIFY(!net.loadBytes(version));
        QVERIFY(!net.loadBytes({}));
        QVERIFY(!net.load("/nonexistent/model.bin"));
        QVERIFY(net.loadBytes(good));
    }
    void tapAndEmptyTraces() {
        SwipeNet net;
        QVERIFY(net.loadBytes(bytes(QString(SWIPEKB_TEST_DATA) + "/swipe_net/model_f32.bin")));
        SwipeInput in;
        QVERIFY(!net.buildInput({}, layout, 0.4f, in));
        QVERIFY(net.buildInput({{0.5f, 0.5f, 100.0}}, layout, 0.4f, in));   // single touch: all steps at one place
        std::vector<float> lp;
        QVERIFY(net.forward(in, lp));
        for (float v : lp) QVERIFY(std::isfinite(v));
    }
};

QTEST_APPLESS_MAIN(SwipeNetTest)
#include "tst_swipenet.moc"
