// The swipe engine inside the keyboard plugin (swipekb::LanguageEngine, the only swipe engine).
// WHY these cases: each guards one promise made to the owner: the pinned model is used, never "no swipe" when the
// model file is missing or damaged, never a crash on a broken swipe, and sensitive fields stay untouched.
#include <QtTest>
#include <QTemporaryDir>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QQuickItem>
#include <QScopeGuard>
#include <QGuiApplication>
#include <QFileInfo>
#include <QtVirtualKeyboard/QVirtualKeyboardInputContext>
#include "languagefixtures.h"
#include "swipeinputmethod.h"

namespace {
QVariantList qwertyKeys() {
    QVariantList keys; const QStringList rows{"qwertyuiop","asdfghjkl","zxcvbnm"};
    for (int r=0;r<rows.size();++r) for (int i=0;i<rows[r].size();++i)
        keys.append(QVariantMap{{"letter",QString(rows[r][i])},{"x",i*100},{"y",r*100},{"w",100},{"h",100}});
    return keys;
}
QVariantList traceFor(const QString &word,const QVariantList &keys) {
    QVariantList points; QPointF previous;
    for (const auto &letter:word) {
        QVariantMap key;
        for (const auto &entry:keys) if (entry.toMap()["letter"].toString()==QString(letter)) key=entry.toMap();
        const QPointF center(key["x"].toDouble()+50,key["y"].toDouble()+50);
        if (points.isEmpty()) points.append(QVariantMap{{"x",center.x()},{"y",center.y()},{"t",0}});
        else for (int step=1;step<=8;++step) {
            const auto p=previous+(center-previous)*(step/8.0);
            points.append(QVariantMap{{"x",p.x()},{"y",p.y()},{"t",points.size()*16}});
        }
        previous=center;
    }
    return points;
}
// Restores an environment variable when the test ends.
struct EnvGuard {
    QByteArray name, old; bool had;
    explicit EnvGuard(const char *n) : name(n), old(qgetenv(n)), had(qEnvironmentVariableIsSet(n)) {}
    ~EnvGuard() { if (had) qputenv(name,old); else qunsetenv(name); }
};
}

class EngineIntegrationTest : public QObject {
    Q_OBJECT
    // Runs one English swipe of `word` in a fresh keyboard. model: use the tiny fixture model (else a missing file);
    // pinned: leave the asset tree's own verified model in charge (no override at all).
    struct Run { QString text; QVariantMap diagnostics; int token = -1; QStringList alternatives; };
    Run swipeThe(bool model, Qt::InputMethodHints hints = Qt::ImhNone, bool damagedModel = false, const QString &word = QStringLiteral("the"), bool nanPoint = false, bool pinned = false) {
        Run run;
        const auto assets=qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS"), packs=qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS");
        QTemporaryDir root, data;
        // WHY SWIPEKB_SWIPE_MODEL instead of copying the assets: the pinned asset folder is read-only and must stay
        // byte-identical; the pinned default location is covered by the `pinned` case.
        const QString modelFile=root.filePath("swipe-model.bin");
        // SWIPEKB_TEST_SWIPE_MODEL (optional) swaps the tiny fixture for a real trained model, for the manual sanity case.
        if (model) QFile::copy(qEnvironmentVariable("SWIPEKB_TEST_SWIPE_MODEL",QStringLiteral(SWIPEKB_TEST_DATA "/swipe_net/model_f32.bin")),modelFile);
        if (damagedModel) { QFile f(modelFile); (void)f.open(QIODevice::WriteOnly); f.write("SKNN-not-a-model"); }
        EnvGuard e1("SWIPEKB_MODEL_DATA"), e2("SWIPEKB_LANGUAGE_PACKS"), e3("SWIPEKB_DATA"), e5("SWIPEKB_SWIPE_MODEL");
        qputenv("SWIPEKB_MODEL_DATA",assets.toUtf8()); qputenv("SWIPEKB_LANGUAGE_PACKS",packs.toUtf8());
        qputenv("SWIPEKB_DATA",data.path().toUtf8());
        if (pinned) qunsetenv("SWIPEKB_SWIPE_MODEL"); else qputenv("SWIPEKB_SWIPE_MODEL",modelFile.toUtf8());   // absent file = missing model
        QQmlEngine qml; QQmlComponent component(&qml);
        component.setData(QByteArray(R"(
            import QtQuick
            import QtQuick.VirtualKeyboard
            Item {
                width: 1000; height: 400
                property QtObject context: InputContext
                property alias text: editor.text
                function focusEditor() { editor.forceActiveFocus() }
                TextInput { id: editor; width: 900; height: 100; focus: true; inputMethodHints: Qt.ImhNone }
            }
        )"),QUrl());
        std::unique_ptr<QObject> item(component.create());
        if (!item) { QTest::qFail(qPrintable(component.errorString()),__FILE__,__LINE__); return run; }
        auto *ic=qobject_cast<QVirtualKeyboardInputContext *>(item->property("context").value<QObject *>());
        QQuickWindow window; window.resize(1000,400);
        qobject_cast<QQuickItem *>(item.get())->setParentItem(window.contentItem()); window.show();
        QMetaObject::invokeMethod(item.get(),"focusEditor");
        if (!QTest::qWaitFor([&] { return ic->inputItem(); })) return run;
        if (hints) ic->inputItem()->setProperty("inputMethodHints",int(hints));
        SwipeInputMethod im;
        ic->inputEngine()->setInputMethod(&im);
        const auto detach=qScopeGuard([&] { ic->inputEngine()->setInputMethod(nullptr); });
        im.setPhysicalLayout("qwerty"); im.setInputMode("en_GB",QVirtualKeyboardInputEngine::InputMode::Latin);
        if (!im.isReady()) { QTest::qFail(qPrintable(im.profileSnapshot().unavailableReason),__FILE__,__LINE__); return run; }
        const auto keys=qwertyKeys();
        run.token=im.beginSwipe();
        auto pts=traceFor(word,keys);
        if (nanPoint) { auto m=pts[3].toMap(); m["x"]=std::nan(""); pts[3]=m; }
        if (run.token>=0) im.finishSwipe(pts,keys,run.token);
        QTest::qWait(50);
        run.text=item->property("text").toString();
        run.diagnostics=im.diagnostics();
        for (int i=0;i<im.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList);++i)
            run.alternatives << im.selectionListData(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,i,
                                                     QVirtualKeyboardSelectionListModel::Role::Display).toString();
        return run;
    }
