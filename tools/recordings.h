// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// recordings.h — reads the owner's opt-in swipe recordings (plugin/swiperecorder.h writes them to
// <data folder>/traces/swipes.jsonl when <data folder>/record exists) for the user-experience scoreboard
// (swipekb-swap-bench --recordings). READ ONLY: the file is opened for reading and nothing is ever written.
//
// Line kinds: "swipe" (or no "type": older recordings) {ts, points, keys, context, candidates, typed};
// "pick" {swipe_ts, word}: the owner tapped `word` on the bar right after that swipe; "erase" / "keep" {swipe_ts}:
// the owner's first Backspace while the cursor was still after the word, or typing on / swiping again without a fix.
//
// What each recorded swipe counts as (its true word, honestly):
//   picked   a pick line names it: the LAST pick is the word meant (the owner chose it). Felt: one tap.
//   erased   an erase line and no pick: the word was wrong and deleted (Backspace). Its true word was never recorded,
//            so it cannot be replayed. Felt: a retype (or a deletion).
//   kept     a keep line and no pick or erase: the owner moved on without a fix. ASSUMED right (they may not have
//            noticed a wrong word). Felt: no tap.
//   unknown  no outcome at all: a swap by tapping the word later (the keyboard records none), a cursor move or a
//            change of field. Not replayed.
//   empty    the keyboard typed nothing for it.
// Only picked and kept swipes have a known word, so only those are returned for replaying.
#pragma once

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <cstdio>
#include <string>
#include <vector>

#include "benchdata.h"

namespace recordings {

struct Counts {
    size_t swipes = 0, picked = 0, erased = 0, kept = 0, unknown = 0, empty = 0;
    // One line of counts, never words: "recorded: swipes=N picked=N erased=N kept=N unknown=N empty=N".
    std::string format() const
    {
        char buf[200];
        std::snprintf(buf, sizeof buf, "recorded: swipes=%zu picked=%zu erased=%zu kept=%zu unknown=%zu empty=%zu\n",
                      swipes, picked, erased, kept, unknown, empty);
        return buf;
    }
};

// The swipes with a known word (picked, kept) from the recording's text, in file order; `counts` gets every swipe.
inline std::vector<benchdata::BenchSwipe> parse(const QByteArray &data, Counts &counts)
{
    // WHY gathered first and judged after: a swipe line comes before the pick and outcome lines that refer to it.
    struct Line { benchdata::BenchSwipe swipe; QString typed; qint64 ts = 0; };
    QList<Line> swipes;
    QHash<qint64, QString> lastPick;
    QHash<qint64, QString> outcome;   // "erase" or "keep"; the first one counts (the tracker writes at most one)
    for (const QByteArray &raw : data.split('\n')) {
        const QJsonObject o = QJsonDocument::fromJson(raw).object();
        if (o.isEmpty()) continue;
        const QString type = o.value("type").toString(QStringLiteral("swipe"));
        const qint64 ref = qint64(o.value("swipe_ts").toDouble());
        if (type == QLatin1String("pick")) { lastPick[ref] = o.value("word").toString(); continue; }
        if (type == QLatin1String("erase") || type == QLatin1String("keep")) { if (!outcome.contains(ref)) outcome[ref] = type; continue; }
        if (type != QLatin1String("swipe")) continue;
        Line l;
        for (const QJsonValue &v : o.value("points").toArray()) {
            const QJsonObject p = v.toObject();
            l.swipe.trace.push_back({float(p.value("x").toDouble()), float(p.value("y").toDouble()), float(p.value("t").toDouble())});
        }
        for (const QJsonValue &v : o.value("keys").toArray()) {
            const QJsonObject k = v.toObject();
            const auto u = k.value("letter").toString().toUcs4();
            if (u.size() != 1) continue;
            l.swipe.keys.push_back({char32_t(u[0]), float(k.value("x").toDouble()), float(k.value("y").toDouble()),
                                    float(k.value("w").toDouble()), float(k.value("h").toDouble())});
        }
        for (const QJsonValue &v : o.value("context").toArray()) l.swipe.before.push_back(v.toString().toStdString());
        l.typed = o.value("typed").toString().trimmed();
        l.ts = qint64(o.value("ts").toDouble());
        swipes.append(std::move(l));
    }
    std::vector<benchdata::BenchSwipe> out;
    for (const auto &l : std::as_const(swipes)) {
        ++counts.swipes;
        const qint64 ts = l.ts;
        if (l.typed.isEmpty()) { ++counts.empty; continue; }
        QString word;
        if (lastPick.contains(ts)) { ++counts.picked; word = lastPick.value(ts).trimmed(); }
        else if (outcome.value(ts) == QLatin1String("erase")) { ++counts.erased; continue; }
        else if (outcome.value(ts) == QLatin1String("keep")) { ++counts.kept; word = l.typed; }
        else { ++counts.unknown; continue; }
        if (word.isEmpty() || l.swipe.trace.size() < 2 || l.swipe.keys.empty()) continue;
        auto s = l.swipe;
        s.word = word.toStdString();
        out.push_back(std::move(s));
    }
    return out;
}

// Reads `path` (opened read-only) and parses it. On a missing or unreadable file returns empty and sets *error.
inline std::vector<benchdata::BenchSwipe> read(const std::string &path, Counts &counts, std::string *error)
{
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "cannot read the recordings file";
        return {};
    }
    return parse(f.readAll(), counts);
}

} // namespace recordings
