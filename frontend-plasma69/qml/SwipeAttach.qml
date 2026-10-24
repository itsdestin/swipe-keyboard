// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Created by each generated layout wrapper (integration/plasma69.py): hands KDE's layout to SwipeHook.
// WHY a type of its own: the wrapper creates it by module name at run time, so a missing or broken module can never
// stop KDE's layout from loading.

import QtQml

import SwipeKeyboard69

QtObject {
    required property QtObject target
    Component.onCompleted: SwipeHook.watch(target)
}
