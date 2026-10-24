// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "nextwordsession.h"
#include <QRegularExpression>

bool NextWordSession::allowed(const EditorSnapshot &e, const PredictionPolicy &p)
{
    const auto deny = Qt::ImhSensitiveData | Qt::ImhHiddenText | Qt::ImhUrlCharactersOnly
        | Qt::ImhEmailCharactersOnly | Qt::ImhDigitsOnly | Qt::ImhFormattedNumbersOnly
        | Qt::ImhDialableCharactersOnly | Qt::ImhDate | Qt::ImhTime | Qt::ImhPreferNumbers;
    // WHY separate modes: native stock Plasma cannot report raw completion vetoes.
    // Its model sees only confirmed keyboard-owned words, never this validation buffer.
    const bool native = p.source == PredictionPolicySource::OwnedHistory;
    const bool bridge = p.source == PredictionPolicySource::RawBridge && p.version == 1
        && p.contextId != 0 && p.contentTypeReceived && p.completionAllowed && (p.purpose == 0 || p.purpose == 1);
    return (native || bridge) && e.focused && !e.preedit && e.cursor >= 0
        && e.cursor <= e.text.size() && e.cursor == e.anchor && !(e.hints & deny)
        && (!(e.hints & Qt::ImhNoPredictiveText) || native || p.syntheticNoPredictiveText);
}

void NextWordSession::invalidate()
{
    ++m_stamp.editorEpoch;
    ++m_stamp.requestId;
    m_armed = m_requested = false;
    m_echoTextSeen = m_echoCursorSeen = m_echoAnchorSeen = false;
    m_candidates.clear();
    m_expected = {};
    m_previous.reset();
    m_policy = {};
}

void NextWordSession::armExpectedEcho(const EditorSnapshot &e, const PredictionPolicy &p, quint64 epoch, bool capable,
                                     std::optional<EditorSnapshot> previous)
{
    invalidate();
    if (!capable || !allowed(e, p)) return;
    // WHY bound storage too: surrounding text can be an entire document, not just a field.
    // Refusing huge echoes avoids keeping a copy of a user's document in this session.
    if (e.text.size() > 16384) return;
    m_expected = e; m_previous = std::move(previous); m_policy = p; m_stamp.profileEpoch = epoch; m_armed = true;
}

bool NextWordSession::matches(const EditorSnapshot &e, const PredictionPolicy &p, quint64 epoch) const
{
    return m_stamp.profileEpoch == epoch && e == m_expected && p == m_policy && allowed(e, p);
}

std::optional<PredictionStamp> NextWordSession::observe(const EditorSnapshot &e, const PredictionPolicy &p,
                                                       quint64 epoch, bool capable)
{
    editorChanged(e, p, epoch, capable);
    if (!m_armed && !m_requested) return {};
    if (!capable || p != m_policy || epoch != m_stamp.profileEpoch || !allowed(e, p)) {
        invalidate(); return {};
    }
    if (m_armed) {
        // WHY pre-echo updates do not publish: only exact reported editor content authorizes inference.
        if (!matches(e, p, epoch)) {
            if (!m_previous || e != *m_previous) invalidate();
            return {};
        }
        m_previous.reset();
        m_armed = false; m_requested = true; ++m_stamp.requestId;
        return m_stamp;
    }
    if (!matches(e, p, epoch)) invalidate();
    return {};
}

void NextWordSession::editorChanged(const EditorSnapshot &e, const PredictionPolicy &p, quint64 epoch, bool capable)
{
    if (!m_armed && !m_requested) return;
    if (!capable || p != m_policy || epoch != m_stamp.profileEpoch || !e.focused || e.hints != m_expected.hints) {
        invalidate(); return;
    }
    if (m_requested) {
        // WHY notifications, not just final update(): an away-and-back cursor/edit
        // must permanently revoke in-flight authority even if inference finishes later.
        if (!matches(e, p, epoch)) invalidate();
        return;
    }
    // Qt can publish text, cursor and anchor separately for our own commit echo.
    // Only combinations of that one known before/after transition may survive;
    // notifications never submit inference. update() still requires the exact echo.
    const auto previous = m_previous.value_or(m_expected);
    const bool knownText = e.text == previous.text || e.text == m_expected.text;
    const bool knownCursor = e.cursor == previous.cursor || e.cursor == m_expected.cursor;
    const bool knownAnchor = e.anchor == previous.anchor || e.anchor == m_expected.anchor;
    // WHY one-way progress: once any expected property was observed, moving it
    // back to its old value is an external edit, not another valid commit echo.
    if (!knownText || !knownCursor || !knownAnchor
        || (m_echoTextSeen && e.text != m_expected.text)
        || (m_echoCursorSeen && e.cursor != m_expected.cursor)
        || (m_echoAnchorSeen && e.anchor != m_expected.anchor)) {
        invalidate(); return;
    }
    m_echoTextSeen = m_echoTextSeen || e.text == m_expected.text;
    m_echoCursorSeen = m_echoCursorSeen || e.cursor == m_expected.cursor;
    m_echoAnchorSeen = m_echoAnchorSeen || e.anchor == m_expected.anchor;
}

bool NextWordSession::accept(PredictionStamp stamp, const QStringList &words)
{
    if (!m_requested || stamp != m_stamp) return false;
    m_candidates.clear();
    static const QRegularExpression lexical(QStringLiteral("^[\\p{L}]+(?:['’\\-][\\p{L}]+)*$"));
    for (const auto &word : words) {
        if (word.size() <= 64 && lexical.match(word).hasMatch() && !m_candidates.contains(word, Qt::CaseInsensitive))
            m_candidates << word;
        if (m_candidates.size() == 3) break;
    }
    // WHY one displayed list is also the selection authority: rank two goes left,
    // strongest in the middle, rank three right; labels and callbacks cannot diverge.
    // A genuinely short beam stays short, never padded with invented suggestions.
    if (m_candidates.size()==3) m_candidates.swapItemsAt(0,1);
    return true;
}

CommitPlan NextWordSession::takePrediction(int i, PredictionStamp displayedSet, const EditorSnapshot &e, const PredictionPolicy &p,
                                          quint64 epoch, CaseMode mode)
{
    CommitPlan plan;
    if (m_requested && displayedSet == m_stamp && matches(e, p, epoch) && i >= 0 && i < m_candidates.size()) {
        QString word = m_candidates.at(i);
        word = SwipeSession::applyCase(word, mode);   // WHY shared: same Shift/Caps rules as a swiped word
        plan.text = (SwipeSession::needsSpaceBefore(e.text.left(e.cursor)) ? QStringLiteral(" ") : QString()) + word;
        // WHY never a replacement: prediction belongs AFTER the confirmed word, unlike corrections.
        plan.replaceFrom = plan.replaceLength = 0;
    }
    invalidate();
    return plan;
}
