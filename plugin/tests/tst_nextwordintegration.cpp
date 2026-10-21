#include <QtTest>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>
#include <QInputMethodEvent>
#include <QTemporaryDir>
#include <QSemaphore>
#include <QJsonDocument>
#include <future>
#include <QtVirtualKeyboard/QVirtualKeyboardInputContext>
#include "swipeinputmethod.h"

class TestPolicyBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap swipePredictionPolicy READ policy NOTIFY swipePredictionPolicyChanged)
public:
    bool permitted = true;
    quint64 revision = 1;
    QVariantMap policy() const {
        return {{"version",1},{"contextId",quint64(8)},{"revision",revision},
            {"contentTypeReceived",true},{"purpose",0},{"completionAllowed",permitted},{"syntheticNoPredictiveText",true}};
    }
    void deny() { permitted = false; ++revision; emit swipePredictionPolicyChanged(); }
signals:
    void swipePredictionPolicyChanged();
};

// WHY delay only the coherent update callback: models/apps can publish property
// notifications before the next input-method update, the race under review.
class DelayedUpdateMethod : public SwipeInputMethod {
public:
    using SwipeInputMethod::SwipeInputMethod;
    bool deferUpdates = false;
    void update() override { if (!deferUpdates) SwipeInputMethod::update(); }
};

// Native source fixture has the stock public proxy identity and NO policy bridge.
// Its validation buffer contains a sentinel the worker must never receive.
class InputListenerItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString text MEMBER text)
    Q_PROPERTY(int cursorPosition MEMBER cursor)
public:
    QString text=QStringLiteral("preexisting sentinel ");
    int cursor=text.size();
    Qt::InputMethodHints hints=Qt::ImhNoPredictiveText;
    mutable int surroundingQueries=0;
    InputListenerItem(QQuickItem *parent):QQuickItem(parent) { setFlag(ItemAcceptsInputMethod); setWidth(400);setHeight(100); }
    QVariant inputMethodQuery(Qt::InputMethodQuery q) const override {
        switch(q) {
        case Qt::ImEnabled:return true;
        case Qt::ImHints:return int(hints);
        case Qt::ImSurroundingText:++surroundingQueries;return text;
        case Qt::ImCursorPosition:case Qt::ImAnchorPosition:case Qt::ImAbsolutePosition:return cursor;
        case Qt::ImCurrentSelection:return QString();
        case Qt::ImCursorRectangle:return QRectF(0,0,2,20);
        default:return QQuickItem::inputMethodQuery(q);
        }
    }
    void inputMethodEvent(QInputMethodEvent *event) override {
        text.replace(cursor+event->replacementStart(),event->replacementLength(),event->commitString());
        cursor+=event->replacementStart()+event->commitString().size();event->accept();
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
    }
};

