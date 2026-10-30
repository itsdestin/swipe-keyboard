// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// The owner's recordings reader (tools/recordings.h) and swipekb-swap-bench --recordings: which word each recorded
// swipe is taken to be, and that reading the recordings never writes anything or prints a word.
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include "../recordings.h"

namespace {
// A made-up recording: keys a..z on a grid, a straight path. Words "zebra"/"quokka" are never in the real output.
QByteArray swipeLine(qint64 ts, const QString &typed)
{
    QJsonArray keys, points;
    const QString rows[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < rows[r].size(); ++c)
            keys.append(QJsonObject{{"letter", QString(rows[r][c])}, {"x", c * 40.0 + r * 20}, {"y", r * 50.0}, {"w", 40.0}, {"h", 50.0}});
    for (int i = 0; i < 10; ++i) points.append(QJsonObject{{"x", 20.0 + i * 30}, {"y", 25.0 + (i % 3) * 40}, {"t", i * 20.0}});
    return QJsonDocument(QJsonObject{{"ts", ts}, {"type", "swipe"}, {"points", points}, {"keys", keys},
                                     {"context", QJsonArray{"quokka"}}, {"candidates", QJsonArray{typed}}, {"typed", typed}})
               .toJson(QJsonDocument::Compact) + '\n';
}
QByteArray line(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n'; }
QByteArray fixture()
{
    QByteArray d;
    d += swipeLine(1, "zebra");                                                   // picked twice: the last pick wins
    d += line({{"ts", 2}, {"type", "pick"}, {"swipe_ts", 1}, {"word", "zebras"}});
    d += line({{"ts", 3}, {"type", "pick"}, {"swipe_ts", 1}, {"word", "zebrine"}});
    d += swipeLine(10, "zebu");                                                    // erased: cannot be replayed
    d += line({{"ts", 11}, {"type", "erase"}, {"swipe_ts", 10}});
    d += swipeLine(20, "zest");                                                    // kept: assumed right
    d += line({{"ts", 21}, {"type", "keep"}, {"swipe_ts", 20}});
    d += swipeLine(30, "zone");                                                    // no outcome: unknown
    d += swipeLine(40, "");                                                        // nothing typed
    d += "not json\n";
    return d;
}
// Every file under `dir` with its size and change time.
QStringList snapshot(const QString &dir)
{
    QStringList out;
    QDirIterator it(dir, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo fi(it.next());
        out << fi.filePath() + ' ' + QString::number(fi.size()) + ' ' + QString::number(fi.lastModified().toMSecsSinceEpoch());
    }
    out.sort();
    return out;
}
}

class TstRecordings : public QObject { Q_OBJECT
private slots:
    void truthRules() {
        recordings::Counts c;
        const auto swipes = recordings::parse(fixture(), c);
        QCOMPARE(c.swipes, size_t(5)); QCOMPARE(c.picked, size_t(1)); QCOMPARE(c.erased, size_t(1));
        QCOMPARE(c.kept, size_t(1)); QCOMPARE(c.unknown, size_t(1)); QCOMPARE(c.empty, size_t(1));
        QCOMPARE(swipes.size(), size_t(2));
        QCOMPARE(swipes[0].word, std::string("zebrine"));
        QCOMPARE(swipes[1].word, std::string("zest"));
        QCOMPARE(swipes[0].before, std::vector<std::string>{"quokka"});
        QCOMPARE(swipes[0].keys.size(), size_t(26));
        QCOMPARE(QString::fromStdString(c.format()),
                 QStringLiteral("recorded: swipes=5 picked=1 erased=1 kept=1 unknown=1 empty=1\n"));
    }
    void missingFileIsAnError() {
        recordings::Counts c; std::string err;
        QVERIFY(recordings::read("/nonexistent/swipes.jsonl", c, &err).empty());
        QVERIFY(!err.empty());
    }
    // The bench on recordings: options that write or print words are refused, and an ordinary run leaves every folder
    // as it was (the recordings' folder is read-only here, HOME and XDG point at an empty folder) and prints no word.
    void benchOnlyReads() {
        QTemporaryDir home, data;
        QVERIFY(home.isValid() && data.isValid());
        const QString traces = data.path() + "/traces";
        QVERIFY(QDir().mkpath(traces));
        QFile f(traces + "/swipes.jsonl");
        QVERIFY(f.open(QIODevice::WriteOnly)); f.write(fixture()); f.close();
        QFile(data.path() + "/record").open(QIODevice::WriteOnly);
        QFile::setPermissions(traces, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        QFile::setPermissions(data.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        const QStringList before = snapshot(data.path()), homeBefore = snapshot(home.path());
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *k : {"HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "SWIPEKB_DATA"})
            env.insert(k, home.path());
        auto run = [&](const QStringList &extra, QByteArray *out = nullptr) {
            const QString assets = qEnvironmentVariable("SWIPEKB_ASSETS");
            QStringList args{"--mode", "new", "--dict", assets + "/language-files/en_swipe.combined", "--model",
                             assets + "/language-files/swipe-model.bin", "--language", "en", "--recordings", f.fileName()};
            QProcess p; p.setProcessEnvironment(env); p.setWorkingDirectory(home.path());
            p.start(SWIPEKB_SWAP_BENCH_EXE, args + extra);
            p.waitForFinished(120000);
            if (out) *out = p.readAllStandardOutput() + p.readAllStandardError();
            return p.exitCode();
        };
        const QString junk = home.path() + "/junk.tsv", diag = home.path() + "/diag.jsonl";
        QCOMPARE(run({"--junk-out", junk}), 2);
        QCOMPARE(run({"--diagnose-out", diag}), 2);
        QCOMPARE(run({"--diagnose"}), 2);
        QVERIFY(!QFileInfo::exists(junk) && !QFileInfo::exists(diag));
        if (!QFileInfo::exists(qEnvironmentVariable("SWIPEKB_ASSETS") + "/language-files/en_swipe.combined")) {
            QFile::setPermissions(data.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
            QFile::setPermissions(traces, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
            QSKIP("SWIPEKB_ASSETS has no English word list; the full run is skipped");
        }
        QByteArray out;
        QCOMPARE(run({}, &out), 0);
        QVERIFY2(out.contains("recorded: swipes=5 picked=1 erased=1 kept=1 unknown=1 empty=1"), out.constData());
        QVERIFY(out.contains("ux: swipes=2 "));
        for (const char *w : {"zebr", "zest", "zebu", "zone", "quokka"}) QVERIFY2(!out.contains(w), w);
        QCOMPARE(snapshot(data.path()), before);
        QCOMPARE(snapshot(home.path()), homeBefore);
        // WHY put back: QTemporaryDir must be able to remove the folders.
        QFile::setPermissions(data.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QFile::setPermissions(traces, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
};
QTEST_GUILESS_MAIN(TstRecordings)
#include "tst_recordings.moc"
