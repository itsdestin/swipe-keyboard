/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once
#include <QVariantMap>
#include <QtGlobal>

// Version-1 raw policy bridge for the pinned Plasma InputListenerItem. No text,
// words, application names, pointer values or history enter this snapshot.
class BackendPredictionPolicy {
public:
    void contextChanged(bool active) {
        ++m_serial; ++m_revision;
        m_context = active ? m_serial : 0;
        m_received = false; m_hints = 0; m_purpose = -1;
    }
    void contentType(quint32 hints, quint32 purpose) {
        ++m_revision;
        m_received = m_context != 0; m_hints = hints; m_purpose = int(purpose);
    }
    void reset() { ++m_revision; }
    QVariantMap snapshot() const {
        // WHY raw auto_completion AND sensitive guards: the Qt bit is synthesized
        // to disable Hunspell, whereas the compositor's application restriction
        // must remain an unconditional veto.
        const bool completion = m_context && m_received && (m_hints & 1) && !(m_hints & 0xc0);
        return {{QStringLiteral("version"), 1}, {QStringLiteral("contextId"), m_context}, {QStringLiteral("revision"), m_revision},
            {QStringLiteral("contentTypeReceived"), m_received}, {QStringLiteral("purpose"), m_purpose}, {QStringLiteral("rawHints"), m_hints},
            {QStringLiteral("completionAllowed"), completion}, {QStringLiteral("syntheticNoPredictiveText"), true}};
    }
private:
    quint64 m_serial = 0, m_context = 0, m_revision = 0;
    quint32 m_hints = 0;
    int m_purpose = -1;
    bool m_received = false;
};
