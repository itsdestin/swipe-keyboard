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

bool SwipeSession::needsSpaceBefore(const QString &textBeforeCursor)
{
    if (textBeforeCursor.isEmpty())
        return false;
    const QChar c = textBeforeCursor.back();
    // WHY: right after an opening bracket/quote, a hyphen, slash, @ or # the word belongs glued on.
    static const QString glue = QStringLiteral("([{\"'“‘-/@#");
    return !c.isSpace() && !glue.contains(c);
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

CommitPlan SwipeSession::commitSwipe(const QString &textBeforeCursor, const QStringList &candidates, CaseMode mode)
{
    clear();
    if (candidates.isEmpty())
        return {};
    m_candidates = candidates;
    m_current = 0;
    m_mode = mode;
    m_typedWord = applyCase(candidates.front(), mode);
    return {(needsSpaceBefore(textBeforeCursor) ? QStringLiteral(" ") : QString()) + m_typedWord, 0, 0};
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
