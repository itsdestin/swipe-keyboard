import QtQuick
import QtQuick.VirtualKeyboard
import QtQuick.VirtualKeyboard.Settings
import QtQuick.VirtualKeyboard.Styles

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

    // The trail follows the system accent colour (the owner's request), live: changing the accent in System
    // Settings recolours the next swipe without restarting the keyboard. Same colour group (Window) as the
    // keyboard's own style, so it matches the accent Plasma Keyboard uses for its highlights.
    // WHY through `themeProbe` (not `import org.kde.kirigami` at the top): a top-level import that fails would
    // make this whole file fail to load, i.e. no swipe typing at all. Plasma Keyboard itself imports Kirigami,
    // so it is always there in practice; if it ever is not, the trail falls back to Breeze blue.
    property color trailColor: themeProbe ? themeProbe.accent : "#3daee9"
    property QtObject themeProbe: null
    function makeThemeProbe(importLine) {
        try {
            // WHY no `visible: false`: Kirigami only resolves theme colours for visible items, so a hidden probe
            // reported #000000 and the trail drew black (seen live 2026-10-14). A zero-size item draws nothing
            // and takes no input, so leaving it visible is harmless.
            return Qt.createQmlObject(importLine + "\nItem { width: 0; height: 0; Kirigami.Theme.inherit: false; "
                                      + "Kirigami.Theme.colorSet: Kirigami.Theme.Window; "
                                      + "readonly property color accent: Kirigami.Theme.highlightColor }", root, "swipe-theme")
        } catch (e) {
            return null
        }
    }
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
    property var originMethod: null
    property int originGeneration: -1
    property var originKeys: []
    // WHY physical identity comes from measured key order, never from the locale name.
    readonly property string measuredLayoutId: identifyLayout(letterKeys())
    onMeasuredLayoutIdChanged: syncProfile()
    function syncProfile() {
        var im = InputContext.inputEngine.inputMethod
        if (im && typeof im.setPhysicalLayout === "function")
            im.setPhysicalLayout(root.identifyLayout(root.letterKeys()))
    }
    function identifyLayout(keys) {
        if (keys.length < 26) return "unknown"
        var sorted = keys.slice().sort(function(a, b) { return a.y === b.y ? a.x - b.x : a.y - b.y })
        var letters = sorted.map(function(k) { return k.letter }).filter(function(c) { return /^[a-z]$/.test(c) }).join("")
        if (letters === "qwertyuiopasdfghjklzxcvbnm") return "qwerty"
        if (letters === "qwertzuiopasdfghjklyxcvbnm") return "qwertz"
        if (letters === "azertyuiopqsdfghjklmwxcvbn") return "azerty"
        return "unknown"
    }
    // WHY bounded: an abandoned/held touch must not accumulate an unbounded path or block the UI.
    readonly property int maxPoints: 2048
    readonly property int maxDuration: 10000

    function cancelGesture() {
        if (root.trackedId === -1)
            return
        // WHY clear first: cancellation may synchronously deliver another reset/cancel signal.
        root.stop()
        InputContext.inputEngine.virtualKeyCancel()
        if (typeof root.touchArea.reset === "function")
            root.touchArea.reset()
    }

    function validGesture() {
        return root.originMethod !== null
            && root.originMethod === InputContext.inputEngine.inputMethod
            && root.originMethod.validateSwipe(root.originGeneration)
            && !(InputContext.inputMethodHints & (Qt.ImhSensitiveData | Qt.ImhHiddenText))
            && Date.now() - root.startTime <= root.maxDuration
            && root.points.length < root.maxPoints
            && JSON.stringify(root.originKeys) === JSON.stringify(root.letterKeys())
    }

    // WHY: a stationary finger emits no movement/hint signal when the proxy changes purpose.
    // Bound work to the held contact, reuse the same public live gate, and erase the trail on refusal.
    Timer {
        interval: 50
        repeat: true
        running: root.trackedId !== -1
        onTriggered: {
            if (!root.validGesture())
                root.cancelGesture()
        }
    }
    Timer {
        interval: root.maxDuration
        running: root.trackedId !== -1
        onTriggered: root.cancelGesture()
    }
    Connections {
        target: root.originMethod
        function onGestureInvalidated() { root.cancelGesture() }
    }
    Connections {
        target: InputContext
        function onInputItemChanged() { root.cancelGesture() }
        function onInputMethodHintsChanged() { root.cancelGesture() }
    }
    Connections {
        target: InputContext.inputEngine
        function onInputMethodChanged() { root.cancelGesture(); root.syncProfile() }
        function onInputMethodReset() { root.cancelGesture() }
    }
    // WHY immediate cancellation: the existing points are in the old key coordinate system.
    Connections {
        target: root.layoutItem
        function onWidthChanged() { root.cancelGesture() }
        function onHeightChanged() { root.cancelGesture() }
        function onXChanged() { root.cancelGesture() }
        function onYChanged() { root.cancelGesture() }
        function onScaleChanged() { root.cancelGesture() }
        function onRotationChanged() { root.cancelGesture() }
    }
    Connections {
        target: root.touchArea
        function onWidthChanged() { root.cancelGesture() }
        function onHeightChanged() { root.cancelGesture() }
        function onXChanged() { root.cancelGesture() }
        function onYChanged() { root.cancelGesture() }
    }

    // WHY: keep the guess bar always shown so the keyboard stays one height instead of growing on each swipe
    // and making the app above jump. Set once and never undone: this layer is destroyed on "?123", but the
    // swipe input method (and its guess bar) stays active on the symbols page, so undoing it there would
    // bring the jump back on every ?123/ABC toggle. Safe to leave on: Qt only applies it while the active
    // input method has a guess bar (Keyboard.qml alwaysVisibleCondition requires
    // wordCandidateListVisibleHint), so pages and keyboards without one (numbers, digits, dialpad, stock
    // keyboard) keep their normal height. It lives in memory only, and --uninstall restarts the keyboard.
    Component.onCompleted: {
        root.themeProbe = root.makeThemeProbe("import QtQuick; import org.kde.kirigami as Kirigami")
        VirtualKeyboardSettings.wordCandidateList.alwaysVisible = true
        root.candidateView = root.findCandidateView()
        root.syncProfile()
        Qt.callLater(root.installPredictionCaptures)
    }
    // WHY: give the guess bar back its stock left alignment when this layer goes away (the "?123" page, or
    // swipe-keyboard uninstalled while running). The Binding below stops with us, but the last value it wrote
    // would otherwise stay. A new swipe layer re-centers at once.
    Component.onDestruction: {
        // WHY the delegate owns the dynamic PointHandler: its deletion can leave a truthy
        // stale JS wrapper in this list. Destroy only still-live dynamically created handlers.
        for (var i = 0; i < root.predictionCaptures.length; ++i) {
            var handler = root.predictionCaptures[i].handler
            if (handler && typeof handler.destroy === "function")
                handler.destroy()
        }
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

    // WHY module-local delegate: Breeze puts a separator before EVERY item,
    // including the outer edge. Keep stock SelectionListItem input/selection, but
    // render only internal dividers and equal slots so rank one is truly centered.
    SystemPalette { id: candidatePalette; colorGroup: SystemPalette.Active }
    Component {
        id: candidateDelegate
        SelectionListItem {
            id: candidateItem
            required property int index
            required property string display
            objectName: "swipeCandidate" + index
            width: root.candidateView ? root.candidateView.width / Math.max(3, root.candidateView.count) : 0
            Text {
                anchors.centerIn: parent
                width: Math.max(0, parent.width - 24)
                text: candidateItem.display
                color: candidatePalette.text
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                font.family: Qt.application.font.family
                font.pixelSize: Math.max(12, parent.height * 0.52)
            }
            Rectangle {
                objectName: "swipeCandidateDivider"
                visible: candidateItem.index > 0
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(1, parent.height * 0.025)
                height: parent.height * 0.42
                color: candidatePalette.mid
            }
        }
    }
    Binding {
        target: root.candidateView
        property: "delegate"
        when: root.candidateView !== null && root.candidateView.model !== null
            && typeof root.candidateView.model.selectItem === "function"
        value: candidateDelegate
        restoreMode: Binding.RestoreBindingOrValue
    }

    // WHY passive press capture: Qt's stock candidate callback carries only an
    // index. Freeze prediction authority before the stock delegate handles release;
    // a refresh during a held touch must reject that old tap.
    property var predictionTapCapture: null
    property var predictionCaptures: []
    function installPredictionCaptures() {
        if (!root.candidateView)
            return
        root.predictionCaptures = root.predictionCaptures.filter(function(c) { return c.handler && c.target })
        function walk(item) {
            if (item.acceptedButtons !== undefined && item.pressAndHoldInterval !== undefined) {
                if (!root.predictionCaptures.some(function(c) { return c.target === item })) {
                    var handler = predictionTapComponent.createObject(item)
                    root.predictionCaptures.push({target: item, handler: handler})
                    root.predictionTapCapture = handler
                }
            }
            for (var i = 0; i < item.children.length; ++i)
                walk(item.children[i])
        }
        walk(root.candidateView.contentItem)
    }
    Connections {
        target: root.candidateView
        function onCountChanged() { Qt.callLater(root.installPredictionCaptures) }
    }
    Connections {
        target: root.candidateView ? root.candidateView.contentItem : null
        function onChildrenChanged() { Qt.callLater(root.installPredictionCaptures) }
    }
    Component {
        id: predictionTapComponent
        PointHandler {
            property var pressMethod: null
            property var pressId: 0
            function clearPress(deferred) {
                var method = pressMethod
                var id = pressId
                pressMethod = null
                pressId = 0
                if (!method || typeof method.endPredictionSelection !== "function")
                    return
                // WHY release cleanup is deferred: stock MouseArea.onClicked runs
                // during this release event. Canceled gestures revoke immediately.
                if (deferred)
                    Qt.callLater(function() { if (method) method.endPredictionSelection(id) })
                else
                    method.endPredictionSelection(id)
            }
            onCanceled: clearPress(false)
            onActiveChanged: {
                if (active) {
                    pressMethod = InputContext.inputEngine.inputMethod
                    if (pressMethod && typeof pressMethod.beginPredictionSelection === "function")
                        pressId = pressMethod.beginPredictionSelection(pressMethod.predictionSelectionToken())
                } else {
                    clearPress(true)
                }
            }
            Component.onDestruction: clearPress(false)
        }
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
        return !!item && item.key !== undefined && typeof item.text === "string"
            && /^[A-Za-zÀ-ÖØ-öø-ÿŒœẞ]$/.test(item.text)
    }

    function letterKeys() {
        var out = []
        function walk(item) {
            if (!item) return
            for (var i = 0; i < item.children.length; ++i) {
                var c = item.children[i]
                if (root.isLetterKey(c)) {
                    if (c.visible) {
                        var p = c.mapToItem(root.touchArea, 0, 0)
                        // WHY localX/Y: mapToItem() alone does not establish QML binding dependencies on key positions.
                        out.push({ letter: String(c.text).toLowerCase(), x: p.x, y: p.y, w: c.width, h: c.height,
                                   localX: c.x, localY: c.y })
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
        originMethod = null
        originGeneration = -1
        originKeys = []
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
            root.syncProfile()
            if (!im || typeof im.finishSwipe !== "function" || !im.isReady()
                    || !im.isReadyForLayout(root.letterKeys()))
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
            var generation = im.beginSwipe()
            if (generation < 0)
                return
            root.originMethod = im
            root.originGeneration = generation
            root.originKeys = root.letterKeys()
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
            if (!root.validGesture()) {
                root.cancelGesture()
                return
            }
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
            if (!root.validGesture()) {
                root.cancelGesture()
                return
            }
            if (root.swiping) {
                root.points.push({ x: tp.x, y: tp.y, t: Date.now() - root.startTime })
                // WHY origin, not whichever method is now active: resets must revoke the held path.
                root.originMethod.finishSwipe(root.points, root.originKeys, root.originGeneration)
            }
            root.stop()
        }

        function onCanceled(touchPoints) {
            if (root.trackedPoint(touchPoints))
                root.cancelGesture()
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
