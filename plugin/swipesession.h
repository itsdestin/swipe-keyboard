// SwipeSession — the text decisions around one swiped word: whether it needs a space before it, its
// capitalization, the guess-bar alternatives, and swapping the typed word for one of them.
// No keyboard or engine here, so every rule is unit-tested (tests/tst_swipesession.cpp).
#pragma once

#include <QString>
#include <QStringList>

// Replace `replaceLength` characters starting `replaceFrom` characters from the cursor (negative = before
// it) with `text`. An empty plan means "send nothing".
struct CommitPlan {
    QString text;
    int replaceFrom = 0;
    int replaceLength = 0;
    bool isEmpty() const { return text.isEmpty() && replaceLength == 0; }
};

enum class CaseMode { AsIs, Capitalize, AllCaps };

class SwipeSession {
public:
    // Last `max` words of the current sentence, oldest first, punctuation trimmed (context for the guesses).
    static QStringList contextWords(const QString &textBeforeCursor, int max = 3);
    // True when the swiped word must be preceded by a space (Gboard-style auto-space).
    static bool needsSpaceBefore(const QString &textBeforeCursor);

    // Starts a new session from the engine's best-first guesses. Empty plan if there are none.
    CommitPlan commitSwipe(const QString &textBeforeCursor, const QStringList &candidates, CaseMode mode);
    // Replaces the typed word with alternatives()[index]. Empty plan if inactive or out of range.
    CommitPlan swapToAlternative(int index);
    // The guesses not currently typed, best first, with the session's capitalization.
    QStringList alternatives() const;
    // False once the user has typed or moved the cursor away from the swiped word.
    bool cursorStillAfterWord(const QString &textBeforeCursor) const;
    void clear();
    bool isActive() const { return m_current >= 0; }

private:
    static QString applyCase(const QString &word, CaseMode mode);

    QStringList m_candidates;
    int m_current = -1;
    CaseMode m_mode = CaseMode::AsIs;
    QString m_typedWord;
};
