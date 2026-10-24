// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QtTest>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QExposeEvent>
#include <QQuickItem>
#include <QInputMethodEvent>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QSemaphore>
#include <QRegularExpression>
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
    void allow() { permitted = true; ++revision; emit swipePredictionPolicyChanged(); }
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
    bool suppressInputMethodUpdate=false;
    // Lets a test deliver an app's cursor notification before its text notification (Qt may do either order).
    bool lagText=false;QString laggedText;
    InputListenerItem(QQuickItem *parent):QQuickItem(parent) { setFlag(ItemAcceptsInputMethod); setWidth(400);setHeight(100); }
    QVariant inputMethodQuery(Qt::InputMethodQuery q) const override {
        switch(q) {
        case Qt::ImEnabled:return true;
        case Qt::ImHints:return int(hints);
        case Qt::ImSurroundingText:++surroundingQueries;return lagText?laggedText:text;
        case Qt::ImCursorPosition:case Qt::ImAnchorPosition:case Qt::ImAbsolutePosition:return cursor;
        case Qt::ImCurrentSelection:return QString();
        case Qt::ImCursorRectangle:return QRectF(0,0,2,20);
        default:return QQuickItem::inputMethodQuery(q);
        }
    }
    void inputMethodEvent(QInputMethodEvent *event) override {
        text.replace(cursor+event->replacementStart(),event->replacementLength(),event->commitString());
        cursor+=event->replacementStart()+event->commitString().size();event->accept();
        if(!suppressInputMethodUpdate) QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
    }
};

