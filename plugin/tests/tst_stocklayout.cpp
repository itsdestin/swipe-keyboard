#include <QtTest>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <memory>
#include <QLoggingCategory>
#include <QInputMethodQueryEvent>
#include <QInputMethod>
#include <functional>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QScopeGuard>

// WHY: the backend proxy can answer sensitive live hints without notifying Qt's cached context.
// An event filter on the real focused item reproduces exactly that divergence with public Qt APIs.
class LiveHintsProxy : public QObject {
public:
    QVariant hints = int(Qt::ImhNone);
    std::function<void()> callback;
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() != QEvent::InputMethodQuery) return false;
        auto *query = static_cast<QInputMethodQueryEvent *>(event);
        if (!(query->queries() & Qt::ImHints)) return false;
        query->setValue(Qt::ImHints, hints);
        if (callback) callback();
        return query->queries() == Qt::ImHints;
    }
};

// WHY: deliver synchronous commit callbacks through the real public input item event path,
// rather than testing only the low-level query helper's receiver deletion.
class CommitCallback : public QObject {
public:
    std::function<void()> callback;
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() != QEvent::InputMethod) return false;
        auto *input = static_cast<QInputMethodEvent *>(event);
        if (input->commitString().isEmpty() || !callback) return false;
        auto once = std::move(callback);
        once();
        return true;
    }
};

