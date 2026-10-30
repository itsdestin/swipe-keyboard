// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "swipeinputmethod.h"

#include <cmath>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QtVirtualKeyboard/QVirtualKeyboardInputContext>

#include "swipekb/language_engine.hpp"
#include "swiperecorder.h"
#include "livehintsquery.h"
#include "predictionpolicy.h"
#include "languagetextrules.h"
#include "languageruntime.h"
#include "profilediagnostics.h"
#include "wordswap.h"
#include "barflow.h"
#include "swapflow.h"
#include <QPointer>
#include <QRegularExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QGuiApplication>
#ifdef SWIPEKB_PRIVATE_FLAGS_DIAGNOSTIC
#include <chrono>
#endif

// Enable with QT_LOGGING_RULES="swipekb.info=true" to see swipe counts and timing, never words.
Q_LOGGING_CATEGORY(lcSwipe, "swipekb", QtWarningMsg)

using Type = QVirtualKeyboardSelectionListModel::Type;

static std::vector<swipekb::KeyRect> measuredKeys(const QVariantList &keys)
{
    std::vector<swipekb::KeyRect> rects;
    for (const QVariant &v:keys) {
        const auto m=v.toMap();
        const auto symbols=swipekb::displayForm(m.value("letter").toString()).toLower().toUcs4();
        if (symbols.size()!=1) return {};
        rects.push_back({char32_t(symbols.front()),m.value("x").toFloat(),m.value("y").toFloat(),
            m.value("w").toFloat(),m.value("h").toFloat()});
    }
    return rects;
}
SwipeInputMethod::~SwipeInputMethod() = default;

void SwipeInputMethod::setPhysicalLayout(const QString &layoutId)
{
    if (layoutId==m_layoutId) return;
    m_layoutId=layoutId;
    resolveCurrentProfile();
}

SwipeInputMethod::SwipeInputMethod(QObject *parent) : SwipeInputMethod(parent, {}) {}

SwipeInputMethod::SwipeInputMethod(QObject *parent, NextWordWorker::Inference inference, NextWordWorker::Scorer scorer)
    : QVirtualKeyboardAbstractInputMethod(parent), m_learning([] { return SwipeRecorder::dataDir(); }),
      m_predictionWorker(std::move(inference), std::move(scorer))
{
    connect(this, &SwipeInputMethod::profileChanged, this, &SwipeInputMethod::diagnosticsChanged);
    connect(&m_predictionWorker, &NextWordWorker::modelStateChanged, this, [this](quint64 epoch,bool loaded) {
        if (epoch!=m_profiles.snapshot().epoch) return;
        m_predictionModelState=loaded ? swipekb::PredictionModelState::Loaded : swipekb::PredictionModelState::Unavailable;
        emit diagnosticsChanged();
    });
    connect(&m_predictionWorker, &NextWordWorker::requestSettled, this, &SwipeInputMethod::predictionWorkSettled);
    connect(&m_predictionWorker, &NextWordWorker::resultReady, this,
        [this](PredictionStamp stamp, const QStringList &words) {
            // WHY delivery checks live editor state as well as request stamps: the
            // editor may have changed before Qt delivered its update callback.
            QPointer<SwipeInputMethod> self(this);
            observePredictionEcho();
            if (!self) return;
            // WHY barflow::nextWordOffer (shared with the other front end and the typing bench): personal words,
            // no swear word out of the blue (unless the person uses it: "i swipe 'fuck' often"), regional order.
            auto *pm=personal();
            const auto typedByPerson=[pm](const QString &key) { return pm && pm->usedByPerson(key.toStdString()); };
            const auto offered=barflow::nextWordOffer(words, m_profiles.snapshot().effectiveLanguage,
                m_profiles.snapshot().requestedLocale,
                [this](const QStringList &l) { return personalize(l, m_nextPrev, QString(), true, 12); }, typedByPerson);
            if (m_nextWord.accept(stamp, offered)) refreshGuessBar();
        });
    connect(&m_predictionWorker, &NextWordWorker::wordScoresReady, this,
        [this](PredictionStamp stamp, const QStringList &words, const QVector<float> &scores) {
            // Card #500: the sentence model's scores for a shown swap list arrived; the list is re-ranked with them
            // (swapflow::handleScores, shared with the Plasma 6.9 front end).
            // WHY the same word and no finger down: a list that changes under a pressed finger would swap in a
            // word other than the one pressed. A stale answer (cursor moved on) is refused by its stamp.
            const bool tapBackOpen=m_swap.active() && stamp==m_swapStamp && m_swapPress.isEmpty() && !m_barPressed;
            const bool afterSwipeOpen=afterSwipeScoring() && !m_barPressed;
            const auto r=swapflow::handleScores(m_swapPending, m_afterSwipePending, tapBackOpen, afterSwipeOpen, stamp,
                                                words, scores, swapRules());
            if ((r.target==swapflow::ScoredList::TapBack && m_swap.setList(r.list))
                || (r.target==swapflow::ScoredList::AfterSwipe && m_session.replaceAlternatives(r.list)))
                refreshGuessBar();
        });
    connect(&m_predictionWorker, &NextWordWorker::completionReady, this,
        [this](PredictionStamp stamp, const QString &prefix, const QStringList &ranked) {
            QPointer<SwipeInputMethod> self(this);
            observePredictionEcho();
            if (!self || ranked.isEmpty() || !isReady()) return;
            // WHY the same unchanged word only, and never while a suggestion is held down: a list that
            // changes under the finger would type a different word than the one pressed.
            if (!m_typedPress.isEmpty() || m_typedWord.token()!=prefix || !(m_typedWord.stamp()==stamp)) return;
            // WHY barflow::completionOffer (shared with the other front end and the typing bench): sentence-ranked
            // words first, the dictionary's after, then the person's own words, cut to three.
            QStringList dictionary;
            for (const auto &candidate:m_engine->complete(prefix.toStdString(),12)) dictionary<<QString::fromStdString(candidate.word);
            const auto old=m_typedWord.candidates();
            m_typedWord.suggest(barflow::completionOffer(ranked, dictionary, m_profiles.snapshot().requestedLocale,
                [this,&prefix](const QStringList &l) { return personalize(l, m_completionPrev, prefix, true, 12); }));
            if (old!=m_typedWord.candidates()) refreshGuessBar();
        });
}

void SwipeInputMethod::resolveCurrentProfile()
{
    // WHY one profile authority: predictions consume the language-owned epoch/assets, never a
    // second English-only resolver that would erase the selected non-English swipe capability.
    const auto epoch=m_profiles.invalidate(m_locale,m_layoutId,m_inputMode);
    QPointer<SwipeInputMethod> self(this);
    reset();
    if (!self) return;
    m_engine.reset();
    m_layoutKeys.clear(); // WHY: another layout's key positions would give a typed word a wrong made-up path
    m_predictionModelState=swipekb::PredictionModelState::NotRequested;
    emit profileChanged();
    if (!self || m_profiles.snapshot().epoch!=epoch) return;
    // WHY the root rule and the profile-to-engine setup live in languageruntime.cpp: the Gaming Mode daemon
    // uses the very same functions, so both keyboards give the same words.
    const QString root=swipekb::languageModelRoot(m_locale,SwipeRecorder::modelDir());
    auto snapshot=swipekb::resolveProfile(m_locale,m_layoutId,m_inputMode,root,epoch);
    if (snapshot.capabilities.swipe) {
        swipekb::LanguageEngineSetup setup;
        swipekb::configureLanguageEngine(snapshot,setup);
        // WHY a fresh engine: never init() one already owned by a consumer/worker.
        auto engine=std::make_shared<swipekb::LanguageEngine>();
        if (engine->init(setup)) {
            // Joint-model step 2: a swipe with words before is re-ranked with the sentence model the guess bar uses,
            // borrowed from the next-word worker (no second copy in memory; see NextWordWorker::scoreForSwipe).
            // WHY the same files and checksums as a next-word job (applyNeuralNextWord): the worker's model is used
            // only when it is exactly this profile's; without a verified neural file this stays off.
            const auto model=snapshot.assets.value(QStringLiteral("contextModel"));
            const auto vocab=snapshot.assets.value(QStringLiteral("contextVocab"));
            const auto nwp=snapshot.assets.value(QStringLiteral("contextNwp"));
            PredictionJob like{{}, model.path, vocab.path, {}, model.sha256 + vocab.sha256 + nwp.sha256};
            like.nwp=nwp.path;
            engine->setContextRescore(swipeContextRescore(&m_predictionWorker, like, snapshot.effectiveLanguage, m_swipeRescore));
            if (m_profiles.snapshot().epoch==epoch) m_engine=std::move(engine);
        } else {
            snapshot.capabilities={}; snapshot.assets.clear(); snapshot.unavailableReason="Verified profile engine initialization failed";
        }
    }
    // WHY forced: a fresh engine has none of the taught words yet.
    // WHY the language is remembered here: the profile is only published after this sync, so the published snapshot
    // still holds the previous language while the new engines are being filled with words.
    m_wordsLanguage=snapshot.effectiveLanguage;
    syncLearnedWords(true);
    if (m_profiles.publish(snapshot)) emit profileChanged();
}

QList<QVirtualKeyboardInputEngine::InputMode> SwipeInputMethod::inputModes(const QString &)
{
    return {QVirtualKeyboardInputEngine::InputMode::Latin};
}

bool SwipeInputMethod::setInputMode(const QString &locale, QVirtualKeyboardInputEngine::InputMode mode)
{
    const QString inputMode=mode==QVirtualKeyboardInputEngine::InputMode::Latin ? "Latin" : "Unsupported";
    // WHY skip an unchanged mode: Qt repeats this call on every "?123"/letters switch (the symbols page
    // names its own mode). Rebuilding the profile reloaded the swipe model each time, a visible hitch
    // (owner, 2026-10-23). The current word is still cleared, exactly as the rebuild did.
    if (!m_locale.isEmpty() && locale==m_locale && inputMode==m_inputMode) {
        reset();
        return true;
    }
    m_locale=locale;
    m_inputMode=inputMode;
    resolveCurrentProfile();
    return true; // WHY unsupported swiping must not interfere with stock tapping.
}

bool SwipeInputMethod::setTextCase(QVirtualKeyboardInputEngine::TextCase textCase)
{
    m_textCase = textCase;
    return true;
}

void SwipeInputMethod::writeOutcome(const SwipeOutcomeTracker::Outcome &o)
{
    // WHY the tracker only holds recorded swipes: an outcome always points at a swipe line that exists.
    if (o)
        SwipeRecorder::appendOutcomeIfEnabled(SwipeRecorder::dataDir(), QDateTime::currentMSecsSinceEpoch(),
                                              o.kind == SwipeOutcomeTracker::Kind::Erase ? "erase" : "keep",
                                              o.swipeTs);
}

