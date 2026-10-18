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
    void packagedModelsNeverRedirectPrivateRecordings()
    {
        const auto oldData = qgetenv("SWIPEKB_DATA");
        const auto oldModels = qgetenv("SWIPEKB_MODEL_DATA");
        QTemporaryDir dir;
        qputenv("SWIPEKB_DATA", (dir.path() + "/private").toUtf8());
        qputenv("SWIPEKB_MODEL_DATA", (dir.path() + "/system-models").toUtf8());
        const auto data = SwipeRecorder::dataDir();
        const auto models = SwipeRecorder::modelDir();
        qunsetenv("SWIPEKB_MODEL_DATA");
        const auto legacyModels = SwipeRecorder::modelDir();
        // Restore the process environment before any assertion can return early.
        if (oldData.isNull()) qunsetenv("SWIPEKB_DATA"); else qputenv("SWIPEKB_DATA", oldData);
        if (oldModels.isNull()) qunsetenv("SWIPEKB_MODEL_DATA"); else qputenv("SWIPEKB_MODEL_DATA", oldModels);
        QCOMPARE(data, dir.path() + "/private");
        QCOMPARE(models, dir.path() + "/system-models");
        QCOMPARE(legacyModels, data);
    }

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
        QCOMPARE(o.value("type").toString(), QStringLiteral("swipe"));
    }

    // A guess-bar pick adds its own line pointing at the swipe it corrects; the swipe line is left as it was.
    void pickAddsALineNamingTheSwipe()
    {
        QTemporaryDir dir;
        touch(dir.path() + "/record");
        QVERIFY(SwipeRecorder::appendIfEnabled(dir.path(), sample()));
        QVERIFY(SwipeRecorder::appendPickIfEnabled(dir.path(), 1760000000999, 1760000000123, "jello"));
        const auto ls = lines(dir.path());
        QCOMPARE(ls.size(), 2);
        QCOMPARE(QJsonDocument::fromJson(ls[0]).object().value("typed").toString(), QStringLiteral("hello"));
        QJsonParseError err;
        const QJsonObject o = QJsonDocument::fromJson(ls[1], &err).object();
        QCOMPARE(err.error, QJsonParseError::NoError);
        QCOMPARE(o.value("type").toString(), QStringLiteral("pick"));
        QCOMPARE(o.value("ts").toInteger(), 1760000000999);
        QCOMPARE(o.value("swipe_ts").toInteger(), 1760000000123);
        QCOMPARE(o.value("word").toString(), QStringLiteral("jello"));
        QCOMPARE(o.keys().size(), 4); // nothing else: no text from the app rides along
    }

    // Erase/keep lines name their swipe and carry no text (counts only), and respect the on/off switch.
    void outcomeLinesNameTheSwipeAndCarryNoText()
    {
        QTemporaryDir dir;
        QVERIFY(!SwipeRecorder::appendOutcomeIfEnabled(dir.path(), 5, "erase", 1));
        QVERIFY(!QFile::exists(dir.path() + "/traces"));
        touch(dir.path() + "/record");
        QVERIFY(SwipeRecorder::appendOutcomeIfEnabled(dir.path(), 5, "erase", 1));
        QVERIFY(SwipeRecorder::appendOutcomeIfEnabled(dir.path(), 6, "keep", 2));
        const auto ls = lines(dir.path());
        QCOMPARE(ls.size(), 2);
        const QJsonObject a = QJsonDocument::fromJson(ls[0]).object();
        QCOMPARE(a.value("type").toString(), QStringLiteral("erase"));
        QCOMPARE(a.value("swipe_ts").toInteger(), 1);
        QCOMPARE(a.keys().size(), 3);
        QCOMPARE(QJsonDocument::fromJson(ls[1]).object().value("type").toString(), QStringLiteral("keep"));
    }

    void pickWritesNothingWhenSwitchedOff()
    {
        QTemporaryDir dir;
        QVERIFY(!SwipeRecorder::appendPickIfEnabled(dir.path(), 2, 1, "jello"));
        QVERIFY(!QFile::exists(dir.path() + "/traces"));
    }

    // WHY: recordings can hold private text, so only the owner's account may read them.
    void recordingsAreOwnerOnly()
    {
        QTemporaryDir dir;
        touch(dir.path() + "/record");
        QVERIFY(SwipeRecorder::appendIfEnabled(dir.path(), sample()));
        const auto owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
        const auto others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
        const auto filePerms = QFile::permissions(dir.path() + "/traces/swipes.jsonl");
        QCOMPARE(filePerms & (owner | others | QFileDevice::ExeOwner), owner);
        const auto dirPerms = QFile::permissions(dir.path() + "/traces");
        QCOMPARE(dirPerms & (owner | others | QFileDevice::ExeOwner), owner | QFileDevice::ExeOwner);
        // A file left readable by an older version is tightened on the next write.
        QFile::setPermissions(dir.path() + "/traces/swipes.jsonl", owner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
        QVERIFY(SwipeRecorder::appendPickIfEnabled(dir.path(), 2, 1, "jello"));
        QCOMPARE(QFile::permissions(dir.path() + "/traces/swipes.jsonl") & (owner | others), owner);
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
