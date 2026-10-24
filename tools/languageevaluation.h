/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once
#include <QSet>
#include <QStringList>
#include <QVariantMap>
#include <QList>
namespace swipekb {
QVariantMap scoreLanguageCase(const QString &target, const QStringList &candidates,
                             const QSet<QString> &dictionary, const QString &language);
QVariantMap summarizeLanguageCases(const QList<QVariantMap> &cases);
}
