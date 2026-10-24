#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>

// Runs swipekb-replay on a tiny synthetic recording (clean swipes through key centers, the same shape as
// engine/tests/test_language_engine.cpp) and checks that the table and summary print and that the counts add up.

static QString assets() { return qEnvironmentVariable("SWIPEKB_ASSETS"); }

struct Key { char letter; double x, y, w, h; };

static QList<Key> qwerty()
{
    const char *rows[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    const double shift[3] = {0, 50, 150};
    QList<Key> keys;
    for (int r = 0; r < 3; ++r)
        for (int i = 0; rows[r][i]; ++i)
            keys.append({rows[r][i], shift[r] + i * 100.0, r * 100.0, 100.0, 100.0});
    return keys;
}

// One line in the recorder's format (see plugin/swiperecorder.h): straight lines between letter centers,
// 12 points per segment, 8 ms apart.
// `type` empty = an older recording without the "type" field.
static QByteArray swipeLine(const QString &word, const QList<Key> &keys, const QString &type = {})
{
    auto center = [&](QChar c) {
        for (const Key &k : keys)
            if (k.letter == c.toLatin1())
                return QPointF(k.x + k.w / 2, k.y + k.h / 2);
        return QPointF();
    };
    QJsonArray points;
    double t = 0;
    for (int i = 0; i + 1 < word.size(); ++i) {
        const QPointF a = center(word[i]), b = center(word[i + 1]);
        for (int s = 0; s < 12; ++s, t += 8) {
            const double f = s / 12.0;
            points.append(QJsonObject{{"x", a.x() + (b.x() - a.x()) * f}, {"y", a.y() + (b.y() - a.y()) * f}, {"t", t}});
        }
    }
    const QPointF e = center(word.back());
    points.append(QJsonObject{{"x", e.x()}, {"y", e.y()}, {"t", t}});
    QJsonArray keyArr;
    for (const Key &k : keys)
        keyArr.append(QJsonObject{{"letter", QString(QChar(k.letter))}, {"x", k.x}, {"y", k.y}, {"w", k.w}, {"h", k.h}});
    QJsonObject o{{"ts", 1}, {"points", points}, {"keys", keyArr}, {"context", QJsonArray()},
                  {"candidates", QJsonArray{word}}, {"typed", word}};
    if (!type.isEmpty())
        o.insert("type", type);
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

class TstReplay : public QObject {
    Q_OBJECT
private slots:
    void badArgumentsPrintUsage()
    {
        QProcess p;
        p.start(QStringLiteral(SWIPEKB_REPLAY_EXE), {"--data"});
        QVERIFY(p.waitForFinished(30000));
        QCOMPARE(p.exitCode(), 2);
        QVERIFY(p.readAllStandardError().contains("usage"));
    }

    // Regression: `o="--no-model --no-ngram"; swipekb-replay ... $o 2>/dev/null` in zsh printed nothing and
    // looked like a crash. zsh passed both options as ONE argument; the tool refused it with only the usage
    // text on stderr. It must now name the refused argument and explain the space.
    void joinedOptionsAreNamedNotSilent()
    {
        QProcess p;
        p.start(QStringLiteral(SWIPEKB_REPLAY_EXE),
                {"--data", "d", "--swipes", "s", "--expect", "e", "--no-model --no-ngram"});
        QVERIFY(p.waitForFinished(30000));
        QCOMPARE(p.exitStatus(), QProcess::NormalExit); // a clean refusal, not a crash
        QCOMPARE(p.exitCode(), 2);
        const QByteArray err = p.readAllStandardError();
        QVERIFY2(err.contains("unknown option '--no-model --no-ngram'"), err.constData());
        QVERIFY2(err.contains("contains a space"), err.constData());
    }

    void replaysAndCountsAddUp()
    {
        if (!QFile::exists(assets() + "/dict/en_swipe.combined"))
            QSKIP("set SWIPEKB_ASSETS to a fetched asset tree (scripts/fetch-assets.sh)");
        QTemporaryDir dir;
        const auto keys = qwerty();
        // Five swipes; the third is skipped with "-". The fourth is expected as a word the swipe does not
        // spell, so it must count as a MISS.
        const QStringList swiped{"hello", "the", "world", "keyboard", "swipe"};
        const QStringList expect{"hello", "the", "-", "zzzzq", "swipe"};
        {
            QFile f(dir.filePath("swipes.jsonl"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            for (int i = 0; i < swiped.size(); ++i) {
                f.write(swipeLine(swiped[i], keys, i == 0 ? QString() : QStringLiteral("swipe")));
                // WHY: a guess-bar pick line between swipes must be skipped, not counted as a swipe.
                if (i == 1)
                    f.write(R"({"ts":2,"type":"pick","swipe_ts":1,"word":"thy"})" "\n");
            }
            QFile e(dir.filePath("words.txt"));
            QVERIFY(e.open(QIODevice::WriteOnly));
            e.write(expect.join('\n').toUtf8() + '\n');
        }
        QProcess p;
        p.start(QStringLiteral(SWIPEKB_REPLAY_EXE),
                {"--data", assets(), "--swipes", dir.filePath("swipes.jsonl"), "--expect", dir.filePath("words.txt")});
        QVERIFY(p.waitForFinished(120000));
        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        QVERIFY2(p.exitCode() == 0, qPrintable(out + p.readAllStandardError()));

        // Per-swipe rows: count each result kind.
        int firstRows = 0, top3Rows = 0, missRows = 0, skipRows = 0;
        for (const QString &line : out.split('\n')) {
            if (!QRegularExpression("^\\d+ ").match(line).hasMatch())
                continue;
            if (line.contains("(skipped)")) ++skipRows;
            else if (line.contains(" first ")) ++firstRows;
            else if (line.contains(" top3 ")) ++top3Rows;
            else if (line.contains(" MISS ")) ++missRows;
        }
        QCOMPARE(skipRows, 1);
        QCOMPARE(firstRows + top3Rows + missRows, 4);
        QVERIFY(missRows >= 1); // "zzzzq" can never be guessed

        const auto m = QRegularExpression("summary: (\\d+) scored, (\\d+) skipped").match(out);
        QVERIFY2(m.hasMatch(), qPrintable(out));
        QCOMPARE(m.captured(1).toInt(), 4);
        QCOMPARE(m.captured(2).toInt(), 1);
        const auto f1 = QRegularExpression("first guess: (\\d+)/(\\d+)").match(out);
        const auto f3 = QRegularExpression("top 3: +(\\d+)/(\\d+)").match(out);
        QVERIFY(f1.hasMatch() && f3.hasMatch());
        QCOMPARE(f1.captured(1).toInt(), firstRows);
        QCOMPARE(f3.captured(1).toInt(), firstRows + top3Rows);
        QCOMPARE(f1.captured(2).toInt(), 4);
        QCOMPARE(f3.captured(2).toInt(), 4);

        // The options are accepted and change the reported settings.
        QProcess p2;
        p2.start(QStringLiteral(SWIPEKB_REPLAY_EXE),
                 {"--data", assets(), "--swipes", dir.filePath("swipes.jsonl"), "--expect", dir.filePath("words.txt"),
                  "--no-model", "--no-ngram"});
        QVERIFY(p2.waitForFinished(120000));
        const QString out2 = QString::fromUtf8(p2.readAllStandardOutput());
        QCOMPARE(p2.exitCode(), 0);
        QVERIFY2(out2.contains("engine=own-geometry ranking=frequency"), qPrintable(out2));
        QVERIFY(out2.contains("summary: 4 scored, 1 skipped"));
    }
};

QTEST_GUILESS_MAIN(TstReplay)
#include "tst_replay.moc"
