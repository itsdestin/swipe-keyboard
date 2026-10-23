// Spacebar slide: dragging sideways on Space moves the text cursor one character per step; a tap still types a space.
// WHY a real MultiPointTouchArea + real input events: the slide is decided by what SwipeCapture.qml does with the
// touch area's own signals, for a finger AND for a mouse, so both are driven through a real window.
#include <QtTest>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtVirtualKeyboard/QVirtualKeyboardAbstractInputMethod>
#include <QtVirtualKeyboard/QVirtualKeyboardInputContext>
#include <QtVirtualKeyboard/QVirtualKeyboardInputEngine>
#include <memory>

// Records every key the engine hands to the input method (this is where a typed space or a cursor key shows up).
class KeyLog : public QVirtualKeyboardAbstractInputMethod {
    Q_OBJECT
public:
    QList<int> keys;
    QList<QVirtualKeyboardInputEngine::InputMode> inputModes(const QString &) override { return {QVirtualKeyboardInputEngine::InputMode::Latin}; }
    bool setInputMode(const QString &, QVirtualKeyboardInputEngine::InputMode) override { return true; }
    bool setTextCase(QVirtualKeyboardInputEngine::TextCase) override { return true; }
    bool keyEvent(Qt::Key key, const QString &, Qt::KeyboardModifiers) override { keys.append(key); return false; }
    int count(int key) const { return int(keys.count(key)); }
};

class SpacebarSlideTest : public QObject {
    Q_OBJECT
    QQmlEngine engine;
    QQuickWindow window;
    std::unique_ptr<QObject> root, layer;
    QVirtualKeyboardInputContext *ic = nullptr;
    KeyLog method;
    QPointer<QObject> area;
    QPointingDevice *touch = nullptr;
    // Letter keys are 40 px wide, so: slide starts past 20 px (0.5 key), then one character per 12 px (0.3 key).
    static constexpr int spaceY = 175, startX = 300;

    bool build() {
        QQmlComponent fixture(&engine);
        fixture.setData(R"(
            import QtQuick
            import QtQuick.VirtualKeyboard
            Item { width: 800; height: 260
                property var context: InputContext
                property alias touch: touch
                property alias layout: layout
                property int resets: 0
                Item { id: layout; width: 800; height: 220
                    Repeater { model: 26
                        delegate: Item { required property int index
                            readonly property string rows: "qwertyuiopasdfghjklzxcvbnm"
                            property int key: 0x41; property string text: rows[index]
                            x: (index < 10 ? index : index < 19 ? index - 10 : index - 19) * 40
                            y: index < 10 ? 0 : index < 19 ? 50 : 100; width: 40; height: 50 } }
                    Item { objectName: "space"; property int key: Qt.Key_Space; property string text: " "; x: 100; y: 150; width: 400; height: 50 }
                }
                MultiPointTouchArea { id: touch; anchors.fill: layout
                    property bool down: false
                    function keyOnPoint(px, py) { return layout.childAt(px, py) }
                    // Qt's own stock behaviour, imitated: press the key, release it (typing it) unless reset() dropped it.
                    function reset() { down = false; parent.resets++ }
                    onPressed: (pts) => { down = true; InputContext.inputEngine.virtualKeyPress(Qt.Key_Space, " ", 0, false) }
                    onReleased: (pts) => { if (down) InputContext.inputEngine.virtualKeyRelease(Qt.Key_Space, " ", 0); down = false }
                }
            }
        )", QUrl());
        root.reset(fixture.create());
        if (!root) { qWarning() << fixture.errorString(); return false; }
        auto *item = qobject_cast<QQuickItem *>(root.get());
        window.resize(800, 260); item->setParentItem(window.contentItem());
        window.show(); window.requestActivate();
        if (!QTest::qWaitForWindowExposed(&window)) return false;
        ic = qvariant_cast<QVirtualKeyboardInputContext *>(root->property("context"));
        ic->inputEngine()->setInputMethod(&method);
        QQmlComponent capture(&engine, QUrl::fromLocalFile(QStringLiteral(SWIPEKB_CAPTURE_SOURCE)));
        layer.reset(capture.createWithInitialProperties({{"layoutItem", root->property("layout")}, {"touchArea", root->property("touch")}}));
        if (!layer) { qWarning() << capture.errorString(); return false; }
        area = qvariant_cast<QObject *>(root->property("touch"));
        touch = QTest::createTouchDevice();
        return true;
    }
    int resets() const { return root->property("resets").toInt(); }
    void pressAt(bool mouse, int x) { if (mouse) QTest::mousePress(&window, Qt::LeftButton, {}, QPoint(x, spaceY)); else QTest::touchEvent(&window, touch).press(0, QPoint(x, spaceY), &window); }
    void moveTo(bool mouse, int x) { if (mouse) QTest::mouseMove(&window, QPoint(x, spaceY)); else QTest::touchEvent(&window, touch).move(0, QPoint(x, spaceY), &window); }
    void liftAt(bool mouse, int x) { if (mouse) QTest::mouseRelease(&window, Qt::LeftButton, {}, QPoint(x, spaceY)); else QTest::touchEvent(&window, touch).release(0, QPoint(x, spaceY), &window); settle(); }
    // WHY: Qt Quick may hold a finger/mouse move until the next frame and merge neighbours, so what the keyboard
    // has done with a move is only certain after the event loop has run once.
    static void settle() { QTest::qWait(25); }
    // Moves in 2 px strides like a real finger, so a fast slide cannot skip the "past the threshold" moment.
    void dragTo(bool mouse, int from, int to) { const int d = to > from ? 2 : -2; for (int x = from + d; d > 0 ? x <= to : x >= to; x += d) moveTo(mouse, x); settle(); }
private slots:
    // WHY a frozen clock by default: these tests send moves in microseconds, which would read as a flick; with the
    // clock stopped the slide never sees a speed and moves exactly one character per step.
    void init() { method.keys.clear(); layer->setProperty("slideClockMs", 0); }
    void initTestCase() { QVERIFY(build()); QVERIFY(layer->property("measuredLayoutId").toString() != "");}
    void cleanupTestCase() { ic->inputEngine()->setInputMethod(nullptr); }

