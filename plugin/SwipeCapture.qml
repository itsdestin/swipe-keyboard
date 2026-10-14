import QtQuick
import QtQuick.VirtualKeyboard

// SwipeCapture — swipe-keyboard's finger listener and trail.
// WHY a separate layer: Qt's keyboard records paths only in handwriting boxes, never across keys.
// It only LISTENS to the keyboard's own touch area, so taps work exactly as before. Once a finger that
// started on a letter drags past `threshold`, it cancels that key press, draws the trail, and on lift
// hands the path to SwipeInputMethod. It must never sit inside the layout: Qt finds the key under a
// finger with childAt() on the layout, and an overlay there would swallow every tap.
Canvas {
    id: root

    required property Item layoutItem   // the KeyboardLayout (to find the letter keys)
    required property Item touchArea    // Qt VK's keyboard.keyboardInputArea

    anchors.fill: parent
    enabled: false                      // never takes input itself
    z: 1000

    property color trailColor: "#3daee9" // Breeze highlight blue
    // Drag distance (fraction of a key width) before a touch counts as a swipe. Tuned in M1.
    property real thresholdKeys: 0.5

    property var points: []
    property int trackedId: -1
    property real startTime: 0
    property Item startKey: null
    property bool swiping: false

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
        swiping = false
        startKey = null
        requestPaint()
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
            var tp = touchPoints[0]
            root.trackedId = tp.pointId
            root.startTime = Date.now()
            root.points = [{ x: tp.x, y: tp.y, t: 0 }]
            root.startKey = root.touchArea.keyOnPoint(tp.x, tp.y)
        }

        function onUpdated(touchPoints) {
            var tp = root.trackedPoint(touchPoints)
            if (!tp)
                return
            root.points.push({ x: tp.x, y: tp.y, t: Date.now() - root.startTime })
            if (!root.swiping && root.isLetterKey(root.startKey)) {
                var dx = tp.x - root.points[0].x, dy = tp.y - root.points[0].y
                if (Math.sqrt(dx * dx + dy * dy) > root.startKey.width * root.thresholdKeys) {
                    root.swiping = true
                    // Cancels Qt's pending key press so lifting the finger types no letter.
                    root.touchArea.reset()
                }
            }
            if (root.swiping)
                root.requestPaint()
        }

        function onReleased(touchPoints) {
            var tp = root.trackedPoint(touchPoints)
            if (!tp)
                return
            if (root.swiping) {
                root.points.push({ x: tp.x, y: tp.y, t: Date.now() - root.startTime })
                InputContext.inputEngine.inputMethod.finishSwipe(root.points, root.letterKeys())
            }
            root.stop()
        }

        function onCanceled(touchPoints) {
            if (root.trackedPoint(touchPoints))
                root.stop()
        }
    }

    onPaint: {
        var ctx = getContext("2d")
        ctx.reset()
        if (!swiping || points.length < 2)
            return
        ctx.lineWidth = 8
        ctx.lineCap = "round"
        ctx.lineJoin = "round"
        ctx.globalAlpha = 0.8
        ctx.strokeStyle = trailColor
        ctx.beginPath()
        ctx.moveTo(points[0].x, points[0].y)
        for (var i = 1; i < points.length; ++i)
            ctx.lineTo(points[i].x, points[i].y)
        ctx.stroke()
    }
}
