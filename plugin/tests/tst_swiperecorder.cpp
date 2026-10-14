#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "swiperecorder.h"

// The recorder must write exactly one valid JSON line per swipe when switched on, and nothing at all when off.
class TstSwipeRecorder : public QObject {
    Q_OBJECT

    static SwipeRecorder::Entry sample()
    {
        SwipeRecorder::Entry e;
        e.ts = 1760000000123;
        e.points = {QVariantMap{{"x", 10.5}, {"y", 20}, {"t", 0}}, QVariantMap{{"x", 30}, {"y", 40.25}, {"t", 16}}};
        e.keys = {QVariantMap{{"letter", "h"}, {"x", 1}, {"y", 2}, {"w", 3}, {"h", 4}}};
        e.context = {"say"};
        e.candidates = {"hello", "jello"};
        e.typed = "hello";
        return e;
    }

    static QList<QByteArray> lines(const QString &dir)
    {
        QFile f(dir + "/traces/swipes.jsonl");
        if (!f.open(QIODevice::ReadOnly))
            return {};
        QList<QByteArray> out = f.readAll().split('\n');
        if (!out.isEmpty() && out.last().isEmpty())
            out.removeLast();
        return out;
    }

    static void touch(const QString &path)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
    }

private slots:
    void writesNothingWhenSwitchedOff()
    {
        QTemporaryDir dir;
        QVERIFY(!SwipeRecorder::appendIfEnabled(dir.path(), sample()));
        QVERIFY(!QFile::exists(dir.path() + "/traces/swipes.jsonl"));
        // WHY: not even the folder, so a switched-off keyboard leaves no trace on disk.
        QVERIFY(!QFile::exists(dir.path() + "/traces"));
    }

    void writesOneValidLinePerSwipeWhenSwitchedOn()
    {
        QTemporaryDir dir;
        touch(dir.path() + "/record");
        QVERIFY(SwipeRecorder::appendIfEnabled(dir.path(), sample()));
        QVERIFY(SwipeRecorder::appendIfEnabled(dir.path(), sample()));
        const auto ls = lines(dir.path());
        QCOMPARE(ls.size(), 2);
        QJsonParseError err;
        const QJsonObject o = QJsonDocument::fromJson(ls[0], &err).object();
        QCOMPARE(err.error, QJsonParseError::NoError);
        QCOMPARE(o.value("ts").toInteger(), 1760000000123);
        QCOMPARE(o.value("points").toArray().size(), 2);
        QCOMPARE(o.value("points").toArray()[1].toObject().value("y").toDouble(), 40.25);
        QCOMPARE(o.value("points").toArray()[1].toObject().value("t").toDouble(), 16.0);
        const QJsonObject k = o.value("keys").toArray()[0].toObject();
        QCOMPARE(k.value("letter").toString(), QStringLiteral("h"));
        QCOMPARE(k.value("h").toDouble(), 4.0);
        QCOMPARE(o.value("context").toArray()[0].toString(), QStringLiteral("say"));
        QCOMPARE(o.value("candidates").toArray().size(), 2);
        QCOMPARE(o.value("candidates").toArray()[0].toString(), QStringLiteral("hello"));
        QCOMPARE(o.value("typed").toString(), QStringLiteral("hello"));
    }

    void switchingOffStopsRecording()
    {
        QTemporaryDir dir;
        touch(dir.path() + "/record");
        QVERIFY(SwipeRecorder::appendIfEnabled(dir.path(), sample()));
        QFile::remove(dir.path() + "/record");
        QVERIFY(!SwipeRecorder::appendIfEnabled(dir.path(), sample()));
        QCOMPARE(lines(dir.path()).size(), 1);
    }

    // Quotes, backslashes, newlines, tabs and non-English letters must not break the one-line-per-swipe format.
    void oddCharactersStayValidJson()
    {
        QTemporaryDir dir;
        touch(dir.path() + "/record");
        SwipeRecorder::Entry e = sample();
        const QString odd = QString::fromUtf8("he said \"hi\"\\ \n new\tline café 😀 \x01");
        e.context = {odd, "it's"};
        e.candidates = {odd};
        e.typed = odd;
        QVERIFY(SwipeRecorder::appendIfEnabled(dir.path(), e));
        const auto ls = lines(dir.path());
        QCOMPARE(ls.size(), 1);
        QJsonParseError err;
        const QJsonObject o = QJsonDocument::fromJson(ls[0], &err).object();
        QCOMPARE(err.error, QJsonParseError::NoError);
        QCOMPARE(o.value("typed").toString(), odd);
        QCOMPARE(o.value("context").toArray()[0].toString(), odd);
        QCOMPARE(o.value("context").toArray()[1].toString(), QStringLiteral("it's"));
    }

    void emptyTypedIsRecordedAsEmpty()
    {
        QTemporaryDir dir;
        touch(dir.path() + "/record");
        SwipeRecorder::Entry e = sample();
        e.candidates = {};
        e.typed = {};
        QVERIFY(SwipeRecorder::appendIfEnabled(dir.path(), e));
        const QJsonObject o = QJsonDocument::fromJson(lines(dir.path())[0]).object();
        QCOMPARE(o.value("typed").toString(), QString());
        QVERIFY(o.value("typed").isString());
        QCOMPARE(o.value("candidates").toArray().size(), 0);
    }
};

QTEST_GUILESS_MAIN(TstSwipeRecorder)
#include "tst_swiperecorder.moc"
