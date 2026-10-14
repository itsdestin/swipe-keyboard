// SwipeOutcomeTracker — decides how each swiped word "ended", so we can measure how often the owner has to
// fix a swipe. Pure logic (no keyboard, no files): unit-tested in tests/tst_swipeoutcome.cpp.
//
// Each recorded swipe gets AT MOST ONE outcome line:
//   erase = first Backspace pressed while the cursor is still right after the swiped word (guess was wrong)
//   keep  = the owner typed a space/punctuation/letter or swiped again, with no erase or pick first
//   (pick lines are written by the guess-bar code; the tracker is only told, so it never adds a keep after one)
//   none  = anything else (focus change, cursor moved away): we cannot tell, so we say nothing.
#pragma once

#include <QString>
#include <QtGlobal>

class SwipeOutcomeTracker {
public:
    enum class Kind { None, Erase, Keep };
    struct Outcome {
        Kind kind = Kind::None;
        qint64 swipeTs = 0;   // the swipe line this outcome is about
        explicit operator bool() const { return kind != Kind::None; }
    };

    // A new swipe was typed. `ts` is its recorder timestamp, 0 if it was not recorded (nothing to point at).
    // Returns the PREVIOUS swipe's outcome: swiping again means the previous word was accepted (Keep).
    Outcome beginSwipe(qint64 ts);
    // A key was tapped. Backspace on a still-unresolved swipe -> Erase (first one only). A key that types
    // text (space, punctuation, letter, Enter) -> Keep. Keys with no text (Shift, arrows) change nothing.
    Outcome keyTapped(bool isBackspace, const QString &text);
    // The owner picked from the guess bar: that is the swipe's outcome (written elsewhere); stop tracking.
    void picked() { m_pendingTs = 0; }
    // Session ended some other way (focus change, cursor moved): no outcome at all.
    void abandon() { m_pendingTs = 0; }

private:
    qint64 m_pendingTs = 0;   // swipe waiting for its outcome, 0 = none
};
