#include <QtTest>
#include <QQmlComponent>
#include <QQmlEngine>

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
};

QTEST_MAIN(TstPluginLoad)
#include "tst_pluginload.moc"
