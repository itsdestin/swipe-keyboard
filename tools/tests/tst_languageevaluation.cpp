#include <QtTest>
#include "languageevaluation.h"
class EvaluationTest : public QObject {
    Q_OBJECT
private slots:
    void countsMissesAndWrongDictionaryWithoutDroppingCases() {
        const QSet<QString> dictionary{"si","sí","casa"};
        const auto hit=swipekb::scoreLanguageCase("sí",{"si","sí"},dictionary,"es");
        QVERIFY(!hit["top1"].toBool()); QVERIFY(hit["top3"].toBool());
        QVERIFY(hit["targetInDictionary"].toBool());
        const auto miss=swipekb::scoreLanguageCase("inventado",{"english"},dictionary,"es");
        QVERIFY(!miss["top3"].toBool()); QVERIFY(!miss["targetInDictionary"].toBool());
        QCOMPARE(miss["wrongDictionaryOutputs"].toInt(),1);
        auto unavailable=QVariantMap{{"status","unavailable"}};
        auto invalid=QVariantMap{{"status","invalid"}};
        const auto totals=swipekb::summarizeLanguageCases({hit,miss,unavailable,invalid});
        QCOMPARE(totals["total"].toInt(),4); QCOMPARE(totals["evaluated"].toInt(),2);
        QCOMPARE(totals["top1"].toInt(),0); QCOMPARE(totals["top3"].toInt(),1);
        QCOMPARE(totals["unavailable"].toInt(),1); QCOMPARE(totals["invalid"].toInt(),1);
        QCOMPARE(totals["targetInDictionary"].toInt(),1);
    }
    void caseAndDiacriticsRemainVisible() {
        const auto score=swipekb::scoreLanguageCase("Straße",{"straße"},{"straße"},"de");
        QVERIFY(!score["top1"].toBool()); QVERIFY(score["lookupTop1"].toBool());
        QVERIFY(!swipekb::scoreLanguageCase("sí",{"si"},{"si","sí"},"es")["lookupTop1"].toBool());
        for(const auto &pair:QList<QPair<QString,QString>>{{"niño","nino"},{"schön","schon"},{"straße","strasse"}})
            QVERIFY(!swipekb::scoreLanguageCase(pair.first,{pair.second},{pair.first,pair.second},"de")["lookupTop1"].toBool());
        const auto empty=swipekb::scoreLanguageCase("casa",{}, {"casa"},"es");
        QVERIFY(!empty["top1"].toBool()); QVERIFY(!empty["top3"].toBool());
        QCOMPARE(swipekb::summarizeLanguageCases({empty})["total"].toInt(),1);
    }
};
QTEST_GUILESS_MAIN(EvaluationTest)
#include "tst_languageevaluation.moc"
