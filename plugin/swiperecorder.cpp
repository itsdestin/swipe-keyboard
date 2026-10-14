#include "swiperecorder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace SwipeRecorder {

QString dataDir()
{
    QString dir = qEnvironmentVariable("SWIPEKB_DATA");
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/swipe-keyboard");
    return dir;
}

// WHY copy only the known fields (not the whole map): the line format stays fixed for the replay tool, and
// numbers are stored as plain numbers whatever type QML handed over.
static QJsonArray pointsJson(const QVariantList &points)
{
    QJsonArray out;
    for (const QVariant &v : points) {
        const QVariantMap m = v.toMap();
        out.append(QJsonObject{{"x", m.value("x").toDouble()}, {"y", m.value("y").toDouble()},
                               {"t", m.value("t").toDouble()}});
    }
    return out;
}

static QJsonArray keysJson(const QVariantList &keys)
{
    QJsonArray out;
    for (const QVariant &v : keys) {
        const QVariantMap m = v.toMap();
        out.append(QJsonObject{{"letter", m.value("letter").toString()}, {"x", m.value("x").toDouble()},
                               {"y", m.value("y").toDouble()}, {"w", m.value("w").toDouble()},
                               {"h", m.value("h").toDouble()}});
    }
    return out;
}

// Appends one compact JSON line to <dir>/traces/swipes.jsonl if <dir>/record exists.
static bool appendLine(const QString &dir, const QJsonObject &line)
{
    if (dir.isEmpty() || !QFileInfo::exists(dir + QStringLiteral("/record")))
        return false;
    const QString tracesDir = dir + QStringLiteral("/traces");
    if (!QDir().mkpath(tracesDir))
        return false;
    // WHY owner-only (folder 0700, file 0600): the recordings hold the words typed and the sentence around
    // each one, which can be private. Other user accounts on this computer must not be able to read them.
    QFile::setPermissions(tracesDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QFile f(tracesDir + QStringLiteral("/swipes.jsonl"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append))
        return false;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    // WHY one write of the full line: a crash mid-swipe can at worst lose this line, never leave half of it
    // glued to the next one. Compact JSON never contains a raw newline (they are escaped as \n).
    const QByteArray bytes = QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n';
    return f.write(bytes) == bytes.size();
}

bool appendIfEnabled(const QString &dir, const Entry &e)
{
    // WHY "type": the file now mixes swipe lines and pick lines; the replay tool reads only swipe lines.
    return appendLine(dir, QJsonObject{{"ts", e.ts},
                                       {"type", QStringLiteral("swipe")},
                                       {"points", pointsJson(e.points)},
                                       {"keys", keysJson(e.keys)},
                                       {"context", QJsonArray::fromStringList(e.context)},
                                       {"candidates", QJsonArray::fromStringList(e.candidates)},
                                       {"typed", e.typed}});
}

bool appendPickIfEnabled(const QString &dir, qint64 ts, qint64 swipeTs, const QString &word)
{
    return appendLine(dir, QJsonObject{{"ts", ts}, {"type", QStringLiteral("pick")}, {"swipe_ts", swipeTs},
                                       {"word", word}});
}

} // namespace SwipeRecorder
