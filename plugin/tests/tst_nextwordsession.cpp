#include <QtTest>
#include "nextwordsession.h"

class NextWordSessionTest : public QObject {
    Q_OBJECT
private:
    static PredictionPolicy policy() { return {1, 17, 1, true, 0, true, true}; }
    static EditorSnapshot editor(QString text = QStringLiteral("hello ")) {
        const int n = text.size();
        return {text, n, n, true, Qt::ImhNoPredictiveText};
    }
    static PredictionStamp ready(NextWordSession &s) {
        s.armExpectedEcho(editor(), policy(), 4, true);
        return *s.observe(editor(), policy(), 4, true);
    }
private slots:
    void waitsForExactEcho() {
        NextWordSession s;
        s.armExpectedEcho(editor(), policy(), 4, true, editor("hello"));
        QVERIFY(!s.observe(editor("hello"), policy(), 4, true));
        QVERIFY(s.observe(editor(), policy(), 4, true));
        QVERIFY(!s.observe(editor(), policy(), 4, true));
    }
    void threeSlotsSelectTheirDisplayedRankWithCase() {
        const QStringList displayed{"second","best","third"};
        for(int slot=0;slot<3;++slot) for(auto mode:{CaseMode::AsIs,CaseMode::Capitalize,CaseMode::AllCaps}) {
            NextWordSession s;auto stamp=ready(s);QVERIFY(s.accept(stamp,{"best","second","third","fourth"}));
            QCOMPARE(s.candidates(),displayed);
            auto plan=s.takePrediction(slot,stamp,editor(),policy(),4,mode);
            auto expected=displayed[slot];
            if(mode==CaseMode::Capitalize) expected=expected.left(1).toUpper()+expected.mid(1);
            else if(mode==CaseMode::AllCaps) expected=expected.toUpper();
            QCOMPARE(plan.text,expected);QCOMPARE(plan.replaceFrom,0);QCOMPARE(plan.replaceLength,0);
        }
    }
    void predictionOnlyInserts() {
        NextWordSession s; auto stamp = ready(s);
        QVERIFY(s.accept(stamp, {"world", "again"}));
        auto p = s.takePrediction(0, stamp, editor(), policy(), 4, CaseMode::AsIs);
        QCOMPARE(p.text, QStringLiteral("world"));
        QCOMPARE(p.replaceFrom, 0); QCOMPARE(p.replaceLength, 0);
        QVERIFY(s.candidates().isEmpty());
    }
    void usesExistingSpacingAndCaseRules() {
        for (const QString text : {QStringLiteral("hello"), QStringLiteral("hello "), QStringLiteral("hello,")}) {
            NextWordSession s;
            s.armExpectedEcho(editor(text), policy(), 4, true);
            auto stamp = s.observe(editor(text), policy(), 4, true);
            QVERIFY(stamp); QVERIFY(s.accept(*stamp, {"world"}));
            const auto p = s.takePrediction(0, *stamp, editor(text), policy(), 4, CaseMode::Capitalize);
            QCOMPARE(p.text, text.endsWith(' ') ? QStringLiteral("World") : QStringLiteral(" World"));
            QCOMPARE(p.replaceLength, 0);
        }
    }
    void oldCandidateSetCannotSelectNewRequest() {
        NextWordSession s; const auto old = ready(s);
        QVERIFY(s.accept(old, {"first"}));
        const auto fresh = ready(s); QVERIFY(s.accept(fresh, {"second"}));
        QVERIFY(s.takePrediction(0, old, editor(), policy(), 4, CaseMode::AsIs).isEmpty());
    }
    void staleDeliveryAndTap() {
        NextWordSession s; auto stamp = ready(s);
        s.invalidate(); QVERIFY(!s.accept(stamp, {"world"}));
        stamp = ready(s); QVERIFY(s.accept(stamp, {"world"}));
        QVERIFY(s.takePrediction(0, stamp, editor(), policy(), 5, CaseMode::AsIs).isEmpty());
        stamp = ready(s); QVERIFY(s.accept(stamp, {"world"}));
        auto changed = policy(); ++changed.revision;
        QVERIFY(s.takePrediction(0, stamp, editor(), changed, 4, CaseMode::AsIs).isEmpty());
    }
    void pendingUnexpectedEditInvalidates() {
        NextWordSession s;
        s.armExpectedEcho(editor(), policy(), 4, true);
        QVERIFY(!s.observe(editor("unrelated"), policy(), 4, true));
        QVERIFY(!s.observe(editor(), policy(), 4, true));
    }
    void inFlightAwayAndBackCannotResurrect() {
        NextWordSession s; const auto stamp = ready(s);
        auto away = editor(); --away.cursor; --away.anchor;
        s.editorChanged(away, policy(), 4, true);
        s.editorChanged(editor(), policy(), 4, true);
        QVERIFY(!s.accept(stamp, {"stale"}));
    }
    void coherentEchoCanArriveAsSeparateProperties() {
        NextWordSession s;
        auto expected = editor(), previous = editor("hello"), intermediate = expected;
        intermediate.cursor = previous.cursor; intermediate.anchor = previous.anchor;
        s.armExpectedEcho(expected, policy(), 4, true, previous);
        s.editorChanged(intermediate, policy(), 4, true);
        s.editorChanged(expected, policy(), 4, true);
        QVERIFY(s.observe(expected, policy(), 4, true));
    }
    void armedCompletedEchoCannotRegress() {
        const auto expected = editor(), previous = editor("hello");
        for (int component = 0; component < 3; ++component) {
            NextWordSession s;
            s.armExpectedEcho(expected, policy(), 4, true, previous);
            s.editorChanged(expected, policy(), 4, true);
            auto away = expected;
            if (component == 0) away.text = previous.text;
            else if (component == 1) away.cursor = previous.cursor;
            else away.anchor = previous.anchor;
            s.editorChanged(away, policy(), 4, true);
            s.editorChanged(expected, policy(), 4, true);
            QVERIFY(!s.observe(expected, policy(), 4, true));
        }
    }
    void armedPartialProgressCannotRegress() {
        NextWordSession s;
        const auto expected = editor(), previous = editor("hello");
        auto partial = expected; partial.cursor = previous.cursor; partial.anchor = previous.anchor;
        s.armExpectedEcho(expected, policy(), 4, true, previous);
        s.editorChanged(partial, policy(), 4, true);
        s.editorChanged(previous, policy(), 4, true);
        s.editorChanged(expected, policy(), 4, true);
        QVERIFY(!s.observe(expected, policy(), 4, true));
    }
    void externalEditsInvalidate() {
        NextWordSession s; auto stamp = ready(s); QVERIFY(s.accept(stamp, {"world"}));
        QVERIFY(!s.observe(editor("edited "), policy(), 4, true));
        QVERIFY(s.candidates().isEmpty());
        QVERIFY(!s.accept(stamp, {"stale"}));
    }
    void deniesMissingAndRestrictedPolicy() {
        auto e = editor(); auto p = policy();
        QVERIFY(NextWordSession::allowed(e, p));
        for (int purpose : {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, -1, 99}) {
            p = policy(); p.purpose = purpose;
            QVERIFY(!NextWordSession::allowed(e, p));
        }
        p = policy(); p.purpose = 1; // alpha is a supported prose purpose
        QVERIFY(NextWordSession::allowed(e, p));
        p = policy(); p.version = 0; QVERIFY(!NextWordSession::allowed(e, p));
        p = policy(); p.contentTypeReceived = false; QVERIFY(!NextWordSession::allowed(e, p));
        p = policy(); p.completionAllowed = false; QVERIFY(!NextWordSession::allowed(e, p));
        p = policy(); p.syntheticNoPredictiveText = false; QVERIFY(!NextWordSession::allowed(e, p));
        p = policy(); p.contextId = 0; QVERIFY(!NextWordSession::allowed(e, p));
        for (auto hint : {Qt::ImhSensitiveData, Qt::ImhHiddenText, Qt::ImhUrlCharactersOnly,
                          Qt::ImhEmailCharactersOnly, Qt::ImhDigitsOnly, Qt::ImhFormattedNumbersOnly,
                          Qt::ImhDialableCharactersOnly, Qt::ImhDate, Qt::ImhTime}) {
            e = editor(); e.hints |= hint; QVERIFY(!NextWordSession::allowed(e, policy()));
        }
        e = editor(); --e.anchor; QVERIFY(!NextWordSession::allowed(e, policy()));
        e = editor(); ++e.cursor; QVERIFY(!NextWordSession::allowed(e, policy()));
        e = editor(); e.focused = false; QVERIFY(!NextWordSession::allowed(e, policy()));
    }
    void nonEnglishOrUnavailableNeverRequests() {
        NextWordSession s;
        s.armExpectedEcho(editor(), policy(), 4, false);
        QVERIFY(!s.observe(editor(), policy(), 4, false));
        QVERIFY(s.candidates().isEmpty());
    }
    void filtersUnsafeCandidates() {
        NextWordSession s; auto stamp = ready(s);
        QVERIFY(s.accept(stamp, {"world", "world", "", "two words", "<unk>", "fine", "third", "fourth"}));
        QCOMPARE(s.candidates(), QStringList({"fine", "world", "third"}));
    }
};
QTEST_GUILESS_MAIN(NextWordSessionTest)
#include "tst_nextwordsession.moc"
