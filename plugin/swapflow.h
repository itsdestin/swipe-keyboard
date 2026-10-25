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

// Step 2. Best first, at most `limit`, never `tapped` (any case). With `dress`, in `tapped`'s capitals
// (WordSwap::matchCase): a tapped word in the text carries its capitals ("Good" -> "Food"). WHY never after a swipe
// (dress false): there `tapped` is the dictionary spelling of the first guess ("I", German "Haus"), not what the
// person's text looks like; the swipe session applies Shift/Caps itself, so dressing would wrongly give "In", "Aus".
QStringList tidySwapList(const QString &tapped, const std::vector<swipekb::SwapCandidate> &cands,
                         const swipekb::SwapParams &params, int limit, const Rules &rules, bool dress);

// The words to send to the worker's scoreWords job: the list's best kMaxScored by rankSwap (before neural scores).
QStringList wordsToScore(const QString &tapped, const std::vector<swipekb::SwapCandidate> &cands,
                         const swipekb::SwapParams &params, int cap = kMaxScored);

// Fills `neural` from the worker's answer (words in the order asked) and drops the words that were not scored.
// WHY an exact match: the worker echoes the very strings it was asked about (PredictionJob::scoreWords, which are the
// candidates' own spellings from wordsToScore); its capitals rule is applied to word lists, never to these.
// WHY drop them: rankSwap compares neural scores when a word has one and the n-gram when it has none; a list with
// both would rank on two scales. False (and `cands` unchanged) when the answer is empty or does not line up.
bool applyScores(std::vector<swipekb::SwapCandidate> &cands, const QStringList &words, const QVector<float> &scores);

// Up to `max` words right after the tapped word (`textAfterWord` starts right after it), for the n-gram's "after"
// evidence. Stops at a sentence end (".", "!", "?"), at punctuation right after the word, or at anything that is not
// a word. WHY only RawBridge fields may call this: stock (owned-history) fields may be read only for the keyboard's
// own words, so there the caller passes no words after.
QStringList wordsAfter(const QString &textAfterWord, int max = 2);

// May the bar after a swipe ask the worker for sentence scores? Only in a field where next-word predictions may run
// (`predictionField`: the sentence model sees a field's words only there), with the language's sentence model
// (`capable`), and never while a next-word or completion list is wanted (`wordListWanted`). WHY the last: the worker
// keeps only its latest job, so the scoring job would cancel that list; the bar then just keeps the n-gram order.
// (Both front ends end the swipe's session before they arm a next-word or completion list, so today this case is
// stopped earlier too; the rule keeps it safe if that order ever changes.)
inline bool mayScoreAfterSwipe(bool predictionField, bool capable, bool wordListWanted)
{
    return predictionField && capable && !wordListWanted;
}

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
    bool dress = true;       // tidySwapList's `dress` (false for the bar after a swipe)
    bool waiting = false;
    // Post-swipe only: the other spelling to keep first, and the SwipeSession::serial() the list belongs to.
    QString otherSpelling;
    quint64 sessionSerial = 0;

    QStringList scoreList() const { return wordsToScore(tapped, cands, params); }
    // Starts waiting for the worker's answer under `stamp`. False (and not waiting) when there is nothing to score.
    bool arm(const PredictionStamp &s)
    {
        stamp = s;
        waiting = !scoreList().isEmpty();
        return waiting;
    }
    // The re-ranked, tidied list when `got` is this list's stamp; nothing otherwise. Stops waiting either way when
    // the stamp matches.
    std::optional<QStringList> answer(const PredictionStamp &got, const QStringList &words, const QVector<float> &scores,
                                      const Rules &rules);
    void clear() { *this = PendingSwap{}; }
};

// The bar right after a swipe: the full guess list for SwipeSession::replaceAlternatives (the first guess, then the
// swap list's best 2, then the other-spelling rule) and the PendingSwap to arm for the worker's scores.
// `candidates` empty = keep the swipe's own guesses (an emptier bar would be worse).
struct AfterSwipeList {
    QStringList candidates;
    PendingSwap pending;
};
// From gathered candidates (no engine needed; tested on its own).
AfterSwipeList afterSwipeList(const QString &first, std::vector<swipekb::SwapCandidate> cands, const QString &otherSpelling,
                              quint64 sessionSerial, const Rules &rules);
// The whole post-swipe build: gather (no words after: they do not exist yet) + afterSwipeList.
AfterSwipeList buildAfterSwipe(swipekb::LanguageEngine &engine, const QString &first, const RecentSwipes::Keys &keys,
                               const QStringList &context, const RecentSwipes::Trace &trace, const QString &otherSpelling,
                               quint64 sessionSerial, const Rules &rules);

// The worker's sentence scores arrived: which shown list they re-rank, and its new content. The front end decides
// whether each list may still change (`tapBackOpen`: the tap-back list is still the one shown under this stamp and
// no finger is down; `afterSwipeOpen`: the same for the bar after a swipe) and applies the result
// (TapBack: WordSwapSession::setList; AfterSwipe: SwipeSession::replaceAlternatives). A list that may no longer change
// stops waiting.
struct ScoredList {
    enum Target { None, TapBack, AfterSwipe } target = None;
    QStringList list;
};
ScoredList handleScores(PendingSwap &tapBack, PendingSwap &afterSwipe, bool tapBackOpen, bool afterSwipeOpen,
                        const PredictionStamp &stamp, const QStringList &words, const QVector<float> &scores,
                        const Rules &rules);

} // namespace swapflow