private slots:
    // ---- plugin level (needs the pinned English assets, like tst_languageintegration) ----
    void pinnedModelIsUsedByDefault() {
        if (!languagefixtures::runtimeAvailable()) QSKIP("Explicit pinned English assets and real language packs required");
        if (!QFileInfo::exists(qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS")+"/swipe-model.bin")) QSKIP("Pinned swipe model not in the fixture");
        const auto r=swipeThe(false,Qt::ImhNone,false,"the",false,true);
        QVERIFY(r.token>=0); QCOMPARE(r.text.trimmed().toLower(),QString("the"));
        QCOMPARE(r.diagnostics["swipeEngine"].toString(),QString("own"));
        QVERIFY(!r.alternatives.isEmpty());
    }
    void overrideModelIsUsed() {
        if (!languagefixtures::runtimeAvailable()) QSKIP("Explicit pinned English assets and real language packs required");
        const auto r=swipeThe(true);
        QVERIFY(r.token>=0); QVERIFY(!r.text.trimmed().isEmpty());
        QCOMPARE(r.diagnostics["swipeEngine"].toString(),QString("own"));
    }
    void missingModelFallsBackToGeometry() {
        if (!languagefixtures::runtimeAvailable()) QSKIP("Explicit pinned English assets and real language packs required");
        const auto r=swipeThe(false);
        QCOMPARE(r.diagnostics["swipeEngine"].toString(),QString("own-geometry"));
        QCOMPARE(r.text.trimmed().toLower(),QString("the")); // sentence start capitalises
    }
    void damagedModelFallsBackToGeometry() {
        if (!languagefixtures::runtimeAvailable()) QSKIP("Explicit pinned English assets and real language packs required");
        const auto r=swipeThe(false,Qt::ImhNone,true);
        QCOMPARE(r.diagnostics["swipeEngine"].toString(),QString("own-geometry"));
        QCOMPARE(r.text.trimmed().toLower(),QString("the")); // sentence start capitalises
    }
    // WHY a trace with a broken point: the engine answers nothing for it. The keyboard must then type nothing (never
    // crash or type junk), and stay ready for the next swipe.
    void unanswerableSwipeTypesNothing() {
        if (!languagefixtures::runtimeAvailable()) QSKIP("Explicit pinned English assets and real language packs required");
        const auto r=swipeThe(true,Qt::ImhNone,false,"the",true);
        QVERIFY(r.token>=0);
        QVERIFY(r.text.trimmed().isEmpty());
        QVERIFY(r.diagnostics["engineReady"].toBool());
    }
    // Manual sanity with a real trained model (skipped unless SWIPEKB_TEST_SWIPE_MODEL names one): clean straight
    // swipes of ordinary words must come out as those words through the plugin path.
    void realModelTypesOrdinaryWords() {
        if (!languagefixtures::runtimeAvailable() || qEnvironmentVariable("SWIPEKB_TEST_SWIPE_MODEL").isEmpty())
            QSKIP("Needs the pinned English assets and SWIPEKB_TEST_SWIPE_MODEL");
        for (const QString &word:{"hello","world","keyboard","because","thing"}) {
            const auto r=swipeThe(true,Qt::ImhNone,false,word);
            QCOMPARE(r.diagnostics["swipeEngine"].toString(),QString("own"));
            QCOMPARE(r.text.trimmed().toLower(),word);
        }
    }
    void deniedFieldsStayUntouched() {
        if (!languagefixtures::runtimeAvailable()) QSKIP("Explicit pinned English assets and real language packs required");
        const auto r=swipeThe(true,Qt::ImhSensitiveData);
        QVERIFY(r.token<0);                 // swipe refused before any recognition
        QVERIFY(r.text.isEmpty());
    }
};

int main(int argc,char **argv) {
    QGuiApplication app(argc,argv); EngineIntegrationTest test;
    const int result=QTest::qExec(&test,argc,argv);
    // WHY CTest must show missing fixtures as skipped, never as a green pass.
    return result ? result : (languagefixtures::runtimeAvailable() ? 0 : 77);
}
#include "tst_engineintegration.moc"
