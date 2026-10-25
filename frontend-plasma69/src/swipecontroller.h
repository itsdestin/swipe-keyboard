// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// SwipeController — Swipe Keyboard's text rules on Plasma 6.9's keyboard core.
//
// It is the 6.9 counterpart of plugin/swipeinputmethod.cpp (the Qt Virtual Keyboard front end) and runs the SAME
// session classes: SwipeSession (swiped word, spacing, guess bar, whole-word Backspace), NextWordSession (next word,
// only on the exact echo of our own typing), TypedWordSession (completion of tapped letters), WordSwapSession
// (tap-to-swap), OwnedHistory (stock-field history), LearningManager/PersonalModel (learning), the texting words and
// the swear-word rule. What differs is only how it is driven:
//   * Taps are typed by KDE's own text composer. The swipe layer reports each key press (keyPressed, before KDE types
//     it) and each finished key (keyClicked, after), so the controller can add Gboard's space before a tapped letter
//     and follow the word being typed.
//   * Keys whose meaning changes right after a swipe (Backspace erases the whole swiped word; "." replaces the
//     automatic space) are claimed by the layer while undoArmed/autoSpaceArmed is true and arrive at claimedKey().
//   * Edits go through EditorPort::commit() with UTF-8 byte offsets, the unit Wayland input-method-v1 uses.
#pragma once

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

#include "learningmanager.h"
#include "nextwordsession.h"
#include "nextwordworker.h"
#include "languageprofile.h"
#include "swipekb/language_engine.hpp"
#include "ownedhistory.h"
#include "swipesession.h"
#include "typedwordsession.h"
#include "wordswap.h"
#include "swapflow.h"

class EditorPort;

class SwipeController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(QString language READ language NOTIFY readyChanged)
    Q_PROPERTY(bool fieldAllowsSwipe READ fieldAllowsSwipe NOTIFY stateChanged)
    Q_PROPERTY(QStringList candidates READ candidates NOTIFY candidatesChanged)
    Q_PROPERTY(bool undoArmed READ undoArmed NOTIFY stateChanged)
    Q_PROPERTY(bool autoSpaceArmed READ autoSpaceArmed NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap diagnostics READ diagnostics NOTIFY candidatesChanged)

public:
    // `inference` replaces the next-word worker's model (tests); empty = the real n-gram predictor.
    // `scorer` (tests): stands in for the worker's swap-list scoring, like `inference` for word lists.
    explicit SwipeController(EditorPort *editor, QObject *parent = nullptr, NextWordWorker::Inference inference = {},
                             NextWordWorker::Scorer scorer = {});
    ~SwipeController() override;

    bool ready() const;
    QString language() const
    {
        return m_profile.effectiveLanguage;
    }
    bool fieldAllowsSwipe() const;
    QStringList candidates() const;
    bool undoArmed() const
    {
        return m_session.isActive();
    }
    bool autoSpaceArmed() const
    {
        return m_autoSpaceCursor > 0;
    }
    QVariantMap diagnostics() const;

    // The language follows the layout's locale; called by the hook (and tests) with "en_US", "de_DE", ...
    void setLocale(const QString &locale);
    // KDE's layout id ("org.kde.plasma.keyboard.de_DE/mobile-qwertz"); the profile needs its physical family.
    void setLayout(const QString &kdeLayoutId);
    // "qwerty", "azerty", "qwertz", or "" for a layout the language packs do not declare (Dvorak...).
    static QString physicalLayout(const QString &kdeLayoutId);
    // Letter keys measured by the layer ({letter,x,y,w,h}): true when every letter of the language has a key.
    Q_INVOKABLE bool readyForKeys(const QVariantList &keys) const;

    // --- swipe ----------------------------------------------------------------------------------------------
    Q_INVOKABLE int beginSwipe(); // generation, or -1 when swiping is not allowed now
    Q_INVOKABLE void finishSwipe(const QVariantList &points, const QVariantList &keys, int generation);

    // --- taps (typed by KDE) ----------------------------------------------------------------------------------
    Q_INVOKABLE void keyPressed(int key, const QString &text);
    Q_INVOKABLE void keyClicked(int key, const QString &text);
    // A key the layer took over while undoArmed/autoSpaceArmed. Always handles it (types it normally if no longer armed).
    Q_INVOKABLE void claimedKey(int key, const QString &text);
    // Spacebar slide: moves the text cursor by `steps` characters (negative = left), like that many arrow keys.
    Q_INVOKABLE void slideCursor(int steps);

    // --- suggestion row ---------------------------------------------------------------------------------------
    Q_INVOKABLE QString selectionToken() const;
    Q_INVOKABLE quint64 pressCandidate(const QString &token);
    Q_INVOKABLE void releaseCandidate(quint64 pressId);
    Q_INVOKABLE void selectCandidate(int index);
    Q_INVOKABLE bool canRemove(int index);
    Q_INVOKABLE bool removeCandidate(int index);
    Q_INVOKABLE void forgetEverythingLearned();

    Q_INVOKABLE void reset();

Q_SIGNALS:
    void readyChanged();
    void stateChanged();
    void candidatesChanged();
    void gestureInvalidated();
    void predictionWorkSettled(); // tests: the worker answered (or dropped) a request

