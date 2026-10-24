// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "predictionpolicy.h"
#include <QVariant>

PredictionPolicy readPredictionPolicy(const QObject *source, bool nativeStock, quint64 localEpoch)
{
    PredictionPolicy unavailable;
    unavailable.source = PredictionPolicySource::Unavailable;
    if (!source) return unavailable;
    // WHY property PRESENCE wins, even malformed/denied/unknown versions: native
    // history must never become a fallback around an explicit bridge veto.
    const bool bridgePresent = source->metaObject()->indexOfProperty("swipePredictionPolicy") >= 0
        || source->dynamicPropertyNames().contains("swipePredictionPolicy");
    if (!bridgePresent) {
        if (nativeStock && QByteArray(source->metaObject()->className()) == "InputListenerItem") {
            auto native = unavailable;
            native.source = PredictionPolicySource::OwnedHistory;
            native.revision = localEpoch; // local reset identity, NOT a Wayland context/revision
            return native;
        }
        return unavailable;
    }
    const auto value = source->property("swipePredictionPolicy");
    if (value.metaType() != QMetaType::fromType<QVariantMap>()) return unavailable;
    const auto map = value.toMap();
    // WHY strict types: QVariant("false").toBool() is true. Permission must not
    // be reconstructed from strings, missing properties or a lossy Qt hint.
    for (const auto *key : {"contentTypeReceived", "completionAllowed", "syntheticNoPredictiveText"})
        if (map.value(key).metaType() != QMetaType::fromType<bool>()) return unavailable;
    for (const auto *key : {"version", "purpose"})
        if (map.value(key).metaType() != QMetaType::fromType<int>()) return unavailable;
    for (const auto *key : {"contextId", "revision"})
        if (map.value(key).metaType() != QMetaType::fromType<qulonglong>()) return unavailable;
    return {map.value("version").toInt(), map.value("contextId").toULongLong(), map.value("revision").toULongLong(),
        map.value("contentTypeReceived").toBool(), map.value("purpose").toInt(),
        map.value("completionAllowed").toBool(), map.value("syntheticNoPredictiveText").toBool()};
}
