// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The swipe layer over Plasma 6.9's keys. It never blocks a normal tap: it only LISTENS to the finger (a passive
// PointHandler) and, once a press that started on a letter has travelled half a key, tells that key to forget the
// press (SwipeHook.cancelKeyPress) and collects the path for the swipe engine. It also reports every key press and
// finished key to the controller, and, only while a swipe's Backspace-undo or automatic-space rule is armed, takes
// over exactly those keys (claim areas).
//
// WHY it imports no KDE module: KDE's QML types are internal; everything is reached as properties of the items KDE
// already created (checked by SwipeHook and tests/plasma69).

import QtQuick

import SwipeKeyboard69

Item {
    id: root

    required property QtObject controller
    required property Item layoutItem // the KeyboardLayout KDE is showing
    required property Item keyboard // KDE's Keyboard item (layoutType, inputEngine)
    property QtObject popup: null // KDE's long-press popup (alternative characters), when found

    anchors.fill: parent

    // How far a press must travel before it is a swipe, in key widths (same as the Qt Virtual Keyboard front end).
    property real thresholdKeys: 0.5
    readonly property bool lettersPage: keyboard && keyboard.layoutType === "main"
    property color trailColor: "#3daee9"

    property var points: []
    property var trailPoints: []
    property var originKeys: []
    property Item startKey: null
    property real startTime: 0
    property bool swiping: false
    property int generation: -1

    // ---- key discovery ------------------------------------------------------------------------------------------

    function isTextKey(item) {
        // WHY commitText: it is what KDE's Key.qml adds to AbstractKey; Shift, ?123 and the globe key lack it.
        return item && item.visible && item.commitText !== undefined && item.key !== undefined && item.clicked !== undefined;
    }

    function isLetterKey(item) {
        // WHY case, not a regular expression: QML's JavaScript has no Unicode property classes; every letter of
        // the supported languages has an upper and a lower case, digits and punctuation do not.
        return isTextKey(item) && typeof item.text === "string" && item.text.length === 1
            && item.text.toLowerCase() !== item.text.toUpperCase();
    }

    function textKeys() {
        var out = [];
        function walk(item) {
            if (!item || !item.visible || item === root)
                return;
            if (root.isTextKey(item)) {
                out.push(item);
                return;
            }
            for (var i = 0; i < item.children.length; ++i)
                walk(item.children[i]);
        }
        walk(root.layoutItem);
        return out;
    }

    function letterKeys() {
        var out = [];
        var keys = textKeys();
        for (var i = 0; i < keys.length; ++i) {
            var k = keys[i];
            if (!isLetterKey(k) || k.width <= 0 || k.height <= 0)
                continue;
            var p = k.mapToItem(root, 0, 0);
            out.push({ letter: k.text, x: p.x, y: p.y, w: k.width, h: k.height });
        }
        return out;
    }

    function keyAt(x, y) {
        var keys = textKeys();
        for (var i = 0; i < keys.length; ++i) {
            var p = keys[i].mapFromItem(root, x, y);
            if (p.x >= 0 && p.y >= 0 && p.x < keys[i].width && p.y < keys[i].height)
                return keys[i];
        }
        return null;
    }

    // ---- taps: tell the controller about each press (before KDE types it) and each finished key (after) ----------

    property var hooked: []
    function hookKeys() {
        var keys = textKeys();
        for (var i = 0; i < keys.length; ++i) {
            var key = keys[i];
            if (hooked.indexOf(key) >= 0)
                continue;
            hooked.push(key);
            for (var c = 0; c < key.children.length; ++c) {
                var area = key.children[c];
                // KDE's KeyMouseArea: a MultiPointTouchArea with these two signals.
                if (area.pressStarted !== undefined && area.keyItem === key) {
                    area.pressStarted.connect(function(k) { return function() { root.controller.keyPressed(k.key, k.text); }; }(key));
                }
            }
            key.clicked.connect(function(k) { return function() { root.controller.keyClicked(k.key, k.text); }; }(key));
        }
    }
    // WHY at once and again at each press: a key must be hooked before its first press reaches it (our handler sees
    // the press first), and KDE may add keys later (pages, form-factor changes).
    Component.onCompleted: hookKeys()
    Connections {
        target: root.layoutItem
        function onChildrenChanged() { Qt.callLater(root.hookKeys) }
    }

    // ---- swipe --------------------------------------------------------------------------------------------------

    function stop() {
        swiping = false;
        startKey = null;
        points = [];
        trailPoints = [];
        generation = -1;
        trail.requestPaint();
    }

    function popupOpen() {
        return !!(popup && popup.active);
    }

    Connections {
        target: root.controller
        function onGestureInvalidated() { if (root.swiping) root.stop() }
    }

    PointHandler {
        id: finger
        // WHY passive: taps, long-press accents and key repeat stay KDE's; we only watch.
        acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Mouse | PointerDevice.Stylus
        onActiveChanged: {
            if (active) {
                root.hookKeys();
                var p = point.position;
                var key = root.keyAt(p.x, p.y);
                root.stop();
                // WHY these gates: no swipe on symbol pages, in password fields (the trail would draw the password),
                // when a letter of the language has no key, or from a non-letter key.
                if (!root.lettersPage || !root.controller.fieldAllowsSwipe || !root.isLetterKey(key))
                    return;
                var keys = root.letterKeys();
                if (!root.controller.readyForKeys(keys))
                    return;
                root.originKeys = keys;
                root.startKey = key;
                root.startTime = Date.now();
                root.points = [{ x: p.x, y: p.y, t: 0 }];
                root.trailPoints = [{ x: p.x, y: p.y }];
            } else {
                if (root.swiping && root.generation >= 0) {
                    var q = point.position;
                    root.points.push({ x: q.x, y: q.y, t: Date.now() - root.startTime });
                    root.controller.finishSwipe(root.points, root.originKeys, root.generation);
                }
                root.stop();
            }
        }
        onPointChanged: {
            if (!active || !root.startKey)
                return;
            var p = point.position;
            root.points.push({ x: p.x, y: p.y, t: Date.now() - root.startTime });
            root.trailPoints.push({ x: p.x, y: p.y });
            if (root.points.length > 2048) {
                root.stop();
                return;
            }
            if (!root.swiping) {
                var dx = p.x - root.points[0].x, dy = p.y - root.points[0].y;
                // WHY only before the accent popup opens: a long press that slides along the popup is not a swipe.
                if (Math.sqrt(dx * dx + dy * dy) > root.startKey.width * root.thresholdKeys && !root.popupOpen()) {
                    root.generation = root.controller.beginSwipe();
                    if (root.generation < 0) {
                        root.stop();
                        return;
                    }
                    root.swiping = true;
                    SwipeHook.cancelKeyPress(root.startKey);
                }
            }
            if (root.swiping)
                trail.requestPaint();
        }
    }

    // ---- claim areas: Backspace right after a swipe, punctuation right after an automatic space -----------------

    readonly property var claimed: {
        var out = [];
        if (!controller || (!controller.undoArmed && !controller.autoSpaceArmed))
            return out;
        var keys = textKeys();
        for (var i = 0; i < keys.length; ++i) {
            var k = keys[i];
            if ((controller.undoArmed && k.key === Qt.Key_Backspace)
                    || (controller.autoSpaceArmed && k.text.length === 1 && ".,!?;:".indexOf(k.text) >= 0))
                out.push(k);
        }
        return out;
    }

    Repeater {
        model: root.claimed
        delegate: MultiPointTouchArea {
            required property Item modelData
            readonly property point origin: modelData ? root.mapFromItem(modelData, 0, 0) : Qt.point(0, 0)
            x: origin.x
            y: origin.y
            width: modelData ? modelData.width : 0
            height: modelData ? modelData.height : 0
            z: 2
            mouseEnabled: true
            maximumTouchPoints: 1
            onPressed: if (modelData) modelData.pressedVisual = true
            onCanceled: if (modelData) modelData.pressedVisual = false
            onReleased: {
                if (!modelData)
                    return;
                modelData.pressedVisual = false;
                root.controller.claimedKey(modelData.key, modelData.text);
            }
        }
    }

    // ---- trail --------------------------------------------------------------------------------------------------

    Canvas {
        id: trail
        objectName: "swipeTrail"
        anchors.fill: parent
        enabled: false // paint only: never catches a touch
        z: 1
        onPaint: {
            var ctx = getContext("2d");
            ctx.reset();
            var pts = root.trailPoints;
            if (!root.swiping || pts.length < 2)
                return;
            ctx.lineWidth = 8;
            ctx.lineCap = "round";
            ctx.lineJoin = "round";
            ctx.globalAlpha = 0.8;
            ctx.strokeStyle = root.trailColor;
            ctx.beginPath();
            ctx.moveTo(pts[0].x, pts[0].y);
            for (var i = 1; i < pts.length; ++i)
                ctx.lineTo(pts[i].x, pts[i].y);
            ctx.stroke();
        }
    }
}