bool SwipeInputMethod::keyEvent(Qt::Key key, const QString &text, Qt::KeyboardModifiers modifiers)
{
    // WHY: stale hints must not authorize spacing/context reads, predictions or recording.
    // Refusal leaves stock single-key handling untouched; only swipe-owned state is revoked.
    QPointer<SwipeInputMethod> alive(this);
    ensureContextConnections();
    if (!alive) return false;
    // WHY before any snapshot below: a missed echo of the previous key would make this key's
    // expected echo wrong, so the word under the cursor could never be confirmed.
    syncInputCache();
    if (!alive) return false;
    const bool allowed = safeContext();
    if (!alive) return false;
    if (!allowed) { reset(); return false; }
    const bool predictSafe=safePredictionContext();
    // WHY first: any key ends "alternatives for the tapped word" (typing returns the bar to normal), and
    // the cursor move and edit this key causes must not look like the person tapping into a word.
    clearWordSwap();
    if (!alive) return false;
    if (!text.isEmpty() || key==Qt::Key_Backspace || key==Qt::Key_Delete || key==Qt::Key_Return
        || key==Qt::Key_Enter || key==Qt::Key_Tab) m_moves.ownEdit();
    const int autoSpace=m_autoSpaceCursor; m_autoSpaceCursor=-1;
    // WHY: "word" + automatic space + "." should read "word. ", not "word .". Only for the space this
    // keyboard just added, with the cursor still right after it in a readable, allowed field.
    if (predictSafe && autoSpace>0 && modifiers==Qt::NoModifier && text.size()==1
        && QStringLiteral(".,!?;:").contains(text)) {
        const auto editor=predictionEditor();
        if (editor.cursor==autoSpace && editor.cursor==editor.anchor && editor.cursor<=editor.text.size()
            && editor.text.at(editor.cursor-1)==QLatin1Char(' ')) {
            QPointer<QVirtualKeyboardInputContext> ic(inputContext());
            if (!ic) return false;
            reset();
            if (!alive || !ic) return true;
            m_moves.ownEdit(); /* WHY: our own typing is not a tap (card #187) */ ic->commit(text+QLatin1Char(' '),-1,1);
            if (alive) m_autoSpaceCursor=editor.cursor+1;
            return true;
        }
    }
    // WHY accept only an unmodified key or a single Shift-letter for private history: stock
    // sentence auto-capitalization supplies Shift on the first character of Hello/Ich/etc.
    const bool shiftedLetter=modifiers==Qt::ShiftModifier && text.size()==1 && text.front().isLetter();
    const bool predictionText=modifiers==Qt::NoModifier || shiftedLetter;
    if (!alive) return false;
    if (predictSafe) {
        const auto editor=predictionEditor();
        m_ownedHistory.observe(editor,predictionPolicy(),m_profiles.snapshot().epoch);
        m_typedWord.observe(editor,predictionPolicy(),m_profiles.snapshot().epoch);
    } else { m_ownedHistory.clear(); m_typedWord.clear(); }
    m_typedPress.clear();
    invalidatePredictions();
    if (!alive) return false;
    if (predictSafe && key == Qt::Key_Space && modifiers == Qt::NoModifier && text == QStringLiteral(" ")) {
        const auto unfinished=m_typedWord.token();
        m_typedWord.clear();
        const auto previous = predictionEditor();
        // Learning: Space finishes the word just typed on this keyboard (never a pasted or preexisting one: the
        // typed-word session only holds letters whose echo this keyboard confirmed).
        if(!unfinished.isEmpty()) {
            learnWord(unfinished, prevBeforeToken(previous, unfinished));
            if(!alive) return false;
        }
        // WHY backspace conservatively drops sentence history, but an exact-echo
        // confirmed owned unfinished token remains valid input when Space finalizes it.
        if(!unfinished.isEmpty() && predictionPolicy().source==PredictionPolicySource::OwnedHistory) {
            // WHY remembered here: in stock Plasma fields tap-to-swap may only offer words this keyboard typed, and
            // this word's letters were all confirmed as its own typing (the typed-word session).
            if (tapSwapAllowed(previous) && previous.cursor>=unfinished.size()
                && previous.text.left(previous.cursor).endsWith(unfinished)) {
                auto context=m_ownedHistory.context();
                if (!context.isEmpty() && context.last()==unfinished.toLower()) context.removeLast();
                m_recentSwipes.remember(previous.text.left(previous.cursor-unfinished.size()), unfinished, {}, {}, context); // typed: no path
            }
            m_ownedHistory.word(previous,previous,predictionPolicy(),m_profiles.snapshot().epoch,unfinished,true);
            m_ownedHistory.observe(previous,predictionPolicy(),m_profiles.snapshot().epoch);
        }
        auto expected = previous;
        if (expected.cursor >= 0 && expected.cursor <= expected.text.size()) {
            expected.text.insert(expected.cursor, text);
            expected.cursor += text.size(); expected.anchor = expected.cursor;
        }
        QPointer<QVirtualKeyboardInputContext> context(inputContext());
        QPointer<QObject> item(context ? context->inputItem() : nullptr);
        const int generation=m_generation;
        if (m_session.isActive()) writeOutcome(m_outcome.keyTapped(false, text));
        else m_outcome.abandon();
        clearSession();
        if (!alive) return false;
        if (!context || !item || context!=inputContext() || item!=context->inputItem()
            || generation!=m_generation) return false;
        clearAfterSwipe(); m_lastRecordedSwipeTs = 0;
        m_ownedHistory.space(previous,expected,predictionPolicy(),m_profiles.snapshot().epoch);
        m_nextWord.armExpectedEcho(expected, predictionPolicy(), m_profiles.snapshot().epoch, predictionCapable(), previous);
        // WHY false: Qt types Space normally. Prediction waits for that exact editor echo.
        return false;
    }
    if (key == Qt::Key_Backspace) m_ownedHistory.clear();
    if (key == Qt::Key_Backspace && modifiers == Qt::NoModifier && m_session.isActive()) {
        QPointer<QVirtualKeyboardInputContext> ic(inputContext());
        QPointer<QObject> item(ic ? ic->inputItem() : nullptr);
        const int generation = m_generation;
        const auto outcome = m_outcome.keyTapped(true, text);
        const CommitPlan plan = m_session.takeBackspace(textBeforeCursor(), false);
        // Learning: erasing the whole swiped word says it was wrong, so its count is taken back.
        if (!plan.isEmpty()) retractLastLearn();
        refreshGuessBar();
        // WHY: candidate notifications may synchronously move focus/reset policy while leaving
        // this method alive. Consume this stale Backspace, never replay a word/stock deletion into
        // the newly current target, and never record an outcome for an unapplied saved plan.
        if (!alive) return true;
        if (!ic || !item || ic != inputContext() || item != ic->inputItem()
            || generation != m_generation) { reset(); return true; }
        const bool stillSafe = safeContext();
        if (!alive) return true;
        if (!stillSafe) { reset(); return true; }
        clearAfterSwipe();
        writeOutcome(outcome);
        // For an unchanged safe target, consume an applied word deletion; an empty plan retains
        // ordinary stock Backspace. Invalidated notifications above instead consume without editing.
        if (!plan.isEmpty()) {
            m_moves.ownEdit(); /* WHY: our own typing is not a tap (card #187) */ ic->commit(plan.text, plan.replaceFrom, plan.replaceLength);
            return true;
        }
        return false;
    }
    // Gboard-style: tapping a letter or digit right after a swiped word starts a new word, so type the space
    // first ("write" + tap "a" = "write a"). WHY checked before anything is cleared: "right after the swiped
    // word" is exactly the session (cursor still behind the word) or, in apps that hide their text, the
    // after-swipe flag. The rule itself (which keys, URL/email fields) is SwipeSession::spaceBeforeTappedKey.
    // The key itself is still typed by Qt (we return false below), right after our space.
    if (QVirtualKeyboardInputContext *ic = inputContext()) {
        const bool rightAfterWord = (m_session.isActive() && m_session.cursorStillAfterWord(textBeforeCursor()))
            || m_afterSwipe;
        const bool urlOrEmail = ic->inputMethodHints() & (Qt::ImhUrlCharactersOnly | Qt::ImhEmailCharactersOnly);
        if (key != Qt::Key_Backspace && SwipeSession::spaceBeforeTappedKey(text, rightAfterWord, urlOrEmail)) {
            QPointer<QVirtualKeyboardInputContext> context(ic);
            QPointer<QObject> item(ic->inputItem());
            const int generation = m_generation;
            m_moves.ownEdit(); /* WHY: our own typing is not a tap (card #187) */ ic->commit(QStringLiteral(" "));
            if (!alive) return false;
            if (!context || !item || context != inputContext() || item != context->inputItem()
                || generation != m_generation) { reset(); return false; }
        }
    }
    // WHY before clearSession: the outcome only counts while the cursor is still right after the swiped word.
    // Backspace itself is untouched (returns false below, so Qt deletes one character as before).
    if (m_session.isActive())
        writeOutcome(m_outcome.keyTapped(key == Qt::Key_Backspace, text));
    else
        m_outcome.abandon();
    // Learning: a word ended by punctuation or Enter counts like one ended by Space (the typed-word session below
    // forgets it on this key).
    if (predictSafe && !m_typedWord.token().isEmpty() && modifiers==Qt::NoModifier
        && (key==Qt::Key_Return || key==Qt::Key_Enter || (text.size()==1 && QStringLiteral(".,!?;:").contains(text)))) {
        const auto token=m_typedWord.token();
        learnWord(token, prevBeforeToken(predictionEditor(), token));
        if (!alive) return false;
    }
    if (predictSafe && predictionText) {
        const auto previous=predictionEditor();auto expected=previous;
        if(modifiers==Qt::NoModifier && key==Qt::Key_Backspace && !m_typedWord.token().isEmpty() && expected.cursor>0) {
            expected.text.remove(expected.cursor-1,1);--expected.cursor;expected.anchor=expected.cursor;
            m_typedWord.edit(previous,expected,predictionPolicy(),m_profiles.snapshot().epoch,{},true);
        } else if(text.size()==1 && (text.front().isLetter() || text=="'" || text=="’" || text=="-")) {
            expected.text.insert(expected.cursor,text);expected.cursor+=text.size();expected.anchor=expected.cursor;
            m_typedWord.edit(previous,expected,predictionPolicy(),m_profiles.snapshot().epoch,text);
        } else m_typedWord.clear();
    } else m_typedWord.clear();
    if (predictSafe && predictionPolicy().source==PredictionPolicySource::OwnedHistory) {
        if (key!=Qt::Key_Backspace && predictionText && text.size()==1 && text.front().isLetter()) {
            const auto previous=predictionEditor(); auto expected=previous;
            if (expected.cursor>=0 && expected.cursor<=expected.text.size()) {
                expected.text.insert(expected.cursor,text); expected.cursor+=text.size(); expected.anchor=expected.cursor;
                m_ownedHistory.tap(previous,expected,predictionPolicy(),m_profiles.snapshot().epoch,text);
            }
        } else m_ownedHistory.clear();
    }
    // Any tapped key ends the swiped word's session (the guess bar no longer applies); false = Qt types it.
    clearSession();
    if (!alive) return false;
    // WHY: after a tapped key the last thing typed is no longer a swiped word, so the "hidden text" space
    // rule must stop (tapping space then swiping must not give two spaces).
    clearAfterSwipe();
    return false;
}

QList<Type> SwipeInputMethod::selectionLists() { return {Type::WordCandidateList}; }

int SwipeInputMethod::selectionListItemCount(Type type)
{
    return type == Type::WordCandidateList ? barCandidates().size() : 0;
}

QVariant SwipeInputMethod::selectionListData(Type type, int index, QVirtualKeyboardSelectionListModel::Role role)
{
    const QStringList alts = barCandidates();
    if (type != Type::WordCandidateList || index < 0 || index >= alts.size())
        return {};
    switch (role) {
    case QVirtualKeyboardSelectionListModel::Role::Display:
        return alts.at(index);
    case QVirtualKeyboardSelectionListModel::Role::WordCompletionLength:
        return 0;
    case QVirtualKeyboardSelectionListModel::Role::Dictionary:
        // WHY "User": Qt then words the long-press menu as "Remove from dictionary" (its "Block word" is for built-ins).
        return int(QVirtualKeyboardSelectionListModel::DictionaryType::User);
    case QVirtualKeyboardSelectionListModel::Role::CanRemoveSuggestion: {
        // WHY only learned words: a long press offers "remove" where the person's own typing put the word, so the
        // menu never appears for ordinary dictionary words and cannot hide them.
        auto *pm = personal();
        return pm && pm->removable(personalKey(alts.at(index)).toStdString());
    }
    default:
        return {};
    }
}

