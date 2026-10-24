// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
// Test stand-in for plasma-keyboard's KConfig-backed settings. The test sets the enabled layouts.
pragma Singleton
import QtQuick

QtObject {
    property bool panelFillScreenWidth: true
    property bool characterPopupEnabled: true
    property var enabledKeyboardLayoutIds: ["org.kde.plasma.keyboard.en/mobile-qwerty", "org.kde.plasma.keyboard.de_DE/mobile-qwertz", "org.kde.plasma.keyboard.ru_RU/mobile-jcuken"]
    property var enabledLocales: []
}