    void tapAndSloppyTapStillTypeASpace_data() { QTest::addColumn<bool>("mouse"); QTest::addColumn<int>("wobble");
        QTest::newRow("touch, still") << false << 0; QTest::newRow("mouse, still") << true << 0;
        QTest::newRow("touch, wobble under threshold") << false << 14; QTest::newRow("mouse, wobble under threshold") << true << 14; }
    void tapAndSloppyTapStillTypeASpace() {
        QFETCH(bool, mouse); QFETCH(int, wobble);
        const int before = resets();
        pressAt(mouse, startX); if (wobble) { dragTo(mouse, startX, startX + wobble); dragTo(mouse, startX + wobble, startX - wobble); }
        liftAt(mouse, startX);
        QCOMPARE(method.count(Qt::Key_Space), 1);
        QCOMPARE(method.count(Qt::Key_Left) + method.count(Qt::Key_Right), 0);
        QCOMPARE(resets(), before);                         // the key was never taken away from Qt
    }

    void slideMovesOneCharacterPerStepAndTypesNoSpace_data() { QTest::addColumn<bool>("mouse"); QTest::addColumn<int>("distance");
        QTest::addColumn<int>("right"); QTest::addColumn<int>("left");
        // 20 px to start, the first character at once, then one per 12 px: 20 -> 1, 32 -> 2, 44 -> 3, 80 -> 6.
        QTest::newRow("touch right 80px") << false << 80 << 6 << 0; QTest::newRow("mouse right 80px") << true << 80 << 6 << 0;
        QTest::newRow("touch left 80px") << false << -80 << 0 << 6; QTest::newRow("mouse left 80px") << true << -80 << 0 << 6;
        QTest::newRow("just past the threshold") << false << 22 << 1 << 0; QTest::newRow("touch left 44px") << false << -44 << 0 << 3; }
    void slideMovesOneCharacterPerStepAndTypesNoSpace() {
        QFETCH(bool, mouse); QFETCH(int, distance); QFETCH(int, right); QFETCH(int, left);
        const int before = resets();
        pressAt(mouse, startX); dragTo(mouse, startX, startX + distance); liftAt(mouse, startX + distance);
        QCOMPARE(resets(), before + 1);                     // Qt's own hold on the Space key was dropped exactly once
        // WHY: without the engine-level cancel Qt keeps "Space is down": the next tap is refused and a held
        // Space would auto-repeat spaces while sliding.
        QCOMPARE(int(ic->inputEngine()->activeKey()), int(Qt::Key_unknown));
        QCOMPARE(method.count(Qt::Key_Right), right);
        QCOMPARE(method.count(Qt::Key_Left), left);
        QCOMPARE(method.count(Qt::Key_Space), 0);           // a slide must never also type a space
        QCOMPARE(layer->property("swiping").toBool(), false); // and never starts a swipe-typing gesture
        QCOMPARE(layer->property("trackedId").toInt(), -1);
    }

