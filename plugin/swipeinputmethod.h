/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

// SwipeInputMethod — Plasma Keyboard's input method while swipe-keyboard is installed. Taps pass through
// untouched (keyEvent returns false, so Qt's normal handling types them). SwipeCapture.qml calls
// finishSwipe() with a finished path; the best guess is typed and the others go to the guess bar.
#pragma once

#include <QtQml/qqmlregistration.h>
#include <QtVirtualKeyboard/QVirtualKeyboardAbstractInputMethod>

#include "swipeoutcome.h"
#include "swipesession.h"
#include "nextwordsession.h"
#include "ownedhistory.h"
#include "typedwordsession.h"
#include "wordswap.h"
#include "swapflow.h"
#include "nextwordworker.h"
#include "languageprofile.h"
#include "predictiondiagnostics.h"
#include "learningmanager.h"
#include <QPointer>
#include <QSet>
#include <QQuickWindow>
#include <QQuickItem>
#include <QEvent>
#include <memory>
namespace swipekb { class LanguageEngine; }

class SwipeInputMethod : public QVirtualKeyboardAbstractInputMethod {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject *predictionPolicySource READ predictionPolicySource WRITE setPredictionPolicySource)
    Q_PROPERTY(QVariantMap diagnostics READ diagnostics NOTIFY diagnosticsChanged)

public:
    explicit SwipeInputMethod(QObject *parent = nullptr);
    // `scorer` (tests): stands in for the worker's swap-list scoring, like `inference` for word lists.
    SwipeInputMethod(QObject *parent, NextWordWorker::Inference inference, NextWordWorker::Scorer scorer = {});
    ~SwipeInputMethod() override;
    swipekb::ProfileSnapshot profileSnapshot() const { return m_profiles.snapshot(); }
    Q_INVOKABLE void setPhysicalLayout(const QString &layoutId);

    QList<QVirtualKeyboardInputEngine::InputMode> inputModes(const QString &locale) override;
    bool setInputMode(const QString &locale, QVirtualKeyboardInputEngine::InputMode inputMode) override;
    bool setTextCase(QVirtualKeyboardInputEngine::TextCase textCase) override;
    bool keyEvent(Qt::Key key, const QString &text, Qt::KeyboardModifiers modifiers) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

    QList<QVirtualKeyboardSelectionListModel::Type> selectionLists() override;
    int selectionListItemCount(QVirtualKeyboardSelectionListModel::Type type) override;
    QVariant selectionListData(QVirtualKeyboardSelectionListModel::Type type, int index,
                               QVirtualKeyboardSelectionListModel::Role role) override;
    void selectionListItemSelected(QVirtualKeyboardSelectionListModel::Type type, int index) override;

    void reset() override;
    void update() override;

    QVariantMap diagnostics() const;
    Q_INVOKABLE QString predictionSelectionToken() const;
    Q_INVOKABLE quint64 beginPredictionSelection(const QString &token);
    Q_INVOKABLE void endPredictionSelection(quint64 pressId);
    QObject *predictionPolicySource() const { return m_policySource; }
    void setPredictionPolicySource(QObject *);
    // Phase 2 "learn from my typing". Off unless the owner created <data>/learn-typing (see learningmanager.h).
    Q_INVOKABLE bool learningEnabled() { return m_learning.enabled(); }
    // Erases everything learned, on disk and in memory, at once.
    Q_INVOKABLE void forgetEverythingLearned();
    // Long-press on a suggestion -> Qt's own "Remove from dictionary" menu -> this.
    bool selectionListRemoveItem(QVirtualKeyboardSelectionListModel::Type type, int index) override;
    Q_INVOKABLE bool isReady() const;
    Q_INVOKABLE bool isReadyForLayout(const QVariantList &keys) const;
    Q_INVOKABLE int beginSwipe();
    Q_INVOKABLE bool validateSwipe(int generation);
    Q_INVOKABLE void finishSwipe(const QVariantList &points, const QVariantList &keys, int generation);

signals:
    void gestureInvalidated();
    void profileChanged();
    void predictionWorkSettled();
    void diagnosticsChanged();

private slots:
    void predictionPolicyChanged();
private:
    bool safeContext();
    bool safePredictionContext();
    void stageOwnedCommit(const CommitPlan &, bool correction = false);
    // --- Phase 2: learning from the person's own typing -------------------------------------------------------
    swipekb::PersonalModel *personal();                 // nullptr while learning is off
    QString personalKey(const QString &word) const;
    // Counts one committed word, only where prediction itself is allowed. Returns what it changed (for undo).
    swipekb::PersonalModel::Undo learnWord(const QString &word, const QString &prev);
    QString prevBeforeToken(const EditorSnapshot &editor, const QString &token) const;
    // Re-orders a suggestion list with what was learned; also drops forgotten words.
    QStringList personalize(const QStringList &base, const QString &prev, const QString &prefix, bool inject, int topK,
                            double boostScale = 1.0);
    void syncLearnedWords(bool force = false);
    QString m_wordsLanguage;   // language the swipe engines were last filled for (see syncLearnedWords)
    void refreshTypedSuggestions(const EditorSnapshot &editor);
    void learnSwiped(const QString &word, const QString &prev);   // learnWord() + remember it for undo
    void retractLastLearn();
    swipekb::PersonalModel::Undo m_lastLearn;           // the last swiped word's count, taken back if it is fixed or erased
    const void *m_lastLearnModel = nullptr;
    QString m_lastLearnPrev;
    QString m_nextPrev, m_completionPrev;               // previous word of the newest next-word / completion request
    LearningManager m_learning;
    QSet<QString> m_injected;                           // taught words handed to the engine as learned words
    std::uint64_t m_syncedVersion = 0;
    const void *m_syncedModel = nullptr, *m_syncedEngine = nullptr;
    bool m_liveQueryActive = false;
