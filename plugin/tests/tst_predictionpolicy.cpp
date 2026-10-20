#include <QtTest>
#include "predictionpolicy.h"
class PolicyObject : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap swipePredictionPolicy MEMBER data NOTIFY swipePredictionPolicyChanged)
public:
    QVariantMap data;
signals:
    void swipePredictionPolicyChanged();
};
class PredictionPolicyTest : public QObject {
    Q_OBJECT
private slots:
    void absentOrMalformedFailsClosed() {
        QVERIFY(!readPredictionPolicy(nullptr).completionAllowed);
        QObject object; QVERIFY(!readPredictionPolicy(&object).completionAllowed);
        PolicyObject bridge;
        bridge.data = {{"version",1},{"contextId",quint64(4)},{"revision",quint64(7)},
            {"contentTypeReceived",true},{"purpose",0},{"completionAllowed",true},{"syntheticNoPredictiveText",true}};
        auto p = readPredictionPolicy(&bridge); QCOMPARE(p.version,1); QVERIFY(p.completionAllowed);
        bridge.data["completionAllowed"] = QStringLiteral("false");
        QVERIFY(!readPredictionPolicy(&bridge).completionAllowed);
        bridge.data.remove("purpose"); QVERIFY(!readPredictionPolicy(&bridge).completionAllowed);
    }
};
QTEST_GUILESS_MAIN(PredictionPolicyTest)
#include "tst_predictionpolicy.moc"
