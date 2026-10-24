/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "languageprofile.h"
#include <QVariantMap>
namespace swipekb {
// Values only; no engine/editor/policy object or text is accepted by this shared interface.
QVariantMap profileDiagnostics(const ProfileSnapshot &profile);
}
