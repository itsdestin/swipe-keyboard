// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Typing simulator for the user-experience scoreboard (bench/ux-score.sh, tools/swipekb-typing-bench): a person types a
// sentence letter by letter on the keyboard's three-slot bar. Before each word the bar shows next-word suggestions;
// after each letter it shows completions. When the word is on the bar the person taps it (one tap: the keyboard adds
// the space itself, for both kinds of suggestion); otherwise they type the next letter, and after the last letter a
// space. Counted against typing every letter plus a space. No engine here (only the keyboard's word rule), so it is
// tested with made-up bars (tools/tests/tst_typingsim.cpp).
// What it leaves out, on purpose: punctuation and numbers (typed the same either way, so counted in neither total),
// Shift (the keyboard capitalises sentence starts itself), and a person's learning (the bench has no history).
#pragma once

#include <QString>
#include <QStringList>
#include <cstddef>
#include <functional>
#include <string>

namespace typingsim {

struct Bar {
    // The bar before any letter of the next word; `text` is what is in the field so far (empty, or ending in a space).
    std::function<QStringList(const QString &text)> nextWord;
    // The bar after the letters `typed` of a word, with `text` before them.
    std::function<QStringList(const QString &text, const QString &typed)> completion;
};

struct Tally {
    size_t lines = 0, words = 0;
    size_t baseline = 0;   // every letter of every counted word, plus one space each
    size_t taps = 0;       // what the simulated person tapped
    // Words taken from the bar before any letter (0), after 1, 2, or 3 or more letters (3).
    size_t fromBar[4] = {};
    size_t typedOut = 0;   // typed to the last letter (the bar never offered the word in time)
    // One line, "typing: lines=N words=N baseline=N taps=N bar0=N bar1=N bar2=N bar3plus=N typedout=N", which
    // bench/ux-score.sh reads (counts only).
    std::string format() const;
};

// The word a person types for one space-separated piece of a sentence ("aquí." -> "aquí"), or empty when the piece
// is not a word the bar could offer (a number, a lone symbol): the keyboard's own rules (swipekb::contextWords,
// filterPredictedWords), as swipekb-nextword-eval picks its targets.
QString targetOf(const QString &piece);

// Types one sentence (`line`, words separated by spaces) and adds it to `tally`.
void typeLine(const QString &line, const Bar &bar, Tally &tally);

} // namespace typingsim
