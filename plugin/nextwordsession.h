/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "swipesession.h"
#include <Qt>
#include <optional>

struct PredictionStamp {
    quint64 profileEpoch = 0, editorEpoch = 0, requestId = 0;
    bool operator==(const PredictionStamp &) const = default;
};
// OwnedHistory is deliberately NOT an affirmative raw Wayland content policy.
enum class PredictionPolicySource { Unavailable, RawBridge, OwnedHistory };
struct PredictionPolicy {
    int version = 0;
    quint64 contextId = 0, revision = 0;
    bool contentTypeReceived = false;
    int purpose = -1;
    bool completionAllowed = false, syntheticNoPredictiveText = false;
    PredictionPolicySource source = PredictionPolicySource::RawBridge;
    bool operator==(const PredictionPolicy &) const = default;
};
struct EditorSnapshot {
    QString text;
    int cursor = -1, anchor = -1;
    bool focused = false;
    Qt::InputMethodHints hints;
    bool preedit = false;
    bool operator==(const EditorSnapshot &) const = default;
};

// The guess bar's display rules, shared by the sessions below (and TypedWordSession) and by the typing bench
// (tools/swipekb-typing-bench). WHY free functions: the bench must show the very three slots a typist sees, without a
// live editor or the stamps that guard it.
namespace barrules {
// NextWordSession::accept: at most three plain words (letters, inner ' ’ -; 64 characters at most), each once
// whatever its case, the strongest in the middle.
QStringList nextWordShown(const QStringList &words);
// TypedWordSession::suggest: the best words for the typed letters `token`, in the token's capitals, with the letters
// themselves always one of the three slots, the strongest in the middle. Empty when `token` is.
QStringList typedShown(const QString &token, const QStringList &ranked);
}

// Owns only bounded, memory-only state. No editor object crosses the worker boundary.
class NextWordSession {
public:
    static bool allowed(const EditorSnapshot &, const PredictionPolicy &);
    void invalidate();
    void armExpectedEcho(const EditorSnapshot &, const PredictionPolicy &, quint64 profileEpoch, bool capable,
                         std::optional<EditorSnapshot> previous = {});
    std::optional<PredictionStamp> observe(const EditorSnapshot &, const PredictionPolicy &, quint64 profileEpoch, bool capable);
    // Editor property notifications can precede Qt's coherent update() snapshot.
    void editorChanged(const EditorSnapshot &, const PredictionPolicy &, quint64 profileEpoch, bool capable);
    bool accept(PredictionStamp, const QStringList &);
    CommitPlan takePrediction(int, PredictionStamp displayedSet, const EditorSnapshot &, const PredictionPolicy &, quint64 profileEpoch, CaseMode);
    std::optional<PredictionStamp> candidateStamp() const { return m_candidates.isEmpty() ? std::nullopt : std::optional(m_stamp); }
    QStringList candidates() const { return m_candidates; }
    QString textBeforeCursor() const { return m_expected.text.left(m_expected.cursor).right(4096); }
    bool pendingOrVisible() const { return m_armed || m_requested; }
private:
    bool matches(const EditorSnapshot &, const PredictionPolicy &, quint64 profileEpoch) const;
    EditorSnapshot m_expected;
    std::optional<EditorSnapshot> m_previous;
    PredictionPolicy m_policy;
    PredictionStamp m_stamp;
    bool m_armed = false, m_requested = false;
    bool m_echoTextSeen = false, m_echoCursorSeen = false, m_echoAnchorSeen = false;
    QStringList m_candidates;
};
