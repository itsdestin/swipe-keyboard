// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
// Test stand-in for plasma-keyboard's Feedback (sounds/vibration): counts calls only.
pragma Singleton
import QtQuick

QtObject {
    enum Kind { Press, SelectionChange, SelectionCommit }
    property int plays: 0
    function play(kind) { plays++ }
}