class NextWordIntegrationTest : public QObject {
    Q_OBJECT
private slots:
    void exposeRefreshesMissedNativeProxyCache() {
        QTemporaryDir data; qputenv("SWIPEKB_DATA",data.path().toUtf8());
        qputenv("SWIPEKB_MODEL_DATA",qgetenv("SWIPEKB_TEST_PROFILE_ASSETS"));
        qputenv("SWIPEKB_NATIVE_PREDICTION","owned-history-v1");
        struct EnvReset { ~EnvReset(){qunsetenv("SWIPEKB_NATIVE_PREDICTION");} } envReset;
        QQmlEngine engine; QQmlComponent fixture(&engine);
        fixture.setData("import QtQuick; import QtQuick.VirtualKeyboard; Item {property var context: InputContext}",QUrl());
        std::unique_ptr<QObject> root(fixture.create()); QVERIFY2(root,qPrintable(fixture.errorString()));
        auto *item=qobject_cast<QQuickItem*>(root.get()); QQuickWindow window; window.resize(500,200);
        item->setParentItem(window.contentItem()); InputListenerItem edit(item);
        window.show(); window.requestActivate(); edit.forceActiveFocus();
        auto *ic=qvariant_cast<QVirtualKeyboardInputContext*>(root->property("context")); QVERIFY(ic);
        QSemaphore completed; QStringList captured;
        // WHY only next-word jobs (empty prefix) are recorded: with the open English n-gram installed every typed letter also
        // sends a completion job, and under load one of those could satisfy `completed` before the real job's context was captured.
        SwipeInputMethod method(nullptr,[&](const PredictionJob &job){if(job.prefix.isEmpty()){captured=job.context;completed.release();}return QStringList{"world","again","today"};});
        ic->inputEngine()->setInputMethod(&method); method.setInputMode("en_US",QVirtualKeyboardInputEngine::InputMode::Latin);
        method.setPhysicalLayout("qwerty"); QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
        QTRY_VERIFY(ic->inputItem()==&edit);
        for(auto c:QStringLiteral("hello")) {
            const QString letter(c); QVERIFY(!method.keyEvent(Qt::Key(letter.front().toUpper().unicode()),letter,Qt::NoModifier));
            ic->commit(letter); QGuiApplication::inputMethod()->update(Qt::ImQueryAll); method.update();
        }
        QVERIFY(!method.keyEvent(Qt::Key_Space," ",Qt::NoModifier));
        edit.suppressInputMethodUpdate=true; ic->commit(" ");
        QCOMPARE(ic->surroundingText(),QStringLiteral("preexisting sentinel hello"));
        QVERIFY(window.isExposed()); QVERIFY(edit.hasActiveFocus());
        // WHY synthetic Expose is confined to this offscreen fixture: it reproduces the compositor notification.
        QExposeEvent expose{QRegion()}; QCoreApplication::sendEvent(&window,&expose);
        QTRY_COMPARE_WITH_TIMEOUT(ic->surroundingText(),QStringLiteral("preexisting sentinel hello "),3000);
        QVERIFY(completed.tryAcquire(1,15000)); QCOMPARE(captured,QStringList{QStringLiteral("hello")});
        QTRY_COMPARE_WITH_TIMEOUT(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList),3,15000);
        method.beginPredictionSelection(method.predictionSelectionToken());
        method.selectionListItemSelected(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,0);
        QTRY_COMPARE_WITH_TIMEOUT(edit.text,QStringLiteral("preexisting sentinel hello again "),5000);
        edit.hints=Qt::ImhSensitiveData;
        const int beforeSensitiveExpose=edit.surroundingQueries;
        QExposeEvent sensitiveExpose{QRegion()}; QCoreApplication::sendEvent(&window,&sensitiveExpose);
        QTest::qWait(20); QCOMPARE(edit.surroundingQueries,beforeSensitiveExpose);
        edit.hints=Qt::ImhNoPredictiveText;
        SwipeInputMethod replacement;
        ic->inputEngine()->setInputMethod(&replacement);
        const int beforeInactiveExpose=edit.surroundingQueries;
        QExposeEvent inactiveExpose{QRegion()}; QCoreApplication::sendEvent(&window,&inactiveExpose);
        QTest::qWait(20); QCOMPARE(edit.surroundingQueries,beforeInactiveExpose);
        window.hide(); const int beforeHiddenExpose=edit.surroundingQueries;
        QExposeEvent hiddenExpose{QRegion()}; QCoreApplication::sendEvent(&window,&hiddenExpose);
        QTest::qWait(20); QCOMPARE(edit.surroundingQueries,beforeHiddenExpose);
        ic->inputEngine()->setInputMethod(nullptr);
    }
    void keyPressRefreshesMissedEchoesForCurrentWordCompletion_data() {
        QTest::addColumn<bool>("native"); QTest::newRow("native-arm")<<true; QTest::newRow("bundled-steamos")<<false;
    }
    void keyPressRefreshesMissedEchoesForCurrentWordCompletion() {
        // WHY: Plasma Keyboard drops text notifications while its window is not exposed (ARM Plasma
        // Mobile mid-word; SteamOS on first activation), so the typed word was never confirmed.
        QFETCH(bool,native);
        QTemporaryDir data; qputenv("SWIPEKB_DATA",data.path().toUtf8());
        qputenv("SWIPEKB_MODEL_DATA",qgetenv("SWIPEKB_TEST_PROFILE_ASSETS"));
        if(native) qputenv("SWIPEKB_NATIVE_PREDICTION","owned-history-v1");
        struct EnvReset { ~EnvReset(){qunsetenv("SWIPEKB_NATIVE_PREDICTION");} } envReset;
        QQmlEngine engine; QQmlComponent fixture(&engine);
        fixture.setData("import QtQuick; import QtQuick.VirtualKeyboard; Item {property var context: InputContext}",QUrl());
        std::unique_ptr<QObject> root(fixture.create()); QVERIFY2(root,qPrintable(fixture.errorString()));
        auto *item=qobject_cast<QQuickItem*>(root.get()); QQuickWindow window; window.resize(500,200);
        item->setParentItem(window.contentItem()); InputListenerItem edit(item);
        // The bundled backend's proxy carries the raw policy itself, like the patched Plasma Keyboard.
        if(!native) edit.setProperty("swipePredictionPolicy",QVariantMap{{"version",1},{"contextId",quint64(9)},
            {"revision",quint64(1)},{"contentTypeReceived",true},{"purpose",0},{"completionAllowed",true},{"syntheticNoPredictiveText",true}});
        window.show(); window.requestActivate(); edit.forceActiveFocus();
        auto *ic=qvariant_cast<QVirtualKeyboardInputContext*>(root->property("context")); QVERIFY(ic);
        SwipeInputMethod method(nullptr,[](const PredictionJob &){return QStringList{"world","again","today"};});
        ic->inputEngine()->setInputMethod(&method); method.setInputMode("en_US",QVirtualKeyboardInputEngine::InputMode::Latin);
        method.setPhysicalLayout("qwerty"); QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
        QTRY_VERIFY(ic->inputItem()==&edit); QVERIFY(window.isExposed());
        edit.suppressInputMethodUpdate=true; // the app's echoes never reach Qt's cache by themselves
        for(auto c:QStringLiteral("hel")) {
            const QString letter(c); QVERIFY(!method.keyEvent(Qt::Key(letter.front().toUpper().unicode()),letter,Qt::NoModifier));
            ic->commit(letter);
        }
        QCOMPARE(ic->surroundingText(),QStringLiteral("preexisting sentinel he")); // last echo still missing
        // Only the final echo arrives normally; the earlier two were recovered at each key press.
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); method.update();
        // WHY wait: the candidates come from a worker thread; under load the check ran before its result arrived.
        QTRY_COMPARE_WITH_TIMEOUT(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList),3,15000);
        const auto chosen=method.selectionListData(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,1,
            QVirtualKeyboardSelectionListModel::Role::Display).toString();
        QVERIFY(!chosen.isEmpty() && chosen!=QStringLiteral("hel"));
        method.beginPredictionSelection(method.predictionSelectionToken());
        method.selectionListItemSelected(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,1);
        QCOMPARE(edit.text,QStringLiteral("preexisting sentinel ")+chosen+" ");
        ic->inputEngine()->setInputMethod(nullptr);
    }
    void typedCompletionReplacesOnlyOwnedPrefix_data() {
        QTest::addColumn<bool>("native");QTest::addColumn<bool>("literal");QTest::addColumn<bool>("shiftedFirst");
        for(bool native:{false,true})for(bool literal:{false,true})for(bool shiftedFirst:{false,true})
            QTest::newRow(qPrintable(QString("%1-%2%3").arg(native?"native":"bridge",literal?"literal":"completion")
                .arg(shiftedFirst?"-shifted-first":"")))<<native<<literal<<shiftedFirst;
    }
    void typedCompletionReplacesOnlyOwnedPrefix() {
        QFETCH(bool,native);QFETCH(bool,literal);QFETCH(bool,shiftedFirst);
        QTemporaryDir data;qputenv("SWIPEKB_DATA",data.path().toUtf8());qputenv("SWIPEKB_MODEL_DATA",qgetenv("SWIPEKB_TEST_PROFILE_ASSETS"));
        if(native)qputenv("SWIPEKB_NATIVE_PREDICTION","owned-history-v1");
        struct EnvReset{~EnvReset(){qunsetenv("SWIPEKB_NATIVE_PREDICTION");}} envReset;
        QQmlEngine engine;QQmlComponent fixture(&engine);fixture.setData("import QtQuick; import QtQuick.VirtualKeyboard; Item {property var context:InputContext}",QUrl());
        std::unique_ptr<QObject> root(fixture.create());QVERIFY(root);auto *item=qobject_cast<QQuickItem*>(root.get());
        QQuickWindow window;window.resize(500,200);item->setParentItem(window.contentItem());InputListenerItem edit(item);
        window.show();window.requestActivate();edit.forceActiveFocus();
        auto *ic=qvariant_cast<QVirtualKeyboardInputContext*>(root->property("context"));QVERIFY(ic);
        QStringList captured;QSemaphore done;
        // WHY only next-word jobs (empty prefix) are recorded: with the open English n-gram installed every typed letter also
        // sends a completion job, and counting those let `done` be satisfied by a completion, racing the check below.
        SwipeInputMethod method(nullptr,[&](const PredictionJob&job){if(job.prefix.isEmpty()){captured=job.context;done.release();}return QStringList{"world","again","today"};});
        ic->inputEngine()->setInputMethod(&method);method.setInputMode("en_US",QVirtualKeyboardInputEngine::InputMode::Latin);method.setPhysicalLayout("qwerty");
        TestPolicyBridge bridge;if(!native)method.setPredictionPolicySource(&bridge);
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);QTRY_VERIFY(ic->inputItem()==&edit);
        int typedIndex=0;const QString literalWord=shiftedFirst?QStringLiteral("Hel"):QStringLiteral("hel");
        for(auto c:QString("hel")){
            const bool shifted=shiftedFirst&&typedIndex==0;const QString typed=shifted?QString(c).toUpper():QString(c);
            const auto modifiers=shifted?Qt::KeyboardModifiers(Qt::ShiftModifier):Qt::KeyboardModifiers(Qt::NoModifier);
            method.keyEvent(Qt::Key(typed.front().toUpper().unicode()),typed,modifiers);ic->commit(typed);
            QGuiApplication::inputMethod()->update(Qt::ImQueryAll);method.update();++typedIndex;
        }
        // WHY wait: the candidates come from a worker thread; under load the check ran before its result arrived.
        QTRY_COMPARE_WITH_TIMEOUT(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList),3,15000);
        QStringList words;for(int i=0;i<3;++i)words<<method.selectionListData(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,i,QVirtualKeyboardSelectionListModel::Role::Display).toString();
        QVERIFY(words.contains(literalWord));QVERIFY(!method.predictionSelectionToken().isEmpty());
        int index=literal?words.indexOf(literalWord):1;const auto selected=words[index];
        method.beginPredictionSelection(method.predictionSelectionToken());method.selectionListItemSelected(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,index);
        // 0.3.1: accepting a completion adds the space and requests next words like a typed Space.
        QCOMPARE(edit.text,QString("preexisting sentinel ")+selected+" ");
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);method.update();
        QVERIFY(done.tryAcquire(1,15000));
        if(native)QCOMPARE(captured,QStringList{selected.toLower()});
        QTRY_VERIFY(!method.predictionSelectionToken().isEmpty());ic->inputEngine()->setInputMethod(nullptr);
    }
    // ---- Tap a word to see and swap alternatives (card #187) -------------------------------------------------