#ifdef SWIPEKB_PRIVATE_FLAGS_DIAGNOSTIC
    bool observeFlags(const char *phase);
    quint64 m_diagnosticContext = 0;
    quint64 m_diagnosticItem = 0;
    quint64 m_diagnosticRecognitionCount = 0;
    quint64 m_diagnosticCommitLength = 0;
    bool m_diagnosticQueryActive = false;
#endif
    void resolveCurrentProfile();
    swipekb::ProfileEpoch m_profiles;
    // The language's engine: the project's own swipe engine plus the dictionary (completion, tap-to-swap, spellings).
    std::shared_ptr<swipekb::LanguageEngine> m_engine;
    QString m_locale, m_layoutId, m_inputMode;
    void ensureContextConnections();
    EditorSnapshot predictionEditor() const;
    PredictionPolicy predictionPolicy() const;
    void invalidatePredictions();
    void observePredictionEcho();
    void requestContextCompletion(const EditorSnapshot &editor);
    bool predictionCapable() const;
    void applyNeuralNextWord(PredictionJob &job) const;
    CaseMode caseMode() const;
    QStringList barCandidates() const;
    // Tap-to-swap (card #187): tapping an existing word shows alternatives; nothing swaps by itself.
    void observeWordSwap(const EditorSnapshot &editor);
    bool tapSwapAllowed(const EditorSnapshot &editor) const;
    void clearWordSwap();
    WordSwapSession m_swap;
    CursorMoveTracker m_moves;
    RecentSwipes m_recentSwipes;   // memory only, never written to disk
    QString m_swapPress;
    PredictionStamp m_swapStamp;
    quint64 m_predictionPressId = 0;
    // Card #500 (one brain): swap lists come from the engine's swap search (swapflow.h), shown at once and re-ranked
    // when the worker's sentence scores arrive. One waiting list for tap-back, one for the bar right after a swipe.
    swapflow::PendingSwap m_swapPending, m_afterSwipePending;
    swapflow::Rules swapRules();
    // The key rectangles of the latest swipe (the current layout), for the made-up path of a typed word.
    RecentSwipes::Keys m_layoutKeys;
    // A finger is on the guess bar (between beginPredictionSelection and its endPredictionSelection). WHY: the bar
    // after a swipe has no press token, so its list must not change under a pressed finger.
    bool m_barPressed = false;
    // Metadata for diagnostics/tests, never words: whether the last tap-back list used a remembered swipe, and how
    // many words after the tapped word it used.
    bool m_lastSwapUsedPath = false;
    int m_lastSwapWordsAfter = 0;
    quint64 m_afterSwipeLists = 0;   // post-swipe lists built (a count, for tests: a stale one is never built)
    void buildAfterSwipeList(quint64 sessionSerial, int generation, quint64 profileEpoch, const QString &first,
                             const RecentSwipes::Keys &keys, const QStringList &context, const RecentSwipes::Trace &trace,
                             const QString &otherSpelling, bool predictionField);
    bool afterSwipeScoring() const;
    std::optional<PredictionStamp> m_predictionTap;
    NextWordSession m_nextWord;
    OwnedHistory m_ownedHistory;
    TypedWordSession m_typedWord;
    QString m_typedPress;
    int m_autoSpaceCursor = -1; // cursor just after a space this keyboard added automatically
    quint64 m_nativePolicyEpoch = 1;
    NextWordWorker m_predictionWorker;
    swipekb::PredictionModelState m_predictionModelState=swipekb::PredictionModelState::NotRequested;
    QPointer<QObject> m_policySource;
    int m_generation = 0;
    bool m_contextConnected = false;
    QPointer<QQuickWindow> m_exposureWindow;
    QPointer<QQuickItem> m_exposureItem;
    bool m_exposureRefreshQueued = false;
    void watchInputWindow();
    void refreshExposedInputCache(QPointer<QVirtualKeyboardInputContext>, QPointer<QQuickItem>,
                                  QPointer<QQuickWindow>, int generation, quint64 profileEpoch);
    // Same guarded public refresh, run synchronously at our own input points (key press, candidate press).
    void syncInputCache();
    bool m_cacheSyncActive = false;
    QString textBeforeCursor() const;
    void clearSession();
    void refreshGuessBar();

    void clearAfterSwipe();

    SwipeSession m_session;
    // Decides erase/keep for the last swiped word (see swipeoutcome.h). WHY: measures how often swipes need fixing.
    SwipeOutcomeTracker m_outcome;
    void writeOutcome(const SwipeOutcomeTracker::Outcome &o);
    // True while the last thing typed was a swiped word and nothing was tapped since. WHY: apps that hide
    // their text (XWayland/Steam, some Chrome setups) report "" before the cursor, which looks like the
    // start of the field; this flag is how the next swiped word still gets its space.
    bool m_afterSwipe = false;
    // Where the cursor sat just before and just after that swiped word was typed. WHY: update() runs on any
    // text or cursor change, including the app echoing our own word back; only a move somewhere else
    // means the user went elsewhere and the flag no longer applies.
    int m_afterSwipeCursorBefore = -1;
    int m_afterSwipeCursorAfter = -1;
    // Timestamp of the last swipe the recorder wrote, 0 if it was not recorded. WHY: a guess-bar pick is
    // recorded as a correction of that swipe, so it must point at the right line; a pick for a swipe that
    // was never recorded (recording off, password field) is not written at all.
    qint64 m_lastRecordedSwipeTs = 0;
    QVirtualKeyboardInputEngine::TextCase m_textCase = QVirtualKeyboardInputEngine::TextCase::Lower;
};
