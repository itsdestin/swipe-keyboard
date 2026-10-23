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
#include "nextwordworker.h"
#include "languageprofile.h"
#include "predictiondiagnostics.h"
#include <QPointer>
#include <QQuickWindow>
#include <QQuickItem>
#include <QEvent>
#include <memory>
namespace swipekb { class Recognizer; class OwnSwipe; }

class SwipeInputMethod : public QVirtualKeyboardAbstractInputMethod {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject *predictionPolicySource READ predictionPolicySource WRITE setPredictionPolicySource)
    Q_PROPERTY(QVariantMap diagnostics READ diagnostics NOTIFY diagnosticsChanged)

public:
    explicit SwipeInputMethod(QObject *parent = nullptr);
    SwipeInputMethod(QObject *parent, NextWordWorker::Inference inference);
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
    std::shared_ptr<swipekb::Recognizer> m_recognizer;
    // The project's own swipe engine, only when the owner opted in (see ownswipe.h). FUTO's m_recognizer stays
    // loaded beside it: it is the fallback for any swipe the own engine cannot answer.
    std::shared_ptr<swipekb::OwnSwipe> m_own;
    // Counts only (never words): how many swipes each engine answered, shown in diagnostics.
    quint64 m_ownAnswered = 0, m_ownFellBack = 0;
    QString m_locale, m_layoutId, m_inputMode;
    void ensureContextConnections();
    EditorSnapshot predictionEditor() const;
    PredictionPolicy predictionPolicy() const;
    void invalidatePredictions();
    void observePredictionEcho();
    void requestContextCompletion(const EditorSnapshot &editor);
    bool predictionCapable() const;
    CaseMode caseMode() const;
    QStringList barCandidates() const;
    quint64 m_predictionPressId = 0;
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
