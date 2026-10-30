// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "swipecontroller.h"

#include "editorport.h"
#include "languageprofile.h"
#include "languageruntime.h"
#include "languagetextrules.h"

#include <QDir>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <cmath>

Q_LOGGING_CATEGORY(lcSwipe69, "swipekb.plasma69", QtWarningMsg)

namespace
{
// WHY the same deny list as NextWordSession::allowed: no reading, predicting or learning in these fields.
constexpr auto kPrivateHints = Qt::ImhSensitiveData | Qt::ImhHiddenText;
constexpr auto kDenyHints = Qt::ImhSensitiveData | Qt::ImhHiddenText | Qt::ImhUrlCharactersOnly | Qt::ImhEmailCharactersOnly | Qt::ImhDigitsOnly
    | Qt::ImhFormattedNumbersOnly | Qt::ImhDialableCharactersOnly | Qt::ImhDate | Qt::ImhTime | Qt::ImhPreferNumbers;

QString dataDir()
{
    const QString dir = qEnvironmentVariable("SWIPEKB_DATA");
    return dir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/swipe-keyboard") : dir;
}

QString modelDir()
{
    const QString dir = qEnvironmentVariable("SWIPEKB_MODEL_DATA");
    return dir.isEmpty() ? dataDir() : dir;
}

int utf8Size(const QString &s)
{
    return int(s.toUtf8().size());
}

bool isPunctuationAfterSpace(const QString &text)
{
    return text.size() == 1 && QStringLiteral(".,!?;:").contains(text);
}

std::vector<swipekb::KeyRect> measuredKeys(const QVariantList &keys)
{
    std::vector<swipekb::KeyRect> rects;
    for (const QVariant &v : keys) {
        const auto m = v.toMap();
        const auto symbols = swipekb::displayForm(m.value(QStringLiteral("letter")).toString()).toLower().toUcs4();
        if (symbols.size() != 1) {
            return {};
        }
        rects.push_back({char32_t(symbols.front()),
                         m.value(QStringLiteral("x")).toFloat(),
                         m.value(QStringLiteral("y")).toFloat(),
                         m.value(QStringLiteral("w")).toFloat(),
                         m.value(QStringLiteral("h")).toFloat()});
    }
    return rects;
}
} // namespace

SwipeController::SwipeController(EditorPort *editor, QObject *parent, NextWordWorker::Inference inference, NextWordWorker::Scorer scorer)
    : QObject(parent)
    , m_editor(editor)
    , m_worker(std::move(inference), std::move(scorer))
    , m_learning([] {
        return dataDir();
    })
{
    if (m_editor) {
        connect(m_editor, &EditorPort::textChanged, this, &SwipeController::editorChanged);
        connect(m_editor, &EditorPort::hintsChanged, this, &SwipeController::hintsChanged);
        connect(m_editor, &EditorPort::localeChanged, this, [this] {
            setLocale(m_editor->locale());
        });
    }
    connect(&m_worker, &NextWordWorker::requestSettled, this, &SwipeController::predictionWorkSettled);
    connect(&m_worker, &NextWordWorker::resultReady, this, [this](PredictionStamp stamp, const QStringList &words) {
        observePredictionEcho();
        // WHY barflow::nextWordOffer (shared with the legacy front end and the typing bench): personal words, no swear
        // word out of the blue (Destin, 2026-10-22) unless the person uses it themselves with learning on, regional order.
        auto *pm = personal();
        const auto typedByPerson = [pm](const QString &key) {
            return pm && pm->usedByPerson(key.toStdString());
        };
        const auto offered = barflow::nextWordOffer(words, m_profile.effectiveLanguage, m_profile.requestedLocale,
            [this](const QStringList &l) { return personalize(l, m_nextPrev, QString(), true, 12); }, typedByPerson);
        if (m_nextWord.accept(stamp, offered)) {
            refreshBar();
        }
    });
    connect(&m_worker, &NextWordWorker::wordScoresReady, this, [this](PredictionStamp stamp, const QStringList &words, const QVector<float> &scores) {
        // Card #500: the sentence model's scores for a shown swap list arrived; the list is re-ranked with them
        // (swapflow::handleScores, shared with the Qt Virtual Keyboard front end).
        // WHY the same word and no finger down: a list that changes under a pressed finger would swap in another word.
        const bool tapBackOpen = m_swap.active() && stamp == m_swapStamp && m_swapPress.isEmpty() && !m_barPressed;
        const bool afterSwipeOpen = afterSwipeScoring() && !m_barPressed;
        const auto r = swapflow::handleScores(m_swapPending, m_afterSwipePending, tapBackOpen, afterSwipeOpen, stamp, words, scores, swapRules());
        if ((r.target == swapflow::ScoredList::TapBack && m_swap.setList(r.list))
            || (r.target == swapflow::ScoredList::AfterSwipe && m_session.replaceAlternatives(r.list))) {
            refreshBar();
        }
    });
    connect(&m_worker, &NextWordWorker::completionReady, this, [this](PredictionStamp stamp, const QString &prefix, const QStringList &ranked) {
        observePredictionEcho();
        if (ranked.isEmpty() || !ready()) {
            return;
        }
        if (!m_typedPress.isEmpty() || m_typedWord.token() != prefix || !(m_typedWord.stamp() == stamp)) {
            return;
        }
        // WHY barflow::completionOffer (shared with the legacy front end and the typing bench): sentence-ranked words
        // first, dictionary after, then the person's own words, cut to three.
        QStringList dictionary;
        for (const auto &c : m_engine->complete(prefix.toStdString(), 12)) {
            dictionary << QString::fromStdString(c.word);
        }
        const auto old = m_typedWord.candidates();
        m_typedWord.suggest(barflow::completionOffer(ranked, dictionary, m_profile.requestedLocale,
            [this, &prefix](const QStringList &l) { return personalize(l, m_completionPrev, prefix, true, 12); }));
        if (old != m_typedWord.candidates()) {
            refreshBar();
        }
    });
    if (m_editor) {
        setLocale(m_editor->locale());
    }
}

SwipeController::~SwipeController()
{
    m_learning.flush();
}

bool SwipeController::ready() const
{
    return m_engine && m_engine->ready() && m_profile.capabilities.swipe;
}

void SwipeController::setLocale(const QString &locale)
{
    if (locale == m_locale && m_engine) {
        return;
    }
    m_locale = locale;
    loadLanguage(locale);
}

void SwipeController::setLayout(const QString &kdeLayoutId)
{
    const QString physical = physicalLayout(kdeLayoutId);
    if (physical == m_physicalLayout) {
        return;
    }
    m_physicalLayout = physical;
    loadLanguage(m_locale);
}

QString SwipeController::physicalLayout(const QString &kdeLayoutId)
{
    // WHY from KDE's layout id: the language packs declare the physical families they were measured on.
    const QString local = kdeLayoutId.section(QLatin1Char('/'), 1).toLower();
    for (const char *family : {"qwertz", "azerty", "qwerty"}) {
        if (local.contains(QLatin1String(family))) {
            return QLatin1String(family);
        }
    }
    return {};
}

