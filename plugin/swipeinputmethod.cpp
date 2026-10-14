#include "swipeinputmethod.h"

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QtVirtualKeyboard/QVirtualKeyboardInputContext>

#include "swipekb/recognizer.hpp"

// Enable with QT_LOGGING_RULES="swipekb.info=true" to see each swipe's guesses and timing.
Q_LOGGING_CATEGORY(lcSwipe, "swipekb", QtWarningMsg)

using Type = QVirtualKeyboardSelectionListModel::Type;

// WHY one shared engine per process: Qt recreates the input method whenever the layout is rebuilt, and
// reloading the models each time would add a visible pause.
static swipekb::Recognizer &recognizer()
{
    static swipekb::Recognizer r;
    static const bool loaded = [] {
        QString dir = qEnvironmentVariable("SWIPEKB_DATA");
        if (dir.isEmpty())
            dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/swipe-keyboard");
        swipekb::ModelPaths paths = swipekb::ModelPaths::fromDir(dir.toStdString());
        if (qEnvironmentVariableIntValue("SWIPEKB_NO_DECODER"))
            paths.decoder.clear();
        const bool ok = r.init(paths);
        if (!ok)
            qCWarning(lcSwipe) << "swipe engine not loaded from" << dir << "- swiping disabled, tapping unaffected";
        return ok;
    }();
    Q_UNUSED(loaded);
    return r;
}

SwipeInputMethod::SwipeInputMethod(QObject *parent) : QVirtualKeyboardAbstractInputMethod(parent)
{
    // WHY: the models load on first use; doing it here makes the brief pause happen when the keyboard
    // layout loads, not mid-typing at the first key press.
    recognizer();
}

QList<QVirtualKeyboardInputEngine::InputMode> SwipeInputMethod::inputModes(const QString &)
{
    return {QVirtualKeyboardInputEngine::InputMode::Latin};
}

bool SwipeInputMethod::setInputMode(const QString &, QVirtualKeyboardInputEngine::InputMode)
{
    clearSession();
    // WHY: a mode change means keys may have been typed elsewhere (e.g. on the symbols page), so we can no
    // longer be sure the last thing typed was our swiped word. Missing one space is safer than adding one.
    clearAfterSwipe();
    return true;
}

bool SwipeInputMethod::setTextCase(QVirtualKeyboardInputEngine::TextCase textCase)
{
    m_textCase = textCase;
    return true;
}

bool SwipeInputMethod::keyEvent(Qt::Key, const QString &, Qt::KeyboardModifiers)
{
    // Any tapped key ends the swiped word's session (the guess bar no longer applies); false = Qt types it.
    clearSession();
    // WHY: after a tapped key the last thing typed is no longer a swiped word, so the "hidden text" space
    // rule must stop (tapping space then swiping must not give two spaces).
    clearAfterSwipe();
    return false;
}

QList<Type> SwipeInputMethod::selectionLists() { return {Type::WordCandidateList}; }

int SwipeInputMethod::selectionListItemCount(Type type)
{
    return type == Type::WordCandidateList ? m_session.alternatives().size() : 0;
}

QVariant SwipeInputMethod::selectionListData(Type type, int index, QVirtualKeyboardSelectionListModel::Role role)
{
    const QStringList alts = m_session.alternatives();
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
    QVirtualKeyboardInputContext *ic = inputContext();
    if (type != Type::WordCandidateList || !ic)
        return;
    // WHY: the swap deletes the typed word before the cursor. If we can't see that word there (the app hides
    // its text, or the cursor moved), the delete would hit nothing or the wrong text and the guess would just
    // be appended ("helloyellow"). Doing nothing is safer; the guess bar closes.
    if (!m_session.cursorStillAfterWord(textBeforeCursor())) {
        clearSession();
        return;
    }
    const CommitPlan plan = m_session.swapToAlternative(index);
    if (!plan.isEmpty())
        ic->commit(plan.text, plan.replaceFrom, plan.replaceLength);
    refreshGuessBar();
}

void SwipeInputMethod::reset()
{
    clearSession();
    // WHY: Qt resets on focus moves and field-type changes; whatever was typed before belongs to another field.
    clearAfterSwipe();
}

void SwipeInputMethod::update()
{
    // Qt calls this when the app reports a new text or cursor position. That includes the app echoing back
    // a word we just committed: the echo arrives later, asynchronously, not during our commit. Keep the
    // guess bar only while the cursor still sits right after the swiped word.
    const QString before = textBeforeCursor();
    if (m_session.isActive() && !m_session.cursorStillAfterWord(before))
        clearSession();
    // WHY: the after-swipe space flag only matters when the app hides its text. Once the app reports real
    // text, that text decides. If it reports nothing but the cursor landed somewhere other than just before
    // or just after our word, the user moved elsewhere, so stop assuming a word sits before the cursor.
    if (m_afterSwipe) {
        QVirtualKeyboardInputContext *ic = inputContext();
        const int pos = ic ? ic->cursorPosition() : -1;
        if (!before.isEmpty() || (pos != m_afterSwipeCursorBefore && pos != m_afterSwipeCursorAfter))
            clearAfterSwipe();
    }
}

bool SwipeInputMethod::isReady() const { return recognizer().ready(); }

QString SwipeInputMethod::textBeforeCursor() const
{
    QVirtualKeyboardInputContext *ic = inputContext();
    return ic ? ic->surroundingText().left(ic->cursorPosition()) : QString();
}

void SwipeInputMethod::finishSwipe(const QVariantList &points, const QVariantList &keys)
{
    QVirtualKeyboardInputContext *ic = inputContext();
    if (!ic || !recognizer().ready())
        return;
    QElapsedTimer timer;
    timer.start();

    std::vector<swipekb::TracePoint> trace;
    trace.reserve(points.size());
    for (const QVariant &v : points) {
        const QVariantMap m = v.toMap();
        trace.push_back({m.value("x").toFloat(), m.value("y").toFloat(), m.value("t").toFloat()});
    }
    std::vector<swipekb::KeyRect> rects;
    for (const QVariant &v : keys) {
        const QVariantMap m = v.toMap();
        const QString letter = m.value("letter").toString();
        if (letter.size() != 1)
            continue;
        rects.push_back({letter.at(0).toLatin1(), m.value("x").toFloat(), m.value("y").toFloat(),
                         m.value("w").toFloat(), m.value("h").toFloat()});
    }

    const QString before = textBeforeCursor();
    std::vector<std::string> context;
    for (const QString &w : SwipeSession::contextWords(before))
        context.push_back(w.toStdString());

    QStringList candidates;
    for (const auto &c : recognizer().recognize(trace, rects, context, 3)) {
        // WHY skip empty: an empty guess would type nothing yet still open a session and a blank guess-bar entry.
        if (!c.word.empty())
            candidates << QString::fromStdString(c.word);
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
        ic->commit(plan.text, plan.replaceFrom, plan.replaceLength);
        // Remember that a swiped word is now the last thing typed (see m_afterSwipe in the header).
        m_afterSwipe = true;
        m_afterSwipeCursorBefore = cursorBefore;
        m_afterSwipeCursorAfter = cursorBefore + static_cast<int>(plan.text.size());
    }
    refreshGuessBar();
    qCInfo(lcSwipe) << "swipe" << trace.size() << "points ->" << candidates << "in" << timer.elapsed() << "ms";
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

void SwipeInputMethod::refreshGuessBar()
{
    emit selectionListChanged(Type::WordCandidateList);
    emit selectionListActiveItemChanged(Type::WordCandidateList, -1);
}