void SwipeInputMethod::selectionListItemSelected(Type type, int index)
{
    QPointer<QVirtualKeyboardInputContext> ic(inputContext());
    if (type != Type::WordCandidateList)
        return;
    if (!m_typedWord.candidates().isEmpty()) {
        QPointer<SwipeInputMethod> self(this);
        if(!safePredictionContext() || !self || !ic) {qCInfo(lcSwipe)<<"typed tap refused: field not allowed";if(self)reset();return;}
        const bool pressed=!m_typedPress.isEmpty(), sameSet=m_typedPress==m_typedWord.selectionToken();
        const auto previous=predictionEditor();
        const QString prevForLearning=prevBeforeToken(previous, m_typedWord.token());
        const auto plan=m_typedWord.take(index,m_typedPress,previous,predictionPolicy(),m_profiles.snapshot().epoch);
        m_typedPress.clear();
        // WHY metadata only (never words): tells a failed device test which gate refused the tap.
        if(plan.isEmpty()) qCInfo(lcSwipe)<<"typed tap refused: press"<<pressed<<"same set"<<sameSet<<"index"<<index;
        if(!plan.isEmpty()) {
            QPointer<QObject> item(ic->inputItem());const int generation=m_generation;
            // WHY arm like Space: the accepted word now ends with its automatic space, so the
            // next-word bar should follow that exact echo just as it does after a typed Space.
            if(plan.text.endsWith(QLatin1Char(' ')) && previous.cursor+plan.replaceFrom>=0) {
                auto expected=previous;
                expected.text.replace(previous.cursor+plan.replaceFrom,plan.replaceLength,plan.text);
                expected.cursor=previous.cursor+plan.replaceFrom+plan.text.size();expected.anchor=expected.cursor;
                m_nextWord.armExpectedEcho(expected,predictionPolicy(),m_profiles.snapshot().epoch,predictionCapable(),previous);
            }
            stageOwnedCommit(plan,true);
            if(!self || !ic || !item || generation!=m_generation)return;
            m_moves.ownEdit(); /* WHY: our own typing is not a tap (card #187) */ ic->commit(plan.text,plan.replaceFrom,plan.replaceLength);
            if(!self)return;
            if(!ic || !item || ic!=inputContext() || item!=ic->inputItem() || generation!=m_generation){reset();return;}
            m_autoSpaceCursor=plan.text.endsWith(QLatin1Char(' ')) ? previous.cursor+plan.replaceFrom+int(plan.text.size()) : -1;
            observePredictionEcho();
            if(!self)return;
            // Learning: the person chose this word (a completion, or their own spelling kept as typed).
            learnWord(plan.text.trimmed(), prevForLearning);
            if(!self)return;
        }
        refreshGuessBar();return;
    }
    if (m_swap.active() && !m_session.isActive()) {
        // Tap-to-swap (card #187): replaces exactly the tapped word, adds no space, moves nothing else.
        QPointer<SwipeInputMethod> self(this);
        const bool safe=safePredictionContext();
        if (!self || !ic || ic!=inputContext()) return;
        const auto editor=predictionEditor();
        if (!safe || !tapSwapAllowed(editor)) { reset(); return; }
        const auto pressed=m_swapPress; m_swapPress.clear();
        const QString oldWord=m_swap.word(); const int oldStart=m_swap.start();
        const auto owned=m_recentSwipes.ownedAt(editor.text, editor.cursor);
        const auto plan=m_swap.take(index,pressed,editor.text,editor.cursor);
        if (plan.isEmpty()) { qCInfo(lcSwipe)<<"swap tap refused: press"<<!pressed.isEmpty()<<"index"<<index; clearWordSwap(); refreshGuessBar(); return; }
        m_recentSwipes.forget(editor.text.left(oldStart), oldWord);
        // WHY remembered again: the swapped-in word is the keyboard's own too, so tapping it again still works
        // (in stock Plasma fields that is the only way it can be offered again).
        if (owned && owned->span.start==oldStart)
            // WHY the same path: the swapped-in word came from the same finger stroke, so a second tap re-scores it.
            m_recentSwipes.remember(editor.text.left(oldStart), plan.text, owned->trace, owned->keys, owned->context);
        QPointer<QObject> item(ic->inputItem()); const int generation=m_generation;
        m_predictionWorker.invalidate();
        m_outcome.abandon(); m_lastRecordedSwipeTs=0; clearAfterSwipe();
        m_moves.ownEdit(); /* WHY: the replacement is our edit, not a tap */
        ic->commit(plan.text, plan.replaceFrom, plan.replaceLength);
        if (!self) return;
        if (!ic || !item || ic!=inputContext() || item!=ic->inputItem() || generation!=m_generation) { reset(); return; }
        observePredictionEcho();
        if (!self) return;
        refreshGuessBar();
        return;
    }
    if (!m_session.isActive() && !m_nextWord.candidates().isEmpty()) {
        // WHY a prediction insertion has the same live/privacy/deletion boundary as a swipe commit.
        QPointer<SwipeInputMethod> self(this);
        const bool safe=safePredictionContext();
        if (!self || !ic || ic!=inputContext()) return;
        if (!safe) { reset(); return; }
        QPointer<QObject> item(ic->inputItem());
        const int generation=m_generation;
        const auto previous = predictionEditor();
        const auto tap = m_predictionTap;
        m_predictionTap.reset();
        auto plan = tap ? m_nextWord.takePrediction(index, *tap, previous, predictionPolicy(), m_profiles.snapshot().epoch, caseMode()) : CommitPlan{};
        // WHY (owner decision, 0.3.1): a picked prediction ends with a space, ready for the next word.
        if (!plan.isEmpty() && (previous.cursor >= previous.text.size() || !previous.text.at(previous.cursor).isSpace()))
            plan.text += QLatin1Char(' ');
        if (plan.isEmpty()) qCInfo(lcSwipe) << "prediction tap refused: press" << tap.has_value() << "index" << index;
        if (!tap) m_nextWord.invalidate();
        m_predictionWorker.invalidate();
        clearSession(); clearAfterSwipe(); m_outcome.abandon(); m_lastRecordedSwipeTs = 0;
        if (!plan.isEmpty() && ic) {
            auto expected = previous;
            expected.text.insert(expected.cursor, plan.text);
            expected.cursor += plan.text.size(); expected.anchor = expected.cursor;
            m_nextWord.armExpectedEcho(expected, predictionPolicy(), m_profiles.snapshot().epoch, predictionCapable(), previous);
            stageOwnedCommit(plan);
            if (!self || !ic || !item || generation!=m_generation) return;
            // WHY explicit zeros: a prediction must never enter the correction replacement path.
            m_moves.ownEdit(); /* WHY: our own typing is not a tap (card #187) */ ic->commit(plan.text, 0, 0);
            if (!self) return;
            if (!ic || !item || ic!=inputContext() || item!=ic->inputItem()
                || generation!=m_generation) { reset(); return; }
            m_autoSpaceCursor = plan.text.endsWith(QLatin1Char(' ')) ? previous.cursor + int(plan.text.size()) : -1;
            observePredictionEcho();
            if (!self) return;
            // Learning: an accepted suggestion is a word the person chose to write after the previous one.
            learnWord(plan.text.trimmed(), m_nextPrev);
            if (!self) return;
        }
        refreshGuessBar();
        return;
    }
    // WHY: the swap deletes the typed word before the cursor. If we can't see that word there (the app hides
    // its text, or the cursor moved), the delete would hit nothing or the wrong text and the guess would just
    // be appended ("helloyellow"). Doing nothing is safer; the guess bar closes.
    QPointer<SwipeInputMethod> self(this);
    const bool safe = safeContext();
    if (!self) return;
    if (!ic || ic != inputContext()) return;
    if (!safe || !m_session.cursorStillAfterWord(textBeforeCursor())) {
        reset();
        return;
    }
    const CommitPlan plan = m_session.swapToAlternative(index);
    if (!plan.isEmpty()) {
        QPointer<QObject> item(ic->inputItem());
        const int generation = m_generation;
        stageOwnedCommit(plan,true);
        if (!self || !ic || !item || generation!=m_generation) return;
        m_moves.ownEdit(); /* WHY: our own typing is not a tap (card #187) */ ic->commit(plan.text, plan.replaceFrom, plan.replaceLength);
        // WHY: commit sends a synchronous public event; its handler may delete this method,
        // replace the target or reset policy. Check the snapshot before any member access/call.
        if (!self) return;
        if (!ic || !item || ic != inputContext() || item != ic->inputItem()
            || generation != m_generation) { reset(); return; }
        // WHY: commit may synchronously change purpose; recording must query again, not reuse the cache.
        const bool recordSafe = safeContext();
        if (!self) return;
        if (!recordSafe) { reset(); return; }
        m_outcome.picked();   // WHY: a pick is this swipe's one outcome; no erase/keep line may follow
        // Learning: the word they picked is the word they meant; the first guess's count is taken back.
        retractLastLearn();
        learnSwiped(plan.text.trimmed(), m_lastLearnPrev);
        if (!self) return;
        // Opt-in recording: the word the owner picked is the word they meant for that swipe (see swiperecorder.h).
        if (m_lastRecordedSwipeTs)
            SwipeRecorder::appendPickIfEnabled(SwipeRecorder::dataDir(), QDateTime::currentMSecsSinceEpoch(),
                                               m_lastRecordedSwipeTs, plan.text);
    }
    refreshGuessBar();
}

void SwipeInputMethod::reset()
{
    ++m_nativePolicyEpoch;
    m_autoSpaceCursor=-1;
    m_barPressed=false; // WHY: a press on another field's bar never ends here; it must not freeze this field's bar
    m_ownedHistory.clear();m_typedWord.clear();m_typedPress.clear();
    // WHY prediction-bar notifications are synchronous deletion boundaries too.
    QPointer<SwipeInputMethod> self(this);
    clearWordSwap();
    if (!self) return;
    invalidatePredictions();
    if (!self) return;
    // WHY: points collected before a focus/privacy/mode reset must never authorize a later commit.
    m_generation = (m_generation + 1) % 1000000000;
    emit gestureInvalidated();
    if (!self) return;
    m_outcome.abandon();   // WHY: focus moved; the swipe's fate is unknown
    m_lastRecordedSwipeTs=0; // WHY a profile/editor reset must not retain an old correction association.
    clearSession();
    if (!self) return;
    // WHY: Qt resets on focus moves and field-type changes; whatever was typed before belongs to another field.
    clearAfterSwipe();
    emit diagnosticsChanged();
}

void SwipeInputMethod::update()
{
    QPointer<SwipeInputMethod> self(this);
    observePredictionEcho();
    if (!self) return;
    // Qt calls this when the app reports a new text or cursor position. That includes the app echoing back
    // a word we just committed: the echo arrives later, asynchronously, not during our commit. Keep the
    // guess bar only while the cursor still sits right after the swiped word.
    const bool safe = safeContext();
    if (!self) return;
    if (!safe) {
        reset();
        return;
    }
    const QString before = textBeforeCursor();
    if (m_session.isActive() && !m_session.cursorStillAfterWord(before)) {
        m_outcome.abandon();   // WHY: cursor moved away; we can't tell if the word was right, so no outcome
        clearSession();
        if (!self) return;
    }
    // WHY: only genuinely hidden buffers need spacing memory. Text after the cursor and an active
    // selection also prove an edit; select-all/delete must not leave a stale word before the next swipe.
    if (m_afterSwipe) {
        QVirtualKeyboardInputContext *ic = inputContext();
        if (!ic || !SwipeSession::keepHiddenTextSpaceFallback(ic->surroundingText(), ic->cursorPosition(),
                ic->anchorPosition(), m_afterSwipeCursorBefore, m_afterSwipeCursorAfter))
            clearAfterSwipe();
    }
}