void SwipeController::loadLanguage(const QString &locale)
{
    reset();
    ++m_epoch;
    m_engine.reset();
    m_layoutKeys.clear(); // WHY: another layout's key positions would give a typed word a wrong made-up path
    // WHY the same profile authority as the Qt Virtual Keyboard front end: one lock-verified set of files per language.
    m_profile = swipekb::resolveProfile(locale, m_physicalLayout, QStringLiteral("Latin"), swipekb::languageModelRoot(locale, modelDir()), m_epoch);
    swipekb::LanguageEngineSetup setup;
    if (swipekb::configureLanguageEngine(m_profile, setup)) {
        auto engine = std::make_unique<swipekb::LanguageEngine>();
        if (engine->init(setup)) {
            m_engine = std::move(engine);
            // WHY at once: the built-in texting words (lol, wyd, jaja, mdr...) raise web-text counts, never lower them.
            m_engine->liftWords(swipekb::textingWordPairs(m_profile.effectiveLanguage));
            // WHY: "I", "Paris", "Monday" shown with their capital (English, Spanish, French; German keeps its own).
            m_engine->setCapitals(swipekb::capitalsFor(m_profile.effectiveLanguage));
            // Joint-model step 2: a swipe with words before is re-ranked with the sentence model the guess bar uses,
            // borrowed from the next-word worker (no second copy in memory; see NextWordWorker::scoreForSwipe).
            // WHY job(): the very files and checksums of a next-word job, so the worker's model is used only when it
            // is exactly this profile's; without a verified neural file this stays off.
            m_engine->setContextRescore(swipeContextRescore(&m_worker, job({}, {}), m_profile.effectiveLanguage, m_swipeRescore));
        } else {
            m_profile.capabilities = {};
            m_profile.unavailableReason = QStringLiteral("Language engine initialization failed");
        }
    }
    syncLearnedWords(true);
    qCInfo(lcSwipe69) << "language" << m_profile.effectiveLanguage << "swipe" << ready() << "next word" << predictionCapable() << m_profile.unavailableReason;
    Q_EMIT readyChanged();
    notifyState();
}

bool SwipeController::readyForKeys(const QVariantList &keys) const
{
    if (!ready()) {
        return false;
    }
    // WHY check before the layer takes a touch away from a key: a layout missing one of the language's letters
    // must stay ordinary tapping.
    QSet<char32_t> present;
    for (const auto &r : measuredKeys(keys)) {
        present.insert(r.letter);
    }
    if (present.isEmpty()) {
        return false;
    }
    for (const char32_t c : swipekb::languageAlphabet(m_profile.effectiveLanguage).toUcs4()) {
        if (!present.contains(c) && !present.contains(swipekb::popupBase(c, m_profile.effectiveLanguage))) {
            return false;
        }
    }
    return true;
}

EditorSnapshot SwipeController::editorSnapshot() const
{
    if (!m_editor) {
        return {};
    }
    return {m_editor->text(), m_editor->cursor(), m_editor->anchor(), m_editor->active(), m_editor->hints(), m_editor->preedit()};
}

PredictionPolicy SwipeController::policy() const
{
    // WHY two modes, decided by what the app itself declared: KDE's 6.9 core maps the Wayland content type into Qt
    // hints (src/inputmethodhints.cpp). No ImhNoPredictiveText means the app positively asked for completion AND
    // correction, the same fact the legacy raw bridge reported, so the field may be read (tap-to-swap, sentence
    // context). Otherwise only words this keyboard typed and saw echoed count (today's stock-Plasma rule).
    PredictionPolicy p;
    const auto hints = m_editor ? m_editor->hints() : Qt::InputMethodHints(kPrivateHints);
    if (!(hints & Qt::ImhNoPredictiveText)) {
        p.version = 1;
        p.contextId = m_focusEpoch;
        p.revision = m_focusEpoch;
        p.contentTypeReceived = true;
        p.purpose = 0;
        p.completionAllowed = true;
        p.source = PredictionPolicySource::RawBridge;
    } else {
        p.source = PredictionPolicySource::OwnedHistory;
        p.revision = m_focusEpoch;
    }
    return p;
}

bool SwipeController::safeContext() const
{
    if (!m_editor || !m_editor->active()) {
        return false;
    }
    // WHY: no swiping, reading or learning in password and hidden fields; a selection is never ours to replace.
    return !(m_editor->hints() & kPrivateHints) && m_editor->cursor() >= 0 && m_editor->cursor() == m_editor->anchor();
}

bool SwipeController::fieldAllowsSwipe() const
{
    return ready() && m_editor && m_editor->active() && !(m_editor->hints() & kPrivateHints);
}

bool SwipeController::safePredictionContext() const
{
    return safeContext() && !(m_editor->hints() & kDenyHints) && !m_editor->preedit() && NextWordSession::allowed(editorSnapshot(), policy());
}

EditorSnapshot SwipeController::effectiveSnapshot() const
{
    // WHY: while the app's report is still one of the states our own edits pass through, the text is really at the
    // last of them (the app has not caught up yet). Used for spacing and our own words only; the echo checks that
    // authorize reading the field or showing predictions still use exactly what the app reported.
    const auto e = editorSnapshot();
    return !m_ownStates.isEmpty() && m_ownStates.contains(e) ? m_ownStates.last() : e;
}

void SwipeController::expectOwnState(const EditorSnapshot &after)
{
    if (m_ownStates.isEmpty()) {
        m_ownStates << editorSnapshot();
    }
    m_ownStates << after;
    while (m_ownStates.size() > 4) {
        m_ownStates.removeFirst();
    }
}

void SwipeController::forgetOwnEdits()
{
    m_ownStates.clear();
    m_ownWords.clear();
}

QString SwipeController::textBeforeCursor() const
{
    if (!m_editor) {
        return {};
    }
    const auto e = effectiveSnapshot();
    return e.text.left(qMax(0, e.cursor));
}

CaseMode SwipeController::caseMode() const
{
    if (!m_editor) {
        return CaseMode::AsIs;
    }
    return m_editor->capsLock() ? CaseMode::AllCaps : m_editor->uppercase() ? CaseMode::Capitalize : CaseMode::AsIs;
}

bool SwipeController::predictionCapable() const
{
    return ready() && m_profile.capabilities.nextWord && m_profile.assets.contains(QStringLiteral("contextModel"))
        && m_profile.assets.contains(QStringLiteral("contextVocab"));
}

bool SwipeController::apply(const CommitPlan &plan)
{
    if (!m_editor || plan.isEmpty()) {
        return false;
    }
    int from = 0, length = 0;
    if (plan.replaceLength != 0 || plan.replaceFrom != 0) {
        // WHY only replace text we can see: input-method-v1 deletes BYTES around the cursor, so the characters to
        // remove must be in the reported surrounding text. When the app reports none, the plan is dropped instead
        // of deleting something we cannot check (the report's "replace only what we can prove" rule).
        const QString text = m_editor->text();
        const int cursor = m_editor->cursor();
        const int start = cursor + plan.replaceFrom;
        if (cursor < 0 || cursor > text.size() || start < 0 || plan.replaceLength < 0 || start + plan.replaceLength > text.size()) {
            qCInfo(lcSwipe69) << "replacement refused: the field does not show that text";
            return false;
        }
        from = start <= cursor ? -utf8Size(text.mid(start, cursor - start)) : utf8Size(text.mid(cursor, start - cursor));
        length = utf8Size(text.mid(start, plan.replaceLength));
    }
    // Where the text will be once the app applies this edit (built on where it is really going, see effectiveSnapshot).
    auto after = effectiveSnapshot();
    const int at = after.cursor + plan.replaceFrom;
    if (at >= 0 && at + plan.replaceLength <= after.text.size()) {
        after.text.replace(at, plan.replaceLength, plan.text);
        after.cursor = after.anchor = at + int(plan.text.size());
        expectOwnState(after);
    } else {
        forgetOwnEdits();
    }
    m_moves.ownEdit(); // WHY: our own typing is not the person tapping into a word (tap-to-swap)
    m_editor->commit(plan.text, from, length);
    return true;
}

PredictionJob SwipeController::job(const PredictionStamp &stamp, const QStringList &context, const QString &prefix) const
{
    const auto model = m_profile.assets.value(QStringLiteral("contextModel"));
    const auto vocab = m_profile.assets.value(QStringLiteral("contextVocab"));
    const auto nwp = m_profile.assets.value(QStringLiteral("contextNwp"));
    // WHY the verified neural file only: an unverified .nwp.bin beside the n-gram is never picked up.
    PredictionJob j{stamp, model.path, vocab.path, context, model.sha256 + vocab.sha256 + nwp.sha256, prefix};
    j.nwp = nwp.path;
    j.capitals = swipekb::capitalsFor(m_profile.effectiveLanguage);   // same capitals as the swipe engine
    return j;
}

// ---------------------------------------------------------------------------------------------------------------
// Swipe
// ---------------------------------------------------------------------------------------------------------------

