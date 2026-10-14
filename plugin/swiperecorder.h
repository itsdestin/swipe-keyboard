// SwipeRecorder — opt-in recording of real swipes, so recognition can be measured and tuned offline
// (tools/swipekb-replay replays the recordings). Off unless the owner creates the file
// <swipe-keyboard data folder>/record; then each swipe adds one line to traces/swipes.jsonl.
// No keyboard here, so it is unit-tested on its own (tests/tst_swiperecorder.cpp).
#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>

namespace SwipeRecorder {

// The swipe-keyboard data folder: $SWIPEKB_DATA, else ~/.local/share/swipe-keyboard.
// WHY shared: the engine loads its models from this same folder, so the record switch and the recordings
// live next to the models the owner already knows about.
QString dataDir();

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

} // namespace SwipeRecorder