bool SwipeInputMethod::isReady() const { return m_profiles.snapshot().capabilities.swipe && m_engine && m_engine->ready(); }

bool SwipeInputMethod::isReadyForLayout(const QVariantList &keys) const
{
    if (!isReady()) return false;
    const auto language=m_profiles.snapshot().effectiveLanguage;
    // WHY check before QML cancels a stock key press: an incomplete alphabet must remain ordinary tapping.
    return swipekb::normalize({{0,0,0},{1,1,1}},measuredKeys(keys),swipekb::languageAlphabet(language).toStdString(),
        swipekb::popupAliasesFor(language)).has_value();
}

QString SwipeInputMethod::textBeforeCursor() const
{
    QVirtualKeyboardInputContext *ic = inputContext();
    return ic ? ic->surroundingText().left(ic->cursorPosition()) : QString();
}

bool SwipeInputMethod::safeContext()
{
    // WHY: the backend proxy's live flags can already be sensitive while Qt's cache is ordinary.
    // Keep the cached veto too; querying must not refresh/mutate that cache or authorize reentrancy.
    if (m_liveQueryActive) {
        // Reentrant authorization cannot lend the outer query a still-valid generation either.
        m_generation = (m_generation + 1) % 1000000000;
        return false;
    }
    QPointer<SwipeInputMethod> self(this);
    QPointer<QVirtualKeyboardInputContext> ic(inputContext());
    QPointer<QObject> item(ic ? ic->inputItem() : nullptr);
    const auto privateHints = Qt::ImhSensitiveData | Qt::ImhHiddenText;
    if (!ic || !item || (ic->inputMethodHints() & privateHints)) return false;
    const int generation = m_generation;
    m_liveQueryActive = true;
    const auto live = queryLiveHints(item);
    if (!self) return false;
    m_liveQueryActive = false;
    return ic && item && ic == inputContext() && item == ic->inputItem()
        && generation == m_generation && live.alive && live.valid
        && !(live.hints & privateHints) && !(ic->inputMethodHints() & privateHints)
        && ic->inputEngine() && ic->inputEngine()->inputMethod() == this && ic->cursorPosition() >= 0
        && ic->cursorPosition() == ic->anchorPosition() && ic->selectedText().isEmpty();
}

bool SwipeInputMethod::validateSwipe(int generation)
{
    QPointer<SwipeInputMethod> self(this);
    const bool safe = generation >= 0 && generation == m_generation && safeContext();
    if (!self) return false;
    if (!safe) reset(); // revoke permanently even if the same proxy becomes ordinary again
    return safe;
}

#ifdef SWIPEKB_PRIVATE_FLAGS_DIAGNOSTIC
bool SwipeInputMethod::observeFlags(const char *phase)
{
    // WHY: do not refresh Qt's cache or alter a decision while measuring it. Public query callbacks
    // may reenter/reset/delete; preserve lifetimes and report invalidation as flags, never dereference
    // a deleted proxy or expose its address, text, key path, coordinates or candidate content.
    if (m_diagnosticQueryActive) return true;
    m_diagnosticQueryActive = true;
    QPointer<SwipeInputMethod> self(this);
    QPointer<QVirtualKeyboardInputContext> context(inputContext());
    QPointer<QObject> item(context ? context->inputItem() : nullptr);
    static QPointer<QObject> previousContext;
    static QPointer<QObject> previousItem;
    static quint64 nextContext = 0, nextItem = 0;
    if (previousContext != context || !m_diagnosticContext) {
        previousContext = context; m_diagnosticContext = ++nextContext;
    }
    if (previousItem != item || !m_diagnosticItem) {
        previousItem = item; m_diagnosticItem = ++nextItem;
    }
    const int before = context ? int(context->inputMethodHints()) : -1;
    const int generation = m_generation;
    const LiveHintsQuery live = queryLiveHints(item);
    if (!self) return false;
    const bool stable = context && context == inputContext() && item
        && item == context->inputItem();
    const int after = context ? int(context->inputMethodHints()) : -1;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    qWarning().nospace() << "SWIPE_FLAGS phase=" << phase << " mono_ns=" << ns
        << " context_id=" << m_diagnosticContext << " item_id=" << m_diagnosticItem
        << " generation_before=" << generation << " generation_after=" << m_generation
        << " cached_before=" << before << " cached_after=" << after
        << " live=" << live.hints << " valid=" << live.valid << " alive=" << live.alive
        << " stable=" << stable << " recognition_count=" << m_diagnosticRecognitionCount
        << " commit_length_count=" << m_diagnosticCommitLength;
    m_diagnosticQueryActive = false;
    return true;
}
#endif

int SwipeInputMethod::beginSwipe()
{
#ifdef SWIPEKB_PRIVATE_FLAGS_DIAGNOSTIC
    if (!observeFlags("capture")) return -1;
#endif
    QPointer<SwipeInputMethod> self(this);
    clearWordSwap(); // WHY: swiping returns the bar to normal
    if (!self) return -1;
    invalidatePredictions();
    if (!self) return -1;
    ensureContextConnections();
    if (!self || !isReady()) return -1;
    if (!safeContext() || !self) return -1;
    m_generation = (m_generation + 1) % 1000000000;
    return m_generation;
}

void SwipeInputMethod::finishSwipe(const QVariantList &points, const QVariantList &keys, int generation)
{
#ifdef SWIPEKB_PRIVATE_FLAGS_DIAGNOSTIC
    if (!observeFlags("release")) return;
#endif
    QPointer<SwipeInputMethod> self(this);
    QPointer<QVirtualKeyboardInputContext> ic(inputContext());
    // WHY before recognition/context extraction/logging: live sensitivity, invalid queries and
    // reentrant resets must revoke the held path even when cached hints did not change.
    if (!validateSwipe(generation) || !self || !ic) return;
    m_generation = (m_generation + 1) % 1000000000; // consume once, even when the trace is rejected
    if (points.size() < 2 || points.size() > 2048 || keys.size() > 64 || !isReady())
        return;
    const double duration = points.last().toMap().value("t").toDouble();
    if (!std::isfinite(duration) || duration < 0 || duration > 10000)
        return;
    QElapsedTimer timer;
    timer.start();

    std::vector<swipekb::TracePoint> trace;
    trace.reserve(points.size());
    for (const QVariant &v : points) {
        const QVariantMap m = v.toMap();
        trace.push_back({m.value("x").toFloat(), m.value("y").toFloat(), m.value("t").toFloat()});
    }
    const auto rects=measuredKeys(keys);
    m_typedWord.clear();m_typedPress.clear();

    if (predictionPolicy().source==PredictionPolicySource::OwnedHistory) {
        const bool predictionSafe=safePredictionContext();
        if (!self || !ic) return;
        if (predictionSafe) m_ownedHistory.observe(predictionEditor(),predictionPolicy(),m_profiles.snapshot().epoch);
        else m_ownedHistory.clear();
    }
    const QString before = textBeforeCursor();
    // WHY native stock cannot attest raw completion policy. Never feed its editor
    // buffer into either model; only confirmed keyboard-owned history may rank words.
    const bool nativeHistory=qEnvironmentVariable("SWIPEKB_NATIVE_PREDICTION")==QStringLiteral("owned-history-v1")
        && predictionPolicy().source!=PredictionPolicySource::RawBridge;
    const QStringList contextWords = nativeHistory ? m_ownedHistory.context() : swipekb::textContextWords(before);
    std::vector<std::string> context;
    for (const QString &w : contextWords)
        context.push_back(w.toStdString());

    const auto snapshot=m_profiles.snapshot();
    const auto engine=m_engine;
    QStringList candidates;
#ifdef SWIPEKB_PRIVATE_FLAGS_DIAGNOSTIC
    ++m_diagnosticRecognitionCount;
    qWarning().nospace() << "SWIPE_FLAGS phase=recognition generation=" << m_generation
        << " recognition_count=" << m_diagnosticRecognitionCount;
#endif
    // WHY regional policy precedes truncation; a wide beam (30) keeps both regional spellings eligible.
    for (const auto &c : engine->recognize(trace, rects, context, 30)) {
        // WHY skip empty: an empty guess would type nothing yet still open a session and a blank guess-bar entry.
        if (!c.word.empty())
            candidates << QString::fromStdString(c.word);
    }
    const QString rawShape=QString::fromStdString(engine->bestShapeWord());
    // WHY half-strength boosts: the finger path is strong evidence, so the person's habits only break near-ties
    // and lift a word a couple of places; the raw-shape guard below still keeps the best path match visible.
    const QString swipePrev=contextWords.isEmpty() ? QString() : contextWords.last();
    candidates=personalize(candidates, swipePrev, QString(), false, int(candidates.size()), 0.5);
    candidates=swipekb::rankRegionalCandidates(candidates,snapshot.requestedLocale,3,rawShape);
    if (m_profiles.snapshot().epoch!=snapshot.epoch) return;

    // WHY: the swipe shape of "its" is also "it's" (apostrophes are not swiped); put the dictionary's other
    // spelling of the best guess first in the guess bar so switching is one tap.
    QString otherSpelling;
    if (!candidates.isEmpty()) {
        const auto others = engine->otherSpellings(candidates.front().toStdString());
        if (!others.empty()) {
            otherSpelling = QString::fromStdString(others.front());
            candidates = SwipeSession::withOtherSpelling(candidates, otherSpelling);
        }
    }

    const CaseMode mode = ic->isCapsLockActive() ? CaseMode::AllCaps
        : m_textCase == QVirtualKeyboardInputEngine::TextCase::Upper ? CaseMode::Capitalize
                                                                       : CaseMode::AsIs;
    // WHY: in web-address and email fields "." "@" "/" are part of the address, so no auto-space after them.
    const bool urlOrEmail = ic->inputMethodHints() & (Qt::ImhUrlCharactersOnly | Qt::ImhEmailCharactersOnly);
    const CommitPlan plan = m_session.commitSwipe(before, candidates, mode,
                                                  {.afterSwipe = m_afterSwipe, .urlOrEmailField = urlOrEmail});
    if (!plan.isEmpty()) {
        const int cursorBefore = ic->cursorPosition();
#ifdef SWIPEKB_PRIVATE_FLAGS_DIAGNOSTIC
        m_diagnosticCommitLength += quint64(plan.text.size());
        qWarning().nospace() << "SWIPE_FLAGS phase=commit generation=" << m_generation
            << " commit_length_count=" << m_diagnosticCommitLength;
#endif
        QPointer<QObject> item(ic->inputItem());
        const int generation = m_generation;
        stageOwnedCommit(plan);
        if (!self || !ic || !item || generation!=m_generation) return;
        m_moves.ownEdit(); /* WHY: our own typing is not a tap (card #187) */ ic->commit(plan.text, plan.replaceFrom, plan.replaceLength);
        // WHY: a synchronous target callback must not resurrect afterSwipe or record a word
        // after deletion, focus replacement, privacy change or reset.
        if (!self) return;
        if (!ic || !item || ic != inputContext() || item != ic->inputItem()
            || generation != m_generation) { reset(); return; }
        const bool stillSafe = safeContext();
        if (!self) return;
        if (!stillSafe) { reset(); return; }
        // Learning: count the swiped word once the field is confirmed safe; a later fix or erase takes it back.
        m_lastLearn = {};
        learnSwiped(plan.text.trimmed(), swipePrev);
        if (!self) return;
        // Remember that a swiped word is now the last thing typed (see m_afterSwipe in the header).
        m_afterSwipe = true;
        m_afterSwipeCursorBefore = cursorBefore;
        m_afterSwipeCursorAfter = cursorBefore + static_cast<int>(plan.text.size());
        // Remember this swipe's guesses so tapping the word later can offer them (memory only, see wordswap.h).
        // WHY the same field gate as prediction: readable (bridge) fields, and stock Plasma fields where the word is
        // remembered as the keyboard's own (tap-to-swap then only ever offers words it typed itself).
        const bool predictionField = tapSwapAllowed(predictionEditor());
        if (predictionField)
            m_recentSwipes.remember(before + (plan.text.startsWith(QLatin1Char(' ')) ? QStringLiteral(" ") : QString()),
                                    plan.text.trimmed(), trace, rects, contextWords);
        m_layoutKeys = rects;
        // Card #500: the bar's other places come from the swap search for the committed word, built a moment later.
        // WHY after the commit, not before: the word appears exactly as fast as before, and the bar shows the
        // swipe's own other guesses until the swap list replaces them (no empty bar in between).
        buildAfterSwipeList(m_session.serial(), m_generation, snapshot.epoch, candidates.front(), rects, contextWords,
                            trace, otherSpelling, predictionField);
    }
    const int barGeneration = m_generation;
    QPointer<QObject> barItem(ic->inputItem());
    refreshGuessBar();
    if (!self) return; // candidate signals may synchronously destroy the method/context
    if (!ic || !barItem || ic != inputContext() || barItem != ic->inputItem()
        || barGeneration != m_generation) { reset(); return; }
    // Opt-in recording for offline tuning (only when the owner created <data>/record; see swiperecorder.h).
    // WHY another privacy check after commit: a synchronous field update may have changed the hints.
    // `typed` is the word without its auto-space.
    const bool recordSafe = safeContext();
    if (!self || !ic) return;
    if (!recordSafe) { reset(); return; }
    const qint64 ts = QDateTime::currentMSecsSinceEpoch();
    const bool recorded = recordSafe
        && SwipeRecorder::appendIfEnabled(SwipeRecorder::dataDir(),
                                          {.ts = ts, .points = points, .keys = keys, .context = contextWords,
                                           .candidates = candidates, .typed = plan.text.trimmed()});
    m_lastRecordedSwipeTs = recorded ? ts : 0;
    // WHY after the new swipe is written: swiping again means the previous word was accepted ("keep"), and
    // the new swipe becomes the one waiting for its outcome (only if it was recorded, or it has no line).
    writeOutcome(m_outcome.beginSwipe(m_lastRecordedSwipeTs));
    // WHY counts only: enabling Qt info diagnostics must not expose the owner's typing guesses.
    qCInfo(lcSwipe) << "swipe" << trace.size() << "points ->" << candidates.size()
                   << "candidates in" << timer.elapsed() << "ms";
}