    void reversingDirectionFollowsTheFinger() {
        pressAt(true, startX); dragTo(true, startX, startX + 80);   // 6 right
        dragTo(true, startX + 80, startX + 80 - 36);                 // 3 steps back
        dragTo(true, startX + 44, startX + 44 - 24);                 // 2 more back
        liftAt(true, startX + 20);
        QCOMPARE(method.count(Qt::Key_Right), 6);
        QCOMPARE(method.count(Qt::Key_Left), 5);
        QCOMPARE(method.count(Qt::Key_Space), 0);
    }

    void movesStopAtLiftAndNextTapTypesNormally() {
        pressAt(false, startX); dragTo(false, startX, startX + 60); liftAt(false, startX + 60);
        const int moved = method.count(Qt::Key_Right); QVERIFY(moved > 0);
        moveTo(false, startX + 200); settle();                               // nothing is down: must not move the cursor
        QCOMPARE(method.count(Qt::Key_Right), moved);
        pressAt(false, startX); liftAt(false, startX);
        QCOMPARE(method.count(Qt::Key_Space), 1);                    // slide state did not leak into the next tap
        QCOMPARE(method.count(Qt::Key_Right), moved);
    }

    void slideDoesNotAffectLetterTouchesOrSwipeTyping() {
        // A finger that starts on a letter is never a cursor slide, however far it goes sideways.
        const int y = 25;
        QTest::mousePress(&window, Qt::LeftButton, {}, QPoint(300, y));
        for (int x = 302; x <= 380; x += 2) QTest::mouseMove(&window, QPoint(x, y));
        QTest::mouseRelease(&window, Qt::LeftButton, {}, QPoint(380, y));
        QCOMPARE(method.count(Qt::Key_Left) + method.count(Qt::Key_Right), 0);
    }

    void secondFingerNeverMovesTheCursorOrSteals() {
        // One sequence object for both fingers: it is what remembers where the first finger is.
        auto fingers = QTest::touchEvent(&window, touch);
        fingers.press(0, QPoint(startX, spaceY), &window).commit(); settle();
        for (int x = startX + 4; x <= startX + 40; x += 4) fingers.move(0, QPoint(x, spaceY), &window).commit(); settle();
        const int moved = method.count(Qt::Key_Right); QVERIFY(moved > 0);
        // a second finger pressing a letter must not be read as part of the slide
        fingers.press(1, QPoint(60, 25), &window).commit(); settle();
        fingers.move(1, QPoint(500, 25), &window).commit(); settle();
        QCOMPARE(method.count(Qt::Key_Right), moved);                // the other finger's travel is not ours
        // ...and the slide itself carries on under the first finger
        fingers.move(0, QPoint(startX + 64, spaceY), &window).commit(); settle();
        QVERIFY(method.count(Qt::Key_Right) > moved);
        fingers.release(0, QPoint(startX + 64, spaceY), &window).release(1, QPoint(500, 25), &window).commit(); settle();
    }

