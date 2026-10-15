import QtQuick
import QtQuick.VirtualKeyboard
import QtQuick.VirtualKeyboard.Settings

// SwipeCapture — swipe-keyboard's finger listener and trail.
// WHY a separate layer: Qt's keyboard records paths only in handwriting boxes, never across keys.
// It only LISTENS to the keyboard's own touch area, so taps work exactly as before. Once a finger that
// started on a letter drags past `threshold`, it cancels that key press, draws the trail, and on lift
// hands the path to SwipeInputMethod. It must never sit inside the layout: Qt finds the key under a
// finger with childAt() on the layout, and an overlay there would swallow every tap.
//
// WHY the listener and the drawing are two items: the layout hook creates this item inside Qt's touch
// area, and in Keyboard.qml that touch area is a child of the layout Loader. The Loader adds the loaded
// layout (the keys) as a later child, so the keys paint ON TOP of the touch area and everything inside it,
// whatever z we use in here (z only orders siblings). That is why the trail used to draw behind the keys.
// So this item stays where it is and only listens; the trail (`trail` below) is moved up to the Keyboard
// root item, beside the whole key area, with a z above every other child there.
Item {
    id: root

    required property Item layoutItem   // the KeyboardLayout (to find the letter keys)
    required property Item touchArea    // Qt VK's keyboard.keyboardInputArea

    anchors.fill: parent
    enabled: false                      // never takes input itself

    property color trailColor: "#3daee9" // Breeze highlight blue
    // Drag distance (fraction of a key width) before a touch counts as a swipe. Tuned in M1.
    property real thresholdKeys: 0.5

    property var points: []
    // The same points mapped into the trail's own coordinates (see `trail`), filled as the finger moves.
    // WHY a second list: `points` must stay in the touch area's coordinates for the engine, and mapping
    // each point once on arrival is cheaper than remapping the whole path on every repaint.
    property var trailPoints: []
    property int trackedId: -1
    property real startTime: 0
    property Item startKey: null
    property bool swiping: false
    // WHY: once the hold-for-accents popup has opened for this touch, sliding must pick an accent, never swipe.
    property bool popupSeen: false

    // WHY: keep the guess bar always shown so the keyboard stays one height instead of growing on each swipe
    // and making the app above jump. Set once and never undone: this layer is destroyed on "?123", but the
    // swipe input method (and its guess bar) stays active on the symbols page, so undoing it there would
    // bring the jump back on every ?123/ABC toggle. Safe to leave on: Qt only applies it while the active
    // input method has a guess bar (Keyboard.qml alwaysVisibleCondition requires
    // wordCandidateListVisibleHint), so pages and keyboards without one (numbers, digits, dialpad, stock
    // keyboard) keep their normal height. It lives in memory only, and --uninstall restarts the keyboard.
    Component.onCompleted: {
        VirtualKeyboardSettings.wordCandidateList.alwaysVisible = true
        root.candidateView = root.findCandidateView()
    }
    // WHY: give the guess bar back its stock left alignment when this layer goes away (the "?123" page, or
    // swipe-keyboard uninstalled while running). The Binding below stops with us, but the last value it wrote
    // would otherwise stay. A new swipe layer re-centers at once.
    Component.onDestruction: {
        if (root.candidateView)
            root.candidateView.leftMargin = 0
    }

    // Qt's guess bar (Keyboard.qml's ListView "wordCandidateView"), found once, or null.
    property ListView candidateView: null

    // Finds the guess bar under the Keyboard root by its objectName. WHY search (not a fixed path): it is a
    // direct child in Qt 6.12, but a search a few levels deep survives Qt moving it into a wrapper item.
    function findCandidateView() {
        function walk(item, depth) {
            if (!item || depth > 3)
                return null
            for (var i = 0; i < item.children.length; ++i) {
                var c = item.children[i]
                if (c.objectName === "wordCandidateView" && c.contentWidth !== undefined)
                    return c
                var found = walk(c, depth + 1)
                if (found)
                    return found
            }
            return null
        }
        return walk(root.keyboardItem(), 0)
    }

    // Centers the guesses in the guess bar (the owner's request; Qt puts them at the left edge).
    // HOW: the bar is a horizontal ListView, and its leftMargin is empty space before the first guess. Making
    // it half of the unused width centers the guesses. WHY leftMargin and not moving or resizing the bar: the
    // bar's background is drawn by the ListView itself and must keep spanning the whole keyboard; leftMargin
    // only shifts the guesses, and the ListView re-positions itself whenever the margin changes (Flickable
    // "fixup"), so nothing fights it. It re-centers by itself because it depends on contentWidth, which Qt
    // updates every time the guesses change. With more guesses than fit, the margin is 0, which is exactly
    // the stock behaviour, so scrolling a long list is unaffected.
    // Without the swipe layer (stock keyboard, symbols page) none of this exists, so nothing changes.
    Binding {
        target: root.candidateView
        property: "leftMargin"
        when: root.candidateView !== null
        value: root.candidateView ? Math.max(0, Math.floor((root.candidateView.width - root.candidateView.contentWidth) / 2)) : 0
        restoreMode: Binding.RestoreValue
    }

    // The Keyboard item that owns the touch area (it exposes alternativeKeys and pressAndHoldDelay).
    function keyboardItem() {
        var it = root.touchArea
        while (it && it.alternativeKeys === undefined)
            it = it.parent
        return it
    }

    // WHY alternativeKeys.active (not touchArea.initialKey): Qt also nulls initialKey when a finger drifts
    // off its key (releaseInaccuracyTimer), which is exactly what a swipe does, so it can't tell "popup
    // opened" from "started swiping". The popup's own active flag is the real signal. We latch it, so
    // once seen it stays seen for the rest of the touch even though reset()/close() clear it.
    // Fallback if the Keyboard item can't be found: a hold longer than pressAndHoldDelay (500ms default)
    // counts as a hold, the same moment Qt's own timer fires.
    function popupOpenedForThisTouch() {
        if (root.popupSeen)
            return true
        var kb = root.keyboardItem()
        if (kb) {
            // WHY only alternativeKeys: Qt's function-key popup (functionPopupList) is private to Keyboard.qml,
            // so it can't be read from here, and it only opens from the settings key, never a letter.
            if (kb.alternativeKeys.active)
                root.popupSeen = true
        } else if (Date.now() - root.startTime >= 500) {
            root.popupSeen = true
        }
        return root.popupSeen
    }

    function isLetterKey(item) {
        return !!item && item.key !== undefined && item.key >= Qt.Key_A && item.key <= Qt.Key_Z
    }

    function letterKeys() {
        var out = []
        function walk(item) {
            for (var i = 0; i < item.children.length; ++i) {
                var c = item.children[i]
                if (root.isLetterKey(c)) {
                    if (c.visible) {
                        var p = c.mapToItem(root.touchArea, 0, 0)
                        out.push({ letter: String(c.text).toLowerCase(), x: p.x, y: p.y, w: c.width, h: c.height })
                    }
                } else {
                    walk(c)
                }
            }
        }
        walk(layoutItem)
        return out
    }

    function trackedPoint(touchPoints) {
        for (var i = 0; i < touchPoints.length; ++i)
            if (touchPoints[i].pointId === trackedId)
                return touchPoints[i]
        return null
    }

    function stop() {
        trackedId = -1
        points = []
        trailPoints = []
        swiping = false
        popupSeen = false
        startKey = null
        trail.requestPaint()
    }

    Connections {
        target: root.touchArea

        function onPressed(touchPoints) {
            if (root.trackedId !== -1) {     // a second finger: not a swipe
                if (!root.swiping)
                    root.stop()
                return
            }
            var im = InputContext.inputEngine.inputMethod
            if (!im || typeof im.finishSwipe !== "function" || !im.isReady())
                return
            // WHY: no swiping in password fields: the trail would show the password's shape on screen and
            // the guess bar would show the guessed word. Plain tapping still works. (Not ImhNoPredictiveText:
            // Plasma Keyboard sets that on every field.)
            if (InputContext.inputMethodHints & (Qt.ImhSensitiveData | Qt.ImhHiddenText))
                return
            var tp = touchPoints[0]
            // WHY: a drag from a non-letter key can never become a swipe, so don't collect points for it.
            var key = root.touchArea.keyOnPoint(tp.x, tp.y)
            if (!root.isLetterKey(key))
                return
            root.trackedId = tp.pointId
            root.startTime = Date.now()
            root.popupSeen = false
            root.points = [{ x: tp.x, y: tp.y, t: 0 }]
            root.trailPoints = [root.toTrail(tp.x, tp.y)]
            root.startKey = key
        }

        function onUpdated(touchPoints) {
            var tp = root.trackedPoint(touchPoints)
            if (!tp)
                return
            root.points.push({ x: tp.x, y: tp.y, t: Date.now() - root.startTime })
            root.trailPoints.push(root.toTrail(tp.x, tp.y))
            // WHY popup check: a swipe may only start before the press-and-hold popup opens.
            if (!root.swiping && root.isLetterKey(root.startKey) && !root.popupOpenedForThisTouch()) {
                var dx = tp.x - root.points[0].x, dy = tp.y - root.points[0].y
                if (Math.sqrt(dx * dx + dy * dy) > root.startKey.width * root.thresholdKeys) {
                    root.swiping = true
                    // WHY virtualKeyCancel first: Qt already told the input engine "this key is down". Without
                    // cancelling, the engine keeps it down: the next tap is refused ("key already active") and a
                    // held Space/Backspace keeps auto-repeating. This is how Qt itself drops a press when the
                    // accent popup opens (Keyboard.qml pressAndHoldTimer). It also stops the repeat timer.
                    InputContext.inputEngine.virtualKeyCancel()
                    // Then clear the touch area's own state so lifting the finger types no letter.
                    root.touchArea.reset()
                }
            }
            if (root.swiping)
                trail.requestPaint()
        }

        function onReleased(touchPoints) {
            var tp = root.trackedPoint(touchPoints)
            if (!tp)
                return
            if (root.swiping) {
                root.points.push({ x: tp.x, y: tp.y, t: Date.now() - root.startTime })
                // WHY re-check: the input method can be swapped mid-swipe; same guard as onPressed.
                var im = InputContext.inputEngine.inputMethod
                if (im && typeof im.finishSwipe === "function")
                    im.finishSwipe(root.points, root.letterKeys())
            }
            root.stop()
        }

        function onCanceled(touchPoints) {
            if (root.trackedPoint(touchPoints))
                root.stop()
        }
    }

    // Touch-area point -> trail point. WHY mapToItem (not a fixed offset): the trail lives on a different
    // item (the Keyboard root), and Qt does the exact conversion whatever margins the style adds.
    function toTrail(x, y) {
        var p = root.touchArea.mapToItem(trail, x, y)
        return { x: p.x, y: p.y }
    }

    // The visible trail. Declared here so it is created and destroyed together with this item (it stays
    // this item's child for memory purposes), but drawn as a child of the Keyboard root (see the top comment).
    Canvas {
        id: trail
        objectName: "swipeTrail"        // lets tests find it
        // WHY the Keyboard root: it holds the key area, the guess bar and the popups as siblings, so a big
        // z there puts the trail over the keys. Qt's own highest z among them is 2 (navigation highlight),
        // so 10000 is safely above. It is NOT inside the layout Loader, so Qt's childAt() key lookup never
        // sees it. Fallback (Keyboard not found, e.g. in tests): draw on this item, the old behaviour.
        parent: root.keyboardItem() || root
        anchors.fill: parent
        z: 10000
        enabled: false                  // WHY: the trail is only paint; it must never catch a touch
        // WHY: the trail and the path it draws are only meaningful while the swipe layer is alive.
        visible: root.visible

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            var pts = root.trailPoints
            if (!root.swiping || pts.length < 2)
                return
            ctx.lineWidth = 8
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            ctx.globalAlpha = 0.8
            ctx.strokeStyle = root.trailColor
            ctx.beginPath()
            ctx.moveTo(pts[0].x, pts[0].y)
            for (var i = 1; i < pts.length; ++i)
                ctx.lineTo(pts[i].x, pts[i].y)
            ctx.stroke()
        }
    }
}