int SwipeController::beginSwipe()
{
    // WHY: the swipe began as a press on its first key, which we expected KDE to type; a swipe cancels that key.
    if (m_press.valid && m_ownStates.size() > 1) {
        auto typed = m_press.before;
        if (m_press.text.size() == 1 && typed.cursor >= 0 && typed.cursor <= typed.text.size()) {
            typed.text.insert(typed.cursor, m_press.text);
            typed.cursor += 1;
            typed.anchor = typed.cursor;
            if (m_ownStates.last() == typed) {
                m_ownStates.removeLast();
            }
        }
    }
    clearWordSwap();
    invalidatePredictions();
    if (!ready() || !safeContext()) {
        return -1;
    }
    m_generation = (m_generation + 1) % 1000000000;
    return m_generation;
}

void SwipeController::finishSwipe(const QVariantList &points, const QVariantList &keys, int generation)
{
    QPointer<SwipeController> self(this);
    // WHY before anything is read: a reset (focus, privacy, language) since the finger went down revokes the path.
    if (generation < 0 || generation != m_generation || !safeContext()) {
        reset();
        return;
    }
    m_generation = (m_generation + 1) % 1000000000; // consumed once, even when the trace is rejected
    if (points.size() < 2 || points.size() > 2048 || keys.size() > 64 || !ready()) {
        return;
    }
    const double duration = points.last().toMap().value(QStringLiteral("t")).toDouble();
    if (!std::isfinite(duration) || duration < 0 || duration > 10000) {
        return;
    }
    std::vector<swipekb::TracePoint> trace;
    trace.reserve(size_t(points.size()));
    for (const QVariant &v : points) {
        const QVariantMap m = v.toMap();
        trace.push_back({m.value(QStringLiteral("x")).toFloat(), m.value(QStringLiteral("y")).toFloat(), m.value(QStringLiteral("t")).toFloat()});
    }
    const auto rects = measuredKeys(keys);
    m_typedWord.clear();
    m_typedPress.clear();
    const auto pol = policy();
    if (pol.source == PredictionPolicySource::OwnedHistory) {
        if (safePredictionContext()) {
            m_ownedHistory.observe(editorSnapshot(), pol, m_epoch);
        } else {
            m_ownedHistory.clear();
        }
    }
    const QString before = textBeforeCursor();
    // WHY: under the stock rule only keyboard-owned words may rank the swipe, never the field's text. When the app
    // has not confirmed them yet (it reports late), the words this keyboard itself typed in this sentence stand in:
    // they are our own output, and any real edit by someone else wipes them (editorChanged).
    QStringList contextWords = pol.source == PredictionPolicySource::OwnedHistory ? m_ownedHistory.context() : swipekb::textContextWords(before);
    if (pol.source == PredictionPolicySource::OwnedHistory && contextWords.isEmpty()) {
        contextWords = m_ownWords;
    }
    std::vector<std::string> context;
    for (const QString &w : contextWords) {
        context.push_back(w.toStdString());
    }
    // WHY counts only: shows on a real device which context rule ran and whether the app reported any text, without
    // ever logging what the person typed (enable with QT_LOGGING_RULES=swipekb.plasma69.info=true).
    qCInfo(lcSwipe69) << "swipe context: source" << (pol.source == PredictionPolicySource::OwnedHistory ? "owned" : "field")
                       << "field chars" << (m_editor ? m_editor->text().size() : -1) << "cursor" << (m_editor ? m_editor->cursor() : -1)
                       << "words" << contextWords.size();
    QStringList candidates;
    for (const auto &c : m_engine->recognize(trace, rects, context, 30)) {
        if (!c.word.empty()) {
            candidates << QString::fromStdString(c.word);
        }
    }
    if (candidates.isEmpty()) {
        return;
    }
    ++m_swipesAnswered;
    const QString rawShape = QString::fromStdString(m_engine->bestShapeWord());
    // WHY half-strength boosts: the finger path is strong evidence; habits only break near-ties.
    const QString swipePrev = contextWords.isEmpty() ? QString() : contextWords.last();
    candidates = personalize(candidates, swipePrev, QString(), false, int(candidates.size()), 0.5);
    candidates = swipekb::rankRegionalCandidates(candidates, m_profile.requestedLocale, 3, rawShape);
    QString otherSpelling;
    if (!candidates.isEmpty()) {
        // WHY: one swipe shape is both "its" and "it's"; the other spelling sits one tap away.
        const auto others = m_engine->otherSpellings(candidates.front().toStdString());
        if (!others.empty()) {
            otherSpelling = QString::fromStdString(others.front());
            candidates = SwipeSession::withOtherSpelling(candidates, otherSpelling);
        }
    }
    const bool urlOrEmail = m_editor->hints() & (Qt::ImhUrlCharactersOnly | Qt::ImhEmailCharactersOnly);
    const CaseMode mode = caseMode();
    const CommitPlan plan = m_session.commitSwipe(before, candidates, mode, {.afterSwipe = m_afterSwipe, .urlOrEmailField = urlOrEmail});
    if (!plan.isEmpty()) {
        const int cursorBefore = m_editor->cursor();
        const int gen = m_generation;
        stageOwnedCommit(plan);
        if (!apply(plan)) {
            m_session.clear();
            refreshBar();
            return;
        }
        if (!self || gen != m_generation) {
            return;
        }
        m_ownWords << plan.text.trimmed().toLower(); // lower case, like the confirmed owned words
        while (m_ownWords.size() > 20) {
            m_ownWords.removeFirst();
        }
        // WHY by hand: KDE drops one-shot Shift only after keys it typed itself; a capitalized swipe uses it up too.
        if (mode == CaseMode::Capitalize) {
            m_editor->releaseShift();
        }
        if (!safeContext()) {
            reset();
            return;
        }
        m_lastLearn = {};
        learnSwiped(plan.text.trimmed(), swipePrev);
        m_afterSwipe = true;
        m_afterSwipeCursorBefore = cursorBefore;
        m_afterSwipeCursorAfter = cursorBefore + int(plan.text.size());
        // Remember this swipe's guesses so tapping the word later offers them (memory only). WHY also in owned-history
        // fields: there tap-to-swap offers only the keyboard's own words, and this is one.
        const bool predictionField = tapSwapAllowed(editorSnapshot());
        if (predictionField) {
            m_recentSwipes.remember(before + (plan.text.startsWith(QLatin1Char(' ')) ? QStringLiteral(" ") : QString()), plan.text.trimmed(), trace,
                                    rects, contextWords);
        }
        m_layoutKeys = rects;
        // Card #500: the row's other places come from the swap search for the committed word, built a moment later.
        // WHY after the commit, not before: the word appears exactly as fast as before, and the row shows the swipe's
        // own other guesses until the swap list replaces them (no empty row in between).
        buildAfterSwipeList(m_session.serial(), m_generation, m_epoch, candidates.front(), rects, contextWords, trace, otherSpelling,
                            predictionField);
    }
    refreshBar();
    notifyState();
}

// ---------------------------------------------------------------------------------------------------------------
// Taps. KDE types them; we see the press first and the finished key after.
// ---------------------------------------------------------------------------------------------------------------

