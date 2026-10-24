// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "wordswap.h"

#include <algorithm>
#include <chrono>

namespace {
bool innerChar(QChar c) { return c == u'\'' || c == QChar(0x2019) || c == u'-'; }
}

std::optional<WordSpan> WordSwap::wordAt(const QString &text, int cursor)
{
    if (cursor < 0 || cursor > text.size()) return std::nullopt;
    // A word character is a letter, or an apostrophe/hyphen that has a letter on both sides.
    auto isWordChar = [&](int i) {
        if (i < 0 || i >= text.size()) return false;
        const QChar c = text.at(i);
        if (c.isLetter()) return true;
        return innerChar(c) && i > 0 && i + 1 < text.size() && text.at(i - 1).isLetter() && text.at(i + 1).isLetter();
    };
    int at;
    if (isWordChar(cursor - 1)) at = cursor - 1;       // inside, or just after, a word
    else if (isWordChar(cursor)) at = cursor;           // just before a word
    else return std::nullopt;
    int s = at, e = at + 1;
    while (isWordChar(s - 1)) --s;
    while (isWordChar(e)) ++e;
    if (e - s > 64) return std::nullopt;
    return WordSpan{s, e - s, text.mid(s, e - s)};
}

QString WordSwap::matchCase(const QString &original, const QString &replacement)
{
    if (replacement.isEmpty() || original.isEmpty()) return replacement;
    int letters = 0, upper = 0;
    for (QChar c : original) if (c.isLetter()) { ++letters; if (c.isUpper()) ++upper; }
    if (letters > 1 && upper == letters) return replacement.toUpper();
    const QChar first = original.front();
    // "Sea" (or a lone "A"): first letter capital. WHY the rest is left as the dictionary spells it: German
    // nouns keep their capital inside a lower-case sentence ("haus" -> "Haus" is the dictionary's call).
    if (first.isUpper() && (upper == 1 || letters == 1)) return replacement.front().toUpper() + replacement.mid(1);
    return replacement;
}

QStringList WordSwap::build(const QString &word, const QStringList &swipeCandidates, const QStringList &neighbours,
                            const QStringList &ranked, int max)
{
    QStringList out;
    auto add = [&](const QString &w) {
        if (w.isEmpty() || w.compare(word, Qt::CaseInsensitive) == 0) return;
        const QString dressed = matchCase(word, w);
        if (!out.contains(dressed, Qt::CaseInsensitive)) out << dressed;
    };
    int fromSwipe = 0;
    for (const auto &w : swipeCandidates) {
        const int before = out.size();
        add(w);
        if (out.size() > before && ++fromSwipe == 2) break;
    }
    // WHY ranked first, then the dictionary's own order: the sentence only reorders the neighbours; words the
    // model did not score still follow, most frequent first.
    QStringList ordered;
    for (const auto &r : ranked)
        for (const auto &n : neighbours)
            if (n.compare(r, Qt::CaseInsensitive) == 0 && !ordered.contains(n)) ordered << n;
    for (const auto &n : neighbours) if (!ordered.contains(n)) ordered << n;
    for (const auto &n : ordered) { if (out.size() >= max) break; add(n); }
    while (out.size() > max) out.removeLast();
    if (out.size() == 3) out.swapItemsAt(0, 1);
    return out;
}

qint64 CursorMoveTracker::nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool CursorMoveTracker::observe(const QString &text, int cursor, qint64 now)
{
    const size_t h = qHash(text);
    const qsizetype n = text.size();
    const bool sameText = m_have && h == m_hash && n == m_size;
    if (m_pending) {
        if (m_have && !sameText) { m_changed = true; m_since = now; }
        if (m_changed ? now - m_since >= kSettleMs : now - m_since >= kGiveUpMs) m_pending = false;
    }
    const bool moved = m_have && sameText && cursor != m_cursor && !m_pending;
    m_have = true; m_hash = h; m_size = n; m_cursor = cursor;
    return moved;
}

void CursorMoveTracker::clear()
{
    m_have = m_pending = m_changed = false;
    m_hash = 0; m_size = 0; m_cursor = -1;
}

bool RecentSwipes::anchored(const Item &it, const QString &before)
{
    // WHY the whole text when it was short: a 5-character tail would match any other "hello ".
    return before.right(it.tail.size()) == it.tail && (!it.whole || before.size() == it.tail.size());
}

void RecentSwipes::remember(const QString &textBeforeWord, const QString &word, const QStringList &candidates)
{
    if (word.isEmpty() || candidates.isEmpty()) return;
    forget(textBeforeWord, word);
    m_items.push_back({textBeforeWord.right(kTail), textBeforeWord.size() <= kTail, word, candidates});
    while (int(m_items.size()) > kMax) m_items.pop_front();
}

QStringList RecentSwipes::candidatesFor(const QString &textBeforeWord, const QString &word) const
{
    for (auto it = m_items.rbegin(); it != m_items.rend(); ++it)
        if (it->word == word && anchored(*it, textBeforeWord)) return it->candidates;
    return {};
}

void RecentSwipes::forget(const QString &textBeforeWord, const QString &word)
{
    std::erase_if(m_items, [&](const Item &it) { return it.word == word && anchored(it, textBeforeWord); });
}

bool WordSwapSession::begin(const QString &text, int cursor, const QStringList &swipeCandidates,
                            const QStringList &neighbours)
{
    const auto span = WordSwap::wordAt(text, cursor);
    if (!span) { clear(); return false; }
    m_span = *span;
    m_hash = qHash(text); m_size = text.size();
    m_swipe = swipeCandidates; m_neighbours = neighbours; m_ranked.clear();
    m_candidates = WordSwap::build(m_span.word, m_swipe, m_neighbours, m_ranked);
    m_active = !m_candidates.isEmpty();
    if (!m_active) { clear(); return false; }
    m_version = ++s_next;
    return true;
}

bool WordSwapSession::setRanked(const QStringList &ranked)
{
    if (!m_active) return false;
    m_ranked = ranked;
    const auto next = WordSwap::build(m_span.word, m_swipe, m_neighbours, m_ranked);
    if (next == m_candidates) return false;
    m_candidates = next;
    m_version = ++s_next;
    return true;
}

bool WordSwapSession::stillValid(const QString &text, int cursor) const
{
    return m_active && qHash(text) == m_hash && text.size() == m_size
        && cursor >= m_span.start && cursor <= m_span.start + m_span.length
        && text.mid(m_span.start, m_span.length) == m_span.word;
}

QString WordSwapSession::token() const
{
    return m_active ? QStringLiteral("swap:%1").arg(m_version) : QString();
}

CommitPlan WordSwapSession::take(int index, const QString &pressedToken, const QString &text, int cursor)
{
    CommitPlan plan;
    // WHY the pressed token: the finger must have landed on the list that is still showing.
    if (m_active && !pressedToken.isEmpty() && pressedToken == token() && index >= 0 && index < m_candidates.size()
        && stillValid(text, cursor))
        plan = {m_candidates.at(index), m_span.start - cursor, m_span.length};
    clear();
    return plan;
}

void WordSwapSession::clear()
{
    m_active = false;
    m_span = {};
    m_hash = 0; m_size = 0;
    m_swipe.clear(); m_neighbours.clear(); m_ranked.clear(); m_candidates.clear();
}

quint64 WordSwapSession::s_next = 0;
