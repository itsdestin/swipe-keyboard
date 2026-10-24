// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "predictiondiagnostics.h"

namespace swipekb {
QVariantMap predictionDiagnostics(const ProfileSnapshot &p, const PredictionDiagnosticContext &c)
{
    const bool capable=p.capabilities.swipe && p.capabilities.nextWord;
    QString model;
    switch (c.model) {
    case PredictionModelState::NotRequested: model=QStringLiteral("not-requested"); break;
    case PredictionModelState::Loading: model=QStringLiteral("loading"); break;
    case PredictionModelState::Loaded: model=QStringLiteral("loaded"); break;
    case PredictionModelState::Unavailable: model=QStringLiteral("unavailable"); break;
    }
    QString status;
    if (p.effectiveLanguage.isEmpty()) status=QStringLiteral("profile-unavailable");
    else if (!p.capabilities.swipe) status=QStringLiteral("profile-unavailable");
    else if (!p.capabilities.nextWord) status=QStringLiteral("context-model-unavailable");
    else if (!c.hasContext) status=QStringLiteral("no-active-context");
    else if (!c.hasBridge && !c.ownedHistory) status=QStringLiteral("backend-policy-unavailable");
    else if (!c.policyKnown && !c.ownedHistory) status=QStringLiteral("content-policy-unavailable");
    else if (!c.fieldAllowed) status=QStringLiteral("disabled-for-field");
    else if (c.model==PredictionModelState::Unavailable) status=QStringLiteral("model-unavailable");
    else if (c.model==PredictionModelState::Loading) status=QStringLiteral("loading");
    else if (c.model==PredictionModelState::Loaded) status=QStringLiteral("ready");
    else status=QStringLiteral("available-after-confirmation");
    // Capability is deliberately separate from both loaded-model status and field
    // policy. Verified assets alone are never reported as permission to predict.
    return {{QStringLiteral("predictionContextSource"),c.ownedHistory ? QStringLiteral("keyboard-owned-history")
            : c.hasBridge ? QStringLiteral("raw-policy-bridge") : QStringLiteral("unavailable")},
        {QStringLiteral("predictionCapability"),capable},
        {QStringLiteral("predictionModelState"),model},
        {QStringLiteral("predictionStatus"),status},
        {QStringLiteral("activeFieldPolicy"),!c.hasContext || !c.hasBridge || !c.policyKnown ? QStringLiteral("unknown")
            : c.fieldAllowed ? QStringLiteral("allowed") : QStringLiteral("restricted")}};
}
}
