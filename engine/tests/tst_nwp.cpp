// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// The next-word neural model (card #170): the C++ runner must reproduce PyTorch's numbers for the same weights
// (tests/data/nwp-tiny*.{nwp.bin,expected}, written by scripts/lm/nwp/make-fixture.py), must refuse damaged
// files, and the hybrid predictor must blend, complete, reuse its sentence state and fall back as designed.
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <fstream>

#include "swipekb/ngram.hpp"
#include "swipekb/nwp_model.hpp"
#include "swipekb/nwp_predictor.hpp"

using namespace swipekb;

namespace {
const QString kData = QStringLiteral(SWIPEKB_TEST_DATA);

struct Expected { std::vector<uint32_t> tokens; std::vector<float> logp; };
std::vector<Expected> readExpected(const QString &path)
{
    std::vector<Expected> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty() || line[0] == '#') continue;
        const auto halves = line.split(QStringLiteral(" ; "));
        Expected e;
        for (const auto &t : halves[0].split(' ', Qt::SkipEmptyParts)) e.tokens.push_back(t.toUInt());
        for (const auto &v : halves[1].split(' ', Qt::SkipEmptyParts)) e.logp.push_back(v.toFloat());
        out.push_back(std::move(e));
    }
    return out;
}
QByteArray readAll(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
bool writeAll(const QString &path, const QByteArray &b) { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(b) == b.size(); }
}

class NwpTest : public QObject {
    Q_OBJECT
    // The 30 words of the fixture, as a keyboard pack: vocab.tsv ("alpha<TAB>frequency<TAB>display") and an
    // n-gram file in which "the" and "of" are by far the most common words.
    static const QStringList words()
    {
        return {"the", "of", "and", "a", "to", "in", "is", "you", "that", "it", "he", "was", "for", "on", "are", "as",
                "with", "his", "they", "at", "be", "this", "have", "from", "or", "one", "had", "by", "word", "but"};
    }
    void makePack(const QString &dir, bool withLm, const char *fixture = "nwp-tiny")
    {
        QByteArray vocab;
        NgramModelData d;
        d.uni_lp = {-2.f, -99.f, -1.5f};
        d.uni_bo = {0.f, 0.f, 0.f};
        int rank = 0;
        for (const auto &w : words()) {
            vocab += w.toUtf8() + "\t100\t" + w.toUtf8() + "\n";
            d.words.push_back(w.toStdString());
            d.uni_lp.push_back(-1.0f - 0.1f * float(rank++));   // "the" most common ... "but" least
            d.uni_bo.push_back(0.f);
        }
        QVERIFY(writeAll(dir + "/xx.vocab.tsv", vocab));
        QVERIFY(NgramLM::write((dir + "/xx.ngram").toStdString(), d));
        if (withLm) QVERIFY(QFile::copy(kData + "/" + fixture + ".nwp.bin", dir + "/xx.nwp.bin"));
    }
private slots:
    void matchesPyTorchFloat() { check("nwp-tiny", 1e-4f); }
    // The expected numbers come from the same rounded weights; the remaining difference is the 13-bit rounding of
    // the vectors the runner multiplies by (about 1e-4 of their largest entry), hence the looser bound.
    void matchesPyTorchInt8() { check("nwp-tiny8", 1e-3f); }

