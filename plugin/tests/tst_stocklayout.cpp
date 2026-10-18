#include <QtTest>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <memory>

// Loaded layouts belong to a Loader's visual tree, not necessarily its QObject tree.
static QQuickItem *swipeLayout(QQuickItem *item)
{
    if (item->property("swipeCapture").value<QObject *>())
        return item;
    for (auto *child : item->childItems())
        if (auto *found = swipeLayout(child))
            return found;
    return nullptr;
}

class TstStockLayout : public QObject {
    Q_OBJECT
private slots:
    void stockInputPanelHooksSwipe()
    {
        // No compiled-in source/build paths: the driver selects either built or installed QML.
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import QtQuick.VirtualKeyboard
            import QtQuick.VirtualKeyboard.Settings
            Item {
                width: 1000; height: 500
                property QtObject hookedInputMethod: InputContext.inputEngine.inputMethod
                property alias committedText: editor.text
                function backspace() { InputContext.inputEngine.virtualKeyClick(Qt.Key_Backspace, "", Qt.NoModifier) }
                function tapLetter() { InputContext.inputEngine.virtualKeyClick(Qt.Key_X, "x", Qt.NoModifier) }
                function chooseAlternative() { InputContext.inputEngine.wordCandidateListModel.selectItem(0) }
                function moveToStart() { editor.cursorPosition = 0 }
                Component.onCompleted: {
                    VirtualKeyboardSettings.locale = "en_US"
                    editor.forceActiveFocus()
                }
                TextInput { id: editor; width: parent.width; height: 40; focus: true }
                InputPanel { width: parent.width; anchors.bottom: parent.bottom; active: true }
            }
        )", QUrl());
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *item = qobject_cast<QQuickItem *>(root.get());
        QVERIFY(item);
        // Declare after root so the window detaches its visual children before root is deleted.
        QQuickWindow window;
        window.resize(1000, 500);
        item->setParentItem(window.contentItem());
        window.show();

        QTRY_VERIFY_WITH_TIMEOUT(swipeLayout(item), 10000);
        auto *layout = swipeLayout(item);
        auto *capture = layout->property("swipeCapture").value<QObject *>();
        QVERIFY2(capture, "The real stock English layout did not create SwipeCapture");
        QCOMPARE(capture->property("layoutItem").value<QObject *>(), layout);
        QVERIFY(capture->property("touchArea").value<QObject *>());

        // Checking a separately constructed SwipeInputMethod would miss a broken layout hook.
        // This is the actual method selected by Qt's input engine for the loaded stock layout.
        QTRY_VERIFY_WITH_TIMEOUT(root->property("hookedInputMethod").value<QObject *>(), 10000);
        auto *method = root->property("hookedInputMethod").value<QObject *>();
        QVERIFY2(QString::fromLatin1(method->metaObject()->className()).contains("SwipeInputMethod"),
                 method->metaObject()->className());
        bool ready = false;
        QVERIFY(QMetaObject::invokeMethod(method, "isReady", Q_RETURN_ARG(bool, ready)));
        QVERIFY2(ready, "The hooked SwipeInputMethod could not load its models/dictionary");
        qInfo("Real stock InputPanel: layout.swipeCapture exists; active SwipeInputMethod.isReady=true");

        // WHY actual engine/field: a pure deletion plan cannot prove keyEvent consumes Backspace
        // without Qt deleting an extra character, or that a real candidate pick remains undoable.
        QVariant geometry;
        QVERIFY(QMetaObject::invokeMethod(capture, "letterKeys", Q_RETURN_ARG(QVariant, geometry)));
        const QVariantList keys = geometry.toList();
        QVERIFY(keys.size() >= 26);
        QVariantList trace;
        QPointF previous;
        for (const QChar letter : QStringLiteral("the")) {
            QVariantMap key;
            for (const auto &value : keys)
                if (value.toMap().value("letter").toString() == QString(letter)) key = value.toMap();
            QVERIFY(!key.isEmpty());
            const QPointF center(key.value("x").toDouble() + key.value("w").toDouble() / 2,
                                 key.value("y").toDouble() + key.value("h").toDouble() / 2);
            if (trace.isEmpty())
                trace.append(QVariantMap{{"x", center.x()}, {"y", center.y()}, {"t", 0}});
            else
                for (int step = 1; step <= 8; ++step) {
                    const auto p = previous + (center - previous) * (step / 8.0);
                    trace.append(QVariantMap{{"x", p.x()}, {"y", p.y()}, {"t", trace.size() * 16}});
                }
            previous = center;
        }
        auto swipe = [&] {
            return QMetaObject::invokeMethod(method, "finishSwipe", Q_ARG(QVariantList, trace), Q_ARG(QVariantList, keys));
        };
        QVERIFY(swipe());
        QTRY_VERIFY(!root->property("committedText").toString().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(root.get(), "backspace"));
        QTRY_COMPARE(root->property("committedText").toString(), QString());
        QVERIFY(QMetaObject::invokeMethod(root.get(), "tapLetter"));
        QTRY_COMPARE(root->property("committedText").toString(), QStringLiteral("x"));
        QVERIFY(swipe());
        QTRY_VERIFY(root->property("committedText").toString().size() > 2);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "chooseAlternative"));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(root.get(), "backspace"));
        QTRY_COMPARE(root->property("committedText").toString(), QStringLiteral("x"));
        QVERIFY(swipe());
        QTRY_VERIFY(root->property("committedText").toString().size() > 2);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "tapLetter"));
        const QString before = root->property("committedText").toString();
        QVERIFY(QMetaObject::invokeMethod(root.get(), "backspace"));
        QTRY_COMPARE(root->property("committedText").toString(), before.chopped(1));
        QVERIFY(QMetaObject::invokeMethod(root.get(), "moveToStart"));
        QCoreApplication::processEvents();
        const QString moved = root->property("committedText").toString();
        QVERIFY(QMetaObject::invokeMethod(root.get(), "backspace"));
        QTRY_COMPARE(root->property("committedText").toString(), moved);
        qInfo("Real stock InputPanel: one-shot word backspace, candidate undo, normal typing and cursor fallback passed");
    }
};

QTEST_MAIN(TstStockLayout)
#include "tst_stocklayout.moc"
