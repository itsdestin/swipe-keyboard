// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QtTest>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <memory>
// WHY real signal names: a passive geometry fixture should not hide QML connection diagnostics.
class TestTouchArea : public QQuickItem {
    Q_OBJECT
signals:
    void pressed(const QVariant &touchPoints);
    void updated(const QVariant &touchPoints);
    void released(const QVariant &touchPoints);
    void canceled(const QVariant &touchPoints);
};
class LanguageCaptureTest : public QObject {
    Q_OBJECT
private slots:
    void identifiesMeasuredLayoutsIndependentlyOfLocale() {
        QQmlEngine engine;
        QQuickItem layout;
        TestTouchArea touch;
        QQmlComponent key(&engine);
        key.setData("import QtQuick; Item { property int key: Qt.Key_A; property string text; width: 10; height: 10 }",QUrl());
        QVERIFY2(key.isReady(),qPrintable(key.errorString()));
        QList<QQuickItem *> keys;
        const QStringList rows{"qwertyuiop","asdfghjkl","zxcvbnm"};
        for (int r=0;r<rows.size();++r) for (int i=0;i<rows[r].size();++i) {
            auto *item=qobject_cast<QQuickItem *>(key.create()); QVERIFY(item);
            item->setParent(&layout); item->setParentItem(&layout);
            item->setProperty("text",QString(rows[r][i])); item->setX(i*10); item->setY(r*10); keys.append(item);
        }
        QQmlComponent capture(&engine,QUrl::fromLocalFile(QStringLiteral(SWIPEKB_CAPTURE_QML)));
        QVERIFY2(capture.isReady(),qPrintable(capture.errorString()));
        std::unique_ptr<QObject> object(capture.createWithInitialProperties({
            {"layoutItem",QVariant::fromValue(&layout)},{"touchArea",QVariant::fromValue(&touch)}}));
        QVERIFY2(object,qPrintable(capture.errorString()));
        QCOMPARE(object->property("measuredLayoutId").toString(),QString("qwerty"));
        auto find=[&](const QString &text) {
            for (auto *item:keys) if(item->property("text").toString()==text) return item;
            return static_cast<QQuickItem *>(nullptr);
        };
        // WHY mutate only positions: language must follow physical geometry, not force an English layout.
        auto *y=find("y"), *z=find("z"); const QPointF yp=y->position();
        y->setPosition(z->position()); z->setPosition(yp);
        QCOMPARE(object->property("measuredLayoutId").toString(),QString("qwertz"));
        // A shuffled nonstandard layout cannot claim decoder-compatible QWERTY merely from its first row.
        auto *b=find("b"), *n=find("n"); const QPointF bp=b->position();
        b->setPosition(n->position()); n->setPosition(bp);
        QCOMPARE(object->property("measuredLayoutId").toString(),QString("unknown"));
    }
};
QTEST_MAIN(LanguageCaptureTest)
#include "tst_languagecapture.moc"
