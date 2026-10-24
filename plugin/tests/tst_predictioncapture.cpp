// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QtTest>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QPalette>
#include <QtVirtualKeyboard/QVirtualKeyboardAbstractInputMethod>
#include <QtVirtualKeyboard/QVirtualKeyboardInputContext>

class CaptureMethod : public QVirtualKeyboardAbstractInputMethod {
    Q_OBJECT
public:
    QString token = "1:2:3", captured;
    Q_INVOKABLE QString predictionSelectionToken() const { return token; }
    quint64 press = 0;
    bool selectionAuthorized = false;
    Q_INVOKABLE quint64 beginPredictionSelection(const QString &value) { captured = value; selectionAuthorized = true; emit capturedChanged(); return ++press; }
    Q_INVOKABLE void endPredictionSelection(quint64 id) { if (id == press) selectionAuthorized = false; }
    QList<QVirtualKeyboardInputEngine::InputMode> inputModes(const QString &) override { return {QVirtualKeyboardInputEngine::InputMode::Latin}; }
    bool setInputMode(const QString &, QVirtualKeyboardInputEngine::InputMode) override { return true; }
    bool setTextCase(QVirtualKeyboardInputEngine::TextCase) override { return true; }
    bool keyEvent(Qt::Key,const QString &,Qt::KeyboardModifiers) override { return false; }
signals:
    void capturedChanged();
};
class PredictionCaptureTest : public QObject {
    Q_OBJECT
private slots:
    void threeEqualSlotsHaveOnlyInternalDividersAndCorrectSelection_data() {
        QTest::addColumn<QColor>("foreground");
        QTest::addColumn<QColor>("background");
        QTest::addColumn<QColor>("separator");
        QTest::newRow("light-keyboard-dark-desktop") << QColor("#232629") << QColor("#eff0f1") << QColor("#a0a0a0");
        QTest::newRow("dark-keyboard-light-desktop") << QColor("#eff0f1") << QColor("#232629") << QColor("#707070");
    }
    void threeEqualSlotsHaveOnlyInternalDividersAndCorrectSelection() {
        QFETCH(QColor,foreground);QFETCH(QColor,background);QFETCH(QColor,separator);
        // WHY independent palettes: Plasma can show a light keyboard on a dark desktop.
        // Desktop text then equals the keyboard background, making selectable words invisible.
        const auto originalPalette=QGuiApplication::palette();
        const auto restorePalette=qScopeGuard([&] {QGuiApplication::setPalette(originalPalette);});
        auto desktopPalette=originalPalette;desktopPalette.setColor(QPalette::Text,background);
        QGuiApplication::setPalette(desktopPalette);
        QQmlEngine engine;QQmlComponent fixture(&engine);
        fixture.setData(R"(
            import QtQuick
            import QtQuick.VirtualKeyboard
            Item { width:600; height:150
                property QtObject style: QtObject {
                    property QtObject theme: QtObject {
                        id: keyboardTheme
                        property color selectionListTextColor: "black"
                        property color selectionListBackgroundColor: "white"
                        property color selectionListSeparatorColor: "gray"
                    }
                }
                property alias theme: keyboardTheme
                Rectangle { width:600; height:50; color:keyboardTheme.selectionListBackgroundColor }
                property var alternativeKeys: ({active:false})
                property var context: InputContext
                property alias bar: bar
                property alias touch: touch
                property int selected: -1
                property int clicked: 0
                ListModel { id: words
                    function selectItem(i) { bar.parent.selected=i; ++bar.parent.clicked }
                    ListElement { display:"second" }
                    ListElement { display:"best" }
                    ListElement { display:"third" }
                }
                ListView { id: bar; objectName:"wordCandidateView"; width:600; height:50
                    orientation:ListView.Horizontal; model:words
                    delegate:Item { width:100; height:50 }
                }
                Item { id:touch; y:60; width:600; height:90
                    signal pressed(var points); signal updated(var points); signal released(var points); signal canceled(var points)
                    function reset() {} }
            }
        )",QUrl());
        std::unique_ptr<QObject> root(fixture.create());QVERIFY2(root,qPrintable(fixture.errorString()));
        auto *theme=qvariant_cast<QObject*>(root->property("theme"));QVERIFY(theme);
        QVERIFY(theme->setProperty("selectionListTextColor",foreground));
        QVERIFY(theme->setProperty("selectionListBackgroundColor",background));
        QVERIFY(theme->setProperty("selectionListSeparatorColor",separator));
        auto *item=qobject_cast<QQuickItem*>(root.get());QQuickWindow window;window.resize(600,150);item->setParentItem(window.contentItem());
        window.show();window.requestActivate();
        auto *ic=qvariant_cast<QVirtualKeyboardInputContext*>(root->property("context"));QVERIFY(ic);
        CaptureMethod method;ic->inputEngine()->setInputMethod(&method);
        auto *bar=qvariant_cast<QQuickItem*>(root->property("bar"));auto *touch=qvariant_cast<QQuickItem*>(root->property("touch"));
        QQmlComponent capture(&engine,QUrl::fromLocalFile(QStringLiteral(SWIPEKB_CAPTURE_SOURCE)));
        std::unique_ptr<QObject> layer(capture.createWithInitialProperties({{"layoutItem",QVariant::fromValue(touch)},{"touchArea",QVariant::fromValue(touch)}}));
        QVERIFY2(layer,qPrintable(capture.errorString()));
        QVERIFY(QMetaObject::invokeMethod(bar,"forceLayout"));
        auto at=[bar](int index) {QQuickItem *out=nullptr;QMetaObject::invokeMethod(bar,"itemAtIndex",Q_RETURN_ARG(QQuickItem*,out),Q_ARG(int,index));return out;};
        QTRY_VERIFY_WITH_TIMEOUT(at(2) && at(2)->objectName()==QStringLiteral("swipeCandidate2"),15000);
        QTRY_VERIFY_WITH_TIMEOUT(layer->property("predictionTapCapture").value<QObject*>(),15000);
        QSignalSpy captured(&method,&CaptureMethod::capturedChanged);
        const QStringList displayed{"second","best","third"};
        for(int slot=0;slot<3;++slot) {
            auto *delegate=at(slot);QVERIFY(delegate);
            QCOMPARE(delegate->property("display").toString(),displayed[slot]);QCOMPARE(delegate->width(),200.0);
            auto *divider=delegate->findChild<QQuickItem*>("swipeCandidateDivider");QVERIFY(divider);
            QCOMPARE(divider->isVisible(),slot>0);
            QQuickItem *label=nullptr;
            for(auto *child:delegate->childItems())
                if(child->property("text").toString()==displayed[slot])label=child;
            QVERIFY(label);
            QCOMPARE(label->property("color").value<QColor>(),foreground);
            QCOMPARE(divider->property("color").value<QColor>(),separator);
            const auto center=delegate->mapToScene(QPointF(100,25)).toPoint();QCOMPARE(center.x(),100+slot*200);
            QTest::mouseClick(&window,Qt::LeftButton,Qt::NoModifier,center);
            QTRY_COMPARE_WITH_TIMEOUT(root->property("selected").toInt(),slot,15000);
            QTRY_COMPARE_WITH_TIMEOUT(captured.size(),slot+1,15000);
            QCOMPARE(method.captured,QStringLiteral("1:2:3"));
            QTRY_VERIFY_WITH_TIMEOUT(!method.selectionAuthorized,15000);
        }
        QCOMPARE(root->property("clicked").toInt(),3);
        const auto shot=qEnvironmentVariable("SWIPEKB_PREDICTION_BAR_SHOT");
        if(!shot.isEmpty()) {QTest::qWait(100);QVERIFY(window.grabWindow().save(shot));}
        ic->inputEngine()->setInputMethod(nullptr);
    }
    void passivePressCapturesStampWithoutSwallowingStockClick() {
        QQmlEngine engine;
        QQmlComponent fixture(&engine);
        fixture.setData(R"(
            import QtQuick
            import QtQuick.VirtualKeyboard
            Item {
                width: 500; height: 200
                property var alternativeKeys: ({active:false})
                property var context: InputContext
                property alias bar: bar
                property alias touch: touch
                property int clicked: 0
                ListView {
                    id: bar; objectName: "wordCandidateView"; width: 500; height: 40
                    orientation: ListView.Horizontal; model: 1
                    delegate: Item {
                        width: 200; height: 40
                        MouseArea { objectName: "candidateMouse"; anchors.fill: parent; onClicked: ++bar.parent.clicked }
                    }
                }
                Item { id: touch; y: 50; width: 500; height: 150
                    signal pressed(var points)
                    signal updated(var points)
                    signal released(var points)
                    signal canceled(var points)
                    function reset() {} }
            }
        )", QUrl());
        std::unique_ptr<QObject> root(fixture.create()); QVERIFY2(root,qPrintable(fixture.errorString()));
        QQuickWindow window; window.resize(500,200);
        auto *item = qobject_cast<QQuickItem *>(root.get()); item->setParentItem(window.contentItem());
        window.show(); window.requestActivate();
        auto *ic = qvariant_cast<QVirtualKeyboardInputContext *>(root->property("context")); QVERIFY(ic);
        CaptureMethod method; ic->inputEngine()->setInputMethod(&method);
        auto *bar = qvariant_cast<QQuickItem *>(root->property("bar"));
        auto *touch = qvariant_cast<QQuickItem *>(root->property("touch")); QVERIFY(bar && touch);
        QQmlComponent capture(&engine,QUrl::fromLocalFile(QStringLiteral(SWIPEKB_CAPTURE_SOURCE)));
        std::unique_ptr<QObject> layer(capture.createWithInitialProperties({
            {"layoutItem",QVariant::fromValue(touch)}, {"touchArea",QVariant::fromValue(touch)}}));
        QVERIFY2(layer,qPrintable(capture.errorString()));
        QVERIFY(QMetaObject::invokeMethod(bar,"forceLayout"));
        QTRY_VERIFY_WITH_TIMEOUT(layer->property("predictionTapCapture").value<QObject *>(),15000);
        QQuickItem *delegate = nullptr;
        QVERIFY(QMetaObject::invokeMethod(bar,"itemAtIndex",Q_RETURN_ARG(QQuickItem *,delegate),Q_ARG(int,0)));
        QVERIFY(delegate);
        const auto tapPoint = delegate->mapToScene(QPointF(delegate->width()/2,delegate->height()/2)).toPoint();
        qInfo() << "candidate position" << tapPoint << "bar" << bar->mapToScene(QPointF()) << "delegate size" << delegate->size();
        QSignalSpy pressed(&method,&CaptureMethod::capturedChanged);
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,tapPoint);
        QTRY_COMPARE_WITH_TIMEOUT(pressed.size(),1,15000);
        QCOMPARE(method.captured,QStringLiteral("1:2:3"));
        method.token = "1:2:4";
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,tapPoint);
        QTRY_COMPARE_WITH_TIMEOUT(root->property("clicked").toInt(),1,15000);
        QCOMPARE(method.captured,QStringLiteral("1:2:3"));
        // WHY distinct device events: synthesized mouse input alone does not pin
        // the keyboard's touchscreen press-before-release authority path.
        auto *device = QTest::createTouchDevice();
        method.token = "2:3:4";
        QTest::touchEvent(&window,device).press(0,tapPoint,&window);
        QTRY_COMPARE_WITH_TIMEOUT(pressed.size(),2,15000);
        QCOMPARE(method.captured,QStringLiteral("2:3:4"));
        method.token = "2:3:5";
        QTest::touchEvent(&window,device).release(0,tapPoint,&window);
        QTRY_COMPARE_WITH_TIMEOUT(root->property("clicked").toInt(),2,15000);
        QCOMPARE(method.captured,QStringLiteral("2:3:4"));
        QTRY_VERIFY_WITH_TIMEOUT(!method.selectionAuthorized,15000);
        method.token = "3:4:5";
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,tapPoint);
        QTRY_VERIFY_WITH_TIMEOUT(method.selectionAuthorized,15000);
        QTest::mouseMove(&window,QPoint(490,190));
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,QPoint(490,190));
        QTRY_VERIFY_WITH_TIMEOUT(!method.selectionAuthorized,15000);
        QCOMPARE(root->property("clicked").toInt(),2);
        QPointer<QObject> handler = layer->property("predictionTapCapture").value<QObject *>();
        // WHY real candidate delegates can disappear before their capture layer during layout
        // switches. Stale JS wrapper entries must not call destroy() on a deleted handler.
        bar->setProperty("model", 0);
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QTest::failOnWarning(QRegularExpression(".*Property 'destroy'.*"));
        layer.reset(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QVERIFY(handler.isNull());
        ic->inputEngine()->setInputMethod(nullptr);
    }
};
QTEST_MAIN(PredictionCaptureTest)
#include "tst_predictioncapture.moc"
