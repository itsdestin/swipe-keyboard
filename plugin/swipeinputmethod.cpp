#include "swipeinputmethod.h"

#include <cmath>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QtVirtualKeyboard/QVirtualKeyboardInputContext>

#include "swipekb/recognizer.hpp"
#include "ownswipe.h"
#include "swiperecorder.h"
#include "livehintsquery.h"
#include "predictionpolicy.h"
#include "languagetextrules.h"
#include "profilediagnostics.h"
#include "wordswap.h"
#include <QPointer>
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
static std::unordered_map<char32_t,char32_t> popupAliases(const QString &language)
{
    std::unordered_map<char32_t,char32_t> aliases;
    for (char32_t c:swipekb::languageAlphabet(language).toUcs4()) {
        const auto base=swipekb::popupBase(c,language);
        if (base!=c) aliases[c]=base;
    }
    return aliases;
}

SwipeInputMethod::~SwipeInputMethod() = default;

void SwipeInputMethod::setPhysicalLayout(const QString &layoutId)
{
    if (layoutId==m_layoutId) return;
    m_layoutId=layoutId;
    resolveCurrentProfile();
}

SwipeInputMethod::SwipeInputMethod(QObject *parent) : SwipeInputMethod(parent, {}) {}

SwipeInputMethod::SwipeInputMethod(QObject *parent, NextWordWorker::Inference inference)
    : QVirtualKeyboardAbstractInputMethod(parent), m_predictionWorker(std::move(inference))
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
            // WHY next words only: a swear word is never offered out of the blue, but swiping or completing
            // one from typed letters still works (Destin, 2026-10-22).
            QStringList offered;
            for (const auto &word:words)
                if (swipekb::offeredUnprompted(word, m_profiles.snapshot().effectiveLanguage)) offered << word;
            if (m_nextWord.accept(stamp, swipekb::rankRegionalCandidates(offered, m_profiles.snapshot().requestedLocale, 12))) refreshGuessBar();
        });
    connect(&m_predictionWorker, &NextWordWorker::alternativesReady, this,
        [this](PredictionStamp stamp, const QStringList &ranked) {
            // WHY the same word and no finger down: a list that changes under a pressed finger would swap in a
            // word other than the one pressed. A stale answer (cursor moved on) is refused by its stamp.
            if (!m_swap.active() || !(stamp==m_swapStamp) || !m_swapPress.isEmpty()) return;
            if (m_swap.setRanked(ranked)) refreshGuessBar();
        });
    connect(&m_predictionWorker, &NextWordWorker::completionReady, this,
        [this](PredictionStamp stamp, const QString &prefix, const QStringList &ranked) {
            QPointer<SwipeInputMethod> self(this);
            observePredictionEcho();
            if (!self || ranked.isEmpty() || !isReady()) return;
            // WHY the same unchanged word only, and never while a suggestion is held down: a list that
            // changes under the finger would type a different word than the one pressed.
            if (!m_typedPress.isEmpty() || m_typedWord.token()!=prefix || !(m_typedWord.stamp()==stamp)) return;
            // WHY context-ranked words first, dictionary after: the sentence picks among words that start with
            // the typed letters; the dictionary still offers one-letter-off corrections when nothing matches.
            QStringList words=ranked;
            for (const auto &candidate:m_recognizer->complete(prefix.toStdString(),12)) words<<QString::fromStdString(candidate.word);
            const auto old=m_typedWord.candidates();
            m_typedWord.suggest(swipekb::rankRegionalCandidates(words,m_profiles.snapshot().requestedLocale,3));
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
    m_recognizer.reset();
    m_own.reset();
    m_predictionModelState=swipekb::PredictionModelState::NotRequested;
    emit profileChanged();
    if (!self || m_profiles.snapshot().epoch!=epoch) return;
    const auto language=swipekb::canonicalLocale(m_locale).section('-',0,0);
    // WHY packaged packs are the default; an explicit local override never enables English fallback.
    const QString root=language=="en" ? SwipeRecorder::modelDir() :
        qEnvironmentVariable("SWIPEKB_LANGUAGE_PACKS",SwipeRecorder::modelDir()+"/language-packs");
    auto snapshot=swipekb::resolveProfile(m_locale,m_layoutId,m_inputMode,root,epoch);
    if (snapshot.capabilities.swipe) {
        swipekb::ModelPaths paths;
        auto path=[&](const QString &role) { return snapshot.assets.value(role).path.toStdString(); };
        paths.encoder=path("encoder"); paths.dictionary=path("dictionary"); paths.decoder=path("decoder");
        // WHY the file kind decides, not the language: English uses the open n-gram when its pack is installed and
        // verified (compatibility "en:context-ngram-v1"); otherwise it keeps FUTO's neural model as before.
        const bool ngramContext=snapshot.assets.value("contextModel").compatibility.endsWith(QStringLiteral(":context-ngram-v1"));
        if (!ngramContext) {
            paths.lm_model=path("contextModel"); paths.lm_vocab=path("contextVocab");
        } else {
            paths.ngram=path("contextModel");
        }
        if (qEnvironmentVariableIntValue("SWIPEKB_NO_DECODER")) paths.decoder.clear();
        swipekb::RecognizerOptions options;
        options.language=snapshot.effectiveLanguage.toStdString();
        // WHY foreign swipe ranking and completion share only that locale's pinned contextual n-gram;
        // English joins them when it has one (see above), else the flag stays off and FUTO's model ranks.
        options.ngram=ngramContext;
        options.alphabet=swipekb::languageAlphabet(snapshot.effectiveLanguage).toStdString();
        options.popupAliases=popupAliases(snapshot.effectiveLanguage);
        // WHY a fresh runtime: never init() on an engine already owned by a consumer/worker.
        auto runtime=std::make_shared<swipekb::Recognizer>();
        if (runtime->init(paths,options)) {
            if (m_profiles.snapshot().epoch==epoch) {
                m_recognizer=std::move(runtime);
                // WHY opt-in and read once per profile resolve: FUTO stays the default; the owner turns the own
                // engine on with an empty `own-engine` file in the data dir (or SWIPEKB_ENGINE=own in tests).
                if (swipekb::OwnSwipe::requested(SwipeRecorder::dataDir().toStdString())) {
                    swipekb::OwnSwipeSetup setup;
                    setup.dictionary=paths.dictionary;
                    // WHY only an n-gram pack: the sentence prior is our n-gram file; FUTO's neural context model is not usable here.
                    if (ngramContext) setup.ngram=paths.ngram;
                    // WHY same folder as the FUTO models; not pinned/verified yet (test-only model), a missing or
                    // damaged file simply leaves the engine geometry-only.
                    // SWIPEKB_SWIPE_MODEL points at a model anywhere (manual trials and tests); never set in normal use.
                    setup.model=qEnvironmentVariable("SWIPEKB_SWIPE_MODEL",SwipeRecorder::modelDir()+"/swipe-model.bin").toStdString();
                    setup.keepCase=snapshot.effectiveLanguage.startsWith(QLatin1String("de"));
                    auto own=std::make_shared<swipekb::OwnSwipe>();
                    if (own->init(setup)) m_own=std::move(own);
                }
            }
        } else {
            snapshot.capabilities={}; snapshot.assets.clear(); snapshot.unavailableReason="Verified profile engine initialization failed";
        }
    }
    if (m_profiles.publish(snapshot)) emit profileChanged();
}

QList<QVirtualKeyboardInputEngine::InputMode> SwipeInputMethod::inputModes(const QString &)
{
    return {QVirtualKeyboardInputEngine::InputMode::Latin};
}

bool SwipeInputMethod::setInputMode(const QString &locale, QVirtualKeyboardInputEngine::InputMode mode)
{
    m_locale=locale;
    m_inputMode=mode==QVirtualKeyboardInputEngine::InputMode::Latin ? "Latin" : "Unsupported";
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
        // WHY backspace conservatively drops sentence history, but an exact-echo
        // confirmed owned unfinished token remains valid input when Space finalizes it.
        if(!unfinished.isEmpty() && predictionPolicy().source==PredictionPolicySource::OwnedHistory) {
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
        }
        refreshGuessBar();return;
    }
    if (m_swap.active() && !m_session.isActive()) {
        // Tap-to-swap (card #187): replaces exactly the tapped word, adds no space, moves nothing else.
        QPointer<SwipeInputMethod> self(this);
        const bool safe=safePredictionContext();
        if (!self || !ic || ic!=inputContext()) return;
        if (!safe || predictionPolicy().source!=PredictionPolicySource::RawBridge) { reset(); return; }
        const auto editor=predictionEditor();
        const auto pressed=m_swapPress; m_swapPress.clear();
        const QString oldWord=m_swap.word(); const int oldStart=m_swap.start();
        const auto plan=m_swap.take(index,pressed,editor.text,editor.cursor);
        if (plan.isEmpty()) { qCInfo(lcSwipe)<<"swap tap refused: press"<<!pressed.isEmpty()<<"index"<<index; clearWordSwap(); refreshGuessBar(); return; }
        m_recentSwipes.forget(editor.text.left(oldStart), oldWord);
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

bool SwipeInputMethod::isReady() const { return m_profiles.snapshot().capabilities.swipe && m_recognizer && m_recognizer->ready(); }

bool SwipeInputMethod::isReadyForLayout(const QVariantList &keys) const
{
    if (!isReady()) return false;
    const auto language=m_profiles.snapshot().effectiveLanguage;
    // WHY check before QML cancels a stock key press: an incomplete alphabet must remain ordinary tapping.
    return swipekb::normalize({{0,0,0},{1,1,1}},measuredKeys(keys),swipekb::languageAlphabet(language).toStdString(),
        popupAliases(language)).has_value();
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
    const auto runtime=m_recognizer;
    QStringList candidates;
#ifdef SWIPEKB_PRIVATE_FLAGS_DIAGNOSTIC
    ++m_diagnosticRecognitionCount;
    qWarning().nospace() << "SWIPE_FLAGS phase=recognition generation=" << m_generation
        << " recognition_count=" << m_diagnosticRecognitionCount;
#endif
    // WHY regional policy precedes truncation; a complete beam keeps both spellings eligible.
    // WHY the own engine goes first when opted in, and FUTO answers whenever it cannot (bad geometry, empty
    // result, exception): swiping never depends on the experimental path.
    QString rawShape;
    bool answeredByOwn=false;
    const auto own=m_own;
    if (own) {
        swipekb::OwnSwipe::Result r;
        if (own->recognize(trace, rects, context, 30, r)) {
            for (const auto &w : r.words) candidates << QString::fromStdString(w);
            rawShape=QString::fromStdString(r.bestShape);
            answeredByOwn=true; ++m_ownAnswered;
        } else {
            ++m_ownFellBack;
        }
    }
    if (!answeredByOwn) {
    for (const auto &c : runtime->recognize(trace, rects, context, 100)) {
        // WHY skip empty: an empty guess would type nothing yet still open a session and a blank guess-bar entry.
        if (!c.word.empty())
            candidates << QString::fromStdString(c.word);
    }
    rawShape=QString::fromStdString(runtime->bestShapeWord());
    }
    candidates=swipekb::rankRegionalCandidates(candidates,snapshot.requestedLocale,3,rawShape);
    if (m_profiles.snapshot().epoch!=snapshot.epoch) return;

    // WHY: the swipe shape of "its" is also "it's" (apostrophes are not swiped); put the dictionary's other
    // spelling of the best guess first in the guess bar so switching is one tap.
    if (!candidates.isEmpty()) {
        const auto others = runtime->otherSpellings(candidates.front().toStdString());
        if (!others.empty())
            candidates = SwipeSession::withOtherSpelling(candidates, QString::fromStdString(others.front()));
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
        // Remember that a swiped word is now the last thing typed (see m_afterSwipe in the header).
        m_afterSwipe = true;
        m_afterSwipeCursorBefore = cursorBefore;
        m_afterSwipeCursorAfter = cursorBefore + static_cast<int>(plan.text.size());
        // Remember this swipe's guesses so tapping the word later can offer them (memory only, see wordswap.h).
        // WHY only where tap-to-swap may read text: the same field gate as prediction, raw-policy bridge only.
        if (predictionPolicy().source==PredictionPolicySource::RawBridge
            && NextWordSession::allowed(predictionEditor(), predictionPolicy()))
            m_recentSwipes.remember(before + (plan.text.startsWith(QLatin1Char(' ')) ? QStringLiteral(" ") : QString()),
                                    plan.text.trimmed(), candidates);
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
            // WHY not while alternatives are open: the running job is their sentence ranking.
            if (!m_nextWord.pendingOrVisible() && !m_swap.active()) {
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
        QStringList words;
        for(const auto &candidate:m_recognizer->complete(m_typedWord.token().toStdString(),12))words<<QString::fromStdString(candidate.word);
        m_typedWord.suggest(swipekb::rankRegionalCandidates(words,m_profiles.snapshot().requestedLocale,3));
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
        if (!words.isEmpty()) m_predictionWorker.submit({*request, model.path, vocab.path,
            words, model.sha256 + vocab.sha256});
        else {
            m_nextWord.invalidate(); m_predictionWorker.invalidate();
            m_predictionModelState=swipekb::PredictionModelState::NotRequested;
            emit diagnosticsChanged();
        }
    } else if (!m_nextWord.pendingOrVisible()) {
        // WHY not while a word is being typed: the running job is that word's completion, not a stale
        // next-word request (those are refused by their stamp anyway).
        if (m_typedWord.token().isEmpty() && !m_swap.active()) m_predictionWorker.invalidate();
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
    m_swap.clear(); m_swapPress.clear();
    ++m_swapStamp.requestId; // a late sentence-ranking answer for the old word is refused
    if (!was) return;
    m_predictionWorker.invalidate();
    refreshGuessBar();
    Q_UNUSED(self);
}

void SwipeInputMethod::observeWordSwap(const EditorSnapshot &editor)
{
    // Tap-to-swap (card #187). WHY raw-policy bridge only: under owned-history the keyboard may inspect only
    // words it committed itself and cannot attest the rest of the field, so the feature stays off there.
    const auto policy = predictionPolicy();
    if (policy.source != PredictionPolicySource::RawBridge || !NextWordSession::allowed(editor, policy) || !isReady()) {
        m_moves.clear(); clearWordSwap(); return;
    }
    QPointer<SwipeInputMethod> self(this);
    const bool tapped = m_moves.observe(editor.text, editor.cursor);
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
    const auto span = WordSwap::wordAt(editor.text, editor.cursor);
    if (!span) { clearWordSwap(); return; }
    if (m_swap.active() && m_swap.start() == span->start && m_swap.word() == span->word) return;
    const auto snapshot = m_profiles.snapshot();
    // Dictionary neighbours: the same swipe shape's other spelling (its / it's), then one-edit words.
    QStringList neighbours;
    for (const auto &o : m_recognizer->otherSpellings(span->word.toLower().toStdString()))
        neighbours << QString::fromStdString(o);
    for (const auto &c : m_recognizer->similar(span->word.toStdString(), 12))
        neighbours << QString::fromStdString(c.word);
    // WHY filtered unless the person's own word is one: a clean word is never answered with a swear word it
    // merely resembles; for a word they typed that is blocked, its neighbours derive from their word, so allowed.
    if (swipekb::offeredUnprompted(span->word, snapshot.effectiveLanguage)) {
        QStringList clean;
        for (const auto &n : neighbours) if (swipekb::offeredUnprompted(n, snapshot.effectiveLanguage)) clean << n;
        neighbours = clean;
    }
    neighbours = swipekb::rankRegionalCandidates(neighbours, snapshot.requestedLocale, 12);
    const auto swiped = m_recentSwipes.candidatesFor(editor.text.left(span->start), span->word);
    if (!m_swap.begin(editor.text, editor.cursor, swiped, neighbours)) { clearWordSwap(); return; }
    m_swapPress.clear();
    ++m_swapStamp.requestId;
    m_swapStamp.profileEpoch = snapshot.epoch;
    refreshGuessBar();
    if (!self) return;
    // Sentence ranking only with an n-gram pack; English without one stays on dictionary order (like completion).
    if (!predictionCapable() || !snapshot.assets.value(QStringLiteral("contextModel")).compatibility.endsWith(QStringLiteral(":context-ngram-v1"))) return;
    const auto model = snapshot.assets.value(QStringLiteral("contextModel"));
    const auto vocab = snapshot.assets.value(QStringLiteral("contextVocab"));
    PredictionJob job{m_swapStamp, model.path, vocab.path, swipekb::textContextWords(editor.text.left(span->start).right(4096)),
        model.sha256 + vocab.sha256, span->word.left(1).toLower()};
    job.alternatives = true; job.topK = 64;
    m_predictionWorker.submit(job);
}

void SwipeInputMethod::requestContextCompletion(const EditorSnapshot &editor)
{
    // Dictionary completions are already showing; this re-ranks them by the sentence so far, on the worker
    // thread, and replaces them when the answer arrives (a few milliseconds later).
    // WHY n-gram packs only: FUTO's neural model (English fallback) cannot complete a word, so English
    // completion works only when the English n-gram pack is installed; otherwise it stays exactly as it was.
    if (!predictionCapable() ||
        !m_profiles.snapshot().assets.value(QStringLiteral("contextModel")).compatibility.endsWith(QStringLiteral(":context-ngram-v1"))) return;
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
    m_predictionWorker.submit({m_typedWord.stamp(), model.path, vocab.path, words, model.sha256 + vocab.sha256, token});
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
    context.ngramOnly=(m_recognizer && m_recognizer->usingNgram());
    context.model=m_predictionModelState;
    const auto profile=m_profiles.snapshot();
    auto out=swipekb::profileDiagnostics(profile);
    const auto prediction=swipekb::predictionDiagnostics(profile,context);
    for (auto it=prediction.begin();it!=prediction.end();++it) out.insert(it.key(),it.value());
    out.insert(QStringLiteral("scope"),QStringLiteral("runtime"));
    out.insert(QStringLiteral("engineReady"),isReady());
    // WHY names and counts only: which recogniser is active, never what was typed.
    out.insert(QStringLiteral("swipeEngine"),m_own ? QString::fromLatin1(m_own->mode()) : QStringLiteral("futo"));
    out.insert(QStringLiteral("ownEngineSwipes"),m_ownAnswered);
    out.insert(QStringLiteral("ownEngineFallbacks"),m_ownFellBack);
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
    if (pressId == m_predictionPressId) {m_predictionTap.reset();m_typedPress.clear();m_swapPress.clear();}
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
