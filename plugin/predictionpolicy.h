/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once
#include "nextwordsession.h"
#include <QObject>
PredictionPolicy readPredictionPolicy(const QObject *source, bool nativeStock = false, quint64 localEpoch = 0);
