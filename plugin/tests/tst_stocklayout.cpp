#include <QtTest>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <memory>
#include <QLoggingCategory>

static QStringList swipeMessages;
static QtMessageHandler previousHandler = nullptr;
static void captureSwipeLog(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (QByteArray(context.category) == "swipekb")
        swipeMessages.append(message);
    if (previousHandler)
        previousHandler(type, context, message);
}

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
                function clearEditor() { editor.text = ""; editor.deselect(); editor.forceActiveFocus() }
                function selectPrefix() { editor.select(0, editor.text.length) }
                function sensitive() { editor.inputMethodHints = Qt.ImhSensitiveData }
                function hidden() { editor.inputMethodHints = Qt.ImhHiddenText }
                function ordinary() { editor.inputMethodHints = Qt.ImhNone }
                function focusOther() { other.forceActiveFocus() }
                function focusPassword() { password.forceActiveFocus() }
                property int passwordLength: password.text.length
                function focusEditor() { editor.forceActiveFocus() }
                function cycleMethod() {
                    var method = InputContext.inputEngine.inputMethod
                    InputContext.inputEngine.inputMethod = null
                    InputContext.inputEngine.inputMethod = method
                }
                property int candidateCount: InputContext.inputEngine.wordCandidateListModel.count
                Component.onCompleted: {
                    VirtualKeyboardSettings.locale = "en_US"
                    editor.forceActiveFocus()
                }
                TextInput { id: editor; width: parent.width; height: 40; focus: true }
                TextInput { id: other; y: 45; width: parent.width; height: 40 }
                TextInput { id: password; y: 90; width: parent.width; height: 40; echoMode: TextInput.Password }
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
        auto finish = [&](int generation, const QVariantList &path) {
            return QMetaObject::invokeMethod(method, "finishSwipe", Q_ARG(QVariantList, path),
                                             Q_ARG(QVariantList, keys), Q_ARG(int, generation));
        };
        auto begin = [&] {
            int generation = -1;
            QMetaObject::invokeMethod(method, "beginSwipe", Q_RETURN_ARG(int, generation));
            return generation;
        };
        auto swipe = [&] { return finish(begin(), trace); };
        QVERIFY(swipe());
        QTRY_VERIFY(!root->property("committedText").toString().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(root.get(), "backspace"));
        QTRY_COMPARE(root->property("committedText").toString(), QString());
        QVERIFY(QMetaObject::invokeMethod(root.get(), "tapLetter"));
        QTRY_COMPARE(root->property("committedText").toString(), QStringLiteral("x"));
        QVERIFY(swipe());
        QTRY_VERIFY(root->property("committedText").toString().size() > 2);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "chooseAlternative"));
        // WHY allow delayed context echoes: immediate in-process success does not prove the candidate
        // session survives an app's asynchronous replacement update before the next backspace.
        QTest::qWait(150);
        QVERIFY(root->property("candidateCount").toInt() > 0);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "backspace"));
        QTRY_COMPARE(root->property("committedText").toString(), QStringLiteral("x"));
        QVERIFY(QMetaObject::invokeMethod(root.get(), "backspace"));
        QTRY_COMPARE(root->property("committedText").toString(), QString());
        QVERIFY(QMetaObject::invokeMethod(root.get(), "tapLetter"));
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

        // WHY real selection ending at the word: its text-before-cursor still matches, but replacement
        // would delete the selected prefix too. A suffix-only check cannot authorize this edit.
        QVERIFY(QMetaObject::invokeMethod(root.get(), "clearEditor"));
        QVERIFY(swipe());
        QTRY_VERIFY(root->property("candidateCount").toInt() > 0);
        const QString selected = root->property("committedText").toString();
        QVERIFY(QMetaObject::invokeMethod(root.get(), "selectPrefix"));
        QCoreApplication::processEvents();
        QTRY_COMPARE(root->property("candidateCount").toInt(), 0); // update, not just pick, revokes the session
        QVERIFY(QMetaObject::invokeMethod(root.get(), "chooseAlternative"));
        QCOMPARE(root->property("committedText").toString(), selected);
        QCOMPARE(root->property("candidateCount").toInt(), 0);

        // Direct invocation deliberately bypasses the QML press guard: C++ is the privacy boundary.
        for (const char *privacy : {"sensitive", "hidden"}) {
            QVERIFY(QMetaObject::invokeMethod(root.get(), "clearEditor"));
            QVERIFY(QMetaObject::invokeMethod(root.get(), privacy));
            QCoreApplication::processEvents();
            QVERIFY(swipe());
            QCoreApplication::processEvents();
            QCOMPARE(root->property("committedText").toString(), QString());
            QCOMPARE(root->property("candidateCount").toInt(), 0);
            QVERIFY(QMetaObject::invokeMethod(root.get(), "ordinary"));
        }

        // A return to the same field/method/hints does not resurrect a path begun before the change.
        for (const char *change : {"reset", "sensitive", "hidden", "focusOther", "focusPassword", "cycleMethod"}) {
            QVERIFY(QMetaObject::invokeMethod(root.get(), "clearEditor"));
            QCoreApplication::processEvents();
            const int generation = begin();
            QVERIFY(generation >= 0);
            // Model the already-held swipe in the real capture layer, then deliver real context signals.
            capture->setProperty("originMethod", QVariant::fromValue(method));
            capture->setProperty("originGeneration", generation);
            capture->setProperty("trackedId", 7);
            capture->setProperty("points", trace);
            capture->setProperty("swiping", true);
            if (QByteArray(change) == "reset")
                QVERIFY(QMetaObject::invokeMethod(method, "reset"));
            else
                QVERIFY(QMetaObject::invokeMethod(root.get(), change));
            QCoreApplication::processEvents();
            QCOMPARE(capture->property("trackedId").toInt(), -1);
            QVERIFY(!capture->property("swiping").toBool());
            // Regression for the VM reproduction: release while the password still owns focus.
            QVERIFY(finish(generation, trace));
            QCOMPARE(root->property("passwordLength").toInt(), 0);
            QVERIFY(QMetaObject::invokeMethod(root.get(), "ordinary"));
            QVERIFY(QMetaObject::invokeMethod(root.get(), "focusEditor"));
            QCoreApplication::processEvents();
            QVERIFY(finish(generation, trace));
            QCOMPARE(root->property("committedText").toString(), QString());
            QCOMPARE(root->property("candidateCount").toInt(), 0);
            // Cancellation must leave the next normal tap usable, never an active virtual key.
            QVERIFY(QMetaObject::invokeMethod(root.get(), "tapLetter"));
            QTRY_COMPARE(root->property("committedText").toString(), QStringLiteral("x"));
        }
        for (const bool overCount : {true, false}) {
            QVERIFY(QMetaObject::invokeMethod(root.get(), "clearEditor"));
            QVariantList oversized = trace;
            if (overCount)
                while (oversized.size() <= 2048) oversized.append(trace.last());
            else {
                auto last = oversized.last().toMap();
                last["t"] = 10001;
                oversized.last() = last;
            }
            QVERIFY(finish(begin(), oversized));
            QCOMPARE(root->property("committedText").toString(), QString());
        }
        QVERIFY(swipe());
        QTRY_VERIFY(!root->property("committedText").toString().isEmpty());
        qInfo("Safety: selection, hidden/sensitive finish, stale reset/focus/privacy/method tokens and trace limits passed");
        // WHY enable logging explicitly: default-disabled info logs still must not expose typing.
        QLoggingCategory::setFilterRules(QStringLiteral("swipekb.info=true"));
        swipeMessages.clear();
        previousHandler = qInstallMessageHandler(captureSwipeLog);
        const bool loggedSwipe = swipe();
        qInstallMessageHandler(previousHandler);
        QLoggingCategory::setFilterRules(QStringLiteral("swipekb.info=false"));
        QVERIFY(loggedSwipe);
        QCOMPARE(swipeMessages.size(), 1);
        QVERIFY2(QRegularExpression(QStringLiteral("^swipe \\d+ points -> \\d+ candidates in \\d+ ms$"))
                     .match(swipeMessages.first()).hasMatch(), qPrintable(swipeMessages.first()));
    }
};

QTEST_MAIN(TstStockLayout)
#include "tst_stocklayout.moc"
