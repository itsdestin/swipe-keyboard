#include <QtTest>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>
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

class NextWordIntegrationTest : public QObject {
    Q_OBJECT
private slots:
    void pendingInferenceIsRevokedByAwayAndBackCursor() {
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
        TestPolicyBridge bridge; method.setPredictionPolicySource(&bridge);
        edit->setProperty("cursorPosition",5); QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
        QTRY_VERIFY_WITH_TIMEOUT(ic->inputItem()==edit,15000);
        QVERIFY(!method.keyEvent(Qt::Key_Space," ",Qt::NoModifier));
        ic->sendKeyClick(Qt::Key_Space," "); QGuiApplication::inputMethod()->update(Qt::ImQueryAll); method.update();
        QVERIFY(started.tryAcquire(1,15000));
        QCOMPARE(method.diagnostics()["predictionModelState"].toString(),QStringLiteral("loading"));
        method.deferUpdates = true;
        const int cursor=ic->cursorPosition();
        edit->setProperty("cursorPosition",cursor-1); QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
        QCOMPARE(ic->cursorPosition(),cursor-1);
        edit->setProperty("cursorPosition",cursor); QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
        QCOMPARE(ic->cursorPosition(),cursor);
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
