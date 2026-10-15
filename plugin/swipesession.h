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

// What the auto-space rule needs to know beyond the text before the cursor.
// WHY a struct (not loose bools): both facts travel together from the input method to the rule, and named
// fields keep call sites readable ({.afterSwipe = true} instead of a bare "true, false").
struct SpaceContext {
    // The last thing typed was a swiped word and no key was tapped since. WHY: some apps (XWayland/Steam,
    // some Chrome setups) never report their text, so the rule sees "" and would glue two swiped words
    // together ("helloworld"). This flag lets the second word still get its space.
    bool afterSwipe = false;
    // The field is a web address or email field. WHY: there "." "@" "/" are part of the address
    // ("example.com", "me@mail"), so a swiped word after them must not get a space.
    bool urlOrEmailField = false;
};

class SwipeSession {
public:
    // Last `max` words of the current sentence, oldest first, punctuation trimmed (context for the guesses).
    // WHY 20, not 3: FUTO's context model reads the whole sentence so far (its own test passes every earlier
    // word, and the engine itself keeps only as many as the model can take). Cutting to 3 words lowered
    // first-guess accuracy on the scoreboard from 93.32% to 93.01%; 20 covers any normal sentence.
    static QStringList contextWords(const QString &textBeforeCursor, int max = 20);
    // True when the swiped word must be preceded by a space (Gboard-style auto-space).
    static bool needsSpaceBefore(const QString &textBeforeCursor, SpaceContext ctx = {});

    // True when a tapped key must get a space typed before it: Gboard-style, tapping a letter or digit right
    // after a swiped word starts a new word ("write" + tap "a" = "write a", not "writea").
    // `rightAfterSwipedWord`: the cursor still sits right after the swiped word (or the app hides its text and
    // the swiped word was the last thing typed). Punctuation, quotes, Backspace, Space and Enter never get one.
    static bool spaceBeforeTappedKey(const QString &keyText, bool rightAfterSwipedWord, bool urlOrEmailField);
    // The guesses with the best guess's other spelling (from the dictionary, e.g. "it's" for "its") inserted
    // right after it, so it is the first guess-bar choice. WHY: one swipe shape is both words; the shape
    // cannot tell them apart, so the other one must be one tap away. Nothing is dropped.
    static QStringList withOtherSpelling(const QStringList &candidates, const QString &otherSpelling);

    // Starts a new session from the engine's best-first guesses. Empty plan if there are none.
    CommitPlan commitSwipe(const QString &textBeforeCursor, const QStringList &candidates, CaseMode mode,
                           SpaceContext ctx = {});
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