class NextWordIntegrationTest : public QObject {
    Q_OBJECT
private slots:
    void typedCompletionReplacesOnlyOwnedPrefix_data() {
        QTest::addColumn<bool>("native");QTest::addColumn<bool>("literal");
        for(bool native:{false,true})for(bool literal:{false,true})QTest::newRow(qPrintable(QString("%1-%2").arg(native?"native":"bridge",literal?"literal":"completion")))<<native<<literal;
    }
    void typedCompletionReplacesOnlyOwnedPrefix() {
        QFETCH(bool,native);QFETCH(bool,literal);
        QTemporaryDir data;qputenv("SWIPEKB_DATA",data.path().toUtf8());qputenv("SWIPEKB_MODEL_DATA",qgetenv("SWIPEKB_TEST_PROFILE_ASSETS"));
        if(native)qputenv("SWIPEKB_NATIVE_PREDICTION","owned-history-v1");
        struct EnvReset{~EnvReset(){qunsetenv("SWIPEKB_NATIVE_PREDICTION");}} envReset;
        QQmlEngine engine;QQmlComponent fixture(&engine);fixture.setData("import QtQuick; import QtQuick.VirtualKeyboard; Item {property var context:InputContext}",QUrl());
        std::unique_ptr<QObject> root(fixture.create());QVERIFY(root);auto *item=qobject_cast<QQuickItem*>(root.get());
        QQuickWindow window;window.resize(500,200);item->setParentItem(window.contentItem());InputListenerItem edit(item);
        window.show();window.requestActivate();edit.forceActiveFocus();
        auto *ic=qvariant_cast<QVirtualKeyboardInputContext*>(root->property("context"));QVERIFY(ic);
        QStringList captured;QSemaphore done;
        SwipeInputMethod method(nullptr,[&](const PredictionJob&job){captured=job.context;done.release();return QStringList{"world","again","today"};});
        ic->inputEngine()->setInputMethod(&method);method.setInputMode("en_US",QVirtualKeyboardInputEngine::InputMode::Latin);method.setPhysicalLayout("qwerty");
        TestPolicyBridge bridge;if(!native)method.setPredictionPolicySource(&bridge);
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);QTRY_VERIFY(ic->inputItem()==&edit);
        for(auto c:QString("hel")){method.keyEvent(Qt::Key(c.toUpper().unicode()),QString(c),Qt::NoModifier);ic->commit(QString(c));QGuiApplication::inputMethod()->update(Qt::ImQueryAll);method.update();}
        QCOMPARE(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList),3);
        QStringList words;for(int i=0;i<3;++i)words<<method.selectionListData(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,i,QVirtualKeyboardSelectionListModel::Role::Display).toString();
        QVERIFY(words.contains("hel"));QVERIFY(!method.predictionSelectionToken().isEmpty());
        int index=literal?words.indexOf("hel"):1;const auto selected=words[index];
        method.beginPredictionSelection(method.predictionSelectionToken());method.selectionListItemSelected(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,index);
        QCOMPARE(edit.text,QString("preexisting sentinel ")+selected);
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);method.update();
        QCOMPARE(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList),0);
        method.keyEvent(Qt::Key_Space," ",Qt::NoModifier);ic->commit(" ");QGuiApplication::inputMethod()->update(Qt::ImQueryAll);method.update();
        QVERIFY(done.tryAcquire(1,15000));
        if(native)QCOMPARE(captured,QStringList{selected.toLower()});
        QTRY_VERIFY(!method.predictionSelectionToken().isEmpty());ic->inputEngine()->setInputMethod(nullptr);
    }
    void unbridgedNativeWordUsesOnlyOwnedHistoryAndInserts_data() {
        QTest::addColumn<bool>("swipe");QTest::addColumn<int>("slot");
        for(bool swipe:{false,true}) for(int slot=0;slot<3;++slot)
            QTest::newRow(qPrintable(QStringLiteral("%1-slot%2").arg(swipe?"swipe":"tap").arg(slot)))<<swipe<<slot;
    }
    void unbridgedNativeWordUsesOnlyOwnedHistoryAndInserts() {
        QFETCH(bool,swipe);QFETCH(int,slot);
        const QStringList displayed{"again","world","today"};const auto inserted=displayed[slot];
        QTemporaryDir data;qputenv("SWIPEKB_DATA",data.path().toUtf8());
        qputenv("SWIPEKB_MODEL_DATA",qgetenv("SWIPEKB_TEST_PROFILE_ASSETS"));
        qputenv("SWIPEKB_NATIVE_PREDICTION","owned-history-v1");
        struct EnvReset { ~EnvReset(){qunsetenv("SWIPEKB_NATIVE_PREDICTION");} } envReset;
        QQmlEngine engine;QQmlComponent fixture(&engine);
        fixture.setData("import QtQuick; import QtQuick.VirtualKeyboard; Item {property var context: InputContext}",QUrl());
        std::unique_ptr<QObject> root(fixture.create());QVERIFY2(root,qPrintable(fixture.errorString()));
        auto *item=qobject_cast<QQuickItem*>(root.get());QQuickWindow window;window.resize(500,200);item->setParentItem(window.contentItem());
        InputListenerItem edit(item);window.show();window.requestActivate();edit.forceActiveFocus();
        auto *ic=qvariant_cast<QVirtualKeyboardInputContext*>(root->property("context"));QVERIFY(ic);
        QStringList captured;QSemaphore completed;
        SwipeInputMethod method(nullptr,[&](const PredictionJob &job){captured=job.context;completed.release();return QStringList{"world","again","today"};});
        ic->inputEngine()->setInputMethod(&method);method.setInputMode("en_US",QVirtualKeyboardInputEngine::InputMode::Latin);method.setPhysicalLayout("qwerty");
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
        QTRY_VERIFY_WITH_TIMEOUT(ic->inputItem()==&edit,15000);
        // ensureContextConnections picks the ACTUAL unbridged input item, no test bridge injection.
        if (swipe) {
            QVariantList keys,points; const QStringList rows{"qwertyuiop","asdfghjkl","zxcvbnm"};
            for(int r=0;r<rows.size();++r) for(int c=0;c<rows[r].size();++c)
                keys<<QVariantMap{{"letter",QString(rows[r][c])},{"x",c*100},{"y",r*100},{"w",100},{"h",100}};
            QPointF previous;
            for(auto letter:QStringLiteral("hello")) {
                QPointF center;
                for(const auto &v:keys) {auto k=v.toMap();if(k["letter"].toString()==QString(letter)) center={k["x"].toDouble()+50,k["y"].toDouble()+50};}
                if(points.isEmpty()) points<<QVariantMap{{"x",center.x()},{"y",center.y()},{"t",0}};
                else for(int step=1;step<=8;++step) {auto p=previous+(center-previous)*(step/8.0);points<<QVariantMap{{"x",p.x()},{"y",p.y()},{"t",points.size()*16}};}
                previous=center;
            }
            const int generation=method.beginSwipe();QVERIFY(generation>=0);method.finishSwipe(points,keys,generation);
            QGuiApplication::inputMethod()->update(Qt::ImQueryAll);method.update();
        } else for(auto c:QStringLiteral("hello")) {
            QVERIFY(!method.keyEvent(Qt::Key(c.toUpper().unicode()),QString(c),Qt::NoModifier));
            ic->commit(QString(c));QGuiApplication::inputMethod()->update(Qt::ImQueryAll);method.update();
        }
        const auto owned=edit.text.mid(QStringLiteral("preexisting sentinel ").size()).trimmed();QVERIFY(!owned.isEmpty());
        QVERIFY(!method.keyEvent(Qt::Key_Space," ",Qt::NoModifier));ic->commit(" ");
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);method.update();
        QVERIFY(completed.tryAcquire(1,15000));QCOMPARE(captured,QStringList{owned.toLower()});
        QTRY_VERIFY_WITH_TIMEOUT(!method.predictionSelectionToken().isEmpty(),15000);
        QCOMPARE(method.diagnostics()["predictionContextSource"].toString(),QStringLiteral("keyboard-owned-history"));
        QCOMPARE(method.diagnostics()["activeFieldPolicy"].toString(),QStringLiteral("unknown"));
        QCOMPARE(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList),3);
        for(int i=0;i<3;++i) QCOMPARE(method.selectionListData(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,i,
            QVirtualKeyboardSelectionListModel::Role::Display).toString(),displayed[i]);
        method.beginPredictionSelection(method.predictionSelectionToken());
        method.selectionListItemSelected(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,slot);
        QTRY_COMPARE_WITH_TIMEOUT(edit.text,QStringLiteral("preexisting sentinel ")+owned+" "+inserted,15000);
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);method.update();
        // Selected predictions automatically confirm their own insertion for chaining.
        QVERIFY(completed.tryAcquire(1,15000));QCOMPARE(captured,(QStringList{owned.toLower(),inserted}));
        QTRY_VERIFY_WITH_TIMEOUT(!method.predictionSelectionToken().isEmpty(),15000);
        QVERIFY(!method.keyEvent(Qt::Key_Space," ",Qt::NoModifier));ic->commit(" ");
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);method.update();
        QVERIFY(completed.tryAcquire(1,15000));QCOMPARE(captured,(QStringList{owned.toLower(),inserted}));
        QTRY_VERIFY_WITH_TIMEOUT(!method.predictionSelectionToken().isEmpty(),15000);
        method.reset();QVERIFY(method.predictionSelectionToken().isEmpty());
        const int queries=edit.surroundingQueries;
        method.diagnostics();QCOMPARE(edit.surroundingQueries,queries); // metadata-only, even without authorization
        // Live-only veto: deliberately leave Qt's cached hints ordinary.
        for(auto hint:{Qt::ImhSensitiveData,Qt::ImhHiddenText,Qt::ImhUrlCharactersOnly,Qt::ImhEmailCharactersOnly,
                Qt::ImhPreferNumbers,Qt::ImhDigitsOnly,Qt::ImhFormattedNumbersOnly,Qt::ImhDialableCharactersOnly,Qt::ImhDate,Qt::ImhTime}) {
            edit.hints=Qt::ImhNoPredictiveText|hint;
            QVERIFY(!method.keyEvent(Qt::Key_Space," ",Qt::NoModifier));method.update();
            QVERIFY(method.predictionSelectionToken().isEmpty());QVERIFY(!completed.tryAcquire());
        }
        edit.hints=Qt::ImhNoPredictiveText;
        // A present bridge denial or malformed/unknown version may NEVER become native history.
        for(auto value:QVariantList{QVariantMap{{"version",2}},QString("bad"),
                QVariantMap{{"version",1},{"contextId",quint64(4)},{"revision",quint64(1)},
                {"contentTypeReceived",true},{"purpose",0},{"completionAllowed",false},{"syntheticNoPredictiveText",true}}}) {
            edit.setProperty("swipePredictionPolicy",value);
            QVERIFY(!method.keyEvent(Qt::Key_Space," ",Qt::NoModifier));method.update();
            QVERIFY(method.predictionSelectionToken().isEmpty());QVERIFY(!completed.tryAcquire());
        }
        ic->inputEngine()->setInputMethod(nullptr);
    }
    void pendingInferenceIsRevokedByAwayAndBackCursor_data() {
        QTest::addColumn<bool>("native");QTest::addColumn<QString>("mutation");
        QTest::newRow("raw-bridge-cursor-away-back")<<false<<QString("cursor");
        for(auto change:{"cursor","sensitive","source","reset"}) QTest::newRow(change)<<true<<QString(change);
    }
    void pendingInferenceIsRevokedByAwayAndBackCursor() {
        QFETCH(bool,native);QFETCH(QString,mutation);
        if(native) qputenv("SWIPEKB_NATIVE_PREDICTION","owned-history-v1");
        struct EnvReset { ~EnvReset(){qunsetenv("SWIPEKB_NATIVE_PREDICTION");} } envReset;
        QTemporaryDir data;
        qputenv("SWIPEKB_DATA",data.path().toUtf8());
        qputenv("SWIPEKB_MODEL_DATA",qgetenv("SWIPEKB_TEST_PROFILE_ASSETS"));
        QQmlEngine engine; QQmlComponent fixture(&engine);
        fixture.setData(R"(
            import QtQuick
            import QtQuick.VirtualKeyboard
            Item { width:500; height:200; property var context: InputContext
                TextInput { objectName:"editor"; width:400; height:100; focus:true; text:"hello" }
            }
        )",QUrl());
        std::unique_ptr<QObject> root(fixture.create()); QVERIFY2(root,qPrintable(fixture.errorString()));
        auto *item=qobject_cast<QQuickItem *>(root.get());
        QQuickWindow window; window.resize(500,200); item->setParentItem(window.contentItem());
        auto *edit=root->findChild<QQuickItem *>("editor"); QVERIFY(edit);
        std::unique_ptr<InputListenerItem> nativeEditor;
        if(native) {nativeEditor=std::make_unique<InputListenerItem>(item);nativeEditor->text.clear();nativeEditor->cursor=0;edit=nativeEditor.get();}
        window.show(); window.requestActivate(); edit->forceActiveFocus();
        auto *ic=qvariant_cast<QVirtualKeyboardInputContext *>(root->property("context")); QVERIFY(ic);
        QSemaphore started,finish;
        std::promise<void> completed; auto done=completed.get_future();
        DelayedUpdateMethod method(nullptr,[&](const PredictionJob &) {
            started.release(); finish.acquire(); completed.set_value(); return QStringList{"world"};
        });
        // WHY bounded release guard: a failed assertion must not leave worker teardown blocked.
        struct Release { QSemaphore &gate; ~Release() { gate.release(); } } release{finish};
        ic->inputEngine()->setInputMethod(&method);
        method.setInputMode("en_US",QVirtualKeyboardInputEngine::InputMode::Latin);
        method.setPhysicalLayout("qwerty"); // real capture publishes measured layout before prediction
        TestPolicyBridge bridge;if(!native) method.setPredictionPolicySource(&bridge);
        edit->setProperty("cursorPosition",native?0:5); QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
        QTRY_VERIFY_WITH_TIMEOUT(ic->inputItem()==edit,15000);
        if(native) for(auto c:QStringLiteral("hello")) {
            method.keyEvent(Qt::Key(c.toUpper().unicode()),QString(c),Qt::NoModifier);ic->commit(QString(c));
            QGuiApplication::inputMethod()->update(Qt::ImQueryAll);method.update();
        }
        QVERIFY(!method.keyEvent(Qt::Key_Space," ",Qt::NoModifier));
        if(native) ic->commit(" ");else ic->sendKeyClick(Qt::Key_Space," ");
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); method.update();
        QVERIFY(started.tryAcquire(1,15000));
        QCOMPARE(method.diagnostics()["predictionModelState"].toString(),QStringLiteral("loading"));
        method.deferUpdates = true;
        const int cursor=ic->cursorPosition();
        if(mutation=="cursor") {
            edit->setProperty("cursorPosition",cursor-1); QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
            QCOMPARE(ic->cursorPosition(),cursor-1);
            edit->setProperty("cursorPosition",cursor); QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
            QCOMPARE(ic->cursorPosition(),cursor);
        } else if(mutation=="sensitive") {
            nativeEditor->hints|=Qt::ImhSensitiveData;QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
            nativeEditor->hints=Qt::ImhNoPredictiveText;QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
        } else if(mutation=="source") {
            QObject unknown;method.setPredictionPolicySource(&unknown);method.setPredictionPolicySource(edit);
        } else method.reset();
        QSignalSpy settled(&method,&SwipeInputMethod::predictionWorkSettled);
        finish.release(); QVERIFY(done.wait_for(std::chrono::seconds(15))==std::future_status::ready);
        // WHY explicit drain evidence: a sleep/event pump could pass before a stale
        // worker result was emitted, hiding the original away-and-back race.
        QTRY_COMPARE_WITH_TIMEOUT(settled.size(),1,15000);
        QCOMPARE(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList),0);
        ic->inputEngine()->setInputMethod(nullptr);
    }
    void spaceThenPredictionInsertsAndPolicyChangeClearsBar() {
        QTemporaryDir data;
        qputenv("SWIPEKB_DATA", data.path().toUtf8());
        qputenv("SWIPEKB_MODEL_DATA", qgetenv("SWIPEKB_TEST_PROFILE_ASSETS"));
        QQmlEngine engine;
        QQmlComponent fixture(&engine);
        fixture.setData(R"(
            import QtQuick
            import QtQuick.VirtualKeyboard
            Item {
                width: 500; height: 200
                property var context: InputContext
                TextInput { objectName: "editor"; width: 400; height: 100; focus: true
                    text: "hello"; inputMethodHints: Qt.ImhNoPredictiveText }
            }
        )", QUrl());
        std::unique_ptr<QObject> root(fixture.create());
        QVERIFY2(root, qPrintable(fixture.errorString()));
        auto *item = qobject_cast<QQuickItem *>(root.get());
        QQuickWindow window; window.resize(500,200); item->setParentItem(window.contentItem());
        auto *edit = root->findChild<QQuickItem *>("editor"); QVERIFY(edit);
        window.show(); window.requestActivate(); edit->forceActiveFocus();
        auto *ic = qvariant_cast<QVirtualKeyboardInputContext *>(root->property("context")); QVERIFY(ic);
        SwipeInputMethod method;
        ic->inputEngine()->setInputMethod(&method);
        QVERIFY(method.setInputMode("en_US", QVirtualKeyboardInputEngine::InputMode::Latin));
        method.setPhysicalLayout("qwerty");
        TestPolicyBridge bridge; method.setPredictionPolicySource(&bridge);
        edit->setProperty("cursorPosition",5);
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
        QTRY_VERIFY_WITH_TIMEOUT(ic->inputItem() == edit, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(ic->surroundingText(), QStringLiteral("hello"), 15000);
        QVERIFY(!method.keyEvent(Qt::Key_Space," ",Qt::NoModifier));
        ic->sendKeyClick(Qt::Key_Space," ");
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); method.update();
        QTRY_VERIFY_WITH_TIMEOUT(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList) > 0, 15000);
        QCOMPARE(method.diagnostics()["scope"].toString(),QStringLiteral("runtime"));
        QCOMPARE(method.diagnostics()["locale"].toString(),QStringLiteral("en-US"));
        QCOMPARE(method.diagnostics()["layout"].toString(),QStringLiteral("qwerty"));
        QVERIFY(method.diagnostics()["swipeCapable"].toBool());
        QCOMPARE(method.diagnostics()["predictionModelState"].toString(),QStringLiteral("loaded"));
        QCOMPARE(method.diagnostics()["activeFieldPolicy"].toString(),QStringLiteral("allowed"));
        const auto diagnosticJson=QJsonDocument::fromVariant(method.diagnostics()).toJson();
        QVERIFY(!diagnosticJson.contains("hello")); QVERIFY(!diagnosticJson.contains("/home/"));
        QVERIFY(!diagnosticJson.contains("contextId"));
        const QString prediction = method.selectionListData(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,0,
            QVirtualKeyboardSelectionListModel::Role::Display).toString();
        QVERIFY(!prediction.isEmpty());
        const auto canceledPress = method.beginPredictionSelection(method.predictionSelectionToken());
        method.endPredictionSelection(canceledPress);
        method.selectionListItemSelected(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,0);
        QCOMPARE(edit->property("text").toString(), QStringLiteral("hello "));
        // A fresh confirmation repopulates predictions; an abandoned press cannot
        // be reused by the index-only stock callback.
        QVERIFY(!method.keyEvent(Qt::Key_Space," ",Qt::NoModifier));
        ic->sendKeyClick(Qt::Key_Space," ");
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); method.update();
        QTRY_VERIFY_WITH_TIMEOUT(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList) > 0,15000);
        const auto nextPrediction = method.selectionListData(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,0,
            QVirtualKeyboardSelectionListModel::Role::Display).toString();
        method.beginPredictionSelection(method.predictionSelectionToken());
        method.selectionListItemSelected(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,0);
        QTRY_COMPARE_WITH_TIMEOUT(edit->property("text").toString(), QStringLiteral("hello  ") + nextPrediction, 15000);
        QSignalSpy diagnosticsChanged(&method,&SwipeInputMethod::diagnosticsChanged);
        bridge.deny();
        QVERIFY(diagnosticsChanged.size()>0);
        QCOMPARE(method.diagnostics()["predictionStatus"].toString(),QStringLiteral("disabled-for-field"));
        QCOMPARE(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList),0);
        const auto epoch = method.profileSnapshot().epoch;
        QVERIFY(method.setInputMode("fr_FR", QVirtualKeyboardInputEngine::InputMode::Latin));
        QVERIFY(method.profileSnapshot().epoch > epoch);
        QVERIFY(!method.profileSnapshot().capabilities.nextWord);
        // WHY combined release: French swiping is owned by its profile; only English
        // prediction is unavailable here, not the language's independent swipe capability.
        QCOMPARE(method.profileSnapshot().effectiveLanguage, QStringLiteral("fr"));
        QCOMPARE(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList),0);
        method.setInputMode("en_GB", QVirtualKeyboardInputEngine::InputMode::Latin);
        QCOMPARE(method.profileSnapshot().requestedLocale, QStringLiteral("en-GB"));
        QVERIFY(method.profileSnapshot().capabilities.nextWord);
        const auto beforeLayout = method.profileSnapshot().epoch;
        method.setPhysicalLayout("azerty");
        QVERIFY(method.profileSnapshot().epoch > beforeLayout);
        QVERIFY(!method.profileSnapshot().assets.contains("decoder"));
        ic->inputEngine()->setInputMethod(nullptr);
    }
};
QTEST_MAIN(NextWordIntegrationTest)
#include "tst_nextwordintegration.moc"