void SwipeController::keyPressed(int key, const QString &rawText)
{
    // WHY everything happens at the press: KDE types the key right after (at release, or at once for Backspace's
    // repeat), and the app may echo it before keyClicked() arrives. Each session must therefore already expect that
    // exact echo, as the legacy front end did by deciding before Qt typed the key.
    m_press = {};
    if (!safeContext()) {
        reset();
        return;
    }
    const bool predictSafe = safePredictionContext();
    clearWordSwap();
    QString text = key == Qt::Key_Space ? QStringLiteral(" ") : rawText;
    if (m_editor->uppercase() && text.size() == 1 && text.front().isLetter()) {
        text = text.toUpper();
    }
    if (!text.isEmpty() || key == Qt::Key_Backspace || key == Qt::Key_Delete || key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Tab) {
        m_moves.ownEdit();
    }
    m_autoSpaceCursor = -1; // an unclaimed key ends the "automatic space" state
    const auto pol = policy();
    if (predictSafe) {
        const auto editor = editorSnapshot();
        m_ownedHistory.observe(editor, pol, m_epoch);
        m_typedWord.observe(editor, pol, m_epoch);
    } else {
        m_ownedHistory.clear();
        m_typedWord.clear();
    }
    m_typedPress.clear();
    invalidatePredictions();
    EditorSnapshot previous = editorSnapshot();
    // Gboard-style: tapping a letter or digit right after a swiped word starts a new word ("write" + "a" = "write a").
    // WHY at press: KDE types the letter later, so our space is guaranteed to arrive first.
    const bool rightAfterWord = (m_session.isActive() && m_session.cursorStillAfterWord(textBeforeCursor())) || m_afterSwipe;
    const bool urlOrEmail = m_editor->hints() & (Qt::ImhUrlCharactersOnly | Qt::ImhEmailCharactersOnly);
    if (key != Qt::Key_Backspace && SwipeSession::spaceBeforeTappedKey(text, rightAfterWord, urlOrEmail)) {
        const auto before = editorSnapshot();
        apply({QStringLiteral(" ")});
        // WHY predicted, not re-read: the app echoes our space asynchronously.
        previous = before;
        if (previous.cursor >= 0 && previous.cursor <= previous.text.size()) {
            previous.text.insert(previous.cursor, QLatin1Char(' '));
            previous.cursor += 1;
            previous.anchor = previous.cursor;
        }
    }
    m_press = {true, predictSafe, key, text, previous};
    if (predictSafe && key == Qt::Key_Space) {
        const auto unfinished = m_typedWord.token();
        m_typedWord.clear();
        if (!unfinished.isEmpty()) {
            // Learning: Space finishes a word typed on this keyboard (never pasted or preexisting text).
            // WHY at keyClicked: a press on Space may turn into a cursor slide, which types no space.
            m_press.finishedWord = unfinished;
            if (pol.source == PredictionPolicySource::OwnedHistory) {
                // WHY remembered: in owned-history fields tap-to-swap may offer only words this keyboard typed, and
                // this word's letters were all confirmed as its own typing (the typed-word session).
                if (tapSwapAllowed(previous) && previous.cursor >= unfinished.size() && previous.text.left(previous.cursor).endsWith(unfinished)) {
                    auto context = m_ownedHistory.context();
                    if (!context.isEmpty() && context.last() == unfinished.toLower()) {
                        context.removeLast();
                    }
                    m_recentSwipes.remember(previous.text.left(previous.cursor - unfinished.size()), unfinished, {}, {}, context); // typed: no path
                }
                m_ownedHistory.word(previous, previous, pol, m_epoch, unfinished, true);
                m_ownedHistory.observe(previous, pol, m_epoch);
            }
        }
        auto expected = previous;
        if (expected.cursor >= 0 && expected.cursor <= expected.text.size()) {
            expected.text.insert(expected.cursor, QLatin1Char(' '));
            expected.cursor += 1;
            expected.anchor = expected.cursor;
        }
        clearSession();
        clearAfterSwipe();
        if (!unfinished.isEmpty()) {
            m_ownWords << unfinished.toLower();
        }
        expectOwnState(expected); // KDE types this space; a late report of the old text is not an edit
        m_ownedHistory.space(previous, expected, pol, m_epoch);
        m_nextWord.armExpectedEcho(expected, pol, m_epoch, predictionCapable(), previous);
        notifyState();
        return;
    }
    if (key == Qt::Key_Backspace) {
        m_ownedHistory.clear();
    }
    // WHY: a letter, digit or mark KDE is about to type is our own edit; anything else (Backspace, Enter, arrows,
    // sentence-ending punctuation) ends the run of own words the next swipe may use as its sentence.
    if (text.size() == 1 && !QStringLiteral(".!?").contains(text) && key != Qt::Key_Backspace && key != Qt::Key_Return && key != Qt::Key_Enter
        && previous.cursor >= 0 && previous.cursor <= previous.text.size()) {
        auto typed = previous;
        typed.text.insert(typed.cursor, text);
        typed.cursor += 1;
        typed.anchor = typed.cursor;
        expectOwnState(typed);
    } else {
        forgetOwnEdits();
    }
    // Learning: a word ended by punctuation or Enter counts like one ended by Space.
    if (predictSafe && !m_typedWord.token().isEmpty()
        && (key == Qt::Key_Return || key == Qt::Key_Enter || (text.size() == 1 && QStringLiteral(".,!?;:").contains(text)))) {
        const auto token = m_typedWord.token();
        learnWord(token, prevBeforeToken(previous, token));
    }
    auto expected = previous;
    if (predictSafe) {
        if (key == Qt::Key_Backspace && !m_typedWord.token().isEmpty() && expected.cursor > 0) {
            expected.text.remove(expected.cursor - 1, 1);
            --expected.cursor;
            expected.anchor = expected.cursor;
            m_typedWord.edit(previous, expected, pol, m_epoch, {}, true);
        } else if (text.size() == 1 && (text.front().isLetter() || text == QLatin1String("'") || text == QStringLiteral("’") || text == QLatin1String("-"))) {
            expected.text.insert(expected.cursor, text);
            expected.cursor += int(text.size());
            expected.anchor = expected.cursor;
            m_typedWord.edit(previous, expected, pol, m_epoch, text);
        } else {
            m_typedWord.clear();
        }
        if (pol.source == PredictionPolicySource::OwnedHistory) {
            if (key != Qt::Key_Backspace && text.size() == 1 && text.front().isLetter()) {
                m_ownedHistory.tap(previous, expected, pol, m_epoch, text);
            } else {
                m_ownedHistory.clear();
            }
        }
    } else {
        m_typedWord.clear();
    }
    // Any tapped key ends the swiped word's session (its alternatives no longer apply).
    clearSession();
    clearAfterSwipe();
    notifyState();
}

void SwipeController::keyClicked(int key, const QString &rawText)
{
    Q_UNUSED(rawText)
    const Press press = m_press;
    m_press = {};
    if (!press.valid || press.key != key) {
        // WHY forget: a key we did not see pressed (keyboard navigation, a second finger) typed something we did not
        // predict; nothing may be completed or learned from a word we cannot vouch for.
        m_ownedHistory.clear();
        m_typedWord.clear();
        clearSession();
        clearAfterSwipe();
        refreshBar();
        notifyState();
        return;
    }
    if (key == Qt::Key_Space && !press.finishedWord.isEmpty()) {
        learnWord(press.finishedWord, prevBeforeToken(press.before, press.finishedWord));
    }
    // WHY look again now: a fast app has already echoed the key; a slow one will, and editorChanged() follows it.
    observePredictionEcho();
}

void SwipeController::slideCursor(int steps)
{
    if (!m_editor || steps == 0) {
        return;
    }
    // WHY one arrow key per character through KDE's normal key path: it reaches every app the keyboard types into,
    // and each one ends the swiped word's session and the word being completed, exactly as an arrow key does.
    // A cap: a stalled UI thread can deliver one huge jump; never flood the app with arrow keys.
    const int key = steps > 0 ? Qt::Key_Right : Qt::Key_Left;
    for (int i = 0, count = qMin(qAbs(steps), 30); i < count; ++i) {
        keyPressed(key, QString());
        m_press = {};
        m_editor->sendKey(key);
    }
}