class CandidateNotification : public QObject {
    Q_OBJECT
public:
    std::function<void()> callback;
public slots:
    void fire() { if (callback) { auto once=std::move(callback); once(); } }
};

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
    void productionCallbacks_data()
    {
        QTest::addColumn<QString>("boundary");
        QTest::addColumn<QString>("action");
        for (const auto &boundary : {QStringLiteral("capture-query"), QStringLiteral("release-query"),
                                    QStringLiteral("swipe-commit"), QStringLiteral("candidate-commit"),
                                    QStringLiteral("backspace-notify")})
            for (const auto &action : {QStringLiteral("delete-method"), QStringLiteral("replace-item"),
                                      QStringLiteral("privacy-reset"), QStringLiteral("live-privacy")}) {
                if (boundary.endsWith("query") && action=="live-privacy") continue;
                QTest::newRow(qPrintable(boundary + "-" + action)) << boundary << action;
            }
    }
    void productionCallbacks()
    {
        QFETCH(QString, boundary);
        QFETCH(QString, action);
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import QtQuick.VirtualKeyboard
            import QtQuick.VirtualKeyboard.Settings
            Item {
                width: 1000; height: 500
                property QtObject method: InputContext.inputEngine.inputMethod
                property QtObject inputItem: InputContext.inputItem
                property alias text: editor.text
                property alias otherText: other.text
                function backspace() { InputContext.inputEngine.virtualKeyClick(Qt.Key_Backspace, "", Qt.NoModifier) }
                property int count: InputContext.inputEngine.wordCandidateListModel.count
                function detachMethod() { InputContext.inputEngine.inputMethod = null }
                function replaceItem() { other.forceActiveFocus() }
                function protect() { editor.inputMethodHints = Qt.ImhSensitiveData }
                function restore() { editor.inputMethodHints = Qt.ImhNone; editor.forceActiveFocus() }
                function alternative() { InputContext.inputEngine.wordCandidateListModel.selectItem(0) }
                function tap() { InputContext.inputEngine.virtualKeyClick(Qt.Key_X, "x", Qt.NoModifier) }
                Component.onCompleted: { VirtualKeyboardSettings.locale = "en_US"; editor.forceActiveFocus() }
                TextInput { id: editor; width: parent.width; height: 40; focus: true }
                TextInput { id: other; y:45; width:parent.width; height:40; text:"sentinel" }
                InputPanel { width:parent.width; anchors.bottom:parent.bottom; active:true }
            }
        )", QUrl());
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *item = qobject_cast<QQuickItem *>(root.get());
        QQuickWindow window; window.resize(1000,500);
        item->setParentItem(window.contentItem()); window.show();
        QTRY_VERIFY_WITH_TIMEOUT(swipeLayout(item), 10000);
        QPointer<QObject> method(root->property("method").value<QObject *>());
        QVERIFY(method);
        auto *capture = swipeLayout(item)->property("swipeCapture").value<QObject *>();
        QVariant geometry;
        QVERIFY(QMetaObject::invokeMethod(capture,"letterKeys",Q_RETURN_ARG(QVariant,geometry)));
        const auto keys = geometry.toList();
        QVariantList trace;
        QPointF prior;
        for (const QChar letter : QStringLiteral("the")) {
            QVariantMap key;
            for (const auto &v : keys) if (v.toMap()["letter"].toString()==QString(letter)) key=v.toMap();
            QVERIFY(!key.isEmpty());
            QPointF center(key["x"].toDouble()+key["w"].toDouble()/2,
                           key["y"].toDouble()+key["h"].toDouble()/2);
            if(trace.isEmpty()) trace.append(QVariantMap{{"x",center.x()},{"y",center.y()},{"t",0}});
            else for(int step=1;step<=8;++step) {
                auto p=prior+(center-prior)*(step/8.0);
                trace.append(QVariantMap{{"x",p.x()},{"y",p.y()},{"t",trace.size()*16}});
            }
            prior=center;
        }
        auto begin=[&] { int token=-1; QMetaObject::invokeMethod(method,"beginSwipe",Q_RETURN_ARG(int,token)); return token; };
        auto finish=[&](int token) { return QMetaObject::invokeMethod(method,"finishSwipe",Q_ARG(QVariantList,trace),Q_ARG(QVariantList,keys),Q_ARG(int,token)); };
        auto *inputItem=root->property("inputItem").value<QObject *>();
        QVERIFY(inputItem);
        int callbacks=0;
        LiveHintsProxy query; CommitCallback commit;
        auto callback=[&] {
            if (callbacks) return; // nested Qt focus/hint refresh must not repeat the destructive action
            ++callbacks;
            if(action=="delete-method") {
                auto *victim=method.data();
                QVERIFY(QMetaObject::invokeMethod(root.get(),"detachMethod"));
                delete victim; // real production method, synchronously inside its query/commit
            } else if(action=="replace-item") {
                QVERIFY(QMetaObject::invokeMethod(root.get(),"replaceItem"));
            } else if(action=="live-privacy") {
                query.hints=int(Qt::ImhSensitiveData); // no notification/reset; tests post-commit live gate
            } else {
                QVERIFY(QMetaObject::invokeMethod(root.get(),"protect"));
                if(method) QVERIFY(QMetaObject::invokeMethod(method,"reset"));
            }
        };
        const QByteArray priorData=qgetenv("SWIPEKB_DATA");
        QTemporaryDir rowData(QString::fromUtf8(priorData)+"/callback-XXXXXX");
        QVERIFY(rowData.isValid());
        qputenv("SWIPEKB_DATA",rowData.path().toUtf8());
        const auto restoreData=qScopeGuard([&] { qputenv("SWIPEKB_DATA",priorData); });
        const QString data=rowData.path();
        QVERIFY(QDir().mkpath(data));
        QFile record(data+"/record");
        QVERIFY(record.open(QIODevice::WriteOnly)); record.close();
        const QString recordingPath=data+"/traces/swipes.jsonl";
        if(boundary=="candidate-commit" || boundary=="backspace-notify") {
            QVERIFY(finish(begin()));
            QTRY_VERIFY(root->property("count").toInt()>0);
        }
        QFile recording(recordingPath);
        if(boundary=="candidate-commit" || boundary=="backspace-notify") QVERIFY(recording.exists());
        QByteArray initialRecording;
        if (recording.exists()) {
            QVERIFY(recording.open(QIODevice::ReadOnly)); initialRecording=recording.readAll(); recording.close();
            const auto entry=QJsonDocument::fromJson(initialRecording.trimmed()).object();
            QCOMPARE(entry["type"].toString(),QStringLiteral("swipe"));
            QVERIFY(entry["ts"].toDouble()>0); // initial swipe really recorded: nonzero appendPick/outcome lease
        }
        const QString priorText=root->property("text").toString();
        const QString priorOther=root->property("otherText").toString();
        if(boundary=="backspace-notify") {
            inputItem->installEventFilter(&query);
            CandidateNotification notification; notification.callback=callback;
            QMetaMethod signal;
            for(int i=0;i<method->metaObject()->methodCount();++i) {
                auto candidate=method->metaObject()->method(i);
                if(candidate.name()=="selectionListChanged") { signal=candidate;break; }
            }
            QVERIFY(signal.isValid());
            const auto slot=notification.metaObject()->method(notification.metaObject()->indexOfSlot("fire()"));
            QVERIFY(QObject::connect(method,signal,&notification,slot,Qt::DirectConnection));
            QVERIFY(QMetaObject::invokeMethod(root.get(),"backspace"));
            inputItem->removeEventFilter(&query);
            QCOMPARE(root->property("text").toString(),priorText);
            QCOMPARE(root->property("otherText").toString(),priorOther);
        } else if(boundary.endsWith("query")) {
            const int token=boundary=="release-query"?begin():-1;
            query.callback=callback; inputItem->installEventFilter(&query);
            if(boundary=="capture-query") QCOMPARE(begin(),-1);
            else QVERIFY(finish(token));
            inputItem->removeEventFilter(&query);
        } else {
            inputItem->installEventFilter(&query);
            commit.callback=callback; inputItem->installEventFilter(&commit);
            if(boundary=="swipe-commit") QVERIFY(finish(begin()));
            else QVERIFY(QMetaObject::invokeMethod(root.get(),"alternative"));
            inputItem->removeEventFilter(&commit);
            inputItem->removeEventFilter(&query);
        }
        QCOMPARE(callbacks,1);
        QByteArray finalRecording;
        if(recording.exists()) { QVERIFY(recording.open(QIODevice::ReadOnly)); finalRecording=recording.readAll();recording.close(); }
        QCOMPARE(finalRecording,initialRecording); // no appended pick/outcome even with recorded initial swipe
        if(action=="delete-method") QVERIFY(method.isNull());
        else {
            QCOMPARE(root->property("count").toInt(),0);
            QVERIFY(QMetaObject::invokeMethod(root.get(),"restore"));
            QCoreApplication::processEvents();
            // Reset/privacy callbacks must not resurrect afterSwipe and add a stale leading space.
            QVERIFY(QMetaObject::invokeMethod(root.get(),"tap"));
            QTRY_COMPARE(root->property("text").toString(),priorText+QStringLiteral("x"));
        }
        QVERIFY(record.remove());
        if(recording.exists()) QVERIFY(recording.remove()); // this row's private synthetic trace only
    }
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
                property QtObject actualInputItem: InputContext.inputItem
                property int cachedHints: InputContext.inputMethodHints
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
        // WHY: qmldir prefers this resource even with NO_CACHEGEN. A reused rcc object can
        // silently load older QML while the installed loose file and stale buildtree test agree.
        QFile selectedCapture(qEnvironmentVariable("SWIPEKB_CAPTURE_QML"));
        QFile embeddedCapture(QStringLiteral(":/qt/qml/SwipeKeyboard/SwipeCapture.qml"));
        QVERIFY2(selectedCapture.open(QIODevice::ReadOnly), "Driver must select the capture QML file");
        QVERIFY(embeddedCapture.open(QIODevice::ReadOnly));
        QCOMPARE(embeddedCapture.readAll(), selectedCapture.readAll());
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

        // Stale-cache regression: no Qt update/notification after the proxy's live flags change.
        QVERIFY(QMetaObject::invokeMethod(root.get(), "clearEditor"));
        QVERIFY(QMetaObject::invokeMethod(root.get(), "ordinary"));
        QCoreApplication::processEvents();
        auto *proxyItem = root->property("actualInputItem").value<QObject *>();
        QVERIFY(proxyItem);
        LiveHintsProxy proxy;
        proxyItem->installEventFilter(&proxy);
        // This directory is provided by the stock-layout driver's private TemporaryDirectory.
        const QString privateData = qEnvironmentVariable("SWIPEKB_DATA");
        QVERIFY(!privateData.isEmpty());
        QVERIFY(QDir().mkpath(privateData));
        QFile recordingSwitch(privateData + QStringLiteral("/record"));
        QVERIFY(recordingSwitch.open(QIODevice::WriteOnly));
        recordingSwitch.close();
        const QString recordingPath = privateData + QStringLiteral("/traces/swipes.jsonl");
        QVERIFY(!QFileInfo::exists(recordingPath));
        QLoggingCategory::setFilterRules(QStringLiteral("swipekb.info=true"));
        swipeMessages.clear();
        previousHandler = qInstallMessageHandler(captureSwipeLog);
        const int cachedHints = root->property("cachedHints").toInt();
        QVERIFY(!(cachedHints & (Qt::ImhHiddenText | Qt::ImhSensitiveData)));
        for (const int flags : {int(Qt::ImhSensitiveData), int(Qt::ImhHiddenText)}) {
            proxy.hints = flags;
            QCOMPARE(root->property("cachedHints").toInt(), cachedHints);
            QCOMPARE(begin(), -1); // red on cached-only authorization, before any path/recognition
            QVERIFY(finish(1, trace));
            QCOMPARE(root->property("committedText").toString(), QString());
            QCOMPARE(root->property("candidateCount").toInt(), 0);
        }
        // Release must independently reject a previously authorized normal token, including an
        // invalid live answer, without relying on the QML timer or cached signal delivery.
        for (const QVariant live : {QVariant(int(Qt::ImhSensitiveData)), QVariant()}) {
            proxy.hints = int(Qt::ImhNone);
            const int normalToken = begin();
            QVERIFY(normalToken >= 0);
            proxy.hints = live;
            QVERIFY(finish(normalToken, trace));
            QCOMPARE(root->property("committedText").toString(), QString());
            QCOMPARE(root->property("candidateCount").toInt(), 0);
        }
        // Exercise the real stock touch area before press, not just direct C++ finish calls.
        proxy.hints = int(Qt::ImhSensitiveData);
        auto *touchArea = qobject_cast<QQuickItem *>(capture->property("touchArea").value<QObject *>());
        QVERIFY(touchArea);
        const auto firstKey = keys.first().toMap();
        const QPointF center(firstKey.value("x").toDouble() + firstKey.value("w").toDouble()/2,
                             firstKey.value("y").toDouble() + firstKey.value("h").toDouble()/2);
        const QPoint touchPosition = touchArea->mapToScene(center).toPoint();
        auto *touchDevice = QTest::createTouchDevice();
        QTest::touchEvent(&window, touchDevice).press(0, touchPosition, &window).commit();
        QCoreApplication::processEvents();
        QCOMPARE(capture->property("trackedId").toInt(), -1);
        QCOMPARE(capture->property("points").toList().size(), 0);
        QCOMPARE(capture->property("trailPoints").toList().size(), 0);
        QTest::touchEvent(&window, touchDevice).release(0, touchPosition, &window).commit();
        QCoreApplication::processEvents();
        QTRY_COMPARE(root->property("committedText").toString().size(), 1); // actual stock tap retained, never a word
        QVERIFY(QMetaObject::invokeMethod(root.get(), "clearEditor"));
        proxy.hints = QVariant();
        QCOMPARE(begin(), -1);
        proxy.hints = int(Qt::ImhNone);
        const int resetGeneration = begin();
        QVERIFY(resetGeneration >= 0);
        proxy.callback = [&] { QMetaObject::invokeMethod(method, "reset"); };
        QCOMPARE(begin(), -1); // a public query may synchronously revoke authorization
        QVERIFY(finish(resetGeneration, trace));
        QCOMPARE(root->property("committedText").toString(), QString());
        proxy.callback = [&] { QCOMPARE(begin(), -1); };
        QCOMPARE(begin(), -1); // nested authorization also invalidates the outer generation
        proxy.callback = {};
        proxy.hints = int(Qt::ImhNoPredictiveText);
        QVERIFY(begin() >= 0); // backend's unconditional no-prediction flag is NOT a privacy veto
        proxy.hints = int(Qt::ImhNone);
        const int heldGeneration = begin();
        QVERIFY(heldGeneration >= 0);
        capture->setProperty("originMethod", QVariant::fromValue(method));
        capture->setProperty("originGeneration", heldGeneration);
        capture->setProperty("originKeys", keys);
        capture->setProperty("trackedId", 7);
        capture->setProperty("startTime", QDateTime::currentMSecsSinceEpoch());
        capture->setProperty("points", trace);
        capture->setProperty("trailPoints", trace);
        capture->setProperty("swiping", true);
        proxy.hints = int(Qt::ImhSensitiveData); // stationary: no signal and no movement event
        QTRY_COMPARE_WITH_TIMEOUT(capture->property("trackedId").toInt(), -1, 500);
        QCOMPARE(capture->property("points").toList().size(), 0);
        QCOMPARE(capture->property("trailPoints").toList().size(), 0);
        QVERIFY(!capture->property("swiping").toBool());
        QVERIFY(finish(heldGeneration, trace));
        QCOMPARE(root->property("committedText").toString(), QString());
        QCOMPARE(root->property("candidateCount").toInt(), 0);
        qInstallMessageHandler(previousHandler);
        QLoggingCategory::setFilterRules(QStringLiteral("swipekb.info=false"));
        QCOMPARE(swipeMessages.size(), 0); // recognition completion log must never run in refusals
        QVERIFY(!QFileInfo::exists(recordingPath)); // opt-in cannot bypass live privacy
        QVERIFY(recordingSwitch.remove());
        proxy.hints = int(Qt::ImhNone);
        QVERIFY(finish(heldGeneration, trace)); // ordinary-again must not resurrect canceled points
        QCOMPARE(root->property("committedText").toString(), QString());
        QVERIFY(QMetaObject::invokeMethod(root.get(), "tapLetter"));
        QTRY_COMPARE(root->property("committedText").toString(), QStringLiteral("x"));
        // Even a live-sensitive/cache-ordinary stock tap stays single-character (no swipe spacing).
        proxy.hints = int(Qt::ImhSensitiveData);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "tapLetter"));
        QTRY_COMPARE(root->property("committedText").toString(), QStringLiteral("xx"));
        proxyItem->removeEventFilter(&proxy);

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
