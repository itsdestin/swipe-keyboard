#include <QtTest>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>

#include "windowblur.h"

// WindowBlur's blur area must match the visible keyboard panel: its rounded shape, its place in the (screen-tall)
// window, and follow it when it moves or resizes. KWin itself is not reachable here (offscreen), so this checks
// the area we would ask for; whether KWin draws it is checked by eye on the device.
class TstWindowBlur : public QObject {
    Q_OBJECT
private slots:
    void roundedRegionShape()
    {
        const QRegion r = swipekb::roundedRegion(QRect(0, 0, 100, 50), 0, 0, 10, 10);
        QCOMPARE(r.boundingRect(), QRect(0, 0, 100, 50));
        // Square top corners (full-width keyboard): the very corner pixels are blurred.
        QVERIFY(r.contains(QPoint(0, 0)));
        QVERIFY(r.contains(QPoint(99, 0)));
        // Rounded bottom corners: the corner pixel is outside, the middle of the bottom edge inside.
        QVERIFY(!r.contains(QPoint(0, 49)));
        QVERIFY(!r.contains(QPoint(99, 49)));
        QVERIFY(r.contains(QPoint(50, 49)));
        // Just inside the curve (radius 10: 3 px in from the side, 3 px up) is blurred.
        QVERIFY(r.contains(QPoint(3, 46)));
        QVERIFY(swipekb::roundedRegion(QRect(), 5, 5, 5, 5).isEmpty());
        // Oversized radii are clamped instead of producing crossing strips.
        QCOMPARE(swipekb::roundedRegion(QRect(0, 0, 20, 20), 50, 50, 50, 50).boundingRect(), QRect(0, 0, 20, 20));
    }

    void followsPanelAndIsOnePerWindow()
    {
        QQmlEngine engine;
        // Stand-in for Plasma Keyboard's main.qml: a screen-tall window, a panel at the bottom with a radius,
        // the keyboard nested inside it.
        QQmlComponent c(&engine);
        c.setData("import QtQuick\n"
                  "Rectangle { id: panel; x: 0; y: 700; width: 400; height: 100; radius: 8\n"
                  "  property alias inner: kb\n"
                  "  Item { anchors.fill: parent; Item { id: kb; anchors.fill: parent } } }\n",
                  QUrl());
        std::unique_ptr<QObject> obj(c.create());
        QVERIFY2(obj, qPrintable(c.errorString()));
        auto *panel = qobject_cast<QQuickItem *>(obj.get());
        QQuickWindow window;
        window.resize(400, 800);
        panel->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *kb = panel->property("inner").value<QQuickItem *>();

        auto blur = std::make_unique<WindowBlur>();
        blur->setTarget(kb);
        // Only the panel, never the whole screen-tall window.
        QCOMPARE(blur->blurBounds(), QRect(0, 700, 400, 100));

        panel->setY(650);
        panel->setHeight(150);   // e.g. the guess bar appeared
        QTRY_COMPARE(blur->blurBounds(), QRect(0, 650, 400, 150));

        // The layout is destroyed and recreated on page switches: the blur must stay, and never be doubled.
        blur.reset();
        QCOMPARE(window.findChildren<PanelBlurTracker *>().size(), 1);
        WindowBlur again;
        again.setTarget(kb);
        QCOMPARE(window.findChildren<PanelBlurTracker *>().size(), 1);
        QCOMPARE(again.blurBounds(), QRect(0, 650, 400, 150));

        panel->setVisible(false);
        QTRY_VERIFY(again.blurBounds().isEmpty());   // hidden panel: blur switched off
    }
};

QTEST_MAIN(TstWindowBlur)
#include "tst_windowblur.moc"
