// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include <QtTest>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include "swipekb/ngram.hpp"

// Runs swipekb-nextword-eval on a hand-made 4-word model where every ranking is known, and checks the
// counts: sentence-start context, punctuation stripping, top-1/top-3 and the vocabulary share.

namespace {
// Words 3.. = "yo", "soy", "estoy", "aquí". "<s> yo" and "yo soy" are strong; "aquí" is the weakest word.
swipekb::NgramModelData tinyModel()
{
    using swipekb::NgramLM;
    swipekb::NgramModelData d;
    d.words = {"yo", "soy", "estoy", "aquí"};
    d.uni_lp = {-2.f, -99.f, -1.5f, -1.0f, -1.2f, -1.3f, -1.9f};
    d.uni_bo = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    d.bi_keys = {NgramLM::key(NgramLM::kBos, 3), NgramLM::key(3, 4)};
    d.bi_lp = {-0.1f, -0.1f};
    d.bi_bo = {0.f, 0.f};
    return d;
}

bool write(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
}

class NextWordEvalTest : public QObject {
    Q_OBJECT
private slots:
    void countsMatchKnownRanking()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString model = dir.filePath("es.ngram"), vocab = dir.filePath("es.vocab.tsv");
        QVERIFY(swipekb::NgramLM::write(model.toStdString(), tinyModel()));
        QVERIFY(write(vocab, "yo\t200\tyo\nsoy\t180\tsoy\nestoy\t170\testoy\naquí\t90\taquí\n"));
        // "Yo" (sentence start, rank 1), "soy," (rank 1 after "yo"), "aquí." (rank 4: outside top 3),
        // "¿" (punctuation only: not scored), "zorro" (unknown: scored, never suggested).
        QVERIFY(write(dir.filePath("s.txt"), "Yo soy, aquí.\n\n¿ zorro\n"));
        QProcess p;
        p.start(QStringLiteral(SWIPEKB_NEXTWORD_EVAL_EXE), {"--model", model, "--vocab", vocab,
            "--sentences", dir.filePath("s.txt"), "--label", "tiny"});
        QVERIFY(p.waitForFinished(30000));
        QCOMPARE(p.exitCode(), 0);
        const QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines[0].startsWith("label\tsentences\twords\ttop1%"));
        const QStringList row = lines[1].split('\t');
        QCOMPARE(row.mid(0, 9), QStringList({"tiny", "2", "4", "50.00", "50.00", "75.00", "50.00", "50.00", "75.00"}));
    }
    void refusesMissingModel()
    {
        QProcess p;
        p.start(QStringLiteral(SWIPEKB_NEXTWORD_EVAL_EXE), {"--model", "/missing", "--vocab", "/missing", "--sentences", "/missing"});
        QVERIFY(p.waitForFinished(30000));
        QCOMPARE(p.exitCode(), 1);
    }
};

QTEST_GUILESS_MAIN(NextWordEvalTest)
#include "tst_nextwordeval.moc"
