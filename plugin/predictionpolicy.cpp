#include "predictionpolicy.h"
#include <QVariant>

PredictionPolicy readPredictionPolicy(const QObject *source)
{
    if (!source) return {};
    const auto value = source->property("swipePredictionPolicy");
    if (value.metaType() != QMetaType::fromType<QVariantMap>()) return {};
    const auto map = value.toMap();
    // WHY strict types: QVariant("false").toBool() is true. Permission must not
    // be reconstructed from strings, missing properties or a lossy Qt hint.
    for (const auto *key : {"contentTypeReceived", "completionAllowed", "syntheticNoPredictiveText"})
        if (map.value(key).metaType() != QMetaType::fromType<bool>()) return {};
    for (const auto *key : {"version", "purpose"})
        if (map.value(key).metaType() != QMetaType::fromType<int>()) return {};
    for (const auto *key : {"contextId", "revision"})
        if (map.value(key).metaType() != QMetaType::fromType<qulonglong>()) return {};
    return {map.value("version").toInt(), map.value("contextId").toULongLong(), map.value("revision").toULongLong(),
        map.value("contentTypeReceived").toBool(), map.value("purpose").toInt(),
        map.value("completionAllowed").toBool(), map.value("syntheticNoPredictiveText").toBool()};
}
