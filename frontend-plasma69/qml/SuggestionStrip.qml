// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Swipe Keyboard's suggestion row on Plasma 6.9: the swiped word's alternatives, completions of tapped letters,
// next-word suggestions and tap-to-swap alternatives, three at a time with the strongest in the middle.
//
// WHY our own row: KDE's row (KeyboardStrip) only shows candidates a built-in text composer sets, which an add-on
// cannot do. This row is inserted as the first row of the layout (SwipeHook), so it always keeps the same height
// and the keys never jump when suggestions come and go.

import QtQuick
import QtQuick.Layouts

import org.kde.kirigami as Kirigami

Item {
    id: root

    required property QtObject controller
    readonly property var words: controller ? controller.candidates : []

    // Row weight in KDE's KeyboardLayout column (key rows weigh 100).
    Layout.fillWidth: true
    Layout.fillHeight: true
    Layout.preferredHeight: 60
    Layout.verticalStretchFactor: 60
    implicitHeight: 60
    objectName: "swipeSuggestionStrip"

    Accessible.role: Accessible.List
    Accessible.name: qsTr("Suggestions")

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Repeater {
            model: 3
            delegate: Item {
                id: cell
                required property int index
                readonly property string word: index < root.words.length ? root.words[index] : ""
                property var pressId: 0
                objectName: "swipeSuggestion" + index
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 1

                Accessible.role: Accessible.Button
                Accessible.name: word

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 2
                    radius: Kirigami.Units.cornerRadius
                    color: Kirigami.Theme.highlightColor
                    opacity: tap.pressed && cell.word.length > 0 ? 0.3 : 0
                }

                Text {
                    anchors.fill: parent
                    anchors.leftMargin: Kirigami.Units.smallSpacing
                    anchors.rightMargin: Kirigami.Units.smallSpacing
                    text: cell.word
                    color: Kirigami.Theme.textColor
                    font.bold: cell.index === 1
                    font.pixelSize: Math.max(10, Math.round(root.height * 0.42))
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                // Thin separators between the three words.
                Rectangle {
                    visible: cell.index > 0
                    width: 1
                    height: parent.height * 0.5
                    anchors.verticalCenter: parent.verticalCenter
                    color: Kirigami.Theme.disabledTextColor
                    opacity: 0.5
                }

                TapHandler {
                    id: tap
                    // WHY freeze the row at press: a list that changes under the finger must not type another word.
                    onPressedChanged: {
                        if (pressed && cell.word.length > 0) {
                            cell.pressId = root.controller.pressCandidate(root.controller.selectionToken());
                        } else if (!pressed) {
                            const id = cell.pressId;
                            Qt.callLater(function() { root.controller.releaseCandidate(id); });
                        }
                    }
                    onTapped: if (cell.word.length > 0) root.controller.selectCandidate(cell.index)
                    // Long press on a word learned from the person's own typing removes it (ordinary words: nothing).
                    onLongPressed: if (root.controller.canRemove(cell.index)) root.controller.removeCandidate(cell.index)
                }
            }
        }
    }
}
