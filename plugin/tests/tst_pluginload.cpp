#include <QtTest>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>

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
};

QTEST_MAIN(TstPluginLoad)
#include "tst_pluginload.moc"
