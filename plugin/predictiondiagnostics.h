/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once
#include "languageprofile.h"
#include <QVariantMap>

namespace swipekb {
enum class PredictionModelState { NotRequested, Loading, Loaded, Unavailable };
// WHY booleans/enums only: diagnostics cannot receive or accidentally serialize
// a user's editor buffer, candidate words, model paths or backend context identity.
struct PredictionDiagnosticContext {
    bool hasContext=false, hasBridge=false, policyKnown=false, fieldAllowed=false, ngramOnly=false;
    PredictionModelState model=PredictionModelState::NotRequested;
    bool ownedHistory=false;
};
QVariantMap predictionDiagnostics(const ProfileSnapshot &, const PredictionDiagnosticContext &);
}