void SwipeController::claimedKey(int key, const QString &rawText)
{
    if (!safeContext()) {
        reset();
        if (m_editor) {
            m_editor->sendKey(key, rawText);
        }
        return;
    }
    if (key == Qt::Key_Backspace && m_session.isActive()) {
        // Whole-word Backspace right after a swipe: one press erases the swiped word (and its automatic space).
        const CommitPlan plan = m_session.takeBackspace(textBeforeCursor(), false);
        clearAfterSwipe();
        if (!plan.isEmpty() && apply(plan)) {
            retractLastLearn(); // erasing the word says it was wrong: its learned count is taken back
        } else {
            m_session.clear();
            m_editor->sendKey(Qt::Key_Backspace);
        }
        m_typedWord.clear();
        m_ownedHistory.clear();
        refreshBar();
        notifyState();
        return;
    }
    if (isPunctuationAfterSpace(rawText) && m_autoSpaceCursor > 0) {
        // "word" + automatic space + "." reads "word. ", not "word .". Only for the space this keyboard just added.
        const auto editor = editorSnapshot();
        const int autoSpace = m_autoSpaceCursor;
        if (editor.cursor == autoSpace && editor.cursor <= editor.text.size() && editor.text.at(editor.cursor - 1) == QLatin1Char(' ')) {
            reset();
            if (apply({rawText + QLatin1Char(' '), -1, 1})) {
                m_autoSpaceCursor = editor.cursor + 1;
            }
            notifyState();
            return;
        }
    }
    // No longer armed: behave exactly like an ordinary tap of this key.
    keyPressed(key, rawText);
    m_editor->sendKey(key, rawText);
    keyClicked(key, rawText);
}

// ---------------------------------------------------------------------------------------------------------------
// Editor changes
// ---------------------------------------------------------------------------------------------------------------

void SwipeController::editorChanged()
{
    QPointer<SwipeController> self(this);
    if (!m_ownStates.isEmpty()) {
        const auto e = editorSnapshot();
        if (!m_ownStates.contains(e)) {
            forgetOwnEdits(); // someone else edited the field (or moved the cursor): our record no longer holds
        } else if (e == m_ownStates.last()) {
            m_ownStates = {e}; // the app has caught up
        }
    }
    observePredictionEcho();
    if (!self) {
        return;
    }
    if (!safeContext()) {
        reset();
        return;
    }
    const QString before = textBeforeCursor();
    if (m_session.isActive() && !m_session.cursorStillAfterWord(before)) {
        clearSession();
    }
    // WHY: only genuinely hidden buffers need the "after a swipe" spacing memory; any visible edit ends it.
    if (m_afterSwipe
        && !SwipeSession::keepHiddenTextSpaceFallback(m_editor->text(), m_editor->cursor(), m_editor->anchor(), m_afterSwipeCursorBefore, m_afterSwipeCursorAfter)) {
        clearAfterSwipe();
    }
    if (m_autoSpaceCursor > 0 && m_editor->cursor() != m_autoSpaceCursor) {
        m_autoSpaceCursor = -1;
    }
    notifyState();
}

void SwipeController::hintsChanged()
{
    // WHY a new field identity: a purpose change (or a new field) must never reuse the previous field's word.
    ++m_focusEpoch;
    reset();
}

void SwipeController::observePredictionEcho()
{
    if (!safePredictionContext()) {
        m_ownedHistory.clear();
        m_typedWord.clear();
        m_typedPress.clear();
        m_moves.clear();
        clearWordSwap();
        invalidatePredictions();
        return;
    }
    const auto pol = policy();
    const auto old = m_nextWord.candidates();
    const auto editor = editorSnapshot();
    m_ownedHistory.observe(editor, pol, m_epoch);
    const auto oldTyped = m_typedWord.candidates();
    bool completionWanted = false;
    if (m_typedWord.observe(editor, pol, m_epoch) && ready()) {
        refreshTypedSuggestions(editor);
        completionWanted = true;
    }
    if (oldTyped != m_typedWord.candidates()) {
        refreshBar();
    }
    const auto request = m_nextWord.observe(editor, pol, m_epoch, predictionCapable());
    if (request) {
        const auto words = pol.source == PredictionPolicySource::OwnedHistory ? m_ownedHistory.context() : swipekb::textContextWords(m_nextWord.textBeforeCursor());
        if (!words.isEmpty()) {
            m_nextPrev = words.last();
            m_worker.submit(job(*request, words));
        } else {
            m_nextWord.invalidate();
            m_worker.invalidate();
        }
    } else if (!m_nextWord.pendingOrVisible() && m_typedWord.token().isEmpty() && !m_swap.active() && !afterSwipeScoring()) {
        m_worker.invalidate();
    }
    if (completionWanted) {
        requestContextCompletion(editor);
    }
    if (old != m_nextWord.candidates()) {
        refreshBar();
    }
    observeWordSwap(editor);
}

void SwipeController::observeWordSwap(const EditorSnapshot &editor)
{
    // Tap-to-swap. Under the owned-history rule the keyboard may inspect only its own words, so there it offers
    // alternatives for words it typed itself and nothing else.
    const auto pol = policy();
    if (!tapSwapAllowed(editor) || !ready()) {
        m_moves.clear();
        clearWordSwap();
        return;
    }
    const bool tapped = m_moves.observe(editor.text, editor.cursor);
    const bool ownedOnly = pol.source == PredictionPolicySource::OwnedHistory;
    // WHY: in owned-history fields the remembered words are trusted only while nobody else edits the field.
    if (ownedOnly && m_moves.outsideEdit()) {
        m_recentSwipes.clear();
    }
    if (m_swap.active() && !m_swap.stillValid(editor.text, editor.cursor)) {
        clearWordSwap();
    }
    if (!tapped) {
        return;
    }
    if (m_session.isActive() && !m_session.cursorStillAfterWord(editor.text.left(editor.cursor))) {
        clearSession();
    }
    if (m_session.isActive() || !m_typedWord.token().isEmpty()) {
        return;
    }
    // WHY two ways to find the word: a readable field may offer any word; an owned-history field only the keyboard's
    // own words, found by comparing what it remembers with the text (no word is read out of the field).
    const auto owned = m_recentSwipes.ownedAt(editor.text, editor.cursor);
    const auto span = ownedOnly ? (owned ? std::optional<WordSpan>(owned->span) : std::nullopt) : WordSwap::wordAt(editor.text, editor.cursor);
    if (!span) {
        clearWordSwap();
        return;
    }
    if (m_swap.active() && m_swap.start() == span->start && m_swap.word() == span->word) {
        return;
    }
    // Card #500 (one brain): the options are the words whose swipe looks like this word's. WHY the remembered swipe
    // when there is one: re-scoring the person's own finger path finds what they meant; a typed word gets a made-up
    // path through its keys on the current layout.
    const QString textBefore = editor.text.left(span->start);
    const bool ownSpan = owned && owned->span == *span;
    std::optional<std::pair<RecentSwipes::Trace, RecentSwipes::Keys>> remembered;
    if (ownSpan) {
        if (!owned->trace.empty()) {
            remembered = std::make_pair(owned->trace, owned->keys);
        }
    } else {
        remembered = m_recentSwipes.traceFor(textBefore, span->word);
    }
    // WHY the remembered words in owned-history fields: there the sentence is known only from the keyboard's own words.
    const QStringList sentence = ownedOnly ? (owned ? owned->context : QStringList()) : swipekb::textContextWords(textBefore.right(4096));
    // WHY words after the tapped word only in readable fields: owned-history fields may be read only for the
    // keyboard's own words, so there the list is built from the words before alone.
    const QStringList after =
        pol.source == PredictionPolicySource::RawBridge ? swapflow::wordsAfter(editor.text.mid(span->start + span->length, 256)) : QStringList();
    // WHY tapBackParams: a tap-back list may offer rarer words that fit the path well (the person asked for options).
    // WHY the words before and after too: with none on either side the list gets the no-sentence weights.
    const auto params = swapflow::tapBackParams(m_profile.effectiveLanguage, sentence, after);
    auto cands = swapflow::gather(*m_engine,
                                  span->word,
                                  remembered ? remembered->second : m_layoutKeys,
                                  sentence,
                                  after,
                                  remembered ? remembered->first : RecentSwipes::Trace{},
                                  params);
    m_lastSwapUsedPath = remembered.has_value();
    m_lastSwapWordsAfter = int(after.size());
    if (!m_swap.begin(editor.text, editor.cursor, swapflow::tidySwapList(span->word, cands, params, 3, swapRules(), true))) {
        clearWordSwap();
        return;
    }
    m_swapPress.clear();
    ++m_swapStamp.requestId;
    m_swapStamp.profileEpoch = m_epoch;
    m_swapPending.clear();
    refreshBar();
    if (!predictionCapable()) {
        return;
    }
    m_swapPending.tapped = span->word;
    m_swapPending.cands = std::move(cands);
    m_swapPending.params = params;
    m_swapPending.limit = 3;
    if (!m_swapPending.arm(m_swapStamp)) {
        m_swapPending.clear();
        return;
    }
    auto j = job(m_swapStamp, sentence, QString());
    j.scoreWords = m_swapPending.scoreList();
    m_worker.submit(j);
}

