/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once
#include "languageprofile.h"
#include <QVariantMap>
namespace swipekb {
// Values only; no engine/editor/policy object or text is accepted by this shared interface.
QVariantMap profileDiagnostics(const ProfileSnapshot &profile);
}