void SwipeInputMethod::clearSession()
{
    if (!m_session.isActive())
        return;
    m_session.clear();
    refreshGuessBar();
}

void SwipeInputMethod::clearAfterSwipe()
{
    m_afterSwipe = false;
    m_afterSwipeCursorBefore = -1;
    m_afterSwipeCursorAfter = -1;
}

bool SwipeInputMethod::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_exposureWindow && event->type() == QEvent::Expose && m_exposureWindow
        && m_exposureWindow->isExposed() && !m_exposureRefreshQueued) {
        m_exposureRefreshQueued = true;
        QPointer<QVirtualKeyboardInputContext> context(inputContext());
        QPointer<QQuickItem> item(m_exposureItem);
        QPointer<QQuickWindow> window(m_exposureWindow);
        const int generation = m_generation;
        const quint64 profileEpoch = m_profiles.snapshot().epoch;
        // WHY defer one event turn: Qt's Expose delivery can precede the proxy's exposed-state update.
        QTimer::singleShot(0, this, [this, context, item, window, generation, profileEpoch] {
            m_exposureRefreshQueued = false;
            refreshExposedInputCache(context, item, window, generation, profileEpoch);
        });
    }
    return QObject::eventFilter(watched, event);
}

void SwipeInputMethod::watchInputWindow()
{
    auto *ic = inputContext();
    auto *item = ic ? qobject_cast<QQuickItem *>(ic->inputItem()) : nullptr;
    auto *window = item ? item->window() : nullptr;
    if (m_exposureWindow == window && m_exposureItem == item) return;
    if (m_exposureWindow) m_exposureWindow->removeEventFilter(this);
    m_exposureWindow = window;
    m_exposureItem = item;
    m_exposureRefreshQueued = false;
    if (window) window->installEventFilter(this);
}

void SwipeInputMethod::refreshExposedInputCache(QPointer<QVirtualKeyboardInputContext> context,
    QPointer<QQuickItem> item, QPointer<QQuickWindow> window, int generation, quint64 profileEpoch)
{
    if (!context || !item || !window || context != inputContext() || item != context->inputItem()
        || window != item->window() || window != m_exposureWindow || item != m_exposureItem
        || generation != m_generation || profileEpoch != m_profiles.snapshot().epoch || !window->isExposed()
        || !context->inputEngine() || context->inputEngine()->inputMethod() != this
        || QGuiApplication::focusObject() != item || predictionPolicy().source == PredictionPolicySource::Unavailable
        || m_policySource != item || m_liveQueryActive) return;
    // WHY bundled (raw-policy) Plasma too: it drops the same hidden-window updates. On SteamOS a
    // field's type and text arrive before the keyboard is shown, so the first word after the
    // keyboard starts had stale hints, no auto-capital and no completions until refocus.
    QPointer<SwipeInputMethod> self(this);
    if (!safePredictionContext() || !self) return;
    // WHY revalidate after live policy queries: proxy callbacks may synchronously swap focus/profile.
    if (!context || !item || !window || context != inputContext() || item != context->inputItem()
        || window != item->window() || window != m_exposureWindow || item != m_exposureItem
        || generation != m_generation || profileEpoch != m_profiles.snapshot().epoch || !window->isExposed()
        || !context->inputEngine() || context->inputEngine()->inputMethod() != this
        || QGuiApplication::focusObject() != item || predictionPolicy().source == PredictionPolicySource::Unavailable
        || m_policySource != item || m_liveQueryActive) return;
    auto *ime = QGuiApplication::inputMethod();
    if (!ime || m_cacheSyncActive) return;
    // WHY guard: Qt answers this update by calling our update(), which must not start another refresh.
    m_cacheSyncActive = true;
    ime->update(Qt::ImSurroundingText | Qt::ImCursorPosition | Qt::ImAnchorPosition);
    if (self) m_cacheSyncActive = false;
    if (!self || !context || !item || !window || generation != m_generation
        || profileEpoch != m_profiles.snapshot().epoch || context != inputContext()
        || item != context->inputItem() || window != item->window()
        || !context->inputEngine() || context->inputEngine()->inputMethod() != this) return;
}

// WHY at every owned input point, not only on Expose: stock Plasma Keyboard forwards an app's text
// change to Qt only while its window is exposed. On ARM Plasma Mobile that drops echoes mid-word, so
// Qt's cached copy lags the field: the typed word cannot be matched and current-word taps and
// next-word predictions silently do nothing. Measured on the ARM guest: the same refresh at each key
// press restored "hello" + Space -> "the | you | I" -> "hello you". All Expose-hook gates still apply.
void SwipeInputMethod::syncInputCache()
{
    auto *ic = inputContext();
    QPointer<QQuickItem> item(ic ? qobject_cast<QQuickItem *>(ic->inputItem()) : nullptr);
    if (!ic || !item || item != m_exposureItem) return;
    refreshExposedInputCache(ic, item, item->window(), m_generation, m_profiles.snapshot().epoch);
}

void SwipeInputMethod::ensureContextConnections()
{
    watchInputWindow();
    auto *ic = inputContext();
    if (!ic || m_contextConnected) return;
    // WHY first key/swipe, not construction: Qt attaches the input context later.
    connect(ic, &QVirtualKeyboardInputContext::inputItemChanged, this, [this, ic] {
        QPointer<SwipeInputMethod> self(this);
        setPredictionPolicySource(ic->inputItem());
        if (!self) return;
        // WHY: other field, other words; the baseline for "cursor moved" starts over too.
        m_recentSwipes.clear(); m_moves.clear();
        watchInputWindow();
        if (self) reset();
    });
    connect(ic, &QVirtualKeyboardInputContext::inputMethodHintsChanged, this, &SwipeInputMethod::reset);
    connect(ic, &QVirtualKeyboardInputContext::preeditTextChanged, this, &SwipeInputMethod::reset);
    connect(ic, &QVirtualKeyboardInputContext::selectedTextChanged, this, [this] {
        QPointer<SwipeInputMethod> self(this);
        const bool safe=safeContext();
        if (self && !safe) reset();
    });
    connect(ic->inputEngine(), &QVirtualKeyboardInputEngine::inputMethodChanged, this, &SwipeInputMethod::reset);
    for (auto signal : {&QVirtualKeyboardInputContext::cursorPositionChanged,
                        &QVirtualKeyboardInputContext::anchorPositionChanged,
                        &QVirtualKeyboardInputContext::surroundingTextChanged}) {
        connect(ic, signal, this, [this] {
            QPointer<SwipeInputMethod> self(this);
            const bool safe=safePredictionContext();
            if (!self) return;
            if (!safe) { m_ownedHistory.clear(); m_typedWord.clear(); m_typedPress.clear(); m_moves.clear(); clearWordSwap(); if (self) invalidatePredictions(); return; }
            const bool visible = !m_nextWord.candidates().isEmpty();
            const auto editor=predictionEditor();
            m_ownedHistory.changed(editor,predictionPolicy(),m_profiles.snapshot().epoch);
            const bool typedVisible=!m_typedWord.candidates().isEmpty();
            m_typedWord.changed(editor,predictionPolicy(),m_profiles.snapshot().epoch);
            if(typedVisible && m_typedWord.candidates().isEmpty())refreshGuessBar();
            if(!self)return; // candidate notifications may synchronously delete the method
            m_nextWord.editorChanged(editor, predictionPolicy(), m_profiles.snapshot().epoch, predictionCapable());
            // WHY not while alternatives are open (tap-back or right after a swipe): the running job is their
            // sentence ranking.
            if (!m_nextWord.pendingOrVisible() && !m_swap.active() && !afterSwipeScoring()) {
                m_predictionWorker.invalidate();
                if (m_predictionModelState==swipekb::PredictionModelState::Loading)
                    m_predictionModelState=swipekb::PredictionModelState::NotRequested;
            }
            if (visible && m_nextWord.candidates().isEmpty()) refreshGuessBar();
            if (self) observeWordSwap(editor);
            if (self) emit diagnosticsChanged();
        });
    }
    m_contextConnected = true;
    if (!m_policySource) setPredictionPolicySource(ic->inputItem());
}

EditorSnapshot SwipeInputMethod::predictionEditor() const
{
    auto *ic = inputContext();
    if (!ic) return {};
    return {ic->surroundingText(), ic->cursorPosition(), ic->anchorPosition(),
        ic->inputItem() && ic->inputEngine()->inputMethod() == this, ic->inputMethodHints(), !ic->preeditText().isEmpty()};
}

