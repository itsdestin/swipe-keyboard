// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QtTest>
#include "ownedhistory.h"
#include "predictionpolicy.h"

// A genuinely unbridged stock-proxy-shaped source, not a TestPolicyBridge.
class InputListenerItem : public QObject { Q_OBJECT };
class OwnedHistoryTest : public QObject {
    Q_OBJECT
    static EditorSnapshot editor(QString text) { const int n=text.size(); return {text,n,n,true,Qt::ImhNoPredictiveText}; }
private slots:
    void nativeIsDistinctAndBridgePresenceNeverFallsThrough() {
        InputListenerItem source;
        auto p=readPredictionPolicy(&source,true,7);
        QCOMPARE(p.source,PredictionPolicySource::OwnedHistory);
        QCOMPARE(p.version,0); QVERIFY(!p.contentTypeReceived); QCOMPARE(p.contextId,quint64(0));
        QVERIFY(NextWordSession::allowed(editor("sentinel "),p));
        source.setProperty("swipePredictionPolicy",QVariantMap{{"version",2}});
        QCOMPARE(readPredictionPolicy(&source,true,7).source,PredictionPolicySource::Unavailable);
        source.setProperty("swipePredictionPolicy",QString("malformed"));
        QCOMPARE(readPredictionPolicy(&source,true,7).source,PredictionPolicySource::Unavailable);
        QObject unknown;
        QCOMPARE(readPredictionPolicy(&unknown,true,7).source,PredictionPolicySource::Unavailable);
        QCOMPARE(readPredictionPolicy(&source,false,7).source,PredictionPolicySource::Unavailable);
    }
    void onlyConfirmedOwnedWordsEnterModelContext() {
        InputListenerItem source; auto p=readPredictionPolicy(&source,true,1);
        OwnedHistory h; auto before=editor("sentinel pasted "); auto after=editor(before.text+"hello");
        h.word(before,after,p,1,"hello",false); QVERIFY(h.context().isEmpty());
        h.observe(after,p,1); QCOMPARE(h.context(),QStringList{"hello"});
        auto spaced=editor(after.text+" "); h.space(after,spaced,p,1); h.observe(spaced,p,1);
        QCOMPARE(h.context(),QStringList{"hello"});
        auto chain=editor(spaced.text+"world"); h.word(spaced,chain,p,1,"world",false); h.observe(chain,p,1);
        QCOMPARE(h.context(),(QStringList{"hello","world"}));
        auto changed=editor(chain.text+" pasted"); h.observe(changed,p,1); QVERIFY(h.context().isEmpty());
    }
    void tappedWordCorrectionAndReset() {
        InputListenerItem source; auto p=readPredictionPolicy(&source,true,1); OwnedHistory h;
        auto e=editor("sentinel ");
        for(auto c:QString("hello")) {auto n=editor(e.text+c);h.tap(e,n,p,1,QString(c));h.observe(n,p,1);e=n;}
        QCOMPARE(h.context(),QStringList{"hello"});
        auto correction=editor("sentinel help"); h.word(e,correction,p,1,"help",true);h.observe(correction,p,1);
        QCOMPARE(h.context(),QStringList{"help"}); h.clear(); QVERIFY(h.context().isEmpty());
    }
    void boundedHistoryAndAwayBackRevokeNativeStamp() {
        InputListenerItem source; auto p=readPredictionPolicy(&source,true,1); OwnedHistory h;
        auto e=editor("");
        for(int i=0;i<25;++i) {
            auto n=editor(e.text+"hello");h.word(e,n,p,1,"hello",false);h.observe(n,p,1);
            e=n;n=editor(e.text+" ");h.space(e,n,p,1);h.observe(n,p,1);e=n;
        }
        QCOMPARE(h.context().size(),20);
        NextWordSession session;session.armExpectedEcho(e,p,1,true);auto stamp=session.observe(e,p,1,true);QVERIFY(stamp);
        auto away=e;--away.cursor;away.anchor=away.cursor;
        h.changed(away,p,1);h.observe(e,p,1);QVERIFY(h.context().isEmpty());
        session.editorChanged(away,p,1,true);session.editorChanged(e,p,1,true);
        QVERIFY(!session.accept(*stamp,{"world"}));
        // A new source/reset epoch cannot resurrect either completed or held authority.
        auto n=editor(e.text+"hello");h.word(e,n,p,1,"hello",false);++p.revision;h.observe(n,p,1);
        QVERIFY(h.context().isEmpty());
    }
    void denialSelectionPreeditAndStaleEpoch() {
        InputListenerItem source;auto p=readPredictionPolicy(&source,true,1);
        const QList<Qt::InputMethodHints> denied={Qt::ImhSensitiveData,Qt::ImhHiddenText,Qt::ImhUrlCharactersOnly,
            Qt::ImhEmailCharactersOnly,Qt::ImhPreferNumbers,Qt::ImhDigitsOnly,Qt::ImhFormattedNumbersOnly,
            Qt::ImhDialableCharactersOnly,Qt::ImhDate,Qt::ImhTime};
        for(auto hint:denied){auto e=editor("hello");e.hints|=hint;QVERIFY(!NextWordSession::allowed(e,p));}
        auto e=editor("hello");e.preedit=true;QVERIFY(!NextWordSession::allowed(e,p));e.preedit=false;e.anchor=0;
        QVERIFY(!NextWordSession::allowed(e,p));
        OwnedHistory h;auto a=editor("");auto b=editor("hello");h.word(a,b,p,1,"hello",false);
        h.observe(b,p,2);QVERIFY(h.context().isEmpty());
    }
};
QTEST_GUILESS_MAIN(OwnedHistoryTest)
#include "tst_ownedhistory.moc"