private:
    // One field with a raw-policy bridge (the supported mode); tests change the text/cursor like an app would.
    struct SwapFixture {
        QTemporaryDir data; QQmlEngine engine; QQmlComponent fixture{&engine};
        std::unique_ptr<QObject> root; QQuickWindow window; std::unique_ptr<InputListenerItem> edit;
        QVirtualKeyboardInputContext *ic = nullptr; std::unique_ptr<SwipeInputMethod> method; TestPolicyBridge bridge;
        QStringList jobPrefixes, jobContexts; QSemaphore ranked;
        bool init(const QString &text, int cursor, Qt::InputMethodHints extra, const QString &locale,
                  SwipeInputMethod *own = nullptr, bool native = false) {
            qputenv("SWIPEKB_DATA", data.path().toUtf8());
            if (locale.startsWith("en")) qputenv("SWIPEKB_MODEL_DATA", qgetenv("SWIPEKB_TEST_PROFILE_ASSETS"));
            if (native) qputenv("SWIPEKB_NATIVE_PREDICTION", "owned-history-v1");
            fixture.setData("import QtQuick; import QtQuick.VirtualKeyboard; Item {property var context:InputContext}", QUrl());
            root.reset(fixture.create()); if (!root) return false;
            auto *item = qobject_cast<QQuickItem*>(root.get()); window.resize(500, 200); item->setParentItem(window.contentItem());
            edit = std::make_unique<InputListenerItem>(item); edit->text = text; edit->cursor = cursor; edit->hints |= extra;
            window.show(); window.requestActivate(); edit->forceActiveFocus();
            ic = qvariant_cast<QVirtualKeyboardInputContext*>(root->property("context"));
            method.reset(own ? own : new SwipeInputMethod(nullptr, [this](const PredictionJob &job) {
                if (job.alternatives) { jobPrefixes << job.prefix; jobContexts = job.context; ranked.release(); return QStringList{"paso", "casi"}; }
                return QStringList{"x", "y", "z"}; }));
            ic->inputEngine()->setInputMethod(method.get()); method->setInputMode(locale, QVirtualKeyboardInputEngine::InputMode::Latin);
            method->setPhysicalLayout("qwerty");
            if (!native) method->setPredictionPolicySource(&bridge);
            QGuiApplication::inputMethod()->update(Qt::ImQueryAll); method->update();
            return true;
        }
        ~SwapFixture() { if (ic) ic->inputEngine()->setInputMethod(nullptr); qunsetenv("SWIPEKB_NATIVE_PREDICTION"); }
        // The person taps in the text: only the cursor moves, then Qt reports it.
        void tapAt(int cursor) { edit->cursor = cursor; QGuiApplication::inputMethod()->update(Qt::ImQueryAll); method->update(); }
        QStringList bar() const {
            QStringList words; const auto type = QVirtualKeyboardSelectionListModel::Type::WordCandidateList;
            for (int i = 0; i < method->selectionListItemCount(type); ++i)
                words << method->selectionListData(type, i, QVirtualKeyboardSelectionListModel::Role::Display).toString();
            return words;
        }
        void choose(int index) {
            method->beginPredictionSelection(method->predictionSelectionToken());
            method->selectionListItemSelected(QVirtualKeyboardSelectionListModel::Type::WordCandidateList, index);
        }
        // A straight-line swipe through the letters of `word` on a 100-pixel qwerty grid. Returns false when refused.
        bool swipe(const QString &word) {
            QVariantList keys, points; const QStringList rows{"qwertyuiop", "asdfghjkl", "zxcvbnm"};
            for (int r = 0; r < rows.size(); ++r) for (int c = 0; c < rows[r].size(); ++c)
                keys << QVariantMap{{"letter", QString(rows[r][c])}, {"x", c * 100}, {"y", r * 100}, {"w", 100}, {"h", 100}};
            QPointF previous;
            for (auto letter : word) {
                QPointF center;
                for (const auto &v : keys) { auto k = v.toMap(); if (k["letter"].toString() == QString(letter)) center = {k["x"].toDouble() + 50, k["y"].toDouble() + 50}; }
                if (points.isEmpty()) points << QVariantMap{{"x", center.x()}, {"y", center.y()}, {"t", 0}};
                else for (int step = 1; step <= 8; ++step) { auto p = previous + (center - previous) * (step / 8.0); points << QVariantMap{{"x", p.x()}, {"y", p.y()}, {"t", points.size() * 16}}; }
                previous = center;
            }
            const int generation = method->beginSwipe();
            if (generation < 0) return false;
            method->finishSwipe(points, keys, generation);
            return true;
        }
        // The app reports its current state (Chromium sends surrounding text + cursor, then Qt asks again).
        void report() { QGuiApplication::inputMethod()->update(Qt::ImQueryAll); method->update(); }
        void connectNative() {
            // WHY a Shift press: it connects the keyboard to the field's stock proxy, as real use does within the
            // first key, so the owned-history rule really runs.
            method->keyEvent(Qt::Key_Shift, QString(), Qt::NoModifier);
            report();
        }
    };
