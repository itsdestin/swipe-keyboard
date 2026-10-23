// The project's own swipe engine inside the keyboard plugin (opt-in; FUTO stays the default).
// WHY these cases: each guards one promise made to the owner: off by default, on only when asked, never "no swipe"
// when the model file is missing, never a crash, and sensitive fields stay untouched.
#include <QtTest>
#include <QTemporaryDir>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QQuickItem>
#include <QScopeGuard>
#include <QGuiApplication>
#include <QtVirtualKeyboard/QVirtualKeyboardInputContext>
#include "languagefixtures.h"
#include "ownswipe.h"
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

class OwnSwipeIntegrationTest : public QObject {
    Q_OBJECT
    // Runs one English swipe of "the" in a fresh keyboard whose data folder holds (or lacks) the own-engine files.
    struct Run { QString text; QVariantMap diagnostics; int token = -1; QStringList alternatives; };
    Run swipeThe(bool engineEnv, bool flagFile, bool model, Qt::InputMethodHints hints = Qt::ImhNone, bool damagedModel = false, const QString &word = QStringLiteral("the"), bool nanPoint = false) {
        Run run;
        const auto assets=qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS"), packs=qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS");
        QTemporaryDir root, data;
        // WHY SWIPEKB_SWIPE_MODEL instead of copying the 130 MB assets: the pinned asset folder is read-only and
        // must stay byte-identical, and the default location (<model dir>/swipe-model.bin) is covered by the
        // "missing" case, since the fixture folder has no such file.
        const QString modelFile=root.filePath("swipe-model.bin");
        // SWIPEKB_TEST_SWIPE_MODEL (optional) swaps the tiny fixture for a real trained model, for the manual sanity case.
        if (model) QFile::copy(qEnvironmentVariable("SWIPEKB_TEST_SWIPE_MODEL",QStringLiteral(SWIPEKB_TEST_DATA "/swipe_net/model_f32.bin")),modelFile);
        if (damagedModel) { QFile f(modelFile); (void)f.open(QIODevice::WriteOnly); f.write("SKNN-not-a-model"); }
        if (flagFile) { QFile f(data.filePath("own-engine")); (void)f.open(QIODevice::WriteOnly); }
        EnvGuard e1("SWIPEKB_MODEL_DATA"), e2("SWIPEKB_LANGUAGE_PACKS"), e3("SWIPEKB_DATA"), e4("SWIPEKB_ENGINE"), e5("SWIPEKB_SWIPE_MODEL");
        qputenv("SWIPEKB_MODEL_DATA",assets.toUtf8()); qputenv("SWIPEKB_LANGUAGE_PACKS",packs.toUtf8());
        qputenv("SWIPEKB_DATA",data.path().toUtf8());
        if (model || damagedModel) qputenv("SWIPEKB_SWIPE_MODEL",modelFile.toUtf8()); else qunsetenv("SWIPEKB_SWIPE_MODEL");
        if (engineEnv) qputenv("SWIPEKB_ENGINE","own"); else qunsetenv("SWIPEKB_ENGINE");
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
    void offByDefaultFutoAnswers() {
        if (!languagefixtures::runtimeAvailable()) QSKIP("Explicit pinned English assets and real language packs required");
        const auto r=swipeThe(false,false,true);
        QVERIFY(r.token>=0); QVERIFY(!r.text.trimmed().isEmpty());
        QCOMPARE(r.diagnostics["swipeEngine"].toString(),QString("futo"));
        QCOMPARE(r.diagnostics["ownEngineSwipes"].toULongLong(),quint64(0));
    }
    void envSwitchUsesOwnEngine() {
        if (!languagefixtures::runtimeAvailable()) QSKIP("Explicit pinned English assets and real language packs required");
        const auto r=swipeThe(true,false,true);
        QVERIFY(r.token>=0); QVERIFY(!r.text.trimmed().isEmpty());
        QCOMPARE(r.diagnostics["swipeEngine"].toString(),QString("own"));
        QCOMPARE(r.diagnostics["ownEngineSwipes"].toULongLong(),quint64(1));
        QCOMPARE(r.diagnostics["ownEngineFallbacks"].toULongLong(),quint64(0));
        QVERIFY(!r.alternatives.isEmpty());
    }
    void flagFileUsesOwnEngine() {
        if (!languagefixtures::runtimeAvailable()) QSKIP("Explicit pinned English assets and real language packs required");
        const auto r=swipeThe(false,true,true);
        QCOMPARE(r.diagnostics["swipeEngine"].toString(),QString("own"));
        QCOMPARE(r.diagnostics["ownEngineSwipes"].toULongLong(),quint64(1));
    }
    void missingModelFallsBackToGeometry() {
        if (!languagefixtures::runtimeAvailable()) QSKIP("Explicit pinned English assets and real language packs required");
        const auto r=swipeThe(true,false,false);
        QCOMPARE(r.diagnostics["swipeEngine"].toString(),QString("own-geometry"));
        QCOMPARE(r.diagnostics["ownEngineSwipes"].toULongLong(),quint64(1)); // still answered by us, not FUTO
        QCOMPARE(r.text.trimmed().toLower(),QString("the")); // sentence start capitalises
    }
    void damagedModelFallsBackToGeometry() {
        if (!languagefixtures::runtimeAvailable()) QSKIP("Explicit pinned English assets and real language packs required");
        const auto r=swipeThe(true,false,false,Qt::ImhNone,true);
        QCOMPARE(r.diagnostics["swipeEngine"].toString(),QString("own-geometry"));
        QCOMPARE(r.text.trimmed().toLower(),QString("the")); // sentence start capitalises
    }
    // WHY a trace with a broken point: the own engine says "no" for it. The keyboard must then hand the swipe to FUTO
    // (never crash or type the own engine's guess) and count the fall-back.
    void unanswerableSwipeFallsBackToFuto() {
        if (!languagefixtures::runtimeAvailable()) QSKIP("Explicit pinned English assets and real language packs required");
        const auto r=swipeThe(true,false,true,Qt::ImhNone,false,"the",true);
        QVERIFY(r.token>=0);
        QCOMPARE(r.diagnostics["ownEngineSwipes"].toULongLong(),quint64(0));
        QCOMPARE(r.diagnostics["ownEngineFallbacks"].toULongLong(),quint64(1));
    }
    // Manual sanity with a real trained model (skipped unless SWIPEKB_TEST_SWIPE_MODEL names one): clean straight
    // swipes of ordinary words must come out as those words through the plugin path.
    void realModelTypesOrdinaryWords() {
        if (!languagefixtures::runtimeAvailable() || qEnvironmentVariable("SWIPEKB_TEST_SWIPE_MODEL").isEmpty())
            QSKIP("Needs the pinned English assets and SWIPEKB_TEST_SWIPE_MODEL");
        for (const QString &word:{"hello","world","keyboard","because","thing"}) {
            const auto r=swipeThe(true,false,true,Qt::ImhNone,false,word);
            QCOMPARE(r.diagnostics["swipeEngine"].toString(),QString("own"));
            QCOMPARE(r.diagnostics["ownEngineSwipes"].toULongLong(),quint64(1));
            QCOMPARE(r.text.trimmed().toLower(),word);
        }
    }
    void deniedFieldsStayUntouched() {
        if (!languagefixtures::runtimeAvailable()) QSKIP("Explicit pinned English assets and real language packs required");
        const auto r=swipeThe(true,false,true,Qt::ImhSensitiveData);
        QVERIFY(r.token<0);                 // swipe refused before any recognition, exactly as with FUTO
        QVERIFY(r.text.isEmpty());
        QCOMPARE(r.diagnostics["ownEngineSwipes"].toULongLong(),quint64(0));
        QCOMPARE(r.diagnostics["ownEngineFallbacks"].toULongLong(),quint64(0));
    }