    void secondFingerBeforeSlidingEndsTheSpacePress() {
        // Two-thumb typing: thumb one rests on Space, thumb two types a letter; thumb one then drifting must not
        // be mistaken for a slide (Qt already handled both keys).
        auto fingers = QTest::touchEvent(&window, touch);
        fingers.press(0, QPoint(startX, spaceY), &window).commit(); settle();
        fingers.move(0, QPoint(startX + 8, spaceY), &window).commit(); settle();
        fingers.press(1, QPoint(60, 25), &window).commit(); settle();
        fingers.release(1, QPoint(60, 25), &window).commit(); settle();
        QCOMPARE(layer->property("spaceId").toInt(), -1);
        for (int x = startX + 12; x <= startX + 100; x += 4) fingers.move(0, QPoint(x, spaceY), &window).commit(); settle();
        QCOMPARE(method.count(Qt::Key_Left) + method.count(Qt::Key_Right), 0);
        fingers.release(0, QPoint(startX + 100, spaceY), &window).commit(); settle();
    }

    // Drags on the slide's own clock: each 2 px stride takes msPerStride, so the speed is chosen by the test.
    void timedDrag(bool mouse, int from, int to, int pxPerStride, int msPerStride) {
        const int d = to > from ? pxPerStride : -pxPerStride;
        double clock = 1000;
        layer->setProperty("slideClockMs", clock);
        pressAt(mouse, from); settle();
        for (int x = from + d; d > 0 ? x <= to : x >= to; x += d) {
            clock += msPerStride; layer->setProperty("slideClockMs", clock);
            moveTo(mouse, x);
            // WHY settle each move: Qt Quick may merge moves within a frame, which would hide the clock between them.
            QTest::qWait(1);
        }
        settle(); liftAt(mouse, to);
    }

    void aSlowSlideMovesOneCharacterPerStep() {
        // 2 px every 20 ms = 2.5 keys a second, under the 4 keys/s where speed-up starts: 22 px -> 1, then 1 per
        // 12 px: 200 px -> 16, exactly as with no speed-up.
        timedDrag(true, startX, startX + 200, 2, 20);
        QCOMPARE(method.count(Qt::Key_Right), 16);
        QCOMPARE(method.count(Qt::Key_Space), 0);
    }

    void aFastSlideMovesMuchFurtherForTheSameDistance() {
        // 4 px every 4 ms = 25 keys a second, past the 16 keys/s of full speed-up (5x): the same 200 px moves far
        // more than the slow slide's 16 characters, but never more than 5x as many.
        timedDrag(true, startX, startX + 200, 4, 4);
        const int moved = method.count(Qt::Key_Right);
        QVERIFY2(moved > 40, qPrintable(QStringLiteral("moved only %1").arg(moved)));
        QVERIFY2(moved <= 5 * 16 + 1, qPrintable(QStringLiteral("moved %1").arg(moved)));
        QCOMPARE(method.count(Qt::Key_Left), 0);
        QCOMPARE(method.count(Qt::Key_Space), 0);
    }

    void aFastSlideLeftMovesLeftOnly() {
        // From 480 (inside Space, which ends at 500) back to 280: the same 200 px, by finger, leftwards.
        timedDrag(false, startX + 180, startX - 20, 4, 4);
        QVERIFY2(method.count(Qt::Key_Left) > 40, qPrintable(QStringLiteral("moved only %1").arg(method.count(Qt::Key_Left))));
        QCOMPARE(method.count(Qt::Key_Right), 0);
    }

    void losingTheInputMethodEndsTheSlide() {
        // WHY: focus moved to another field / the keyboard changed mid-slide; the rest of the drag must not edit there.
        pressAt(true, startX); dragTo(true, startX, startX + 40);
        const int moved = method.count(Qt::Key_Right); QVERIFY(moved > 0);
        KeyLog other; ic->inputEngine()->setInputMethod(&other);
        dragTo(true, startX + 40, startX + 120);
        QCOMPARE(other.count(Qt::Key_Right), 0);
        QCOMPARE(method.count(Qt::Key_Right), moved);
        liftAt(true, startX + 120);
        QCOMPARE(other.count(Qt::Key_Space), 0);
        ic->inputEngine()->setInputMethod(&method);
    }
};
QTEST_MAIN(SpacebarSlideTest)
#include "tst_spacebarslide.moc"
