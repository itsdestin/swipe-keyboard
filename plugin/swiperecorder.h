/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

// SwipeRecorder — opt-in recording of real swipes, so recognition can be measured and tuned offline
// (tools/swipekb-replay replays the recordings). Off unless the owner creates the file
// <swipe-keyboard data folder>/record; then each swipe adds one line to traces/swipes.jsonl
// ({"type":"swipe", ...}), and each guess-bar pick that swaps a swiped word adds a "pick" line naming the
// swipe it corrects ({"type":"pick","swipe_ts":...,"word":...}).
// No keyboard here, so it is unit-tested on its own (tests/tst_swiperecorder.cpp).
#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>

namespace SwipeRecorder {

// Writable options/recordings: $SWIPEKB_DATA, else ~/.local/share/swipe-keyboard.
QString dataDir();
// WHY separate: native packages own read-only models under /usr/share; personal typed text must
// remain in the user's private folder. Unpackaged installs keep the original shared-folder behavior.
QString modelDir();

// One swipe, as SwipeInputMethod saw it.
struct Entry {
    qint64 ts = 0;            // milliseconds since 1970
    QVariantList points;      // [{x, y, t}] in the touch area's pixels, t = ms since the swipe began
    QVariantList keys;        // [{letter, x, y, w, h}] letter key rectangles, same pixels
    QStringList context;      // the words before the cursor that were given to the engine
    QStringList candidates;   // the engine's guesses, best first
    QString typed;            // the word actually typed ("" if nothing was typed)
};

// Appends `e` as one JSON line to <dir>/traces/swipes.jsonl, but ONLY if <dir>/record exists.
// Returns true when a line was written. WHY check every time: it is a cheap file check, and it lets the
// owner switch recording on and off by creating or deleting a file, without restarting the keyboard.
bool appendIfEnabled(const QString &dir, const Entry &e);

// Appends a "pick" line: at time `ts` the owner tapped `word` in the guess bar, replacing the word typed for
// the swipe recorded with timestamp `swipeTs`. Same on/off switch as appendIfEnabled. If the owner picks
// again for the same swipe, a new line is added; the last pick for a swipe is the word that stayed.
// WHY a separate line (not editing the swipe line): the file is only ever appended to, so a crash can never
// damage earlier lines; and a picked word is the owner saying which word they meant, the most trustworthy
// label there is for later training and scoring.
bool appendPickIfEnabled(const QString &dir, qint64 ts, qint64 swipeTs, const QString &word);

// Appends an outcome line: {"ts", "type":"erase"|"keep", "swipe_ts"}. Same on/off switch.
// WHY: "erase" (owner backspaced the swiped word) and "keep" (owner moved on without fixing it) let
// tools/swipe-stats count how often a swipe needed fixing. Only call with type "erase" or "keep".
bool appendOutcomeIfEnabled(const QString &dir, qint64 ts, const QString &type, qint64 swipeTs);

} // namespace SwipeRecorder