swapflow::Rules SwipeController::swapRules()
{
    // WHY the list's own boost (swapflow::Rules::personalBoost): a tap-back list only drops forgotten words and taught
    // words no longer wanted (boost 0, order kept); the bar after a swipe also lifts learned words at half strength.
    return {m_profile.effectiveLanguage, m_profile.requestedLocale,
            [this](const QStringList &l, const QString &prev, double boost) {
                return personalize(l, prev, QString(), false, int(l.size()), boost);
            }};
}

bool SwipeController::afterSwipeScoring() const
{
    return m_afterSwipePending.waiting && m_session.isActive() && m_session.serial() == m_afterSwipePending.sessionSerial;
}

void SwipeController::buildAfterSwipeList(quint64 sessionSerial,
                                          int generation,
                                          quint64 profileEpoch,
                                          const QString &first,
                                          const RecentSwipes::Keys &keys,
                                          const QStringList &context,
                                          const RecentSwipes::Trace &trace,
                                          const QString &otherSpelling,
                                          bool predictionField)
{
    m_afterSwipePending.clear();
    // WHY deferred to the next turn of the event loop: the swap search takes several milliseconds, and the swiped
    // word must reach the app first. WHY every check again when it runs: by then the person may have picked a guess,
    // typed, swiped again, or left the field, and a list for a word no longer shown must be dropped.
    QPointer<SwipeController> self(this);
    QTimer::singleShot(0, this, [self, sessionSerial, generation, profileEpoch, first, keys, context, trace, otherSpelling, predictionField] {
        if (!self) {
            return;
        }
        auto *c = self.data();
        if (!c->m_session.isActive() || c->m_session.serial() != sessionSerial || generation != c->m_generation || profileEpoch != c->m_epoch
            || !c->ready() || c->m_barPressed) {
            return;
        }
        auto built = swapflow::buildAfterSwipe(*c->m_engine, first, keys, context, trace, otherSpelling, sessionSerial, c->swapRules());
        ++c->m_afterSwipeLists;
        // An empty list keeps the swipe's own guesses (swapflow::AfterSwipeList).
        if (built.candidates.isEmpty()) {
            return;
        }
        if (c->m_session.replaceAlternatives(built.candidates)) {
            c->refreshBar();
        }
        // WHY these conditions: swapflow::mayScoreAfterSwipe (prediction fields only; never over a wanted word list).
        if (!swapflow::mayScoreAfterSwipe(predictionField, c->predictionCapable(), c->m_nextWord.pendingOrVisible() || !c->m_typedWord.token().isEmpty())) {
            return;
        }
        // WHY one id counter with tap-back: the two kinds of answer can then never be mistaken for each other.
        const PredictionStamp stamp{c->m_epoch, 0, ++c->m_swapStamp.requestId};
        if (!built.pending.arm(stamp)) {
            return;
        }
        c->m_afterSwipePending = std::move(built.pending);
        auto j = c->job(stamp, context, QString());
        j.scoreWords = c->m_afterSwipePending.scoreList();
        c->m_worker.submit(j);
    });
}

bool SwipeController::tapSwapAllowed(const EditorSnapshot &editor) const
{
    // Tap-to-swap follows the prediction field rules: readable fields offer any word, owned-history fields only the
    // keyboard's own words (observeWordSwap()).
    const auto pol = policy();
    return NextWordSession::allowed(editor, pol)
        && (pol.source == PredictionPolicySource::RawBridge || pol.source == PredictionPolicySource::OwnedHistory);
}

void SwipeController::requestContextCompletion(const EditorSnapshot &editor)
{
    if (!predictionCapable()) {
        return;
    }
    const auto token = m_typedWord.token();
    if (token.isEmpty()) {
        return;
    }
    QStringList words;
    if (policy().source == PredictionPolicySource::OwnedHistory) {
        words = m_ownedHistory.context();
        if (!words.isEmpty() && words.last() == token.toLower()) {
            words.removeLast();
        }
    } else {
        const auto before = editor.text.left(qMax(0, editor.cursor));
        if (before.endsWith(token)) {
            words = swipekb::textContextWords(before.chopped(token.size()).right(4096));
        }
    }
    m_completionPrev = words.isEmpty() ? QString() : words.last();
    m_worker.submit(job(m_typedWord.stamp(), words, token));
}

void SwipeController::refreshTypedSuggestions(const EditorSnapshot &editor)
{
    const QString token = m_typedWord.token();
    if (token.isEmpty() || !ready()) {
        return;
    }
    QStringList dictionary;
    for (const auto &c : m_engine->complete(token.toStdString(), 12)) {
        dictionary << QString::fromStdString(c.word);
    }
    // WHY completionOffer with no sentence-ranked words: the dictionary's list shows at once (shared rule).
    const QString prev = prevBeforeToken(editor, token);
    m_typedWord.suggest(barflow::completionOffer({}, dictionary, m_profile.requestedLocale,
        [this, &prev, &token](const QStringList &l) { return personalize(l, prev, token, true, 12); }));
}

void SwipeController::invalidatePredictions()
{
    const bool visible = !m_nextWord.candidates().isEmpty();
    m_predictionTap.reset();
    m_nextWord.invalidate();
    m_worker.invalidate();
    if (visible) {
        refreshBar();
    }
}

void SwipeController::clearWordSwap()
{
    const bool was = m_swap.active();
    m_swap.clear();
    m_swapPress.clear();
    m_swapPending.clear();
    ++m_swapStamp.requestId; // a late ranking answer for the old word is refused
    if (was) {
        m_worker.invalidate();
        refreshBar();
    }
}

void SwipeController::clearSession()
{
    if (!m_session.isActive()) {
        return;
    }
    m_session.clear();
    refreshBar();
}

void SwipeController::clearAfterSwipe()
{
    m_afterSwipe = false;
    m_afterSwipeCursorBefore = m_afterSwipeCursorAfter = -1;
}

void SwipeController::stageOwnedCommit(const CommitPlan &plan, bool correction)
{
    const auto pol = policy();
    if (pol.source != PredictionPolicySource::OwnedHistory) {
        return;
    }
    if (!safePredictionContext()) {
        m_ownedHistory.clear();
        return;
    }
    const auto previous = editorSnapshot();
    auto expected = previous;
    const int start = expected.cursor + plan.replaceFrom;
    if (start < 0 || start + plan.replaceLength > expected.text.size()) {
        m_ownedHistory.clear();
        return;
    }
    expected.text.replace(start, plan.replaceLength, plan.text);
    expected.cursor = start + int(plan.text.size());
    expected.anchor = expected.cursor;
    m_ownedHistory.word(previous, expected, pol, m_epoch, plan.text.trimmed(), correction);
}

void SwipeController::reset()
{
    forgetOwnEdits();
    m_autoSpaceCursor = -1;
    m_ownedHistory.clear();
    m_typedWord.clear();
    m_typedPress.clear();
    m_press = {};
    m_barPressed = false; // WHY: a press on another field's row never ends here; it must not freeze this field's row
    clearWordSwap();
    invalidatePredictions();
    // WHY: points collected before a focus/privacy/language reset must never authorize a later commit.
    m_generation = (m_generation + 1) % 1000000000;
    Q_EMIT gestureInvalidated();
    m_session.clear();
    clearAfterSwipe();
    refreshBar();
    notifyState();
}

// ---------------------------------------------------------------------------------------------------------------
// Suggestion row
// ---------------------------------------------------------------------------------------------------------------

QStringList SwipeController::candidates() const
{
    if (!m_typedWord.candidates().isEmpty()) {
        return m_typedWord.candidates();
    }
    if (m_session.isActive()) {
        return m_session.alternatives();
    }
    return m_swap.active() ? m_swap.candidates() : m_nextWord.candidates();
}

