#include <QtTest>
#include "backendpredictionpolicy.h"
class BackendPredictionPolicyTest : public QObject {
    Q_OBJECT
private slots:
    void permissionRequiresCurrentContentType() {
        BackendPredictionPolicy p;
        QVERIFY(!p.snapshot().value("completionAllowed").toBool());
        p.contextChanged(true);
        const auto first = p.snapshot();
        QVERIFY(!first.value("contentTypeReceived").toBool());
        p.contentType(1,0);
        QVERIFY(p.snapshot().value("completionAllowed").toBool());
        QCOMPARE(p.snapshot().value("purpose").toInt(),0);
        p.contextChanged(false);
        QCOMPARE(p.snapshot().value("contextId").toULongLong(),quint64(0));
        QVERIFY(!p.snapshot().value("completionAllowed").toBool());
        p.contextChanged(true);
        QVERIFY(p.snapshot().value("contextId").toULongLong() != first.value("contextId").toULongLong());
        QVERIFY(!p.snapshot().value("contentTypeReceived").toBool());
    }
    void restrictionsAndPurposeOnlyChangesAreObservable() {
        BackendPredictionPolicy p; p.contextChanged(true); p.contentType(1,0);
        const auto revision = p.snapshot().value("revision").toULongLong();
        p.contentType(1,7);
        QVERIFY(p.snapshot().value("revision").toULongLong() > revision);
        QCOMPARE(p.snapshot().value("purpose").toInt(),7);
        p.contentType(0,0); QVERIFY(!p.snapshot().value("completionAllowed").toBool());
        for (quint32 hint : {quint32(0x41),quint32(0x81),quint32(0xc1)}) {
            p.contentType(hint,0); QVERIFY(!p.snapshot().value("completionAllowed").toBool());
        }
        p.contentType(1,0); const auto before = p.snapshot().value("revision").toULongLong();
        p.reset(); QVERIFY(p.snapshot().value("revision").toULongLong() > before);
    }
};
QTEST_GUILESS_MAIN(BackendPredictionPolicyTest)
#include "tst_backendpredictionpolicy.moc"