PredictionPolicy SwipeInputMethod::predictionPolicy() const
{
    return readPredictionPolicy(m_policySource,
        qEnvironmentVariable("SWIPEKB_NATIVE_PREDICTION") == QStringLiteral("owned-history-v1"), m_nativePolicyEpoch);
}

bool SwipeInputMethod::safePredictionContext()
{
    QPointer<SwipeInputMethod> self(this);
    if (!safeContext() || !self) return false;
    QPointer<QVirtualKeyboardInputContext> ic(inputContext());
    QPointer<QObject> item(ic ? ic->inputItem() : nullptr);
    const int generation=m_generation;
    const auto deny=Qt::ImhSensitiveData | Qt::ImhHiddenText | Qt::ImhUrlCharactersOnly
        | Qt::ImhEmailCharactersOnly | Qt::ImhDigitsOnly | Qt::ImhFormattedNumbersOnly
        | Qt::ImhDialableCharactersOnly | Qt::ImhDate | Qt::ImhTime | Qt::ImhPreferNumbers;
    // WHY query before obtaining ANY validation text. Native mode cannot reconstruct
    // raw NoCompletion, but must honor every observable cached AND live veto.
    if (!ic || !item || (ic->inputMethodHints() & deny) || !ic->preeditText().isEmpty()) return false;
    const auto policy=predictionPolicy();
    m_liveQueryActive=true;
    const auto live=queryLiveHints(item);
    if (!self) return false;
    m_liveQueryActive=false;
    return ic && item && ic==inputContext() && item==ic->inputItem() && generation==m_generation
        && policy==predictionPolicy() && live.alive && live.valid && !(live.hints & deny) && !(ic->inputMethodHints() & deny)
        && ic->preeditText().isEmpty()
        && ((policy.source==PredictionPolicySource::OwnedHistory && m_policySource==item)
            || (policy.source==PredictionPolicySource::RawBridge && policy.version==1 && policy.contextId
                && policy.contentTypeReceived && policy.completionAllowed && (policy.purpose==0 || policy.purpose==1)
                && (!((ic->inputMethodHints() | live.hints) & Qt::ImhNoPredictiveText) || policy.syntheticNoPredictiveText)));
}

void SwipeInputMethod::stageOwnedCommit(const CommitPlan &plan, bool correction)
{
    if (predictionPolicy().source!=PredictionPolicySource::OwnedHistory) return;
    QPointer<SwipeInputMethod> self(this);
    if (!safePredictionContext() || !self) { if (self) m_ownedHistory.clear(); return; }
    const auto previous=predictionEditor();
    auto expected=previous;
    const int start=expected.cursor+plan.replaceFrom;
    if (start<0 || start+plan.replaceLength>expected.text.size()) { m_ownedHistory.clear(); return; }
    expected.text.replace(start,plan.replaceLength,plan.text);
    expected.cursor=start+plan.text.size(); expected.anchor=expected.cursor;
    m_ownedHistory.word(previous,expected,predictionPolicy(),m_profiles.snapshot().epoch,plan.text.trimmed(),correction);
}

void SwipeInputMethod::setPredictionPolicySource(QObject *source)
{
    if (m_policySource == source) return;
    if (m_policySource) disconnect(m_policySource, nullptr, this, nullptr);
    m_policySource = source;
    if (source) {
        // WHY retain even an unbridged proxy identity: known stock native history
        // is a distinct mode, while a present raw bridge always remains authoritative.
        const auto signal = source->metaObject()->indexOfSignal("swipePredictionPolicyChanged()");
        if (signal >= 0)
            connect(source, SIGNAL(swipePredictionPolicyChanged()), this, SLOT(predictionPolicyChanged()));
        connect(source, &QObject::destroyed, this, [this] { m_policySource = nullptr; predictionPolicyChanged(); });
    }
    predictionPolicyChanged();
}

void SwipeInputMethod::predictionPolicyChanged()
{
    // WHY reset correction/gesture authority too: a raw-purpose change can happen
    // without Qt changing any hint and must not reuse an older field's word.
    reset();
}

void SwipeInputMethod::invalidatePredictions()
{
    QPointer<SwipeInputMethod> self(this);
    const bool visible = !m_nextWord.candidates().isEmpty();
    m_predictionTap.reset();
    m_nextWord.invalidate(); m_predictionWorker.invalidate();
    if (m_predictionModelState==swipekb::PredictionModelState::Loading) {
        m_predictionModelState=swipekb::PredictionModelState::NotRequested;
        emit diagnosticsChanged();
    }
    if (self && visible) refreshGuessBar();
}

void SwipeInputMethod::applyNeuralNextWord(PredictionJob &job) const
{
    // WHY from the snapshot: that role exists only when the file passed the lock's size + SHA-256 check. Without it the
    // worker loads the n-gram alone, even when an (unverified) .nwp.bin lies beside it.
    // WHY here: every next-word, completion and tap-to-swap job passes through, so all show the same capitals.
    job.capitals=swipekb::capitalsFor(m_wordsLanguage);
    const auto nwp=m_profiles.snapshot().assets.value(QStringLiteral("contextNwp"));
    if (nwp.path.isEmpty()) return;
    job.nwp=nwp.path;
    job.modelIdentity+=nwp.sha256;
}

bool SwipeInputMethod::predictionCapable() const
{
    return isReady() && m_profiles.snapshot().capabilities.nextWord
        && m_profiles.snapshot().assets.contains(QStringLiteral("contextModel"))
        && m_profiles.snapshot().assets.contains(QStringLiteral("contextVocab"));
}

void SwipeInputMethod::observePredictionEcho()
{
    QPointer<SwipeInputMethod> self(this);
    const bool safe=safePredictionContext();
    if (!self) return;
    if (!safe) { m_ownedHistory.clear(); m_typedWord.clear(); m_typedPress.clear(); m_moves.clear(); clearWordSwap(); if (self) invalidatePredictions(); return; }
    const auto old = m_nextWord.candidates();
    const auto editor=predictionEditor();
    m_ownedHistory.observe(editor,predictionPolicy(),m_profiles.snapshot().epoch);
    const auto oldTyped=m_typedWord.candidates();
    bool completionWanted=false;
    if(m_typedWord.observe(editor,predictionPolicy(),m_profiles.snapshot().epoch) && isReady()) {
        refreshTypedSuggestions(editor);
        completionWanted=true;
    }
    if(oldTyped!=m_typedWord.candidates())refreshGuessBar();
    if(!self)return;
    const auto request = m_nextWord.observe(editor, predictionPolicy(), m_profiles.snapshot().epoch, predictionCapable());
    if (request) {
        m_predictionModelState=swipekb::PredictionModelState::Loading;
        emit diagnosticsChanged();
        if (!self || request->profileEpoch!=m_profiles.snapshot().epoch) return;
        const auto model = m_profiles.snapshot().assets.value(QStringLiteral("contextModel"));
        const auto vocab = m_profiles.snapshot().assets.value(QStringLiteral("contextVocab"));
        const auto words=predictionPolicy().source==PredictionPolicySource::OwnedHistory
            ? m_ownedHistory.context() : swipekb::textContextWords(m_nextWord.textBeforeCursor());
        // WHY never use a native validation snapshot as model input, including pasted prefixes.
        if (!words.isEmpty()) {
            m_nextPrev=words.last();   // for the personal re-ranking of this request's answer
            PredictionJob job{*request, model.path, vocab.path, words, model.sha256 + vocab.sha256};
            applyNeuralNextWord(job);
            m_predictionWorker.submit(job);
        }
        else {
            m_nextWord.invalidate(); m_predictionWorker.invalidate();
            m_predictionModelState=swipekb::PredictionModelState::NotRequested;
            emit diagnosticsChanged();
        }
    } else if (!m_nextWord.pendingOrVisible()) {
        // WHY not while a word is being typed: the running job is that word's completion, not a stale
        // next-word request (those are refused by their stamp anyway).
        if (m_typedWord.token().isEmpty() && !m_swap.active() && !afterSwipeScoring()) m_predictionWorker.invalidate();
        if (m_predictionModelState==swipekb::PredictionModelState::Loading) {
            m_predictionModelState=swipekb::PredictionModelState::NotRequested;
            emit diagnosticsChanged();
        }
    }
    // WHY after the next-word block: the worker keeps only the latest job, and mid-word the completion matters.
    if (self && completionWanted) requestContextCompletion(editor);
    if (self && old != m_nextWord.candidates()) refreshGuessBar();
    if (self) observeWordSwap(editor);
}

void SwipeInputMethod::clearWordSwap()
{
    QPointer<SwipeInputMethod> self(this);
    const bool was = m_swap.active();
    m_swap.clear(); m_swapPress.clear(); m_swapPending.clear();
    ++m_swapStamp.requestId; // a late sentence-ranking answer for the old word is refused
    if (!was) return;
    m_predictionWorker.invalidate();
    refreshGuessBar();
    Q_UNUSED(self);
}

void SwipeInputMethod::observeWordSwap(const EditorSnapshot &editor)
{
    // Tap-to-swap (card #187). Under owned-history (stock Plasma) the keyboard may inspect only words it committed
    // itself and cannot attest the rest of the field, so there it offers alternatives for its own words only.
    const auto policy = predictionPolicy();
    if (!tapSwapAllowed(editor) || !isReady()) {
        m_moves.clear(); clearWordSwap(); return;
    }
    QPointer<SwipeInputMethod> self(this);
    const bool tapped = m_moves.observe(editor.text, editor.cursor);
    const bool ownedOnly = policy.source == PredictionPolicySource::OwnedHistory;
    // WHY: in stock Plasma fields the remembered words are trusted only while nobody else edits the field.
    if (ownedOnly && m_moves.outsideEdit()) m_recentSwipes.clear();
    if (m_swap.active() && !m_swap.stillValid(editor.text, editor.cursor)) {
        clearWordSwap();
        if (!self) return;
    }
    if (!tapped) return;
    // A tap away from a swiped word ends that swipe's guess bar (update() would, a moment later).
    if (m_session.isActive() && !m_session.cursorStillAfterWord(editor.text.left(editor.cursor))) {
        m_outcome.abandon();
        clearSession();
        if (!self) return;
    }
    if (m_session.isActive() || !m_typedWord.token().isEmpty()) return;
    // WHY two ways to find the word: a readable field may offer any word; a stock Plasma field only the keyboard's
    // own words, found by comparing what it remembers with the text (no word is read out of the field).
    const auto owned = m_recentSwipes.ownedAt(editor.text, editor.cursor);
    const auto span = ownedOnly ? (owned ? std::optional<WordSpan>(owned->span) : std::nullopt)
                                : WordSwap::wordAt(editor.text, editor.cursor);
    if (!span) { clearWordSwap(); return; }
    if (m_swap.active() && m_swap.start() == span->start && m_swap.word() == span->word) return;
    const auto snapshot = m_profiles.snapshot();
    // Card #500 (one brain): the options are the words whose swipe looks like this word's. WHY the remembered swipe
    // when there is one: re-scoring the person's own finger path finds what they meant; a typed word gets a made-up
    // path through its keys on the current layout.
    const QString textBefore = editor.text.left(span->start);
    const bool ownSpan = owned && owned->span == *span;
    std::optional<std::pair<RecentSwipes::Trace, RecentSwipes::Keys>> remembered;
    if (ownSpan) { if (!owned->trace.empty()) remembered = std::make_pair(owned->trace, owned->keys); }
    else remembered = m_recentSwipes.traceFor(textBefore, span->word);
    // WHY the remembered words in stock Plasma fields: there the sentence is known only from the keyboard's own words.
    const QStringList sentence = ownedOnly ? (owned ? owned->context : QStringList())
                                           : swipekb::textContextWords(textBefore.right(4096));
    // WHY words after the tapped word only in readable (bridge) fields: stock fields may be read only for the
    // keyboard's own words, so there the list is built from the words before alone.
    const QStringList after = policy.source == PredictionPolicySource::RawBridge
        ? swapflow::wordsAfter(editor.text.mid(span->start + span->length, 256)) : QStringList();
    // WHY tapBackParams: a tap-back list may offer rarer words that fit the path well (the person asked for options).
    // WHY the words before and after too: with none on either side the list gets the no-sentence weights.
    const auto params = swapflow::tapBackParams(snapshot.effectiveLanguage, sentence, after);
    auto cands = swapflow::gather(*m_engine, span->word, remembered ? remembered->second : m_layoutKeys, sentence, after,
                                  remembered ? remembered->first : RecentSwipes::Trace{}, params);
    m_lastSwapUsedPath = remembered.has_value();
    m_lastSwapWordsAfter = int(after.size());
    if (!m_swap.begin(editor.text, editor.cursor, swapflow::tidySwapList(span->word, cands, params, 3, swapRules(), true))) {
        clearWordSwap(); return;
    }
    m_swapPress.clear();
    ++m_swapStamp.requestId;
    m_swapStamp.profileEpoch = snapshot.epoch;
    m_swapPending.clear();
    refreshGuessBar();
    if (!self) return;
    // Sentence scores only with the language's sentence model; without one the list stays on path + n-gram order.
    // WHY predictionCapable() alone (as the Plasma 6.9 front end): the profile loader accepts a context model only
    // when it is "<language>:context-ngram-v1" (languageprofile.cpp), so the extra compatibility check this used to
    // carry could never fail.
    if (!predictionCapable()) return;
    m_swapPending.tapped = span->word; m_swapPending.cands = std::move(cands); m_swapPending.params = params; m_swapPending.limit = 3;
    if (!m_swapPending.arm(m_swapStamp)) { m_swapPending.clear(); return; }
    const auto model = snapshot.assets.value(QStringLiteral("contextModel"));
    const auto vocab = snapshot.assets.value(QStringLiteral("contextVocab"));
    PredictionJob job{m_swapStamp, model.path, vocab.path, sentence, model.sha256 + vocab.sha256, QString()};
    job.scoreWords = m_swapPending.scoreList();
    applyNeuralNextWord(job);
    m_predictionWorker.submit(job);
}