void SwipeController::refreshBar()
{
    const auto bar = candidates();
    if (bar != m_lastBar) {
        m_lastBar = bar;
        Q_EMIT candidatesChanged();
    }
}

void SwipeController::notifyState()
{
    const bool undo = undoArmed(), autoSpace = autoSpaceArmed(), field = fieldAllowsSwipe();
    if (undo != m_lastUndo || autoSpace != m_lastAutoSpace || field != m_lastField) {
        m_lastUndo = undo;
        m_lastAutoSpace = autoSpace;
        m_lastField = field;
        Q_EMIT stateChanged();
    }
}

QString SwipeController::selectionToken() const
{
    if (!m_typedWord.candidates().isEmpty()) {
        return m_typedWord.selectionToken();
    }
    if (m_swap.active() && !m_session.isActive()) {
        return m_swap.token();
    }
    if (m_session.isActive()) {
        return QStringLiteral("swipe");
    }
    const auto stamp = m_nextWord.candidateStamp();
    if (!stamp) {
        return {};
    }
    return QString::number(stamp->profileEpoch) + QLatin1Char(':') + QString::number(stamp->editorEpoch) + QLatin1Char(':') + QString::number(stamp->requestId);
}

quint64 SwipeController::pressCandidate(const QString &token)
{
    // WHY freeze the pressed set: if the row changes under the finger, the release is refused rather than inserting a
    // word the person did not see.
    ++m_pressId;
    m_barPressed = true;
    m_typedPress = !token.isEmpty() && token == m_typedWord.selectionToken() ? token : QString();
    m_swapPress = m_swap.active() && !token.isEmpty() && token == m_swap.token() ? token : QString();
    m_predictionTap = !m_swap.active() && !token.isEmpty() && token == selectionToken() ? m_nextWord.candidateStamp() : std::nullopt;
    return m_pressId;
}

void SwipeController::releaseCandidate(quint64 pressId)
{
    if (pressId == m_pressId) {
        m_predictionTap.reset();
        m_typedPress.clear();
        m_swapPress.clear();
        m_barPressed = false;
    }
}

void SwipeController::selectCandidate(int index)
{
    QPointer<SwipeController> self(this);
    const auto pol = policy();
    if (!m_typedWord.candidates().isEmpty()) {
        // Completion of tapped letters.
        if (!safePredictionContext()) {
            reset();
            return;
        }
        const auto previous = editorSnapshot();
        const QString prevForLearning = prevBeforeToken(previous, m_typedWord.token());
        const auto plan = m_typedWord.take(index, m_typedPress, previous, pol, m_epoch);
        m_typedPress.clear();
        if (!plan.isEmpty()) {
            if (plan.text.endsWith(QLatin1Char(' ')) && previous.cursor + plan.replaceFrom >= 0) {
                // WHY arm like Space: the accepted word ends with its automatic space, so next words follow it.
                auto expected = previous;
                expected.text.replace(previous.cursor + plan.replaceFrom, plan.replaceLength, plan.text);
                expected.cursor = previous.cursor + plan.replaceFrom + int(plan.text.size());
                expected.anchor = expected.cursor;
                m_nextWord.armExpectedEcho(expected, pol, m_epoch, predictionCapable(), previous);
            }
            stageOwnedCommit(plan, true);
            if (apply(plan)) {
                m_ownWords << plan.text.trimmed().toLower();
                m_autoSpaceCursor = plan.text.endsWith(QLatin1Char(' ')) ? previous.cursor + plan.replaceFrom + int(plan.text.size()) : -1;
                learnWord(plan.text.trimmed(), prevForLearning);
                if (!self) {
                    return;
                }
                observePredictionEcho();
            } else {
                m_nextWord.invalidate();
            }
        }
        refreshBar();
        notifyState();
        return;
    }
    if (m_swap.active() && !m_session.isActive()) {
        // Tap-to-swap: replaces exactly the tapped word, adds no space, moves nothing else.
        const auto editor = editorSnapshot();
        if (!safePredictionContext() || !tapSwapAllowed(editor)) {
            reset();
            return;
        }
        const auto pressed = m_swapPress;
        m_swapPress.clear();
        const QString oldWord = m_swap.word();
        const int oldStart = m_swap.start();
        const auto owned = m_recentSwipes.ownedAt(editor.text, editor.cursor);
        const auto plan = m_swap.take(index, pressed, editor.text, editor.cursor);
        if (plan.isEmpty()) {
            clearWordSwap();
            refreshBar();
            return;
        }
        m_recentSwipes.forget(editor.text.left(oldStart), oldWord);
        // WHY remembered again: the swapped-in word is the keyboard's own too, so tapping it again still works.
        if (owned && owned->span.start == oldStart) {
            // WHY the same path: the swapped-in word came from the same finger stroke, so a second tap re-scores it.
            m_recentSwipes.remember(editor.text.left(oldStart), plan.text, owned->trace, owned->keys, owned->context);
        }
        m_worker.invalidate();
        clearAfterSwipe();
        apply(plan);
        clearWordSwap();
        if (!self) {
            return;
        }
        refreshBar();
        notifyState();
        return;
    }
    if (!m_session.isActive() && !m_nextWord.candidates().isEmpty()) {
        // Next-word suggestion: inserted after the word, never a replacement.
        if (!safePredictionContext()) {
            reset();
            return;
        }
        const auto previous = editorSnapshot();
        const auto tap = m_predictionTap;
        m_predictionTap.reset();
        auto plan = tap ? m_nextWord.takePrediction(index, *tap, previous, pol, m_epoch, caseMode()) : CommitPlan{};
        // WHY (owner decision, 0.3.1): a picked prediction ends with a space, ready for the next word.
        if (!plan.isEmpty() && (previous.cursor >= previous.text.size() || !previous.text.at(previous.cursor).isSpace())) {
            plan.text += QLatin1Char(' ');
        }
        if (!tap) {
            m_nextWord.invalidate();
        }
        m_worker.invalidate();
        clearSession();
        clearAfterSwipe();
        if (!plan.isEmpty()) {
            auto expected = previous;
            expected.text.insert(expected.cursor, plan.text);
            expected.cursor += int(plan.text.size());
            expected.anchor = expected.cursor;
            m_nextWord.armExpectedEcho(expected, pol, m_epoch, predictionCapable(), previous);
            stageOwnedCommit(plan);
            const QString prev = m_nextPrev;
            if (apply({plan.text, 0, 0})) {
                m_ownWords << plan.text.trimmed().toLower();
                if (caseMode() == CaseMode::Capitalize) {
                    m_editor->releaseShift();
                }
                m_autoSpaceCursor = plan.text.endsWith(QLatin1Char(' ')) ? previous.cursor + int(plan.text.size()) : -1;
                learnWord(plan.text.trimmed(), prev);
                if (!self) {
                    return;
                }
                observePredictionEcho();
            }
        }
        refreshBar();
        notifyState();
        return;
    }
    // A swiped word's alternative. WHY only when the word still sits right before the cursor: otherwise the swap
    // would delete something else and append the guess ("helloyellow"). Doing nothing is safer.
    if (!safeContext() || !m_session.isActive() || !m_session.cursorStillAfterWord(textBeforeCursor())) {
        reset();
        return;
    }
    const CommitPlan plan = m_session.swapToAlternative(index);
    if (!plan.isEmpty()) {
        stageOwnedCommit(plan, true);
        if (apply(plan)) {
            if (!m_ownWords.isEmpty()) {
                m_ownWords.last() = plan.text.trimmed().toLower(); // the picked alternative replaces the swiped word
            }
            retractLastLearn(); // the picked word is the one they meant; the first guess's count is taken back
            learnSwiped(plan.text.trimmed(), m_lastLearnPrev);
        } else {
            m_session.clear();
        }
    }
    refreshBar();
    notifyState();
}

bool SwipeController::canRemove(int index)
{
    const QStringList shown = candidates();
    auto *model = personal();
    return model && index >= 0 && index < shown.size() && model->removable(personalKey(shown.at(index)).toStdString());
}

