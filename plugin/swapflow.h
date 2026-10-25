// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Swap flow (card #500, "one brain"): how a swap list is built, tidied and re-ranked, shared by BOTH front ends
// (plugin/swipeinputmethod.cpp and frontend-plasma69/src/swipecontroller.cpp) and by the swap bench, so the list a
// person sees is the list the scoreboard grades. Order of operations (tools/swipekb-swap-bench.cpp, new mode):
//   1. gather(): LanguageEngine::swapCandidates for the word (its remembered swipe, or a made-up path through its keys);
//   2. tidySwapList(): rankSwap over everything, then the keyboard's tidy-up (swear-word rule, the person's forgotten
//      words, regional spelling order, the tapped word's capitals), cut to the places shown;
//   3. the worker's sentence scores arrive (PendingSwap::answer()), fill each word's neural score, and step 2 runs again.
// No keyboard state here: each front end keeps its own PendingSwap and decides when a list may still change.
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <optional>
#include <vector>

#include "nextwordsession.h"
#include "swipekb/swap.hpp"
#include "wordswap.h"

namespace swipekb { class LanguageEngine; }

namespace swapflow {

// The front end's personal-words step (SwipeInputMethod/SwipeController::personalize with boost 0: drops forgotten
// and no-longer-wanted taught words, keeps the order). Empty = nothing dropped.
using Personalize = std::function<QStringList(const QStringList &)>;
struct Rules {
    QString language;   // effective language ("en", "de", ...): the swear-word list
    QString locale;     // requested locale ("en_GB"): regional spelling order
    Personalize personalize;
};

// WHY 32: a gathered list is at most SwapParams::pool (24) words plus 12 neighbours. The worker scores one word per
// network lookup, so the list sent is bounded; the words cut are the ones the path and n-gram already ranked lowest,
// which never reach the three places shown in practice.
inline constexpr int kMaxScored = 32;

// Step 1. `keys` = the key rectangles the trace was drawn on (or the current layout's, for a typed word; empty when
// no swipe has measured the layout yet). WHY a fallback: with no usable path (no keys yet, or a word with a digit)
// the engine has nothing to compare, so the dictionary's other spellings and one-edit neighbours are offered as
// before, in the dictionary's order.
std::vector<swipekb::SwapCandidate> gather(swipekb::LanguageEngine &engine, const QString &word,
                                           const RecentSwipes::Keys &keys, const QStringList &before,
                                           const QStringList &after, const RecentSwipes::Trace &trace,
                                           const swipekb::SwapParams &params);

// Step 2. Best first, at most `limit`, never `tapped` (any case), dressed in `tapped`'s capitals (WordSwap::matchCase).
QStringList tidySwapList(const QString &tapped, const std::vector<swipekb::SwapCandidate> &cands,
                         const swipekb::SwapParams &params, int limit, const Rules &rules);

// The words to send to the worker's scoreWords job: the list's best kMaxScored by rankSwap (before neural scores).
QStringList wordsToScore(const QString &tapped, const std::vector<swipekb::SwapCandidate> &cands,
                         const swipekb::SwapParams &params, int cap = kMaxScored);

// Fills `neural` from the worker's answer (words in the order asked) and drops the words that were not scored.
// WHY drop them: rankSwap compares neural scores when a word has one and the n-gram when it has none; a list with
// both would rank on two scales. False (and `cands` unchanged) when the answer is empty or does not line up.
bool applyScores(std::vector<swipekb::SwapCandidate> &cands, const QStringList &words, const QVector<float> &scores);

// Up to `max` words right after the tapped word (`textAfterWord` starts right after it), for the n-gram's "after"
// evidence. Stops at a sentence end (".", "!", "?"), at punctuation right after the word, or at anything that is not
// a word. WHY only RawBridge fields may call this: stock (owned-history) fields may be read only for the keyboard's
// own words, so there the caller passes no words after.
QStringList wordsAfter(const QString &textAfterWord, int max = 2);

// The post-swipe guess list: the committed word, then the swap list, then the other-spelling rule exactly as
// finishSwipe applies it ("it's" right after "its" unless already listed; SwipeSession::withOtherSpelling).
QStringList afterSwipeCandidates(const QString &first, const QStringList &swapList, const QString &otherSpelling);

// One shown swap list waiting for the worker's sentence scores. The front end owns one for tap-back and one for the
// post-swipe bar, and checks its own conditions (same word, no finger down) before calling answer().
struct PendingSwap {
    PredictionStamp stamp;
    QString tapped;
    std::vector<swipekb::SwapCandidate> cands;
    swipekb::SwapParams params;
    int limit = 3;
    bool waiting = false;
    // Post-swipe only: the other spelling to keep first, and the SwipeSession::serial() the list belongs to.
    QString otherSpelling;
    quint64 sessionSerial = 0;

    QStringList scoreList() const { return wordsToScore(tapped, cands, params); }
    // The re-ranked, tidied list when `got` is this list's stamp; nothing otherwise. Stops waiting either way when
    // the stamp matches.
    std::optional<QStringList> answer(const PredictionStamp &got, const QStringList &words, const QVector<float> &scores,
                                      const Rules &rules);
    void clear() { *this = PendingSwap{}; }
};

} // namespace swapflow