swapflow::Rules SwipeInputMethod::swapRules()
{
    const auto snapshot = m_profiles.snapshot();
    // WHY the list's own boost (swapflow::Rules::personalBoost): a tap-back list only drops forgotten words and taught
    // words no longer wanted (boost 0, order kept); the bar after a swipe also lifts learned words at half strength.
    return {snapshot.effectiveLanguage, snapshot.requestedLocale,
            [this](const QStringList &l, const QString &prev, double boost) {
                return personalize(l, prev, QString(), false, int(l.size()), boost);
            }};
}

bool SwipeInputMethod::afterSwipeScoring() const
{
    return m_afterSwipePending.waiting && m_session.isActive() && m_session.serial() == m_afterSwipePending.sessionSerial;
}

void SwipeInputMethod::buildAfterSwipeList(quint64 sessionSerial, int generation, quint64 profileEpoch, const QString &first,
                                           const RecentSwipes::Keys &keys, const QStringList &context,
                                           const RecentSwipes::Trace &trace, const QString &otherSpelling,
                                           bool predictionField)
{
    m_afterSwipePending.clear();
    // WHY deferred to the next turn of the event loop: the swap search takes several milliseconds, and the swiped
    // word must reach the app first. WHY every check again when it runs: by then the person may have picked a guess,
    // typed, swiped again, or left the field, and a list for a word no longer shown must be dropped.
    QPointer<SwipeInputMethod> self(this);
    QTimer::singleShot(0, this, [self, sessionSerial, generation, profileEpoch, first, keys, context, trace, otherSpelling, predictionField] {
        if (!self) return;
        auto *im = self.data();
        const auto snapshot = im->m_profiles.snapshot();
        if (!im->m_session.isActive() || im->m_session.serial() != sessionSerial || generation != im->m_generation
            || snapshot.epoch != profileEpoch || !im->isReady() || im->m_barPressed)
            return;
        auto built = swapflow::buildAfterSwipe(*im->m_engine, first, keys, context, trace, otherSpelling, sessionSerial,
                                               im->swapRules());
        ++im->m_afterSwipeLists;
        // An empty list keeps the swipe's own guesses (swapflow::AfterSwipeList).
        if (built.candidates.isEmpty()) return;
        if (im->m_session.replaceAlternatives(built.candidates)) {
            im->refreshGuessBar();
            if (!self) return;
        }
        // WHY these conditions: swapflow::mayScoreAfterSwipe (prediction fields only; never over a wanted word list).
        if (!swapflow::mayScoreAfterSwipe(predictionField, im->predictionCapable(),
                                          im->m_nextWord.pendingOrVisible() || !im->m_typedWord.token().isEmpty()))
            return;
        // WHY one id counter with tap-back: the two kinds of answer can then never be mistaken for each other.
        const PredictionStamp stamp{snapshot.epoch, 0, ++im->m_swapStamp.requestId};
        if (!built.pending.arm(stamp)) return;
        im->m_afterSwipePending = std::move(built.pending);
        const auto model = snapshot.assets.value(QStringLiteral("contextModel"));
        const auto vocab = snapshot.assets.value(QStringLiteral("contextVocab"));
        PredictionJob job{stamp, model.path, vocab.path, context, model.sha256 + vocab.sha256, QString()};
        job.scoreWords = im->m_afterSwipePending.scoreList();
        im->applyNeuralNextWord(job);
        im->m_predictionWorker.submit(job);
    });
}

bool SwipeInputMethod::tapSwapAllowed(const EditorSnapshot &editor) const
{
    // Tap-to-swap (card #187) follows the prediction field rules: a readable bridge field, or a stock Plasma field
    // whose policy comes from this very field (there only the keyboard's own words are offered).
    const auto policy = predictionPolicy();
    if (!NextWordSession::allowed(editor, policy)) return false;
    if (policy.source == PredictionPolicySource::RawBridge) return true;
    auto *ic = inputContext();
    return policy.source == PredictionPolicySource::OwnedHistory && ic && ic->inputItem() && m_policySource == ic->inputItem();
}

void SwipeInputMethod::requestContextCompletion(const EditorSnapshot &editor)
{
    // Dictionary completions are already showing; this re-ranks them by the sentence so far, on the worker
    // thread, and replaces them when the answer arrives (a few milliseconds later).
    // WHY n-gram packs only: without the language's pack there is no sentence model, and dictionary completion
    // stays exactly as it was. predictionCapable() is that test: the profile loader accepts a context model only when it
    // is "<language>:context-ngram-v1" (languageprofile.cpp), so no separate compatibility check is needed.
    if (!predictionCapable()) return;
    const auto token=m_typedWord.token();
    if (token.isEmpty()) return;
    QStringList words;
    if (predictionPolicy().source==PredictionPolicySource::OwnedHistory) {
        words=m_ownedHistory.context();
        if (!words.isEmpty() && words.last()==token.toLower()) words.removeLast();
    } else {
        const auto before=editor.text.left(qMax(0,editor.cursor));
        // WHY context only when the letters sit right before the cursor: otherwise the text is not what the
        // typed-word session confirmed, and the word is completed as a sentence start.
        if (before.endsWith(token)) words=swipekb::textContextWords(before.chopped(token.size()).right(4096));
    }
    const auto model=m_profiles.snapshot().assets.value(QStringLiteral("contextModel"));
    const auto vocab=m_profiles.snapshot().assets.value(QStringLiteral("contextVocab"));
    m_completionPrev=words.isEmpty() ? QString() : words.last();
    PredictionJob job{m_typedWord.stamp(), model.path, vocab.path, words, model.sha256 + vocab.sha256, token};
    applyNeuralNextWord(job);
    m_predictionWorker.submit(job);
}

CaseMode SwipeInputMethod::caseMode() const
{
    const auto *ic = inputContext();
    return ic && ic->isCapsLockActive() ? CaseMode::AllCaps
        : m_textCase == QVirtualKeyboardInputEngine::TextCase::Upper ? CaseMode::Capitalize : CaseMode::AsIs;
}

QVariantMap SwipeInputMethod::diagnostics() const
{
    // WHY metadata-only: diagnostics must never read surroundingText, even when
    // invoked in a sensitive field without a live authorization query.
    const auto *ic=inputContext();
    const auto policy=predictionPolicy();
    swipekb::PredictionDiagnosticContext context;
    context.ownedHistory=policy.source==PredictionPolicySource::OwnedHistory;
    context.hasBridge=policy.source==PredictionPolicySource::RawBridge && policy.version==1 && m_policySource;
    context.hasContext=ic && ic->inputItem() && ic->inputEngine()->inputMethod()==this;
    context.policyKnown=policy.contextId!=0 && policy.contentTypeReceived;
    const auto deny=Qt::ImhSensitiveData | Qt::ImhHiddenText | Qt::ImhUrlCharactersOnly | Qt::ImhEmailCharactersOnly
        | Qt::ImhDigitsOnly | Qt::ImhFormattedNumbersOnly | Qt::ImhDialableCharactersOnly | Qt::ImhDate | Qt::ImhTime | Qt::ImhPreferNumbers;
    context.fieldAllowed=context.hasContext && !(ic->inputMethodHints() & deny) && ic->preeditText().isEmpty()
        && ic->cursorPosition()>=0 && ic->cursorPosition()==ic->anchorPosition()
        && (context.ownedHistory || (context.policyKnown && policy.completionAllowed && (policy.purpose==0 || policy.purpose==1)));
    context.ngramOnly=(m_engine && m_engine->usingNgram());
    context.model=m_predictionModelState;
    const auto profile=m_profiles.snapshot();
    auto out=swipekb::profileDiagnostics(profile);
    const auto prediction=swipekb::predictionDiagnostics(profile,context);
    for (auto it=prediction.begin();it!=prediction.end();++it) out.insert(it.key(),it.value());
    out.insert(QStringLiteral("scope"),QStringLiteral("runtime"));
    out.insert(QStringLiteral("engineReady"),isReady());
    // WHY names and counts only: which recogniser is active, never what was typed.
    out.insert(QStringLiteral("swipeEngine"),m_engine ? QString::fromLatin1(m_engine->mode()) : QStringLiteral("none"));
    // WHY a flag and a count only (never words): shows which evidence the last tap-to-swap list used.
    out.insert(QStringLiteral("swapUsedPath"),m_lastSwapUsedPath);
    out.insert(QStringLiteral("swapWordsAfter"),m_lastSwapWordsAfter);
    out.insert(QStringLiteral("afterSwipeLists"),m_afterSwipeLists);
    // WHY counts and times only: whether swipes were re-ranked by the sentence model, and how long they waited for it.
    out.insert(QStringLiteral("swipeRescoreAsked"),m_swipeRescore->asked);
    out.insert(QStringLiteral("swipeRescoreScored"),m_swipeRescore->scored);
    out.insert(QStringLiteral("swipeRescoreMaxWaitMs"),m_swipeRescore->maxWaitMs);
    // WHY counts only: whether learning is on and how much is remembered, never which words.
    auto *self=const_cast<SwipeInputMethod *>(this);
    const bool learning=self->m_learning.enabled();
    out.insert(QStringLiteral("learning"),learning);
    auto *pm=learning ? self->personal() : nullptr;
    out.insert(QStringLiteral("learnedWords"),pm ? quint64(pm->wordCount()) : quint64(0));
    out.insert(QStringLiteral("learnedPairs"),pm ? quint64(pm->pairCount()) : quint64(0));
    return out;
}