    // ---- the glue class on its own (no assets needed) ----
    void requestedOnlyWhenAsked() {
        EnvGuard env("SWIPEKB_ENGINE"); qunsetenv("SWIPEKB_ENGINE");
        QTemporaryDir dir;
        QVERIFY(!swipekb::OwnSwipe::requested(dir.path().toStdString()));
        QFile f(dir.filePath("own-engine")); QVERIFY(f.open(QIODevice::WriteOnly)); f.close();
        QVERIFY(swipekb::OwnSwipe::requested(dir.path().toStdString()));
        qputenv("SWIPEKB_ENGINE","futo");
        QVERIFY(!swipekb::OwnSwipe::requested(dir.path().toStdString())); // an explicit env value wins
    }
    void glueAnswersAndRefusesBadInput() {
        QTemporaryDir dir;
        QFile d(dir.filePath("t.combined")); QVERIFY(d.open(QIODevice::WriteOnly));
        d.write("dictionary=main:en,locale=en,version=1\n word=the,f=220,flags=,originalFreq=220\n word=he,f=210\n word=she,f=200\n"
                " word=don't,f=190\n word=bad,f=0\n word=zzz,f=100,nonword=true\n word=cat,f=150\n"); d.close();
        swipekb::OwnSwipeSetup setup; setup.dictionary=d.fileName().toStdString();
        swipekb::OwnSwipe own; QVERIFY(own.init(setup));
        QVERIFY(!own.hasModel()); QCOMPARE(QString(own.mode()),QString("own-geometry"));
        std::vector<swipekb::KeyRect> keys; const QStringList rows{"qwertyuiop","asdfghjkl","zxcvbnm"};
        for (int r=0;r<rows.size();++r) for (int i=0;i<rows[r].size();++i) keys.push_back({char32_t(rows[r][i].unicode()),float(i*100),float(r*100),100,100});
        std::vector<swipekb::TracePoint> trace; float t=0;
        for (const auto &p:traceFor("the",qwertyKeys())) { const auto m=p.toMap(); trace.push_back({m["x"].toFloat(),m["y"].toFloat(),t}); t+=16; }
        swipekb::OwnSwipe::Result res;
        QVERIFY(own.recognize(trace,keys,{},10,res));
        QCOMPARE(QString::fromStdString(res.words.front()),QString("the"));
        QVERIFY(std::find(res.words.begin(),res.words.end(),std::string("zzz"))==res.words.end()); // nonword entries never offered
        QCOMPARE(QString::fromStdString(res.bestShape).isEmpty(),false);
        // Moving the keys (a window resize) must keep answering without a rebuild failure.
        auto scaled=keys; for (auto &k:scaled) { k.x*=2; k.y*=2; k.w*=2; k.h*=2; }
        auto big=trace; for (auto &p:big) { p.x*=2; p.y*=2; }
        QVERIFY(own.recognize(big,scaled,{},10,res)); QCOMPARE(QString::fromStdString(res.words.front()),QString("the"));
        // Unusable inputs answer "no" (the plugin then uses FUTO) instead of guessing or crashing.
        QVERIFY(!own.recognize({trace.front()},keys,{},10,res));
        QVERIFY(!own.recognize(trace,{},{},10,res));
        auto zeroSized=keys; zeroSized[0].w=0;
        QVERIFY(!own.recognize(trace,zeroSized,{},10,res));
        auto nan=trace; nan[3].x=std::nanf("");
        QVERIFY(!own.recognize(nan,keys,{},10,res));
    }
    void glueRefusesUnreadableDictionary() {
        swipekb::OwnSwipeSetup setup; setup.dictionary="/nonexistent/dictionary.combined";
        swipekb::OwnSwipe own; QVERIFY(!own.init(setup));
        QTemporaryDir dir; QFile d(dir.filePath("empty.combined")); QVERIFY(d.open(QIODevice::WriteOnly)); d.write("dictionary=x\n"); d.close();
        setup.dictionary=d.fileName().toStdString();
        QVERIFY(own.init(setup));      // file exists, but holds no words: the first swipe says no
        std::vector<swipekb::KeyRect> keys{{'a',0,0,10,10},{'b',10,0,10,10}};
        swipekb::OwnSwipe::Result res;
        QVERIFY(!own.recognize({{1,1,0},{15,5,10}},keys,{},5,res));
    }
};

int main(int argc,char **argv) {
    QGuiApplication app(argc,argv); OwnSwipeIntegrationTest test;
    return QTest::qExec(&test,argc,argv);
}
#include "tst_ownswipeintegration.moc"
