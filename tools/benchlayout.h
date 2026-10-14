// Turns FUTO's en_qwerty.json (key centres + half sizes, 0..1) into the pixel rectangles our Recognizer takes.
// Header-only so swipekb-bench and its unit test share the exact same code.
#pragma once

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <vector>

#include "swipekb/recognizer.hpp"

namespace benchlayout {

struct LayoutKey {
    char letter;
    double cx, cy, rx, ry;   // centre and half-width / half-height, FUTO's 0..1 letter-area units
};

inline bool loadLayout(const QString &path, std::vector<LayoutKey> &out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonArray keys = QJsonDocument::fromJson(f.readAll()).object().value("keys").toArray();
    for (const QJsonValue &v : keys) {
        const QJsonObject k = v.toObject();
        const QString letter = k.value("letter").toString();
        if (letter.size() != 1)
            continue;
        out.push_back({letter.at(0).toLatin1(), k.value("cx").toDouble(), k.value("cy").toDouble(),
                       k.value("rx").toDouble(), k.value("ry").toDouble()});
    }
    return out.size() == 26;
}

// WHY scale (1000 px): the bench's recorded traces are already 0..1 of the letter area. Scaling the keys and
// the trace by the same number makes our letter-area normalisation come out as (near) identity, so the engine
// sees the same numbers FUTO's own tool feeds it, while still running through the real pixel-space entry point.
// Each rect is the centre +- the half key spacing from the file, so neighbouring keys touch.
inline std::vector<swipekb::KeyRect> toRects(const std::vector<LayoutKey> &keys, float scale)
{
    std::vector<swipekb::KeyRect> out;
    for (const LayoutKey &k : keys)
        out.push_back({k.letter, float((k.cx - k.rx) * scale), float((k.cy - k.ry) * scale), float(2 * k.rx * scale),
                       float(2 * k.ry * scale)});
    return out;
}

} // namespace benchlayout
