/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: MIT
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
