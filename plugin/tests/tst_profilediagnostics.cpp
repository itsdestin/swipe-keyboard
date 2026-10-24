// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QtTest>
#include "profilediagnostics.h"
class DiagnosticsTest : public QObject {
    Q_OBJECT
private slots:
    void whitelistOnly() {
        swipekb::ProfileSnapshot p;
        p.requestedLocale="fr-FR"; p.effectiveLanguage="fr"; p.layoutId="azerty"; p.inputMode="Latin";
        p.packId="fineweb-fr"; p.packVersion="4.0.0-test"; p.capabilities.swipe=true;
        p.assets["dictionary"]={"/private/words", "secret", "context"};
        p.unavailableReason="/private/editor typed words";
        const auto d=swipekb::profileDiagnostics(p);
        QCOMPARE(d.keys(),QStringList({"contextRankingCapable","inputMode","language","layout","locale","nextWordCapable","packId","packVersion","swipeCapable","unavailableReason"}));
        QCOMPARE(d["unavailableReason"].toString(),QString("Unavailable reason not recognized"));
        QVERIFY(!QJsonDocument::fromVariant(d).toJson().contains("private"));
        p.packId="/private/pack"; p.requestedLocale="/private/locale";
        QCOMPARE(swipekb::profileDiagnostics(p)["packId"].toString(),QString());
        QCOMPARE(swipekb::profileDiagnostics(p)["locale"].toString(),QString());
        p.packId="fineweb-fr\n";
        QCOMPARE(swipekb::profileDiagnostics(p)["packId"].toString(),QString());
    }
    void missingAndUnsupported() {
        QTemporaryDir dir;
        for(const auto &locale:{"es-ES","fr-FR","de-DE","xx-XX"}) {
            auto p=swipekb::resolveProfile(locale,"qwerty","Latin",dir.path());
            auto d=swipekb::profileDiagnostics(p);
            QVERIFY(!d["swipeCapable"].toBool());
            QVERIFY(!d["unavailableReason"].toString().isEmpty());
            QVERIFY(d["unavailableReason"]!="Unavailable reason not recognized");
        }
    }
};
QTEST_GUILESS_MAIN(DiagnosticsTest)
#include "tst_profilediagnostics.moc"
