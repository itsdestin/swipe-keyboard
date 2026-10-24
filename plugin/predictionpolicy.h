/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "nextwordsession.h"
#include <QObject>
PredictionPolicy readPredictionPolicy(const QObject *source, bool nativeStock = false, quint64 localEpoch = 0);
