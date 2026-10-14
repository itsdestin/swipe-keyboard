#include "swipesession.h"

#include <QRegularExpression>
#include <algorithm>

QStringList SwipeSession::contextWords(const QString &textBeforeCursor, int max)
{
    // Only the current sentence counts: after . ! ? the next word starts fresh, like Gboard.
    QString sentence = textBeforeCursor;
    const qsizetype end = std::max({sentence.lastIndexOf(u'.'), sentence.lastIndexOf(u'!'), sentence.lastIndexOf(u'?')});
    if (end >= 0)
        sentence = sentence.mid(end + 1);

    QStringList words;
    for (QString w : sentence.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
        while (!w.isEmpty() && w.front().isPunct())
            w.remove(0, 1);
        while (!w.isEmpty() && w.back().isPunct())
            w.chop(1);
        if (!w.isEmpty())
            words << w;
    }
    return words.mid(std::max<qsizetype>(0, words.size() - max));
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

QString SwipeSession::applyCase(const QString &word, CaseMode mode)
{
    switch (mode) {
    case CaseMode::AllCaps:
        return word.toUpper();
    case CaseMode::Capitalize:
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
    return {(needsSpaceBefore(textBeforeCursor, ctx) ? QStringLiteral(" ") : QString()) + m_typedWord, 0, 0};
}

QStringList SwipeSession::alternatives() const
{
    QStringList out;
    for (int i = 0; i < m_candidates.size(); ++i)
        if (i != m_current)
            out << applyCase(m_candidates.at(i), m_mode);
    return out;
}

CommitPlan SwipeSession::swapToAlternative(int index)
{
    if (!isActive() || index < 0 || index >= m_candidates.size() - 1)
        return {};
    const int target = index < m_current ? index : index + 1; // skip the typed word in the list
    const QString old = m_typedWord;
    m_current = target;
    m_typedWord = applyCase(m_candidates.at(target), m_mode);
    return {m_typedWord, -static_cast<int>(old.size()), static_cast<int>(old.size())};
}

bool SwipeSession::cursorStillAfterWord(const QString &textBeforeCursor) const
{
    return isActive() && textBeforeCursor.endsWith(m_typedWord);
}

void SwipeSession::clear()
{
    m_candidates.clear();
    m_current = -1;
    m_mode = CaseMode::AsIs;
    m_typedWord.clear();
}
