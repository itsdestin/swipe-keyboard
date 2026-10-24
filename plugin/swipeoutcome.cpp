// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "swipeoutcome.h"

SwipeOutcomeTracker::Outcome SwipeOutcomeTracker::beginSwipe(qint64 ts)
{
    // WHY resolve then replace: one pending swipe at a time, so every swipe ends with at most one outcome.
    const Outcome prev = m_pendingTs ? Outcome{Kind::Keep, m_pendingTs} : Outcome{};
    m_pendingTs = ts;
    return prev;
}

SwipeOutcomeTracker::Outcome SwipeOutcomeTracker::keyTapped(bool isBackspace, const QString &text)
{
    if (!m_pendingTs)
        return {};
    // WHY Backspace checked first: its text may be non-empty on some setups, but it is never "acceptance".
    if (!isBackspace && text.isEmpty())
        return {};   // Shift, arrows etc. type nothing and say nothing about the word
    const Outcome out{isBackspace ? Kind::Erase : Kind::Keep, m_pendingTs};
    m_pendingTs = 0;   // WHY: later backspaces/keys must not add a second outcome
    return out;
}
