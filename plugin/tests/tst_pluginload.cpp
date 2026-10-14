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
    // The guess bar must stay pinned while ANY swipe layer exists, and the user's own value come back once
    // the last one is gone. WHY two layers, old deleted first: when Qt rebuilds the layout the new layer can
    // appear before the old one is destroyed; the old one's cleanup must not unpin the bar under the new one.
    void guessBarPinnedWhileCaptureExists()
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
        std::unique_ptr<QObject> oldLayer(c.createWithInitialProperties(props));
        QVERIFY2(oldLayer, qPrintable(c.errorString()));
        QCOMPARE(probe->property("pinned").toBool(), true);
        std::unique_ptr<QObject> newLayer(c.createWithInitialProperties(props));
        QVERIFY(newLayer);
        oldLayer.reset();
        QCOMPARE(probe->property("pinned").toBool(), true); // still pinned under the new layer
        newLayer.reset();
        QCOMPARE(probe->property("pinned").toBool(), false); // user's value restored
    }
};

QTEST_MAIN(TstPluginLoad)
#include "tst_pluginload.moc"