private slots:
    void tappingAWordShowsAlternativesAndChangesNothing() {
        if (qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS").isEmpty()) QSKIP("English assets required");
        SwapFixture f; const QString text = QStringLiteral("we can sea you today ");
        QVERIFY(f.init(text, text.size(), {}, "en_US")); QTRY_VERIFY(f.ic->inputItem() == f.edit.get());
        QVERIFY2(f.method->isReady(), qPrintable(f.method->profileSnapshot().unavailableReason));
        QVERIFY(f.bar().isEmpty());
        f.tapAt(9);                                   // inside "sea"
        const auto shown = f.bar();
        QVERIFY(!shown.isEmpty() && shown.size() <= 3);
        QVERIFY2(shown.contains("see"), qPrintable(shown.join(",")));
        QVERIFY(!shown.contains("sea"));
        QCOMPARE(f.edit->text, text);                 // nothing swapped by itself
        QCOMPARE(f.edit->cursor, 9);
        f.tapAt(7);                                   // directly before the word: same word
        QCOMPARE(f.bar(), shown);
    }
    void choosingAnAlternativeReplacesOnlyThatWordKeepingItsCapitals_data() {
        QTest::addColumn<QString>("text"); QTest::addColumn<int>("tap"); QTest::addColumn<QString>("expected"); QTest::addColumn<int>("cursorAfter");
        QTest::newRow("lower") << "we can sea you today " << 9 << "we can see you today " << 10;
        QTest::newRow("capitalized") << "Sea is calm and sea is deep." << 1 << "See is calm and sea is deep." << 3;
        QTest::newRow("all caps") << "WE CAN SEA YOU" << 9 << "WE CAN SEE YOU" << 10;
        QTest::newRow("cursor just after the word") << "we can sea, you" << 10 << "we can see, you" << 10;
        QTest::newRow("same word twice") << "sea and sea" << 9 << "sea and see" << 11;
    }
    void choosingAnAlternativeReplacesOnlyThatWordKeepingItsCapitals() {
        if (qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS").isEmpty()) QSKIP("English assets required");
        QFETCH(QString, text); QFETCH(int, tap); QFETCH(QString, expected); QFETCH(int, cursorAfter);
        SwapFixture f; QVERIFY(f.init(text, text.size(), {}, "en_US")); QTRY_VERIFY(f.ic->inputItem() == f.edit.get());
        f.tapAt(tap);
        const auto shown = f.bar();
        const int index = shown.indexOf(QRegularExpression("^[Ss][Ee][Ee]$"));
        QVERIFY2(index >= 0, qPrintable(shown.join(",")));
        f.choose(index);
        QCOMPARE(f.edit->text, expected);             // no added space, no other word touched
        QCOMPARE(f.edit->cursor, cursorAfter);        // cursor ends right after the replaced word
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); f.method->update();
        QVERIFY(f.bar().isEmpty());                   // and the bar is normal again, with nothing left to swap
    }
    void nothingIsOfferedInPrivateOrDeniedFields_data() {
        QTest::addColumn<int>("hints"); QTest::addColumn<bool>("offered");
        QTest::newRow("ordinary") << 0 << true;
        QTest::newRow("lowercase hint") << int(Qt::ImhPreferLowercase) << true;
        QTest::newRow("password") << int(Qt::ImhHiddenText | Qt::ImhSensitiveData) << false;
        QTest::newRow("private") << int(Qt::ImhSensitiveData) << false;
        QTest::newRow("hidden") << int(Qt::ImhHiddenText) << false;
        QTest::newRow("web address") << int(Qt::ImhUrlCharactersOnly) << false;
        QTest::newRow("email") << int(Qt::ImhEmailCharactersOnly) << false;
        QTest::newRow("number") << int(Qt::ImhDigitsOnly) << false;
    }
    void nothingIsOfferedInPrivateOrDeniedFields() {
        if (qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS").isEmpty()) QSKIP("English assets required");
        QFETCH(int, hints); QFETCH(bool, offered);
        SwapFixture f; const QString text = QStringLiteral("we can sea you today ");
        QVERIFY(f.init(text, text.size(), Qt::InputMethodHints(hints), "en_US")); QTRY_VERIFY(f.ic->inputItem() == f.edit.get());
        f.tapAt(9);
        QCOMPARE(!f.bar().isEmpty(), offered);
        if (!offered) {
            // not even a press can swap: the field is simply not readable for this feature
            f.choose(0); QCOMPARE(f.edit->text, text);
        }
    }
    void typingSwipingOrMovingAwayReturnsTheBarToNormal() {
        if (qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS").isEmpty()) QSKIP("English assets required");
        SwapFixture f; const QString text = QStringLiteral("we can sea you today ");
        QVERIFY(f.init(text, text.size(), {}, "en_US")); QTRY_VERIFY(f.ic->inputItem() == f.edit.get());
        // moving the cursor out into empty space
        f.tapAt(9); QVERIFY(!f.bar().isEmpty());
        f.tapAt(text.size()); QVERIFY(f.bar().isEmpty());
        // tapping a different word replaces the list
        f.tapAt(9); const auto sea = f.bar(); f.tapAt(12); QVERIFY(!f.bar().isEmpty()); QVERIFY(f.bar() != sea);
        // typing a letter
        f.tapAt(9); QVERIFY(!f.bar().isEmpty());
        f.method->keyEvent(Qt::Key_S, "s", Qt::NoModifier);
        QVERIFY(f.bar().isEmpty());
        // starting a swipe
        f.ic->commit("s"); QGuiApplication::inputMethod()->update(Qt::ImQueryAll); f.method->update();
        QTest::qWait(300);                            // the typing above has settled
        f.tapAt(9); QVERIFY(!f.bar().isEmpty());
        f.method->beginSwipe(); QVERIFY(f.bar().isEmpty());
        // a field that turns private takes the bar away
        f.tapAt(f.edit->text.size()); f.tapAt(9); QVERIFY(!f.bar().isEmpty());
        f.edit->hints |= Qt::ImhSensitiveData; QGuiApplication::inputMethod()->update(Qt::ImQueryAll); f.method->update();
        QVERIFY(f.bar().isEmpty());
    }
    void alternativesCloseWhenTheTextChangesUnderThem() {
        // WHY: an app edit (paste, autocorrect, sync) while the bar is open must never leave a list that could
        // swap the wrong characters.
        if (qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS").isEmpty()) QSKIP("English assets required");
        SwapFixture f; const QString text = QStringLiteral("we can sea you today ");
        QVERIFY(f.init(text, text.size(), {}, "en_US")); QTRY_VERIFY(f.ic->inputItem() == f.edit.get());
        f.tapAt(9); QVERIFY(!f.bar().isEmpty());
        f.edit->text.insert(0, "oh "); f.edit->cursor = 12;
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); f.method->update();
        QVERIFY(!f.method->predictionSelectionToken().startsWith("swap:"));
    }
    void ownTypingNeverLooksLikeATap() {
        // WHY: after every typed letter the cursor sits right after a word; that must not open alternatives.
        // Qt reports an edit's cursor and text separately, in either order, so both orders are exercised.
        if (qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS").isEmpty()) QSKIP("English assets required");
        for (bool cursorFirst : {true, false}) {
            // text after the cursor, so the stale cursor of the cursor-first order still lands inside a word
            SwapFixture f; QVERIFY(f.init("we can sea go", 11, {}, "en_US")); QTRY_VERIFY(f.ic->inputItem() == f.edit.get());
            auto swapShowing = [&] { return f.method->predictionSelectionToken().startsWith("swap:"); };
            f.edit->suppressInputMethodUpdate = true;
            f.method->keyEvent(Qt::Key_S, "s", Qt::NoModifier);
            const QString old = f.edit->text;
            f.ic->commit("s");                            // the app now holds "we can sea sgo", cursor 12
            if (cursorFirst) {
                f.edit->lagText = true; f.edit->laggedText = old;
                QGuiApplication::inputMethod()->update(Qt::ImCursorPosition | Qt::ImAnchorPosition); f.method->update();
                QVERIFY2(!swapShowing(), "cursor arrived before the text of our own edit");
                f.edit->lagText = false;
                QGuiApplication::inputMethod()->update(Qt::ImSurroundingText); f.method->update();
            } else {
                QGuiApplication::inputMethod()->update(Qt::ImSurroundingText); f.method->update();
                QGuiApplication::inputMethod()->update(Qt::ImCursorPosition | Qt::ImAnchorPosition); f.method->update();
            }
            QVERIFY(!swapShowing());
            QCOMPARE(f.edit->text, QStringLiteral("we can sea sgo"));
        }
    }
    void tappingASwipedWordLaterOffersThatSwipesOtherGuesses() {
        // WHY: the keyboard remembers (in memory only) the guesses of recent swipes, anchored to the text around the word.
        if (qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS").isEmpty()) QSKIP("English assets required");
        SwapFixture f; QVERIFY(f.init("we say ", 7, {}, "en_US")); QTRY_VERIFY(f.ic->inputItem() == f.edit.get());
        QVariantList keys, points; const QStringList rows{"qwertyuiop", "asdfghjkl", "zxcvbnm"};
        for (int r = 0; r < rows.size(); ++r) for (int c = 0; c < rows[r].size(); ++c)
            keys << QVariantMap{{"letter", QString(rows[r][c])}, {"x", c * 100}, {"y", r * 100}, {"w", 100}, {"h", 100}};
        QPointF previous;
        for (auto letter : QStringLiteral("hello")) {
            QPointF center;
            for (const auto &v : keys) { auto k = v.toMap(); if (k["letter"].toString() == QString(letter)) center = {k["x"].toDouble() + 50, k["y"].toDouble() + 50}; }
            if (points.isEmpty()) points << QVariantMap{{"x", center.x()}, {"y", center.y()}, {"t", 0}};
            else for (int step = 1; step <= 8; ++step) { auto p = previous + (center - previous) * (step / 8.0); points << QVariantMap{{"x", p.x()}, {"y", p.y()}, {"t", points.size() * 16}}; }
            previous = center;
        }
        const int generation = f.method->beginSwipe(); QVERIFY(generation >= 0);
        f.method->finishSwipe(points, keys, generation);
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); f.method->update();
        const QString typed = f.edit->text.mid(7); QVERIFY(typed.size() >= 2);
        const auto swipeGuesses = f.bar();            // right after the swipe the bar shows that swipe's other guesses
        QVERIFY(!swipeGuesses.isEmpty());
        QTest::qWait(400);                            // the swipe's own edit has settled: a move now is a tap
        f.tapAt(7 + typed.size() / 2);
        const auto shown = f.bar();
        for (const auto &guess : swipeGuesses) QVERIFY2(shown.contains(guess), qPrintable(guess + " not in " + shown.join(",")));
        QVERIFY(!shown.contains(typed));
        QCOMPARE(f.edit->text, QStringLiteral("we say ") + typed);
        f.tapAt(0);                                   // "we": a different word, none of the swipe's guesses
        for (const auto &guess : swipeGuesses) QVERIFY(!f.bar().contains(guess));
    }
    void aCleanWordIsNeverAnsweredWithASwearWordItResembles() {
        // WHY: alternatives are derived from the tapped word, but a swear word must not appear for an innocent one.
        // "<swear>q" is an ordinary mistyped word whose only close neighbour is the swear word, so an unfiltered
        // list would show it. The blocked spellings are assembled in pieces.
        if (qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS").isEmpty()) QSKIP("English assets required");
        SwapFixture f;
        const QStringList blocked{QStringLiteral("f") + "uck", QStringLiteral("sh") + "it", QStringLiteral("di") + "ck"};
        QString text; for (const auto &w : blocked) text += w + "q ";
        QVERIFY(f.init(text, text.size(), {}, "en_US")); QTRY_VERIFY(f.ic->inputItem() == f.edit.get());
        int start = 0;
        for (const auto &w : blocked) {
            f.tapAt(start + 1);
            const auto shown = f.bar();
            qInfo() << "swear-filter check shows" << shown;
            QVERIFY2(!shown.contains(w), qPrintable(w + "q -> " + shown.join(",")));
            start += w.size() + 2;
        }
    }
    void aSwearWordTheyTypedStillGetsItsNeighbours() {
        // WHY: the filter exists so the keyboard never volunteers a swear word; the person's own word is theirs.
        if (qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS").isEmpty()) QSKIP("English assets required");
        SwapFixture f; const QString text = QStringLiteral("so ") + "sh" + "it " + "again ";
        QVERIFY(f.init(text, text.size(), {}, "en_US")); QTRY_VERIFY(f.ic->inputItem() == f.edit.get());
        f.tapAt(4);
        QVERIFY(!f.bar().isEmpty());
    }
    void unbridgedNativeModeNeverOffersAlternativesForTextItDidNotType() {
        // WHY: under owned-history the keyboard may read only words it committed itself, so text already in the
        // field (or typed some other way) never gets alternatives.
        if (qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS").isEmpty()) QSKIP("English assets required");
        SwapFixture f; const QString text = QStringLiteral("we can sea you today ");
        QVERIFY(f.init(text, text.size(), {}, "en_US", nullptr, true)); QTRY_VERIFY(f.ic->inputItem() == f.edit.get());
        // WHY a Shift press first: it connects the keyboard to the field's stock proxy, as real use does within the
        // first key, so this really runs the owned-history policy rather than an unconnected keyboard.
        f.method->keyEvent(Qt::Key_Shift, QString(), Qt::NoModifier);
        QVERIFY(f.method->diagnostics().value("predictionContextSource").toString() == QStringLiteral("keyboard-owned-history"));
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); f.method->update(); // the "before the tap" look
        f.tapAt(9);
        QVERIFY(f.bar().isEmpty());
        f.choose(0); QCOMPARE(f.edit->text, text);
    }
    void unbridgedNativeModeOffersAlternativesForItsOwnSwipedWord() {
        // WHY (Destin, stock Plasma): tapping back into a word the keyboard swiped must offer that swipe's other
        // guesses even though the field is not readable; text that was already there stays untouched and unoffered.
        if (qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS").isEmpty()) QSKIP("English assets required");
        SwapFixture f; QVERIFY(f.init("we say ", 7, {}, "en_US", nullptr, true)); QTRY_VERIFY(f.ic->inputItem() == f.edit.get());
        f.connectNative();
        QCOMPARE(f.method->diagnostics().value("predictionContextSource").toString(), QStringLiteral("keyboard-owned-history"));
        QVERIFY(f.swipe("hello")); f.report();
        const QString typed = f.edit->text.mid(7).trimmed(); QVERIFY(typed.size() >= 2);
        const auto swipeGuesses = f.bar();
        QVERIFY(!swipeGuesses.isEmpty());
        QTest::qWait(400);                            // the swipe's own edit has settled: a move now is a tap
        f.tapAt(7 + typed.size() / 2);                // into the swiped word
        const auto shown = f.bar();
        QVERIFY2(!shown.isEmpty(), "no alternatives for the keyboard's own word");
        for (const auto &guess : swipeGuesses) QVERIFY2(shown.contains(guess), qPrintable(guess + " not in " + shown.join(",")));
        f.tapAt(1);                                   // "we" was already in the field: never offered
        QVERIFY(f.bar().isEmpty());
        f.tapAt(7 + typed.size() / 2);
        const QString pick = f.bar().first();
        f.choose(0);
        QCOMPARE(f.edit->text.left(7), QStringLiteral("we say "));   // nothing before it touched
        QVERIFY2(f.edit->text.mid(7).startsWith(pick), qPrintable(f.edit->text));
        QTest::qWait(400);
        f.tapAt(f.edit->text.size()); f.tapAt(7 + 1);
        QVERIFY2(!f.bar().isEmpty(), "the swapped-in word is the keyboard's own too");
        f.tapAt(f.edit->text.size());
        // Someone else types after it (a separate word, so the word itself is intact): the keyboard no longer
        // vouches for any word in the field.
        QTest::qWait(400);
        f.edit->text += QStringLiteral(" there"); f.edit->cursor = f.edit->text.size(); f.report();
        QTest::qWait(400);
        f.tapAt(7 + 1);
        QVERIFY2(f.bar().isEmpty(), qPrintable("offered after an outside edit: " + f.bar().join(",")));
        f.choose(0); QVERIFY(f.edit->text.endsWith(QStringLiteral(" there")));
    }
    void chromiumStyleReportsStillOfferTheKeyboardsOwnWord() {
        // WHY (Destin: no alternatives in YouCoded and Chrome): Chromium apps talk to the keyboard through KWin
        // (Wayland text-input-v3) and get the owned-history rule on stock Plasma. They report the result of the
        // keyboard's own typing late (first repeating the old state), repeat the same report, and answer a
        // tap with a cursor-only report, possibly twice. None of that may look like someone else's edit.
        if (qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS").isEmpty()) QSKIP("English assets required");
        SwapFixture f; QVERIFY(f.init("ok ", 3, {}, "en_US", nullptr, true)); QTRY_VERIFY(f.ic->inputItem() == f.edit.get());
        f.connectNative();
        const QString before = f.edit->text;
        f.edit->suppressInputMethodUpdate = true;      // the app takes the text but says nothing yet
        QVERIFY(f.swipe("hello"));
        const QString after = f.edit->text; const int end = f.edit->cursor;
        QVERIFY(after.size() > before.size());
        QTest::qWait(120);
        f.edit->lagText = true; f.edit->laggedText = before; f.edit->cursor = before.size();
        f.report();                                                        // first: the old state, again
        QTest::qWait(1500);                                                // late: a busy machine, past 1 second
        f.edit->lagText = false; f.edit->cursor = end; f.report();         // then: the new text and cursor
        QTest::qWait(60);
        f.report();                                                        // the same state again
        f.edit->suppressInputMethodUpdate = false;
        QCOMPARE(f.edit->text, after); QCOMPARE(f.edit->cursor, end);
        QTest::qWait(400);
        const QString word = after.mid(3).trimmed();
        f.tapAt(3 + word.size() / 2); f.report();                          // a tap, reported twice
        QVERIFY2(!f.bar().isEmpty(), qPrintable("no alternatives after Chromium-style reports in: " + after));
        QCOMPARE(f.edit->text, after);
        const QString pick = f.bar().first();
        f.choose(0);
        QCOMPARE(f.edit->text.left(3), QStringLiteral("ok "));
        QVERIFY2(f.edit->text.mid(3).startsWith(pick), qPrintable(f.edit->text));
    }
    void anAppThatSendsNoTextGetsNoAlternativesAndLosesNoText() {
        // WHY: Chromium may leave the surrounding text out. Then the keyboard cannot see its word, so it offers
        // nothing and never deletes blind.
        if (qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS").isEmpty()) QSKIP("English assets required");
        SwapFixture f; QVERIFY(f.init("ok ", 3, {}, "en_US", nullptr, true)); QTRY_VERIFY(f.ic->inputItem() == f.edit.get());
        f.connectNative();
        QVERIFY(f.swipe("hello")); f.report();
        const QString after = f.edit->text;
        QTest::qWait(400);
        f.edit->lagText = true; f.edit->laggedText = QString();
        f.tapAt(0); f.tapAt(5);
        QVERIFY(f.bar().isEmpty());
        f.choose(0);
        QCOMPARE(f.edit->text, after);
    }
    void sentenceAwareRankingUsesTheWordsBeforeTheTappedWord() {
        // WHY: with an n-gram pack the worker reorders the dictionary's neighbours by the words before the tapped word.
        const QString packs = qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS");
        if (packs.isEmpty()) QSKIP("Explicit local language pack fixture not supplied");
        qputenv("SWIPEKB_LANGUAGE_PACKS", packs.toUtf8());
        struct EnvReset { ~EnvReset() { qunsetenv("SWIPEKB_LANGUAGE_PACKS"); } } envReset;
        SwapFixture f; const QString text = QStringLiteral("vivo en una caso ");
        QVERIFY(f.init(text, text.size(), {}, "es_ES")); QTRY_VERIFY(f.ic->inputItem() == f.edit.get());
        f.tapAt(14);                                  // inside "caso"
        const auto dictionaryOrder = f.bar();
        QVERIFY(!dictionaryOrder.isEmpty()); QVERIFY(!dictionaryOrder.contains("caso"));
        QVERIFY(f.ranked.tryAcquire(1, 15000));
        QCOMPARE(f.jobContexts, QStringList({"vivo", "en", "una"}));
        QCOMPARE(f.jobPrefixes.last(), QStringLiteral("c"));
        // WHY no exact words: they come from the real Spanish dictionary, which changes with each word-list
        // release. What must hold is that the sentence changes the order, never the text.
        QTRY_VERIFY_WITH_TIMEOUT(f.bar() != dictionaryOrder, 5000);
        QVERIFY(!f.bar().isEmpty()); QVERIFY(!f.bar().contains("caso"));
        QCOMPARE(f.edit->text, text);
    }
    void spanishCompletionRanksBySentenceBeforeTheWord_data() {
        QTest::addColumn<int>("hints");QTest::addColumn<bool>("private_");
        QTest::newRow("ordinary-field")<<0<<false;
        // Control: a harmless field hint still completes, so the private rows below fail for privacy alone.
        QTest::newRow("lowercase-hint")<<int(Qt::ImhPreferLowercase)<<false;
        // WHY: the sentence-aware completion reads the text before the word; it must never run where the
        // keyboard is told the text is a password or private.
        QTest::newRow("password")<<int(Qt::ImhHiddenText|Qt::ImhSensitiveData)<<true;
        QTest::newRow("private")<<int(Qt::ImhSensitiveData)<<true;
        QTest::newRow("hidden")<<int(Qt::ImhHiddenText)<<true;
    }
    void spanishCompletionRanksBySentenceBeforeTheWord() {
        QFETCH(int,hints);QFETCH(bool,private_);
        // WHY: n-gram languages re-rank completions by the words before the typed letters (card #170). The
        // worker stub stands in for the model; the test pins the plumbing: right letters, right sentence,
        // and the context answer replacing the dictionary's list with the typed letters still offered.
        const QString packs=qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS");
        if(packs.isEmpty())QSKIP("Explicit local language pack fixture not supplied");
        qputenv("SWIPEKB_LANGUAGE_PACKS",packs.toUtf8());
        struct EnvReset{~EnvReset(){qunsetenv("SWIPEKB_LANGUAGE_PACKS");}} envReset;
        QTemporaryDir data;qputenv("SWIPEKB_DATA",data.path().toUtf8());
        QQmlEngine engine;QQmlComponent fixture(&engine);fixture.setData("import QtQuick; import QtQuick.VirtualKeyboard; Item {property var context:InputContext}",QUrl());
        std::unique_ptr<QObject> root(fixture.create());QVERIFY(root);auto *item=qobject_cast<QQuickItem*>(root.get());
        QQuickWindow window;window.resize(500,200);item->setParentItem(window.contentItem());InputListenerItem edit(item);
        edit.text=QStringLiteral("vivo en una ");edit.cursor=edit.text.size();edit.hints|=Qt::InputMethodHints(hints);
        window.show();window.requestActivate();edit.forceActiveFocus();
        auto *ic=qvariant_cast<QVirtualKeyboardInputContext*>(root->property("context"));QVERIFY(ic);
        QStringList prefixes,contexts;QSemaphore completions,anyCompletion;
        SwipeInputMethod method(nullptr,[&](const PredictionJob&job){
            if(job.prefix.isEmpty())return QStringList{"x","y","z"};
            anyCompletion.release();
            // WHY release only for the full "cas" (see englishCompletion): earlier letters ask too.
            if(job.prefix!=QStringLiteral("cas"))return QStringList{};
            prefixes<<job.prefix;contexts=job.context;completions.release();
            return QStringList{"casona","casa"};});
        ic->inputEngine()->setInputMethod(&method);method.setInputMode("es_ES",QVirtualKeyboardInputEngine::InputMode::Latin);method.setPhysicalLayout("qwerty");
        TestPolicyBridge bridge;method.setPredictionPolicySource(&bridge);
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);QTRY_VERIFY(ic->inputItem()==&edit);
        for(auto c:QString("cas")){
            method.keyEvent(Qt::Key(QChar(c).toUpper().unicode()),QString(c),Qt::NoModifier);ic->commit(QString(c));
            QGuiApplication::inputMethod()->update(Qt::ImQueryAll);method.update();
        }
        if(private_) {
            // settle: nothing may reach the model, and no suggestion may show.
            QVERIFY(!anyCompletion.tryAcquire(1,1500));
            QCOMPARE(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList),0);
            ic->inputEngine()->setInputMethod(nullptr);
            return;
        }
        QVERIFY(completions.tryAcquire(1,15000));
        QCOMPARE(contexts,QStringList({"vivo","en","una"}));
        auto shown=[&]{QStringList words;for(int i=0;i<method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList);++i)
            words<<method.selectionListData(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,i,QVirtualKeyboardSelectionListModel::Role::Display).toString();return words;};
        // Best guess in the middle, the typed letters always offered, as with dictionary completion.
        QTRY_COMPARE_WITH_TIMEOUT(shown(),QStringList({"casa","casona","cas"}),15000);
        QCOMPARE(prefixes.last(),QStringLiteral("cas"));
        ic->inputEngine()->setInputMethod(nullptr);
    }
    // WHY one body for both English cases: completion must follow the pack that is installed. With the open
    // English n-gram the typed letters are re-ranked by the sentence (like Spanish); without it nothing is sent to
    // the completion worker and the dictionary completes alone.
private:
    void englishCompletion(bool withNgram) {
        const QString profile=qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS"), packs=qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS");
        if(profile.isEmpty() || !QFileInfo::exists(packs+"/en.ngram"))QSKIP("English assets and an installed English n-gram fixture required");
        QTemporaryDir modelRoot;
        for(const QString &path:{QString("dict/en_swipe.combined")}) {
            QVERIFY(QDir().mkpath(QFileInfo(modelRoot.filePath(path)).absolutePath()));
            QVERIFY2(QFile::copy(QDir(profile).filePath(path),modelRoot.filePath(path)),qPrintable(path));
        }
        if(withNgram) {
            QVERIFY(QDir().mkpath(modelRoot.filePath("language-packs")));
            for(const QString &name:{QString("en.ngram"),QString("en.vocab.tsv"),QString("en-CONTRIBUTORS.txt"),QString("TATOEBA-CC-BY-2.0-FR.txt"),
                QString("ODC-By-1.0.txt"),QString("LANGUAGE-FILES-NOTICE.md"),QString("Apache-2.0.txt")})
                QVERIFY2(QFile::copy(QDir(packs).filePath(name),modelRoot.filePath("language-packs/"+name)),qPrintable(name));
        }
        qputenv("SWIPEKB_MODEL_DATA",modelRoot.path().toUtf8());
        struct EnvReset{~EnvReset(){qunsetenv("SWIPEKB_MODEL_DATA");}} envReset;
        QTemporaryDir data;qputenv("SWIPEKB_DATA",data.path().toUtf8());
        QQmlEngine engine;QQmlComponent fixture(&engine);fixture.setData("import QtQuick; import QtQuick.VirtualKeyboard; Item {property var context:InputContext}",QUrl());
        std::unique_ptr<QObject> root(fixture.create());QVERIFY(root);auto *item=qobject_cast<QQuickItem*>(root.get());
        QQuickWindow window;window.resize(500,200);item->setParentItem(window.contentItem());InputListenerItem edit(item);
        edit.text=QStringLiteral("we live in a ");edit.cursor=edit.text.size();
        window.show();window.requestActivate();edit.forceActiveFocus();
        auto *ic=qvariant_cast<QVirtualKeyboardInputContext*>(root->property("context"));QVERIFY(ic);
        QStringList prefixes,contexts;QSemaphore completions;
        SwipeInputMethod method(nullptr,[&](const PredictionJob&job){
            if(job.prefix.isEmpty())return QStringList{"x","y","z"};
            // WHY release only for the full "hou": earlier letters ("h", "ho") also ask, from the worker thread;
            // waiting for any one of them raced the check below.
            if(job.prefix!=QStringLiteral("hou"))return QStringList{};
            prefixes<<job.prefix;contexts=job.context;completions.release();
            return QStringList{"houses","house"};});
        ic->inputEngine()->setInputMethod(&method);method.setInputMode("en_US",QVirtualKeyboardInputEngine::InputMode::Latin);method.setPhysicalLayout("qwerty");
        QVERIFY2(method.isReady(),qPrintable(method.profileSnapshot().unavailableReason));
        QCOMPARE(method.profileSnapshot().assets.value("contextModel").compatibility,
                 withNgram ? QString("en:context-ngram-v1") : QString());
        TestPolicyBridge bridge;method.setPredictionPolicySource(&bridge);
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);QTRY_VERIFY(ic->inputItem()==&edit);
        for(auto c:QString("hou")){
            method.keyEvent(Qt::Key(QChar(c).toUpper().unicode()),QString(c),Qt::NoModifier);ic->commit(QString(c));
            QGuiApplication::inputMethod()->update(Qt::ImQueryAll);method.update();
        }
        if(withNgram) {
            QVERIFY(completions.tryAcquire(1,15000));
            QCOMPARE(contexts,QStringList({"we","live","in","a"}));
            QCOMPARE(prefixes.last(),QStringLiteral("hou"));
        } else {
            QVERIFY(!completions.tryAcquire(1,1500)); // no n-gram pack: dictionary completion only.
        }
        ic->inputEngine()->setInputMethod(nullptr);
    }
private slots:
    void englishCompletionUsesTheOpenNgramWhenInstalled() { englishCompletion(true); }
    void englishCompletionStaysDictionaryOnlyWithoutTheNgram() { englishCompletion(false); }
    void unbridgedNativeWordUsesOnlyOwnedHistoryAndInserts_data() {
        QTest::addColumn<bool>("swipe");QTest::addColumn<int>("slot");QTest::addColumn<bool>("shiftedFirst");
        for(bool swipe:{false,true}) for(int slot=0;slot<3;++slot) for(bool shiftedFirst:{false,true})
            if (!swipe || !shiftedFirst)
                QTest::newRow(qPrintable(QStringLiteral("%1-slot%2%3").arg(swipe?"swipe":"tap").arg(slot)
                    .arg(shiftedFirst?"-auto-capitalized":"")))<<swipe<<slot<<shiftedFirst;
    }
    void unbridgedNativeWordUsesOnlyOwnedHistoryAndInserts() {
        QFETCH(bool,swipe);QFETCH(int,slot);QFETCH(bool,shiftedFirst);
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
        // WHY ignore completion jobs: when the open English n-gram is installed (as in every real install) each tapped
        // letter also asks for completions; this test is about the next-word request after the space.
        SwipeInputMethod method(nullptr,[&](const PredictionJob &job){if(!job.prefix.isEmpty())return QStringList{};captured=job.context;completed.release();return QStringList{"world","again","today"};});
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
        } else {
            int characterIndex=0;
            for(auto c:QStringLiteral("hello")) {
                const bool shifted=shiftedFirst && characterIndex==0;
                const QString typed=shifted ? QString(c).toUpper() : QString(c);
                const auto modifiers=shifted ? Qt::KeyboardModifiers(Qt::ShiftModifier) : Qt::KeyboardModifiers(Qt::NoModifier);
                QVERIFY(!method.keyEvent(Qt::Key(typed.front().toUpper().unicode()),typed,modifiers));
                ic->commit(typed);QGuiApplication::inputMethod()->update(Qt::ImQueryAll);method.update();
                ++characterIndex;
            }
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
        QTRY_COMPARE_WITH_TIMEOUT(edit.text,QStringLiteral("preexisting sentinel ")+owned+" "+inserted+" ",15000);
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
        qputenv("SWIPEKB_LANGUAGE_PACKS",qgetenv("SWIPEKB_TEST_LANGUAGE_PACKS"));
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
        QTRY_COMPARE_WITH_TIMEOUT(edit->property("text").toString(), QStringLiteral("hello  ") + nextPrediction + " ", 15000);
        QSignalSpy diagnosticsChanged(&method,&SwipeInputMethod::diagnosticsChanged);
        bridge.deny();
        QVERIFY(diagnosticsChanged.size()>0);
        QCOMPARE(method.diagnostics()["predictionStatus"].toString(),QStringLiteral("disabled-for-field"));
        QCOMPARE(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList),0);
        const auto epoch = method.profileSnapshot().epoch;
        QVERIFY(method.setInputMode("fr_FR", QVirtualKeyboardInputEngine::InputMode::Latin));
        QVERIFY(method.profileSnapshot().epoch > epoch);
        QCOMPARE(method.profileSnapshot().capabilities.nextWord,
            QFileInfo::exists(qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS")+"/fr.json"));
        // WHY verified foreign context assets preserve the profile's prediction capability; the denied
        // field policy above remains authoritative and keeps the visible suggestions cleared.
        QCOMPARE(method.profileSnapshot().effectiveLanguage, QStringLiteral("fr"));
        QCOMPARE(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList),0);
        method.setInputMode("en_GB", QVirtualKeyboardInputEngine::InputMode::Latin);
        QCOMPARE(method.profileSnapshot().requestedLocale, QStringLiteral("en-GB"));
        QVERIFY(method.profileSnapshot().capabilities.nextWord);
        const auto beforeLayout = method.profileSnapshot().epoch;
        method.setPhysicalLayout("azerty");
        QVERIFY(method.profileSnapshot().epoch > beforeLayout);
        QVERIFY(method.profileSnapshot().assets.contains("dictionary"));
        ic->inputEngine()->setInputMethod(nullptr);
    }
    void frenchContextModelProducesThreeSelectableQtPredictions() {
        if(qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS").isEmpty() ||
           !QFileInfo::exists(qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS")+"/fr.json"))
            QSKIP("Explicit English runtime and verified French pack fixture required");
        QTemporaryDir data; qputenv("SWIPEKB_DATA",data.path().toUtf8());
        qputenv("SWIPEKB_MODEL_DATA",qgetenv("SWIPEKB_TEST_PROFILE_ASSETS"));
        qputenv("SWIPEKB_LANGUAGE_PACKS",qgetenv("SWIPEKB_TEST_LANGUAGE_PACKS"));
        QQmlEngine engine; QQmlComponent fixture(&engine);
        fixture.setData(R"(
            import QtQuick
            import QtQuick.VirtualKeyboard
            Item { width:500; height:200; property var context:InputContext
                TextInput { objectName:"editor"; width:400; height:100; focus:true; text:"je suis"; inputMethodHints:Qt.ImhNoPredictiveText }
            }
        )",QUrl());
        std::unique_ptr<QObject> root(fixture.create()); QVERIFY2(root,qPrintable(fixture.errorString()));
        auto *item=qobject_cast<QQuickItem *>(root.get()); QQuickWindow window; window.resize(500,200);
        item->setParentItem(window.contentItem()); window.show(); window.requestActivate();
        auto *edit=root->findChild<QQuickItem *>("editor"); QVERIFY(edit); edit->forceActiveFocus();
        auto *ic=qvariant_cast<QVirtualKeyboardInputContext *>(root->property("context")); QVERIFY(ic);
        SwipeInputMethod method; ic->inputEngine()->setInputMethod(&method);
        method.setPhysicalLayout("azerty"); QVERIFY(method.setInputMode("fr_FR",QVirtualKeyboardInputEngine::InputMode::Latin));
        QVERIFY2(method.isReady(),qPrintable(method.profileSnapshot().unavailableReason));
        QVERIFY(method.profileSnapshot().capabilities.nextWord);
        TestPolicyBridge bridge; method.setPredictionPolicySource(&bridge);
        edit->setProperty("cursorPosition",7); QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
        QTRY_VERIFY_WITH_TIMEOUT(ic->inputItem()==edit,15000);
        QTRY_COMPARE_WITH_TIMEOUT(ic->surroundingText(),QStringLiteral("je suis"),15000);
        QVERIFY(!method.keyEvent(Qt::Key_Space," ",Qt::NoModifier)); ic->sendKeyClick(Qt::Key_Space," ");
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); method.update();
        QTRY_COMPARE_WITH_TIMEOUT(method.diagnostics()["predictionModelState"].toString(),QStringLiteral("loaded"),15000);
        QCOMPARE(method.diagnostics()["predictionStatus"].toString(),QStringLiteral("ready"));
        QCOMPARE(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList),3);
        QStringList candidates;
        for(int i=0;i<3;++i) candidates<<method.selectionListData(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,i,
            QVirtualKeyboardSelectionListModel::Role::Display).toString();
        QVERIFY(!candidates[0].isEmpty() && !candidates[1].isEmpty() && !candidates[2].isEmpty());
        const auto accepted=candidates[1];
        method.beginPredictionSelection(method.predictionSelectionToken());
        method.selectionListItemSelected(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,1);
        QTRY_VERIFY_WITH_TIMEOUT(edit->property("text").toString().contains(accepted),15000);
        QVERIFY(edit->property("text").toString().endsWith(accepted+" "));
        ic->inputEngine()->setInputMethod(nullptr);
    }
    void languageSwitchesReplaceContextModelIdentityWithoutStaleCandidates() {
        const QString english=qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS");
        const QString packs=qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS");
        if(english.isEmpty() || !QFileInfo::exists(packs+"/es.json")) QSKIP("Pinned English and multilingual model fixtures required");
        qputenv("SWIPEKB_MODEL_DATA",english.toUtf8()); qputenv("SWIPEKB_LANGUAGE_PACKS",packs.toUtf8());
        SwipeInputMethod method; quint64 epoch=0; QString spanishHash;
        for(const auto &entry:QList<QPair<QString,QString>>{{QStringLiteral("es_ES"),QStringLiteral("qwerty")},
                {QStringLiteral("es_MX"),QStringLiteral("qwerty")},{QStringLiteral("fr_FR"),QStringLiteral("azerty")},
                {QStringLiteral("de_DE"),QStringLiteral("qwertz")},{QStringLiteral("en_US"),QStringLiteral("qwerty")}}) {
            method.setPhysicalLayout(entry.second);
            QVERIFY(method.setInputMode(entry.first,QVirtualKeyboardInputEngine::InputMode::Latin));
            const auto profile=method.profileSnapshot();
            QVERIFY(profile.epoch>epoch); epoch=profile.epoch;
            QVERIFY2(method.isReady(),qPrintable(profile.unavailableReason));
            QVERIFY(profile.capabilities.nextWord);
            QVERIFY(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList)==0);
            if(entry.first=="es_ES") spanishHash=profile.assets["contextModel"].sha256;
            if(entry.first=="es_MX") QCOMPARE(profile.assets["contextModel"].sha256,spanishHash);
            if(entry.first=="fr_FR" || entry.first=="de_DE") QVERIFY(profile.assets["contextModel"].sha256!=spanishHash);
            if(entry.first=="en_US") QVERIFY(profile.assets["contextModel"].sha256!=profile.assets["contextVocab"].sha256);
        }
    }
    void frenchContextModelUsesOnlyKeyboardOwnedNativeHistory() {
        if(qEnvironmentVariable("SWIPEKB_TEST_PROFILE_ASSETS").isEmpty() ||
           !QFileInfo::exists(qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS")+"/fr.json"))
            QSKIP("Explicit English runtime and verified French pack fixture required");
        QTemporaryDir data; qputenv("SWIPEKB_DATA",data.path().toUtf8());
        qputenv("SWIPEKB_MODEL_DATA",qgetenv("SWIPEKB_TEST_PROFILE_ASSETS"));
        qputenv("SWIPEKB_LANGUAGE_PACKS",qgetenv("SWIPEKB_TEST_LANGUAGE_PACKS"));
        qputenv("SWIPEKB_NATIVE_PREDICTION","owned-history-v1");
        struct EnvReset{~EnvReset(){qunsetenv("SWIPEKB_NATIVE_PREDICTION");}} envReset;
        QQmlEngine engine; QQmlComponent fixture(&engine);
        fixture.setData("import QtQuick; import QtQuick.VirtualKeyboard; Item {property var context:InputContext}",QUrl());
        std::unique_ptr<QObject> root(fixture.create()); QVERIFY2(root,qPrintable(fixture.errorString()));
        auto *item=qobject_cast<QQuickItem *>(root.get()); QQuickWindow window; window.resize(500,200);
        item->setParentItem(window.contentItem()); InputListenerItem edit(item); window.show(); window.requestActivate(); edit.forceActiveFocus();
        auto *ic=qvariant_cast<QVirtualKeyboardInputContext *>(root->property("context")); QVERIFY(ic);
        SwipeInputMethod method; ic->inputEngine()->setInputMethod(&method);
        method.setPhysicalLayout("azerty"); QVERIFY(method.setInputMode("fr_FR",QVirtualKeyboardInputEngine::InputMode::Latin));
        QVERIFY(method.profileSnapshot().capabilities.nextWord);
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); QTRY_VERIFY_WITH_TIMEOUT(ic->inputItem()==&edit,15000);
        for(const auto c:QStringLiteral("je suis")) {
            QVERIFY(!method.keyEvent(Qt::Key(c.toUpper().unicode()),QString(c),Qt::NoModifier));
            ic->commit(QString(c)); QGuiApplication::inputMethod()->update(Qt::ImQueryAll); method.update();
        }
        QVERIFY(!method.keyEvent(Qt::Key_Space," ",Qt::NoModifier)); ic->commit(" ");
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); method.update();
        QTRY_COMPARE_WITH_TIMEOUT(method.diagnostics()["predictionModelState"].toString(),QStringLiteral("loaded"),15000);
        QCOMPARE(method.diagnostics()["predictionContextSource"].toString(),QStringLiteral("keyboard-owned-history"));
        QCOMPARE(method.diagnostics()["activeFieldPolicy"].toString(),QStringLiteral("unknown"));
        QCOMPARE(method.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList),3);
        const auto accepted=method.selectionListData(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,1,
            QVirtualKeyboardSelectionListModel::Role::Display).toString(); QVERIFY(!accepted.isEmpty());
        method.beginPredictionSelection(method.predictionSelectionToken());
        method.selectionListItemSelected(QVirtualKeyboardSelectionListModel::Type::WordCandidateList,1);
        QTRY_VERIFY_WITH_TIMEOUT(edit.text.contains(accepted),15000);
        QVERIFY(edit.text.endsWith(accepted+" "));
        ic->inputEngine()->setInputMethod(nullptr);
    }
};
QTEST_MAIN(NextWordIntegrationTest)
#include "tst_nextwordintegration.moc"
