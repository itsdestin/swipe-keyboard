.pragma library

// candidatebar.js — keeps the guess bar permanently shown while any swipe layer exists.
// WHY: by default Qt hides the guess bar when it's empty, so the keyboard grew on every swipe and shrank
// again, making the app above jump. The spec wants the keyboard one finger-height taller, all the time.
// WHY shared (".pragma library" = one copy for the whole keyboard): when Qt rebuilds the layout, the new
// swipe layer can appear BEFORE the old one is destroyed. If each layer saved and restored the setting on
// its own, the old one's restore would switch the bar back to auto-hide while the new one is in use.
// Counting the layers means only the first one saves the user's value and only the last one restores it.
// The setting lives in memory only (never written to disk), so a crash can't leave it changed.

var holders = 0
var saved = false

function pin(settings) {
    if (holders === 0)
        saved = settings.wordCandidateList.alwaysVisible
    holders += 1
    settings.wordCandidateList.alwaysVisible = true
}

function unpin(settings) {
    if (holders === 0)
        return
    holders -= 1
    if (holders === 0)
        settings.wordCandidateList.alwaysVisible = saved
}