    void refusesDamagedFiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray good = readAll(kData + "/nwp-tiny8.nwp.bin");
        QVERIFY(good.size() > 100);
        NwpModel m;
        QVERIFY(!m.load((dir.path() + "/missing.nwp.bin").toStdString()));
        const auto loads = [&](const QByteArray &bytes) {
            const QString p = dir.path() + "/x.nwp.bin";
            writeAll(p, bytes);
            NwpModel model;
            return model.load(p.toStdString());
        };
        QVERIFY(loads(good));
        QVERIFY(!loads(good.left(good.size() - 1)));                      // truncated
        QVERIFY(!loads(good + QByteArray(1, 'x')));                       // padded
        QByteArray flipped = good; flipped[good.size() / 2] ^= 0x01;
        QVERIFY(!loads(flipped));                                          // one wrong bit in the weights
        QByteArray magic = good; magic[0] = 'X';
        QVERIFY(!loads(magic));
        QVERIFY(!loads(QByteArray()));
    }

    void withoutModelFileIsNgramOnly()
    {
        QTemporaryDir dir;
        makePack(dir.path(), false);
        NwpPredictor p;
        QVERIFY(p.load((dir.path() + "/xx.ngram").toStdString(), (dir.path() + "/xx.vocab.tsv").toStdString()));
        QVERIFY(!p.hasLm());
        NgramPredictor plain;
        QVERIFY(plain.load((dir.path() + "/xx.ngram").toStdString(), (dir.path() + "/xx.vocab.tsv").toStdString()));
        QCOMPARE(p.predict({"the"}, 3), plain.predict({"the"}, 3));
    }

    void damagedModelFileFallsBackToNgram()
    {
        QTemporaryDir dir;
        makePack(dir.path(), true);
        QByteArray bytes = readAll(dir.path() + "/xx.nwp.bin");
        bytes[bytes.size() / 2] ^= 0x10;
        writeAll(dir.path() + "/xx.nwp.bin", bytes);
        NwpPredictor p;
        QVERIFY(p.load((dir.path() + "/xx.ngram").toStdString(), (dir.path() + "/xx.vocab.tsv").toStdString()));
        QVERIFY(p.ready());
        QVERIFY(!p.hasLm());
        QVERIFY(!p.predict({}, 3).empty());
    }

    // lambda 1 = the model alone: the order must be the model's own probabilities (PyTorch's numbers).
    // lambda 0 = the n-gram alone. This is what proves the blend really uses both opinions.
    void blendEndpoints()
    {
        QTemporaryDir dir;
        makePack(dir.path(), true);
        NwpPredictor p;
        QVERIFY(p.load((dir.path() + "/xx.ngram").toStdString(), (dir.path() + "/xx.vocab.tsv").toStdString()));
        QVERIFY(p.hasLm());
        const auto expected = readExpected(kData + "/nwp-tiny.expected");
        const auto &e = expected[2];   // tokens 1 7 12 3 9 = <s> to it the is
        std::vector<std::string> context = {"to", "it", "the", "is"};
        std::vector<std::pair<float, QString>> order;
        for (int i = 0; i < words().size(); ++i) order.emplace_back(-e.logp[i + 3], words()[i]);
        std::sort(order.begin(), order.end());
        p.setBlend(1.f, 0.f);
        const auto lmOnly = p.predict(context, 3);
        QCOMPARE(lmOnly.size(), size_t(3));
        for (int i = 0; i < 3; ++i) QCOMPARE(QString::fromStdString(lmOnly[i]), order[i].second);
        p.setBlend(0.f, 0.f);
        p.disableLm();
        NgramPredictor plain;
        QVERIFY(plain.load((dir.path() + "/xx.ngram").toStdString(), (dir.path() + "/xx.vocab.tsv").toStdString()));
        QCOMPARE(p.predict(context, 3), plain.predict(context, 3));
        // The model's order differs from the n-gram's, otherwise this test would prove nothing.
        QVERIFY(lmOnly != plain.predict(context, 3));
    }

    void completionOnlyOffersMatchingWords()
    {
        QTemporaryDir dir;
        makePack(dir.path(), true);
        NwpPredictor p;
        QVERIFY(p.load((dir.path() + "/xx.ngram").toStdString(), (dir.path() + "/xx.vocab.tsv").toStdString()));
        const auto got = p.predict({"he", "was"}, 5, "t");
        QVERIFY(!got.empty());
        for (const auto &w : got) QVERIFY2(w[0] == 't', w.c_str());
        QVERIFY(std::find(got.begin(), got.end(), "the") != got.end());
        QVERIFY(p.predict({"he"}, 3, "zzz").empty());
    }

    // The predictor remembers the sentence state; growing a sentence one word at a time must give exactly
    // what a fresh predictor gives for the whole sentence.
    void reusedStateEqualsFreshState()
    {
        QTemporaryDir dir;
        makePack(dir.path(), true);
        const std::vector<std::string> sentence = {"he", "was", "on", "the", "one", "that", "had", "a", "word"};
        NwpPredictor growing;
        QVERIFY(growing.load((dir.path() + "/xx.ngram").toStdString(), (dir.path() + "/xx.vocab.tsv").toStdString()));
        growing.setBlend(0.9f, 0.f);
        for (size_t n = 0; n <= sentence.size(); ++n) {
            const std::vector<std::string> context(sentence.begin(), sentence.begin() + n);
            NwpPredictor fresh;
            QVERIFY(fresh.load((dir.path() + "/xx.ngram").toStdString(), (dir.path() + "/xx.vocab.tsv").toStdString()));
            fresh.setBlend(0.9f, 0.f);
            QCOMPARE(growing.predict(context, 5), fresh.predict(context, 5));
            QCOMPARE(growing.predict(context, 5, "a"), fresh.predict(context, 5, "a"));   // typing letters
        }
        // A different sentence afterwards (cursor moved elsewhere) must not inherit the old state.
        NwpPredictor fresh;
        QVERIFY(fresh.load((dir.path() + "/xx.ngram").toStdString(), (dir.path() + "/xx.vocab.tsv").toStdString()));
        fresh.setBlend(0.9f, 0.f);
        QCOMPARE(growing.predict({"is", "it"}, 5), fresh.predict({"is", "it"}, 5));
    }

private:
    void check(const char *name, float tolerance)
    {
        NwpModel m;
        QVERIFY(m.load((kData + "/" + name + ".nwp.bin").toStdString()));
        QCOMPARE(m.vocabSize(), 33u);
        QVERIFY(std::fabs(m.lambda() - 0.7f) < 1e-6f && std::fabs(m.oovBonus() + 0.25f) < 1e-6f);
        QCOMPARE(m.token("word"), 31u);
        QCOMPARE(m.token("nonsense"), 0u);
        const auto expected = readExpected(kData + "/" + name + ".expected");
        QVERIFY(expected.size() >= 5);
        float worst = 0;
        for (const auto &e : expected) {
            auto state = m.start();
            for (uint32_t t : e.tokens) m.step(state, t);
            std::vector<float> got;
            m.logProbs(state, got);
            QCOMPARE(got.size(), e.logp.size());
            for (size_t i = 0; i < got.size(); ++i) worst = std::max(worst, std::fabs(got[i] - e.logp[i]));
        }
        QVERIFY2(worst < tolerance, qPrintable(QString::number(worst)));
    }
};

QTEST_APPLESS_MAIN(NwpTest)
#include "tst_nwp.moc"
