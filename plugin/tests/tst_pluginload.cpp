#include <QtTest>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>

// Loads the built module the way Plasma Keyboard will: by import path, then by type name.
// SWIPEKB_QML_DIR (env) lets install.sh's check point this at the installed copy instead of the build.
static QString qmlDir() { return qEnvironmentVariable("SWIPEKB_QML_DIR", QStringLiteral(SWIPEKB_QML_DIR)); }

class TstPluginLoad : public QObject {
    Q_OBJECT
private slots:
    void inputMethodLoadsAndFindsModels()
    {
        QQmlEngine engine;
        engine.addImportPath(qmlDir());
        QQmlComponent c(&engine);
        c.setData("import QtQuick\nimport SwipeKeyboard\nSwipeInputMethod {}\n", QUrl());
        std::unique_ptr<QObject> im(c.create());
        QVERIFY2(im, qPrintable(c.errorString()));
        bool ready = false;
        QVERIFY(QMetaObject::invokeMethod(im.get(), "isReady", Q_RETURN_ARG(bool, ready)));
        QVERIFY2(ready, "models not found — SWIPEKB_DATA should point at assets/");
        // No input context attached: a swipe must be ignored, not crash.
        QVERIFY(QMetaObject::invokeMethod(im.get(), "finishSwipe", Q_ARG(QVariantList, QVariantList()), Q_ARG(QVariantList, QVariantList())));
    }
    void captureComponentCompiles()
    {
        QQmlEngine engine;
        engine.addImportPath(qmlDir());
        QQmlComponent c(&engine, QStringLiteral("SwipeKeyboard"), QStringLiteral("SwipeCapture"));
        QVERIFY2(c.status() == QQmlComponent::Ready, qPrintable(c.errorString()));
    }
    // The trail uses the system accent colour (Kirigami's highlight, Window colours, like the keyboard style),
    // and falls back to Breeze blue when Kirigami cannot be loaded instead of breaking the swipe layer.
    void trailFollowsSystemAccent()
    {
        QQmlEngine engine;
        engine.addImportPath(qmlDir());
        QQmlComponent probeC(&engine);
        probeC.setData("import QtQuick\nimport org.kde.kirigami as Kirigami\n"
                       "Item { Kirigami.Theme.inherit: false; Kirigami.Theme.colorSet: Kirigami.Theme.Window\n"
                       "       readonly property color accent: Kirigami.Theme.highlightColor }\n",
                       QUrl());
        std::unique_ptr<QObject> probe(probeC.create());
        QVERIFY2(probe, qPrintable(probeC.errorString()));

        QQmlComponent c(&engine, QStringLiteral("SwipeKeyboard"), QStringLiteral("SwipeCapture"));
        QVERIFY2(c.status() == QQmlComponent::Ready, qPrintable(c.errorString()));
        QQuickItem dummy;
        const QVariantMap props{{"layoutItem", QVariant::fromValue(&dummy)}, {"touchArea", QVariant::fromValue(&dummy)}};
        std::unique_ptr<QObject> layer(c.createWithInitialProperties(props));
        QVERIFY2(layer, qPrintable(c.errorString()));
        QVERIFY(layer->property("themeProbe").value<QObject *>());
        QCOMPARE(layer->property("trailColor").value<QColor>(), probe->property("accent").value<QColor>());

        // Kirigami missing: no probe, Breeze blue, no error thrown into the layer.
        QVariant none;
        QVERIFY(QMetaObject::invokeMethod(layer.get(), "makeThemeProbe", Q_RETURN_ARG(QVariant, none),
                                          Q_ARG(QVariant, QStringLiteral("import QtQuick; import NoSuchKirigami as Kirigami"))));
        QVERIFY(none.value<QObject *>() == nullptr);
        layer->setProperty("themeProbe", QVariant::fromValue<QObject *>(nullptr));
        QCOMPARE(layer->property("trailColor").value<QColor>(), QColor("#3daee9"));
    }
    // WindowBlur (the glass keyboard's blur request) is part of the module, so the layout can create it by name.
    void windowBlurTypeLoads()
    {
        QQmlEngine engine;
        engine.addImportPath(qmlDir());
        QQmlComponent c(&engine, QStringLiteral("SwipeKeyboard"), QStringLiteral("WindowBlur"));
        QVERIFY2(c.status() == QQmlComponent::Ready, qPrintable(c.errorString()));
        std::unique_ptr<QObject> blur(c.create());
        QVERIFY2(blur, qPrintable(c.errorString()));
        QVERIFY(blur->property("blurBounds").toRect().isEmpty());   // no target yet: asks for nothing
    }
    // The guess bar must be pinned once a swipe layer exists and STAY pinned after the layer is destroyed.
    // WHY: the layer is destroyed on "?123", but the swipe input method and its guess bar stay active on the
    // symbols page; unpinning there would make the keyboard height jump on every ?123/ABC toggle.
    void guessBarStaysPinnedAfterCaptureDestroyed()
    {
        QQmlEngine engine;
        engine.addImportPath(qmlDir());
        QQmlComponent probeC(&engine);
        probeC.setData("import QtQuick\nimport QtQuick.VirtualKeyboard.Settings\n"
                       "Item { property bool pinned: VirtualKeyboardSettings.wordCandidateList.alwaysVisible\n"
                       "       function setPinned(v) { VirtualKeyboardSettings.wordCandidateList.alwaysVisible = v } }\n",
                       QUrl());
        std::unique_ptr<QObject> probe(probeC.create());
        QVERIFY2(probe, qPrintable(probeC.errorString()));
        QMetaObject::invokeMethod(probe.get(), "setPinned", Q_ARG(QVariant, false));
        QCOMPARE(probe->property("pinned").toBool(), false);

        QQmlComponent c(&engine, QStringLiteral("SwipeKeyboard"), QStringLiteral("SwipeCapture"));
        QVERIFY2(c.status() == QQmlComponent::Ready, qPrintable(c.errorString()));
        QQuickItem dummy;
        const QVariantMap props{{"layoutItem", QVariant::fromValue(&dummy)}, {"touchArea", QVariant::fromValue(&dummy)}};
        std::unique_ptr<QObject> layer(c.createWithInitialProperties(props));
        QVERIFY2(layer, qPrintable(c.errorString()));
        QCOMPARE(probe->property("pinned").toBool(), true);
        layer.reset(); // like switching to the symbols page
        QCOMPARE(probe->property("pinned").toBool(), true);
    }
    // The trail must draw ABOVE the keys. WHY: inside Qt's touch area it painted under the keys (the layout
    // Loader stacks the keys over the touch area). It must sit on the Keyboard root, above every sibling,
    // never take input, and disappear together with the swipe layer (destroyed on "?123").
    void trailDrawsOnKeyboardRootAndDiesWithLayer()
    {
        QQmlEngine engine;
        engine.addImportPath(qmlDir());
        // A stand-in for Qt's Keyboard: the root has `alternativeKeys` (how SwipeCapture recognizes it) and a
        // Loader-like child holding the touch area (a real MultiPointTouchArea, like Qt's keyboardInputArea),
        // plus a sibling with Qt's highest z (2).
        QQmlComponent kbC(&engine);
        kbC.setData("import QtQuick\n"
                    "Item { width: 400; height: 300; property var alternativeKeys: ({ active: false })\n"
                    "  property alias touchArea: ta; property alias layout: lay\n"
                    "  Item { z: 2 }\n"
                    "  Item { x: 10; y: 50; width: 380; height: 240\n"
                    "    MultiPointTouchArea { id: ta; anchors.fill: parent }\n"
                    "    Item { id: lay; anchors.fill: parent } } }\n",
                    QUrl());
        std::unique_ptr<QObject> kbObj(kbC.create());
        QVERIFY2(kbObj, qPrintable(kbC.errorString()));
        auto *kb = qobject_cast<QQuickItem *>(kbObj.get());
        auto *ta = kb->property("touchArea").value<QQuickItem *>();
        auto *lay = kb->property("layout").value<QQuickItem *>();
        QVERIFY(ta && lay);
        const int childrenBefore = kb->childItems().size();

        QQmlComponent c(&engine, QStringLiteral("SwipeKeyboard"), QStringLiteral("SwipeCapture"));
        QVERIFY2(c.status() == QQmlComponent::Ready, qPrintable(c.errorString()));
        const QVariantMap props{{"layoutItem", QVariant::fromValue(lay)}, {"touchArea", QVariant::fromValue(ta)},
                                {"parent", QVariant::fromValue(ta)}};
        std::unique_ptr<QObject> layer(c.createWithInitialProperties(props));
        QVERIFY2(layer, qPrintable(c.errorString()));

        auto *trail = layer->findChild<QQuickItem *>(QStringLiteral("swipeTrail"));
        QVERIFY(trail);
        QCOMPARE(trail->parentItem(), kb);
        QVERIFY(!trail->isEnabled());
        for (QQuickItem *sib : kb->childItems())
            if (sib != trail)
                QVERIFY(trail->z() > sib->z());
        // Touch-area point (0,0) lands at the touch area's offset inside the Keyboard root.
        QVariant mapped;
        QVERIFY(QMetaObject::invokeMethod(layer.get(), "toTrail", Q_RETURN_ARG(QVariant, mapped),
                                          Q_ARG(QVariant, 0), Q_ARG(QVariant, 0)));
        QCOMPARE(mapped.toMap().value("x").toDouble(), 10.0);
        QCOMPARE(mapped.toMap().value("y").toDouble(), 50.0);

        layer.reset(); // like switching to the symbols page
        QCOMPARE(kb->childItems().size(), childrenBefore);
    }
    // The guess bar's words must sit centered (the owner's request), re-center when the words change, fall
    // back to Qt's left alignment when they overflow, and go back to stock when the swipe layer is destroyed.
    // WHY a real ListView in a real (offscreen) window: the centering relies on how Qt's ListView positions
    // its content when leftMargin changes, so the test must run Qt's own ListView code, not a mock.
    void guessBarIsCentered()
    {
        QQmlEngine engine;
        engine.addImportPath(qmlDir());
        // Stand-in for Qt's Keyboard root: `alternativeKeys` (how SwipeCapture finds it) and the guess bar as a
        // direct child with Keyboard.qml's own settings (objectName, horizontal, snap, anchors left/right).
        // Words are 100 px wide each, the bar 600 px.
        QQmlComponent kbC(&engine);
        kbC.setData("import QtQuick\n"
                    "Item { width: 600; height: 300; property var alternativeKeys: ({ active: false })\n"
                    "  property alias touchArea: ta; property alias bar: wcv\n"
                    "  function setWords(n) { words.clear(); for (var i = 0; i < n; ++i) words.append({ w: 'w' + i }) }\n"
                    "  function firstX() { wcv.forceLayout(); var it = wcv.itemAtIndex(0); return it ? it.mapToItem(wcv, 0, 0).x : -1 }\n"
                    "  ListModel { id: words }\n"
                    "  ListView { id: wcv; objectName: 'wordCandidateView'; clip: true; height: 40\n"
                    "    anchors.left: parent.left; anchors.right: parent.right; spacing: 0\n"
                    "    orientation: ListView.Horizontal; snapMode: ListView.SnapToItem; model: words\n"
                    "    delegate: Item { width: 100; height: 40 } }\n"
                    "  Item { y: 40; width: 600; height: 260; MultiPointTouchArea { id: ta; anchors.fill: parent } } }\n",
                    QUrl());
        std::unique_ptr<QObject> kbObj(kbC.create());
        QVERIFY2(kbObj, qPrintable(kbC.errorString()));
        auto *kb = qobject_cast<QQuickItem *>(kbObj.get());
        QQuickWindow window;
        window.resize(600, 300);
        kb->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *bar = kb->property("bar").value<QQuickItem *>();
        auto *ta = kb->property("touchArea").value<QQuickItem *>();
        auto firstX = [&] {
            QVariant x;
            QMetaObject::invokeMethod(kb, "firstX", Q_RETURN_ARG(QVariant, x));
            return x.toDouble();
        };
        auto setWords = [&](int n) { QMetaObject::invokeMethod(kb, "setWords", Q_ARG(QVariant, n)); };

        setWords(2);
        QCOMPARE(firstX(), 0.0);   // stock: left-aligned before the swipe layer exists

        QQmlComponent c(&engine, QStringLiteral("SwipeKeyboard"), QStringLiteral("SwipeCapture"));
        QVERIFY2(c.status() == QQmlComponent::Ready, qPrintable(c.errorString()));
        const QVariantMap props{{"layoutItem", QVariant::fromValue(ta)}, {"touchArea", QVariant::fromValue(ta)},
                                {"parent", QVariant::fromValue(ta)}};
        std::unique_ptr<QObject> layer(c.createWithInitialProperties(props));
        QVERIFY2(layer, qPrintable(c.errorString()));

        QTRY_COMPARE(firstX(), 200.0);   // 2 words (200 px) in 600 px: 200 px empty on each side
        setWords(3);
        QTRY_COMPARE(firstX(), 150.0);   // re-centers when the words change
        setWords(0);
        setWords(1);
        QTRY_COMPARE(firstX(), 250.0);   // empty, then one word (after each swipe the list is rebuilt)
        setWords(9);
        QTRY_COMPARE(firstX(), 0.0);     // more words than fit: stock left alignment, normal scrolling
        QCOMPARE(bar->property("leftMargin").toDouble(), 0.0);
        setWords(2);
        QTRY_COMPARE(firstX(), 200.0);

        layer.reset();                   // swipe layer gone ("?123" page): stock left alignment again
        QCOMPARE(bar->property("leftMargin").toDouble(), 0.0);
        QTRY_COMPARE(firstX(), 0.0);
    }
};

QTEST_MAIN(TstPluginLoad)
#include "tst_pluginload.moc"
