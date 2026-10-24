// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QtTest>
#include <QJsonDocument>
#include "predictiondiagnostics.h"

class PredictionDiagnosticsTest : public QObject {
    Q_OBJECT
    static swipekb::ProfileSnapshot profile() {
        swipekb::ProfileSnapshot p; p.effectiveLanguage="en";
        p.capabilities={true,true,true};
        p.assets["contextModel"]={"/private/model/path","private hash","en"};
        return p;
    }
private slots:
    void distinguishesCapabilityFromActivePermission() {
        auto p=profile(); swipekb::PredictionDiagnosticContext state;
        auto d=swipekb::predictionDiagnostics(p,state);
        QVERIFY(d["predictionCapability"].toBool());
        QCOMPARE(d["activeFieldPolicy"].toString(),QStringLiteral("unknown"));
        QCOMPARE(d["predictionStatus"].toString(),QStringLiteral("no-active-context"));
        state.hasContext=true;
        d=swipekb::predictionDiagnostics(p,state);
        QCOMPARE(d["predictionStatus"].toString(),QStringLiteral("backend-policy-unavailable"));
        state.hasBridge=true;
        d=swipekb::predictionDiagnostics(p,state);
        QCOMPARE(d["activeFieldPolicy"].toString(),QStringLiteral("unknown"));
        QCOMPARE(d["predictionStatus"].toString(),QStringLiteral("content-policy-unavailable"));
        state.policyKnown=true;
        d=swipekb::predictionDiagnostics(p,state);
        QCOMPARE(d["activeFieldPolicy"].toString(),QStringLiteral("restricted"));
        QCOMPARE(d["predictionStatus"].toString(),QStringLiteral("disabled-for-field"));
        state.fieldAllowed=true;
        d=swipekb::predictionDiagnostics(p,state);
        QCOMPARE(d["activeFieldPolicy"].toString(),QStringLiteral("allowed"));
        QCOMPARE(d["predictionStatus"].toString(),QStringLiteral("available-after-confirmation"));
    }
    void reportsActualModelLifecycle() {
        const auto p=profile(); swipekb::PredictionDiagnosticContext state;
        state.hasContext=state.hasBridge=state.policyKnown=state.fieldAllowed=true;
        state.model=swipekb::PredictionModelState::Loading;
        auto d=swipekb::predictionDiagnostics(p,state);
        QCOMPARE(d["predictionModelState"].toString(),QStringLiteral("loading"));
        QCOMPARE(d["predictionStatus"].toString(),QStringLiteral("loading"));
        state.model=swipekb::PredictionModelState::Loaded;
        d=swipekb::predictionDiagnostics(p,state);
        QCOMPARE(d["predictionStatus"].toString(),QStringLiteral("ready"));
        state.model=swipekb::PredictionModelState::Unavailable;
        d=swipekb::predictionDiagnostics(p,state);
        QCOMPARE(d["predictionStatus"].toString(),QStringLiteral("model-unavailable"));
    }
    void nativeHistoryDoesNotInventRawPermission() {
        auto p=profile();swipekb::PredictionDiagnosticContext state;
        state.hasContext=state.fieldAllowed=state.ownedHistory=true;
        auto d=swipekb::predictionDiagnostics(p,state);
        QCOMPARE(d["predictionContextSource"].toString(),QStringLiteral("keyboard-owned-history"));
        QCOMPARE(d["predictionStatus"].toString(),QStringLiteral("available-after-confirmation"));
        QCOMPARE(d["activeFieldPolicy"].toString(),QStringLiteral("unknown"));
        state.fieldAllowed=false;
        QCOMPARE(swipekb::predictionDiagnostics(p,state)["predictionStatus"].toString(),QStringLiteral("disabled-for-field"));
    }
    void failsClosedAndContainsOnlyWhitelistedValues() {
        auto p=profile(); swipekb::PredictionDiagnosticContext state;
        QCOMPARE(swipekb::predictionDiagnostics({},state)["predictionStatus"].toString(),QStringLiteral("profile-unavailable"));
        state.hasContext=state.hasBridge=state.policyKnown=state.fieldAllowed=true;
        state.ngramOnly=true;
        QCOMPARE(swipekb::predictionDiagnostics(p,state)["predictionStatus"].toString(),QStringLiteral("available-after-confirmation"));
        state.ngramOnly=false; p.effectiveLanguage="fr";
        auto d=swipekb::predictionDiagnostics(p,state);
        QVERIFY(d["predictionCapability"].toBool());
        QCOMPARE(d["predictionStatus"].toString(),QStringLiteral("available-after-confirmation"));
        p=profile(); p.capabilities.nextWord=false;
        QCOMPARE(swipekb::predictionDiagnostics(p,state)["predictionStatus"].toString(),QStringLiteral("context-model-unavailable"));
        p.capabilities.swipe=false;
        d=swipekb::predictionDiagnostics(p,state);
        QCOMPARE(d["predictionStatus"].toString(),QStringLiteral("profile-unavailable"));
        QCOMPARE(d.keys(),QStringList({"activeFieldPolicy","predictionCapability","predictionContextSource","predictionModelState","predictionStatus"}));
        const auto json=QJsonDocument::fromVariant(d).toJson();
        QVERIFY(!json.contains("private")); QVERIFY(!json.contains("hash")); QVERIFY(!json.contains("path"));
    }
};
QTEST_GUILESS_MAIN(PredictionDiagnosticsTest)
#include "tst_predictiondiagnostics.moc"
