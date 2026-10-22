#pragma once
#include "nextwordsession.h"
#include <QObject>
PredictionPolicy readPredictionPolicy(const QObject *source, bool nativeStock = false, quint64 localEpoch = 0);
