#pragma once
#include "nextwordsession.h"

// Only keyboard-tapped, exact-echo-confirmed unfinished text can authorize replacement.
// Separate from insertion-only next-word predictions and whole-word swipe undo.
class TypedWordSession {
public:
    void clear() { m_echo.invalidate();m_token.clear();m_pending.clear();m_candidates.clear();m_start=-1;m_suppress=false; }
    void edit(const EditorSnapshot &before,const EditorSnapshot &after,const PredictionPolicy &policy,
              quint64 epoch,const QString &text,bool backspace=false) {
        observe(before,policy,epoch);
        QString token=m_token;m_suppress=false;
        if(backspace) {if(token.isEmpty()){clear();return;}token.chop(1);}
        else {
            if(text.size()!=1 || !(text.front().isLetter() || text=="'" || text=="’" || text=="-")) {clear();return;}
            if(token.isEmpty()) {
                if(before.cursor>0 && before.text[before.cursor-1].isLetter()){clear();return;}
                m_start=before.cursor;
            }
            token+=text;
        }
        if(token.isEmpty() || token.size()>64){clear();return;}
        m_echo.armExpectedEcho(after,policy,epoch,true,before);
        m_pending=token;m_candidates.clear();
        if(!m_echo.pendingOrVisible()) clear();
    }
    bool observe(const EditorSnapshot &editor,const PredictionPolicy &policy,quint64 epoch) {
        auto stamp=m_echo.observe(editor,policy,epoch,true);
        if(stamp && !m_pending.isEmpty()){m_token=m_pending;m_pending.clear();m_stamp=*stamp;return !m_suppress;}
        if(!m_echo.pendingOrVisible())clear();
        return false;
    }
    void changed(const EditorSnapshot &e,const PredictionPolicy &p,quint64 epoch) {
        m_echo.editorChanged(e,p,epoch,true);if(!m_echo.pendingOrVisible())clear();
    }
    QString token()const{return m_token;}
    PredictionStamp stamp()const{return m_stamp;}
    QStringList candidates()const{return m_candidates;}
    QString selectionToken()const {
        return m_candidates.isEmpty()?QString():QStringLiteral("typed:%1:%2:%3").arg(m_stamp.profileEpoch).arg(m_stamp.editorEpoch).arg(m_stamp.requestId);
    }
    void suggest(const QStringList &ranked) {
        if(m_token.isEmpty())return;
        QStringList words;for(auto word:ranked) {
            if(m_token==m_token.toUpper())word=word.toUpper();
            else if(m_token.front().isUpper())word=word.left(1).toUpper()+word.mid(1);
            if(!words.contains(word,Qt::CaseInsensitive))words<<word;
            if(words.size()==3)break;
        }
        // Literal spelling is always a selectable escape hatch, never silently corrected.
        const int literal=words.indexOf(m_token);
        if(literal<0){while(words.size()>2)words.removeLast();words<<m_token;}
        if(words.size()==3)words.swapItemsAt(0,1);
        m_candidates=words;
    }
    CommitPlan take(int index,const QString &pressed,const EditorSnapshot &e,const PredictionPolicy &p,quint64 epoch) {
        const auto token=selectionToken();observe(e,p,epoch);
        CommitPlan out;
        if(!token.isEmpty() && token==selectionToken() && token==pressed && index>=0 && index<m_candidates.size()
            && m_start>=0 && e.cursor==m_start+m_token.size() && e.text.mid(m_start,m_token.size())==m_token)
            out={m_candidates[index],-int(m_token.size()),int(m_token.size())};
        if(out.isEmpty()){clear();return out;}
        // WHY (owner decision, 0.3.1): accepting a suggestion finishes the word like Gboard, so
        // the next word can start immediately. Skip it when a space already follows the cursor.
        if(e.cursor>=e.text.size() || !e.text.at(e.cursor).isSpace()) out.text+=QLatin1Char(' ');
        clear(); // the finished word no longer accepts letters; the next tap starts a new word
        return out;
    }
private:
    NextWordSession m_echo;PredictionStamp m_stamp;
    QString m_token,m_pending;QStringList m_candidates;int m_start=-1;bool m_suppress=false;
};