private:
    struct Press {
        bool valid = false, predictSafe = false;
        int key = 0;
        QString text;
        EditorSnapshot before; // the field as it will be right before KDE types the key
        QString finishedWord; // Space: the typed word it finishes, learned only once the space was really typed
    };

    void loadLanguage(const QString &locale);
    EditorSnapshot editorSnapshot() const;
    EditorSnapshot effectiveSnapshot() const;
    void expectOwnState(const EditorSnapshot &after);
    void forgetOwnEdits();
    PredictionPolicy policy() const;
    bool safeContext() const;
    bool safePredictionContext() const;
    bool apply(const CommitPlan &plan);
    QString textBeforeCursor() const;
    CaseMode caseMode() const;
    bool predictionCapable() const;
    void editorChanged();
    void hintsChanged();
    void observePredictionEcho();
    void observeWordSwap(const EditorSnapshot &editor);
    bool tapSwapAllowed(const EditorSnapshot &editor) const;
    void requestContextCompletion(const EditorSnapshot &editor);
    void refreshTypedSuggestions(const EditorSnapshot &editor);
    void invalidatePredictions();
    void clearWordSwap();
    void clearSession();
    void clearAfterSwipe();
    void stageOwnedCommit(const CommitPlan &plan, bool correction = false);
    void refreshBar();
    void notifyState();
    PredictionJob job(const PredictionStamp &stamp, const QStringList &context, const QString &prefix = {}) const;

    // learning (inert unless <data>/learn-typing exists)
    swipekb::PersonalModel *personal();
    static QString personalKey(const QString &word);
    QString prevBeforeToken(const EditorSnapshot &editor, const QString &token) const;
    swipekb::PersonalModel::Undo learnWord(const QString &word, const QString &prev);
    void learnSwiped(const QString &word, const QString &prev);
    void retractLastLearn();
    QStringList personalize(const QStringList &base, const QString &prev, const QString &prefix, bool inject, int topK, double boostScale = 1.0);
    void syncLearnedWords(bool force = false);

    QPointer<EditorPort> m_editor;
    swipekb::ProfileSnapshot m_profile;
    std::unique_ptr<swipekb::LanguageEngine> m_engine;
    QString m_physicalLayout = QStringLiteral("qwerty");
    QString m_locale;
    quint64 m_epoch = 1; // profile epoch: changes with the language
    quint64 m_focusEpoch = 1; // field identity: changes with hints/focus (the policy's context id)
    int m_generation = 0;

    SwipeSession m_session;
    NextWordSession m_nextWord;
    OwnedHistory m_ownedHistory;
    TypedWordSession m_typedWord;
    WordSwapSession m_swap;
    CursorMoveTracker m_moves;
    RecentSwipes m_recentSwipes;
    NextWordWorker m_worker;
    Press m_press;
    QString m_typedPress, m_swapPress;
    std::optional<PredictionStamp> m_predictionTap;
    quint64 m_pressId = 0;
    PredictionStamp m_swapStamp;
    // Card #500 (one brain): swap lists come from the engine's swap search (swapflow.h), shown at once and re-ranked
    // when the worker's sentence scores arrive. One waiting list for tap-back, one for the bar right after a swipe.
    swapflow::PendingSwap m_swapPending, m_afterSwipePending;
    swapflow::Rules swapRules();
    // The key rectangles of the latest swipe (the current layout), for the made-up path of a typed word.
    RecentSwipes::Keys m_layoutKeys;
    // A finger is on the suggestion row (pressCandidate .. releaseCandidate). WHY: the row after a swipe has a fixed
    // token, so its list must not change under a pressed finger.
    bool m_barPressed = false;
    // Metadata for diagnostics/tests, never words (see diagnostics()).
    bool m_lastSwapUsedPath = false;
    int m_lastSwapWordsAfter = 0;
    void buildAfterSwipeList(quint64 sessionSerial, int generation, quint64 profileEpoch, const QString &first,
                             const RecentSwipes::Keys &keys, const QStringList &context, const RecentSwipes::Trace &trace,
                             const QString &otherSpelling, bool predictionField);
    bool afterSwipeScoring() const;
    int m_autoSpaceCursor = -1;
    bool m_afterSwipe = false;
    int m_afterSwipeCursorBefore = -1, m_afterSwipeCursorAfter = -1;
    // The field states this keyboard's own recent edits pass through (oldest first, the last is where the text is
    // going), and the words it typed in this sentence. WHY: some apps (Firefox on Plasma 6.9 dev) report each edit
    // one step late; a report that is one of these states is "not caught up yet", anything else is a real edit.
    QList<EditorSnapshot> m_ownStates;
    QStringList m_ownWords;
    QString m_nextPrev, m_completionPrev;
    QStringList m_lastBar;
    bool m_lastUndo = false, m_lastAutoSpace = false, m_lastField = false;
    quint64 m_swipesAnswered = 0;

    LearningManager m_learning;
    swipekb::PersonalModel::Undo m_lastLearn;
    const void *m_lastLearnModel = nullptr;
    QString m_lastLearnPrev;
    QSet<QString> m_injected;
    std::uint64_t m_syncedVersion = 0;
    const void *m_syncedModel = nullptr, *m_syncedEngine = nullptr;
};
