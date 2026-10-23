// WordSwap — "tap an existing word, see alternatives, swap it" (card #187). Pure text rules: finding the word
// under the cursor, spotting a real cursor move (a tap) versus the keyboard's own typing, building and
// ordering the alternatives, and the small memory of recent swipes. No Qt Virtual Keyboard types here, so it
// is unit-tested on its own (tests/tst_wordswap.cpp) and can move to KDE's new keyboard core (card #181).
#pragma once

#include <QString>
#include <QStringList>
#include <optional>
#include <deque>

#include "swipesession.h"

struct WordSpan {
    int start = 0;
    int length = 0;
    QString word;
    bool operator==(const WordSpan &) const = default;
};

class WordSwap {
public:
    // The word the cursor is inside of or directly next to (just after it, else just before it). A word is
    // letters with inner apostrophes/hyphens ("don't", "well-known"). Nothing when the cursor sits in space or
    // punctuation, or beside a word longer than 64 characters.
    static std::optional<WordSpan> wordAt(const QString &text, int cursor);
    // `replacement` dressed like `original`: ALL CAPS stays all caps, "Sea" -> "See", anything else as spelled.
    static QString matchCase(const QString &original, const QString &replacement);
    // Up to `max` alternatives, best first, never the word itself (any capitalization). Swipe candidates come
    // first but take at most two places so a context-ranked neighbour always gets one. `neighbours` are the
    // dictionary's one-edit words (most frequent first); `ranked` is the sentence model's order and only
    // reorders them. The result is in guess-bar order: with three, the strongest sits in the middle (same
    // layout as next-word predictions).
    static QStringList build(const QString &word, const QStringList &swipeCandidates, const QStringList &neighbours,
                             const QStringList &ranked, int max = 3);
};

// Tells a person's tap in the text (cursor moved, text did not) from the keyboard's own typing. WHY: after
// every key or swipe the cursor sits right after a word, which must not look like a tap. Stores only a hash
// and size of the text, never the text itself.
class CursorMoveTracker {
public:
    static qint64 nowMs();
    // Call whenever the keyboard itself is about to change the text (a key, a swipe, a picked word).
    void ownEdit(qint64 now = nowMs()) { m_pending = true; m_changed = false; m_since = now; }
    // True when this snapshot is a cursor move by the person: same text as last time, different cursor, and
    // no own edit still settling. WHY a settle time: Qt reports the text and the cursor of one edit as
    // separate notifications in either order, so the cursor can "move" right after the text changed. The edit
    // counts as settled 250 ms after its text arrived (or 1 s after it was made if the text never did).
    bool observe(const QString &text, int cursor, qint64 now = nowMs());
    void clear();
    static constexpr qint64 kSettleMs = 250, kGiveUpMs = 1000;
private:
    bool m_have = false, m_pending = false, m_changed = false;
    size_t m_hash = 0;
    qsizetype m_size = 0;
    int m_cursor = -1;
    qint64 m_since = 0;
};

// Memory-only record of the last few swiped words so a later tap can offer that swipe's other guesses.
// Bounded; never written to disk.
class RecentSwipes {
public:
    static constexpr int kMax = 8;
    // `textBeforeWord` = everything before the word when it was typed (incl. its auto-space).
    void remember(const QString &textBeforeWord, const QString &word, const QStringList &candidates);
    // Candidates of the swipe that typed `word` at this spot, or empty when the text around it changed.
    QStringList candidatesFor(const QString &textBeforeWord, const QString &word) const;
    void forget(const QString &textBeforeWord, const QString &word);
    void clear() { m_items.clear(); }
    int size() const { return int(m_items.size()); }
private:
    struct Item { QString tail; bool whole; QString word; QStringList candidates; };
    static constexpr int kTail = 32;
    static bool anchored(const Item &, const QString &before);
    std::deque<Item> m_items;
};

// The open "alternatives for this word" bar. Holds one word and its list; every later step re-checks the
// text so a swap can only ever replace exactly the word that was shown.
class WordSwapSession {
public:
    bool active() const { return m_active; }
    // Starts (or refreshes) for the word at the cursor. Returns false when there is no usable word or no alternative.
    bool begin(const QString &text, int cursor, const QStringList &swipeCandidates, const QStringList &neighbours);
    // The sentence model's order arrived; rebuilds the list. False when nothing changed.
    bool setRanked(const QStringList &ranked);
    // The editor still shows the same text with the cursor inside/next to the shown word.
    bool stillValid(const QString &text, int cursor) const;
    QStringList candidates() const { return m_candidates; }
    QString word() const { return m_span.word; }
    int start() const { return m_span.start; }
    QString token() const;
    // Replace exactly the shown word with candidates()[index]. Empty when anything no longer matches.
    // Relative to the cursor, like every other plan; adds no space.
    CommitPlan take(int index, const QString &pressedToken, const QString &text, int cursor);
    void clear();
private:
    bool m_active = false;
    WordSpan m_span;
    size_t m_hash = 0;
    qsizetype m_size = 0;
    QStringList m_swipe, m_neighbours, m_ranked, m_candidates;
    quint64 m_version = 0;
    static quint64 s_next;
};