bool SwipeController::removeCandidate(int index)
{
    // Long press on a learned word in the row: "remove from my words". Ordinary dictionary words never offer it.
    if (!canRemove(index)) {
        return false;
    }
    auto *model = personal();
    model->forget(personalKey(candidates().at(index)).toStdString());
    m_learning.changed();
    syncLearnedWords();
    if (!m_typedWord.candidates().isEmpty()) {
        const auto editor = editorSnapshot();
        refreshTypedSuggestions(editor);
        requestContextCompletion(editor);
    } else {
        invalidatePredictions();
    }
    refreshBar();
    return true;
}

void SwipeController::forgetEverythingLearned()
{
    m_learning.forgetAll();
    m_lastLearn = {};
    m_lastLearnModel = nullptr;
    syncLearnedWords(true);
    invalidatePredictions();
    refreshBar();
}

QVariantMap SwipeController::diagnostics() const
{
    // WHY names and counts only, never words or field text.
    auto *self = const_cast<SwipeController *>(this);
    QVariantMap out;
    out.insert(QStringLiteral("frontEnd"), QStringLiteral("plasma-6.9"));
    out.insert(QStringLiteral("language"), m_profile.effectiveLanguage);
    out.insert(QStringLiteral("engineReady"), ready());
    out.insert(QStringLiteral("swipeEngine"), m_engine ? QString::fromLatin1(m_engine->mode()) : QString());
    out.insert(QStringLiteral("nextWord"), predictionCapable());
    out.insert(QStringLiteral("unavailableReason"), m_profile.unavailableReason);
    out.insert(QStringLiteral("swipes"), m_swipesAnswered);
    // WHY a flag and a count only (never words): shows which evidence the last tap-to-swap list used.
    out.insert(QStringLiteral("swapUsedPath"), m_lastSwapUsedPath);
    out.insert(QStringLiteral("swapWordsAfter"), m_lastSwapWordsAfter);
    out.insert(QStringLiteral("afterSwipeLists"), m_afterSwipeLists);
    // WHY counts and times only: whether swipes were re-ranked by the sentence model, and how long they waited for it.
    out.insert(QStringLiteral("swipeRescoreAsked"), m_swipeRescore->asked);
    out.insert(QStringLiteral("swipeRescoreScored"), m_swipeRescore->scored);
    out.insert(QStringLiteral("swipeRescoreMaxWaitMs"), m_swipeRescore->maxWaitMs);
    out.insert(QStringLiteral("policy"), policy().source == PredictionPolicySource::RawBridge ? QStringLiteral("app-declared") : QStringLiteral("owned-history"));
    const bool learning = self->m_learning.enabled();
    out.insert(QStringLiteral("learning"), learning);
    auto *pm = learning ? self->personal() : nullptr;
    out.insert(QStringLiteral("learnedWords"), pm ? quint64(pm->wordCount()) : quint64(0));
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Learning (same store and switch as the legacy front end: <data>/learn-typing, <data>/learning/LANG.swklearn)
// ---------------------------------------------------------------------------------------------------------------

swipekb::PersonalModel *SwipeController::personal()
{
    return m_profile.effectiveLanguage.isEmpty() ? nullptr : m_learning.model(m_profile.effectiveLanguage);
}

QString SwipeController::personalKey(const QString &word)
{
    return swipekb::displayForm(word).toLower().replace(QChar(0x2019), QLatin1Char('\''));
}

QString SwipeController::prevBeforeToken(const EditorSnapshot &editor, const QString &token) const
{
    if (token.isEmpty()) {
        return {};
    }
    QStringList words;
    if (policy().source == PredictionPolicySource::OwnedHistory) {
        words = m_ownedHistory.context();
        if (!words.isEmpty() && words.last() == token.toLower()) {
            words.removeLast();
        }
    } else {
        const auto before = editor.text.left(qMax(0, editor.cursor));
        if (!before.endsWith(token)) {
            return {};
        }
        words = swipekb::textContextWords(before.chopped(token.size()).right(4096));
    }
    return words.isEmpty() ? QString() : words.last();
}

swipekb::PersonalModel::Undo SwipeController::learnWord(const QString &rawWord, const QString &prev)
{
    // Hard rules: nothing before the owner switched learning on, nothing in a field the keyboard cannot vouch for.
    if (!m_learning.enabled() || !ready()) {
        return {};
    }
    const QString word = swipekb::displayForm(rawWord.trimmed());
    static const QRegularExpression lexical(QStringLiteral("^[\\p{L}]+(?:['’\\-][\\p{L}]+)*$"));
    if (word.isEmpty() || word.size() > 40 || !lexical.match(word).hasMatch()) {
        return {};
    }
    if (!safePredictionContext()) {
        return {};
    }
    auto *model = personal();
    if (!model) {
        return {};
    }
    const bool known = m_engine->hasWord(word.toStdString()) && !m_injected.contains(personalKey(word));
    if (!known) {
        // WHY: a lone letter, SHOUTING and "heyyyyy" are slips, not words worth remembering.
        static const QRegularExpression run(QStringLiteral("(.)\\1{3,}"));
        if (word.size() < 2 || word == word.toUpper() || run.match(word).hasMatch()) {
            return {};
        }
    }
    const bool capitalized = word.front().isUpper() && word != word.toUpper();
    const auto undo = model->learn(personalKey(prev).toStdString(), personalKey(word).toStdString(), known, capitalized, LearningManager::now());
    m_learning.changed();
    syncLearnedWords();
    return undo;
}

void SwipeController::learnSwiped(const QString &word, const QString &prev)
{
    m_lastLearn = learnWord(word, prev);
    m_lastLearnPrev = prev;
    m_lastLearnModel = m_lastLearn.valid ? personal() : nullptr;
}

void SwipeController::retractLastLearn()
{
    if (!m_lastLearn.valid) {
        return;
    }
    if (auto *model = personal(); model && model == m_lastLearnModel) {
        model->undo(m_lastLearn);
        m_learning.changed();
        syncLearnedWords();
    }
    m_lastLearn = {};
    m_lastLearnModel = nullptr;
}

QStringList SwipeController::personalize(const QStringList &base, const QString &prev, const QString &prefix, bool inject, int topK, double boostScale)
{
    auto *model = personal();
    const qint64 now = LearningManager::now();
    QStringList kept;
    for (const auto &word : base) {
        const auto key = personalKey(word);
        if (m_injected.contains(key) && (!model || !model->learnedNewWord(key.toStdString(), now))) {
            continue;
        }
        kept << word;
    }
    if (!model || model->empty()) {
        return kept;
    }
    std::vector<std::string> keys;
    keys.reserve(size_t(kept.size()));
    for (const auto &word : std::as_const(kept)) {
        keys.push_back(personalKey(word).toStdString());
    }
    QStringList out;
    for (const auto &ranked :
         model->rerank(keys, personalKey(prev).toStdString(), personalKey(prefix).toStdString(), now, size_t(qMax(0, topK)), inject, boostScale)) {
        QString shown = ranked.baseIndex >= 0 ? kept.at(ranked.baseIndex) : QString::fromStdString(ranked.key);
        if (model->wantsCapital(ranked.key) && !shown.isEmpty() && shown == shown.toLower()) {
            shown = shown.left(1).toUpper() + shown.mid(1);
        }
        out << shown;
    }
    return out;
}

void SwipeController::syncLearnedWords(bool force)
{
    auto *model = personal();
    const std::uint64_t version = model ? model->swipeWordsVersion() : 0;
    if (!force && model == m_syncedModel && version == m_syncedVersion && m_engine.get() == m_syncedEngine) {
        return;
    }
    m_syncedModel = model;
    m_syncedVersion = version;
    m_syncedEngine = m_engine.get();
    std::vector<std::pair<std::string, int>> words;
    if (model) {
        words = model->swipeWords(LearningManager::now());
    }
    for (const auto &w : words) {
        m_injected.insert(QString::fromStdString(w.first));
    }
    if (m_engine) {
        m_engine->setLearnedWords(words); // replaces the set: forgotten words leave swiping and completion at once
    }
}