QString SwipeInputMethod::predictionSelectionToken() const
{
    if(!m_typedWord.candidates().isEmpty())return m_typedWord.selectionToken();
    if(m_swap.active() && !m_session.isActive())return m_swap.token();
    const auto stamp = m_nextWord.candidateStamp();
    if (!stamp) return {};
    return QString::number(stamp->profileEpoch) + ':' + QString::number(stamp->editorEpoch) + ':' + QString::number(stamp->requestId);
}

quint64 SwipeInputMethod::beginPredictionSelection(const QString &token)
{
    // WHY capture at pointer/key press, never synthesize a current stamp inside
    // the index-only Qt selection callback after a candidate refresh.
    ++m_predictionPressId;
    const quint64 pressId=m_predictionPressId;
    m_barPressed=true;
    m_typedPress=!token.isEmpty() && token==m_typedWord.selectionToken()?token:QString();
    m_swapPress = m_swap.active() && !token.isEmpty() && token == m_swap.token() ? token : QString();
    m_predictionTap = !m_swap.active() && !token.isEmpty() && token == predictionSelectionToken() ? m_nextWord.candidateStamp() : std::nullopt;
    // WHY after freezing the pressed set: the release check must compare against the live field. If
    // the refresh changes the suggestions under the finger, the frozen token no longer matches and
    // the tap is refused rather than inserting a word the person did not see.
    QPointer<SwipeInputMethod> self(this);
    syncInputCache();
    if (!self) return 0;
    qCInfo(lcSwipe) << "candidate press: typed" << !m_typedPress.isEmpty() << "next" << m_predictionTap.has_value();
    return pressId;
}

void SwipeInputMethod::endPredictionSelection(quint64 pressId)
{
    // WHY identify the press too: a deferred cleanup from an old gesture must not
    // revoke a fresh press on an unchanged candidate set.
    if (pressId == m_predictionPressId) {m_predictionTap.reset();m_typedPress.clear();m_swapPress.clear();m_barPressed=false;}
}

QStringList SwipeInputMethod::barCandidates() const
{
    if(!m_typedWord.candidates().isEmpty())return m_typedWord.candidates();
    if (m_session.isActive()) return m_session.alternatives();
    return m_swap.active() ? m_swap.candidates() : m_nextWord.candidates();
}

void SwipeInputMethod::refreshGuessBar()
{
    QPointer<SwipeInputMethod> self(this);
    emit selectionListChanged(Type::WordCandidateList);
    if (!self) return;
    emit selectionListActiveItemChanged(Type::WordCandidateList, -1);
}

// ---------------------------------------------------------------------------------------------------------------
// Phase 2: learning from the person's own typing. Everything below is inert while <data>/learn-typing is absent.
// ---------------------------------------------------------------------------------------------------------------

swipekb::PersonalModel *SwipeInputMethod::personal()
{
    const auto language=m_profiles.snapshot().effectiveLanguage;
    return language.isEmpty() ? nullptr : m_learning.model(language);
}

QString SwipeInputMethod::personalKey(const QString &word) const
{
    // WHY the same folding for every language: one key per word however it was typed (case, curly apostrophe).
    return swipekb::displayForm(word).toLower().replace(QChar(0x2019),QLatin1Char('\''));
}

QString SwipeInputMethod::prevBeforeToken(const EditorSnapshot &editor, const QString &token) const
{
    if (token.isEmpty()) return {};
    QStringList words;
    if (predictionPolicy().source==PredictionPolicySource::OwnedHistory) {
        // WHY the keyboard's own history, never the editor text: native stock cannot vouch for what else is in the field.
        words=m_ownedHistory.context();
        if (!words.isEmpty() && words.last()==token.toLower()) words.removeLast();
    } else {
        const auto before=editor.text.left(qMax(0,editor.cursor));
        if (!before.endsWith(token)) return {};
        words=swipekb::textContextWords(before.chopped(token.size()).right(4096));
    }
    return words.isEmpty() ? QString() : words.last();
}

swipekb::PersonalModel::Undo SwipeInputMethod::learnWord(const QString &rawWord, const QString &prev)
{
    // Hard rules: nothing before the owner switched learning on, nothing while the keyboard cannot vouch for the
    // field (password, hidden, URL, e-mail, number, date fields; prediction not allowed), nothing it did not type.
    QPointer<SwipeInputMethod> self(this);
    if (!m_learning.enabled() || !isReady()) return {};
    const QString word=swipekb::displayForm(rawWord.trimmed());
    static const QRegularExpression lexical(QStringLiteral("^[\\p{L}]+(?:['’\\-][\\p{L}]+)*$"));
    if (word.isEmpty() || word.size()>40 || !lexical.match(word).hasMatch()) return {};
    // WHY the same gate as prediction, asked now: the field may have changed since the key was pressed.
    const bool safe=safePredictionContext();
    if (!self) return {};
    if (!safe || !NextWordSession::allowed(predictionEditor(), predictionPolicy())) return {};
    auto *model=personal();
    if (!model) return {};
    // WHY not words we added ourselves: a taught word is in the engine's word list, so it would look like a built-in
    // word and skip the two-use rule.
    const bool known=m_engine && m_engine->hasWord(word.toStdString()) && !m_injected.contains(personalKey(word));
    if (!known) {
        // WHY these skips: a lone letter, SHOUTING and "heyyyyy" are slips, not words worth remembering.
        static const QRegularExpression run(QStringLiteral("(.)\\1{3,}"));
        if (word.size()<2 || (word.size()>1 && word==word.toUpper()) || run.match(word).hasMatch()) return {};
    }
    const bool capitalized=word.front().isUpper() && word!=word.toUpper();
    const auto undo=model->learn(personalKey(prev).toStdString(), personalKey(word).toStdString(), known, capitalized,
                                 LearningManager::now());
    m_learning.changed();
    syncLearnedWords();
    return undo;
}

void SwipeInputMethod::learnSwiped(const QString &word, const QString &prev)
{
    m_lastLearn=learnWord(word, prev);
    m_lastLearnPrev=prev;
    m_lastLearnModel=m_lastLearn.valid ? personal() : nullptr;
}

void SwipeInputMethod::retractLastLearn()
{
    if (!m_lastLearn.valid) return;
    // WHY only while it is still the same model: the folder, language or on/off state may have changed since.
    if (auto *model=personal(); model && model==m_lastLearnModel) {
        model->undo(m_lastLearn);
        m_learning.changed();
        syncLearnedWords();
    }
    m_lastLearn={};
    m_lastLearnModel=nullptr;
}

QStringList SwipeInputMethod::personalize(const QStringList &base, const QString &prev, const QString &prefix, bool inject,
                                          int topK, double boostScale)
{
    auto *model=personal();
    const qint64 now=LearningManager::now();
    // WHY first: a taught word that was forgotten (or learning switched off) must vanish from every list at once,
    // even before the engine's word list is next rebuilt.
    QStringList kept;
    for (const auto &word:base) {
        const auto key=personalKey(word);
        if (m_injected.contains(key) && (!model || !model->learnedNewWord(key.toStdString(), now))) continue;
        kept << word;
    }
    if (!model || model->empty()) return kept;
    std::vector<std::string> keys;
    keys.reserve(size_t(kept.size()));
    for (const auto &word:kept) keys.push_back(personalKey(word).toStdString());
    QStringList out;
    for (const auto &ranked:model->rerank(keys, personalKey(prev).toStdString(), personalKey(prefix).toStdString(), now,
                                          size_t(qMax(0,topK)), inject, boostScale)) {
        QString shown=ranked.baseIndex>=0 ? kept.at(ranked.baseIndex) : QString::fromStdString(ranked.key);
        // Names are shown with their capital (a word typed capitalised in the middle of sentences).
        if (model->wantsCapital(ranked.key) && !shown.isEmpty() && shown==shown.toLower())
            shown=shown.left(1).toUpper()+shown.mid(1);
        out << shown;
    }
    return out;
}

void SwipeInputMethod::syncLearnedWords(bool force)
{
    auto *model=personal();
    const std::uint64_t version=model ? model->swipeWordsVersion() : 0;
    if (!force && model==m_syncedModel && version==m_syncedVersion && m_engine.get()==m_syncedEngine) return;
    m_syncedModel=model; m_syncedVersion=version; m_syncedEngine=m_engine.get();
    std::vector<std::pair<std::string,int>> words;
    if (model) words=model->swipeWords(LearningManager::now());
    for (const auto &w:words) m_injected.insert(QString::fromStdString(w.first));
    if (!m_engine) return;
    // WHY a replaced set: the engine drops forgotten words itself, and rebuilds its swipe word list at the next swipe.
    m_engine->setLearnedWords(words);
    // WHY the built-in texting abbreviations (lol, wyd, jaja, mdr, lg...) too: the web-text word lists miss or
    // under-count them. liftWords only RAISES a word, so a learned or ordinary word is never weakened. They reach
    // swipes and completions only; the next-word model is not retrained, and swear acronyms stay out of unprompted
    // suggestions.
    m_engine->liftWords(swipekb::textingWordPairs(m_wordsLanguage));
    // WHY: "I", "Paris", "Monday" shown with their capital (English, Spanish, French; German keeps its own).
    m_engine->setCapitals(swipekb::capitalsFor(m_wordsLanguage));
}

void SwipeInputMethod::refreshTypedSuggestions(const EditorSnapshot &editor)
{
    const QString token=m_typedWord.token();
    if (token.isEmpty() || !isReady()) return;
    QStringList dictionary;
    for (const auto &candidate:m_engine->complete(token.toStdString(),12)) dictionary<<QString::fromStdString(candidate.word);
    // WHY completionOffer with no sentence-ranked words: the dictionary's list shows at once, the same way the bench
    // grades it before the worker answers.
    const QString prev=prevBeforeToken(editor, token);
    m_typedWord.suggest(barflow::completionOffer({}, dictionary, m_profiles.snapshot().requestedLocale,
        [this,&prev,&token](const QStringList &l) { return personalize(l, prev, token, true, 12); }));
}

bool SwipeInputMethod::selectionListRemoveItem(Type type, int index)
{
    // Reached from the long-press menu on a suggestion ("Remove from dictionary"); only learned words offer it.
    const QStringList shown=barCandidates();
    if (type!=Type::WordCandidateList || index<0 || index>=shown.size()) return false;
    auto *model=personal();
    const auto key=personalKey(shown.at(index)).toStdString();
    if (!model || !model->removable(key)) return false;
    QPointer<SwipeInputMethod> self(this);
    model->forget(key);
    m_learning.changed();
    syncLearnedWords();
    // WHY the bar is rebuilt rather than patched: its three slots are ranked and centred as a set, and a press
    // already under way must not meet a shifted list. The typed word's suggestions come straight back.
    const bool typed=!m_typedWord.candidates().isEmpty();
    if (typed) {
        const auto editor=predictionEditor();
        refreshTypedSuggestions(editor);
        if (self) requestContextCompletion(editor);
    } else {
        invalidatePredictions();
    }
    if (self) refreshGuessBar();
    return true;
}

void SwipeInputMethod::forgetEverythingLearned()
{
    QPointer<SwipeInputMethod> self(this);
    m_learning.forgetAll();
    m_lastLearn={}; m_lastLearnModel=nullptr;
    syncLearnedWords(true);
    invalidatePredictions();
    if (self) refreshGuessBar();
}
