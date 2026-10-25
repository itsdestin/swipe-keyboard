// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "swipesession.h"

#include <algorithm>

#include "swipekb/text.hpp"

QStringList SwipeSession::contextWords(const QString &textBeforeCursor, int max)
{
    // WHY forwarded: the rules live in the engine (swipekb::contextWords) so the scoreboard
    // (tools/swipekb-bench) builds context exactly the way the live keyboard does.
    QStringList out;
    for (const std::string &w : swipekb::contextWords(textBeforeCursor.toStdString(), max))
        out << QString::fromStdString(w);
    return out;
}

bool SwipeSession::needsSpaceBefore(const QString &textBeforeCursor, SpaceContext ctx)
{
    // WHY: empty normally means "start of the field" (no space). But right after a swiped word it means
    // the app hides its text from the keyboard, and the word we just typed is really there.
    if (textBeforeCursor.isEmpty())
        return ctx.afterSwipe;
    const QChar c = textBeforeCursor.back();
    if (c.isSpace())
        return false;
    // WHY: in a web address or email field, "." "@" "/" join the parts of the address, so no space.
    if (ctx.urlOrEmailField && (c == u'.' || c == u'@' || c == u'/'))
        return false;
    // WHY straight quotes are special: the same " or ' both opens and closes a quote. It opens one (glue the
    // word on) only at the start of the text, after a space, or after an opening bracket: `say "` or `('`.
    // After a letter it closes one (`say "hello"`, `students'`), so the next word needs its space.
    if (c == u'"' || c == u'\'') {
        if (textBeforeCursor.size() == 1)
            return false;
        const QChar prev = textBeforeCursor.at(textBeforeCursor.size() - 2);
        return !(prev.isSpace() || prev == u'(' || prev == u'[' || prev == u'{');
    }
    // WHY: right after an opening bracket, a curly opening quote, a hyphen, slash, @ or # the word belongs
    // glued on. Curly opening quotes are unambiguous, so they always glue.
    static const QString glue = QStringLiteral("([{“‘-/@#");
    return !glue.contains(c);
}

bool SwipeSession::keepHiddenTextSpaceFallback(const QString &surroundingText, int cursor, int anchor,
                                                int cursorBeforeSwipe, int cursorAfterSwipe)
{
    // WHY full buffer + selection: Firefox select-all reports cursor 0 before deletion. Looking only
    // before that cursor mistakes an edited field for a hidden buffer and prefixes the next word with space.
    return surroundingText.isEmpty() && cursor >= 0 && cursor == anchor
        && (cursor == cursorBeforeSwipe || cursor == cursorAfterSwipe);
}

bool SwipeSession::spaceBeforeTappedKey(const QString &keyText, bool rightAfterSwipedWord, bool urlOrEmailField)
{
    // WHY letters and digits only: they start a new word. Punctuation (. , ! ? ; : ' " and closing quotes)
    // belongs glued to the word ("write."), and Backspace/Space/Enter must behave exactly as before.
    // WHY not in web-address/email fields: there a tapped letter usually continues the address
    // ("example" + tap "s"), and a stray space would break it; same rule as the swipe auto-space.
    if (!rightAfterSwipedWord || urlOrEmailField || keyText.size() != 1)
        return false;
    return keyText.front().isLetterOrNumber();
}

QStringList SwipeSession::withOtherSpelling(const QStringList &candidates, const QString &otherSpelling)
{
    if (candidates.isEmpty() || otherSpelling.isEmpty()
        || candidates.contains(otherSpelling, Qt::CaseInsensitive))
        return candidates;
    QStringList out = candidates;
    out.insert(1, otherSpelling);
    return out;
}

QString SwipeSession::applyCase(const QString &word, CaseMode mode)
{
    switch (mode) {
    case CaseMode::AllCaps:
        return word.toUpper();
    case CaseMode::Capitalize:
        // WHY a word with a capital inside is left alone: "iPhone", "eBay" or "YouTube" at a sentence start stay
        // as the word is written, not "IPhone".
        if (std::any_of(word.begin() + (word.isEmpty() ? 0 : 1), word.end(), [](QChar c) { return c.isUpper(); }))
            return word;
        return word.isEmpty() ? word : word.front().toUpper() + word.mid(1);
    case CaseMode::AsIs:
        break;
    }
    return word;
}

CommitPlan SwipeSession::commitSwipe(const QString &textBeforeCursor, const QStringList &candidates, CaseMode mode,
                                     SpaceContext ctx)
{
    clear();
    if (candidates.isEmpty())
        return {};
    m_candidates = candidates;
    m_current = 0;
    m_mode = mode;
    m_typedWord = applyCase(candidates.front(), mode);
    m_beforeSwipe = textBeforeCursor;
    m_insertedSeparator = needsSpaceBefore(textBeforeCursor, ctx);
    return {(m_insertedSeparator ? QStringLiteral(" ") : QString()) + m_typedWord, 0, 0};
}

QStringList SwipeSession::alternatives() const
{
    QStringList out;
    for (int i = 0; i < m_candidates.size(); ++i)
        if (i != m_current)
            out << applyCase(m_candidates.at(i), m_mode);
    return out;
}

bool SwipeSession::replaceAlternatives(const QStringList &candidates)
{
    if (!isActive() || m_current != 0 || candidates.isEmpty() || candidates.front() != m_candidates.front()
        || candidates == m_candidates)
        return false;
    m_candidates = candidates;
    return true;
}

CommitPlan SwipeSession::swapToAlternative(int index)
{
    if (!isActive() || index < 0 || index >= m_candidates.size() - 1)
        return {};
    ++m_serial;
    const int target = index < m_current ? index : index + 1; // skip the typed word in the list
    const QString old = m_typedWord;
    m_current = target;
    m_typedWord = applyCase(m_candidates.at(target), m_mode);
    return {m_typedWord, -static_cast<int>(old.size()), static_cast<int>(old.size())};
}

CommitPlan SwipeSession::takeBackspace(const QString &textBeforeCursor, bool hasSelection)
{
    CommitPlan plan;
    const QString inserted = (m_insertedSeparator ? QStringLiteral(" ") : QString()) + m_typedWord;
    // WHY exact prefix, not suffix: a moved cursor or unrelated field ending in the same word must
    // never authorize deletion. Hidden/changed context and selections keep Qt's ordinary backspace.
    if (isActive() && !hasSelection && !m_typedWord.isEmpty()
        && textBeforeCursor == m_beforeSwipe + inserted) {
        const int length = static_cast<int>(inserted.size());
        plan = {QString(), -length, length};
    }
    clear(); // even a rejected attempt consumes this one-shot opportunity
    return plan;
}

bool SwipeSession::cursorStillAfterWord(const QString &textBeforeCursor) const
{
    // WHY exact prefix: an earlier occurrence of the same word is a cursor move, not the last swipe.
    return isActive() && textBeforeCursor == m_beforeSwipe
        + (m_insertedSeparator ? QStringLiteral(" ") : QString()) + m_typedWord;
}

void SwipeSession::clear()
{
    ++m_serial;
    m_candidates.clear();
    m_current = -1;
    m_mode = CaseMode::AsIs;
    m_typedWord.clear();
    m_beforeSwipe.clear();
    m_insertedSeparator = false;
}
