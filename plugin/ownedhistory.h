#pragma once
#include "nextwordsession.h"
#include <QRegularExpression>

// Native stock mode's model input. The editor snapshot is ONLY an exact-echo
// authority check; no word is ever extracted from it. Nothing is persisted.
class OwnedHistory {
public:
    void clear() { m_echo.invalidate(); m_words.clear(); m_token.clear(); m_pendingWords.clear(); m_pendingToken.clear(); m_pending=false; }
    void observe(const EditorSnapshot &e, const PredictionPolicy &p, quint64 epoch) {
        if (p.source != PredictionPolicySource::OwnedHistory) { clear(); return; }
        const auto confirmed=m_echo.observe(e,p,epoch,true);
        if (confirmed && m_pending) {
            m_words=m_pendingWords; m_token=m_pendingToken; m_pending=false;
            m_pendingWords.clear(); m_pendingToken.clear();
        }
        if (!m_echo.pendingOrVisible()) clear();
    }
    void changed(const EditorSnapshot &e, const PredictionPolicy &p, quint64 epoch) {
        m_echo.editorChanged(e,p,epoch,true);
        if (!m_echo.pendingOrVisible()) clear();
    }
    QStringList context() const { auto out=m_words; if (!m_token.isEmpty()) out<<m_token; while(out.size()>20) out.removeFirst(); for(auto &word:out) word=word.toLower(); return out; }
    void word(const EditorSnapshot &before,const EditorSnapshot &after,const PredictionPolicy &p,
              quint64 epoch,const QString &word,bool correction) {
        observe(before,p,epoch);
        if (!lexical(word)) { clear(); return; }
        auto words=m_words;
        if (!correction && !m_token.isEmpty()) words<<m_token;
        stage(before,after,p,epoch,words,word);
    }
    void tap(const EditorSnapshot &before,const EditorSnapshot &after,const PredictionPolicy &p,
             quint64 epoch,const QString &text) {
        observe(before,p,epoch);
        // WHY don't adopt a suffix of pasted/preexisting text as an owned word.
        if (text.size()!=1 || !text.front().isLetter() || m_token.size()>=64
            || (m_token.isEmpty() && before.cursor>0 && !before.text.at(before.cursor-1).isSpace())) { clear(); return; }
        stage(before,after,p,epoch,m_words,m_token+text);
    }
    void space(const EditorSnapshot &before,const EditorSnapshot &after,const PredictionPolicy &p,quint64 epoch) {
        observe(before,p,epoch);
        auto words=m_words; if (!m_token.isEmpty()) words<<m_token;
        stage(before,after,p,epoch,words,{});
    }
private:
    static bool lexical(const QString &s) {
        static const QRegularExpression rx(QStringLiteral("^[\\p{L}]+(?:['’\\-][\\p{L}]+)*$"));
        return s.size()<=64 && rx.match(s).hasMatch();
    }
    void stage(const EditorSnapshot &before,const EditorSnapshot &after,const PredictionPolicy &p,quint64 epoch,
               QStringList words,const QString &token) {
        if (p.source!=PredictionPolicySource::OwnedHistory || !NextWordSession::allowed(before,p)
            || !NextWordSession::allowed(after,p)) { clear(); return; }
        while(words.size()>20) words.removeFirst();
        m_echo.armExpectedEcho(after,p,epoch,true,before);
        m_pendingWords=words; m_pendingToken=token; m_pending=m_echo.pendingOrVisible();
        if (!m_pending) clear();
    }
    NextWordSession m_echo;
    QStringList m_words,m_pendingWords;
    QString m_token,m_pendingToken;
    bool m_pending=false;
};
