// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "swapflow.h"

#include <unordered_set>

#include "languagetextrules.h"
#include "swipekb/language_engine.hpp"
#include "swipesession.h"

namespace swapflow {

namespace {
std::vector<std::string> toStd(const QStringList &l)
{
    std::vector<std::string> o;
    o.reserve(size_t(l.size()));
    for (const auto &s : l) o.push_back(s.toStdString());
    return o;
}
}

swipekb::SwapParams tapBackParams(const QString &language)
{
    return swipekb::SwapParams::forLanguage(language.toStdString()).forTapBack();
}

std::vector<swipekb::SwapCandidate> gather(swipekb::LanguageEngine &engine, const QString &word,
                                           const RecentSwipes::Keys &keys, const QStringList &before,
                                           const QStringList &after, const RecentSwipes::Trace &trace,
                                           const swipekb::SwapParams &params)
{
    const std::string w = word.toStdString();
    auto out = engine.swapCandidates(w, keys, toStd(before), toStd(after), trace, params);
    if (!out.empty()) return out;
    // WHY the old neighbours when there is no path: a typed word before any swipe measured the layout (or a word
    // with a digit) still gets the dictionary's other spellings and one-edit words, as before card #500. All score
    // the same here, so rankSwap keeps this order until the worker's sentence scores arrive.
    std::unordered_set<std::string> seen{word.toLower().toStdString()};
    auto add = [&](const std::string &x) {
        if (x.empty() || !seen.insert(QString::fromStdString(x).toLower().toStdString()).second) return;
        swipekb::SwapCandidate c;
        c.word = x;
        out.push_back(std::move(c));
    };
    for (const auto &o : engine.otherSpellings(word.toLower().toStdString())) add(o);
    for (const auto &c : engine.similar(w, 12)) add(c.word);
    return out;
}

QStringList tidySwapList(const QString &tapped, const std::vector<swipekb::SwapCandidate> &cands,
                         const swipekb::SwapParams &params, int limit, const Rules &rules, bool dress)
{
    // WHY rank them all, then tidy (as the swap bench does): the tidy-up may drop some, and the list must still fill
    // its places.
    QStringList list;
    for (const auto &x : swipekb::rankSwap(tapped.toStdString(), cands, params, int(cands.size())))
        list << QString::fromStdString(x);
    // WHY filtered unless the tapped word is one itself: a clean word is never answered with a swear word it merely
    // resembles; for a blocked word the person typed, its look-alikes derive from their word, so allowed.
    if (swipekb::offeredUnprompted(tapped, rules.language)) {
        QStringList clean;
        for (const auto &x : std::as_const(list))
            if (swipekb::offeredUnprompted(x, rules.language)) clean << x;
        list = clean;
    }
    // WHY boost 0 (the caller's callback): only drops forgotten words and taught words no longer wanted; order stays.
    if (rules.personalize) list = rules.personalize(list);
    list = swipekb::rankRegionalCandidates(list, rules.locale, limit);
    QStringList out;
    for (const auto &x : std::as_const(list)) {
        const QString dressed = dress ? WordSwap::matchCase(tapped, x) : x;
        if (dressed.compare(tapped, Qt::CaseInsensitive) == 0 || out.contains(dressed, Qt::CaseInsensitive)) continue;
        out << dressed;
    }
    return out;
}

QStringList wordsToScore(const QString &tapped, const std::vector<swipekb::SwapCandidate> &cands,
                         const swipekb::SwapParams &params, int cap)
{
    QStringList out;
    for (const auto &x : swipekb::rankSwap(tapped.toStdString(), cands, params, cap)) out << QString::fromStdString(x);
    return out;
}

bool applyScores(std::vector<swipekb::SwapCandidate> &cands, const QStringList &words, const QVector<float> &scores)
{
    if (words.isEmpty() || scores.size() != words.size()) return false;
    std::vector<swipekb::SwapCandidate> kept;
    kept.reserve(size_t(words.size()));
    for (qsizetype i = 0; i < words.size(); ++i) {
        const std::string w = words.at(i).toStdString();
        for (const auto &c : cands)
            if (c.word == w) {
                kept.push_back(c);
                kept.back().neural = scores.at(i);
                kept.back().hasNeural = true;
                break;
            }
    }
    if (kept.empty()) return false;
    cands = std::move(kept);
    return true;
}

QStringList wordsAfter(const QString &textAfterWord, int max)
{
    QStringList out;
    const QString text = textAfterWord.left(256);
    int i = 0;
    const int n = int(text.size());
    // WHY punctuation right after the word stops it: "good, then" or "good. Then" are not one phrase.
    if (i < n && !text.at(i).isSpace()) return out;
    while (int(out.size()) < max) {
        while (i < n && text.at(i).isSpace()) ++i;
        if (i >= n || !text.at(i).isLetter()) break;
        int e = i;
        while (e < n && (text.at(e).isLetter() || ((text.at(e) == u'\'' || text.at(e) == QChar(0x2019) || text.at(e) == u'-')
                                                  && e + 1 < n && text.at(e + 1).isLetter())))
            ++e;
        out << text.mid(i, e - i);
        // WHY stop at a sentence end or other punctuation: words of the next sentence are no evidence for this one.
        if (e < n && !text.at(e).isSpace()) break;
        i = e;
    }
    return out;
}

QStringList afterSwipeCandidates(const QString &first, const QStringList &swapList, const QString &otherSpelling)
{
    QStringList out{first};
    for (const auto &x : swapList)
        if (x.compare(first, Qt::CaseInsensitive) != 0 && !out.contains(x, Qt::CaseInsensitive)) out << x;
    return otherSpelling.isEmpty() ? out : SwipeSession::withOtherSpelling(out, otherSpelling);
}

std::optional<QStringList> PendingSwap::answer(const PredictionStamp &got, const QStringList &words,
                                               const QVector<float> &scores, const Rules &rules)
{
    if (!waiting || !(got == stamp)) return std::nullopt;
    waiting = false;
    if (!applyScores(cands, words, scores)) return std::nullopt;
    return tidySwapList(tapped, cands, params, limit, rules, dress);
}

AfterSwipeList afterSwipeList(const QString &first, std::vector<swipekb::SwapCandidate> cands, const QString &otherSpelling,
                              quint64 sessionSerial, const Rules &rules)
{
    AfterSwipeList out;
    const auto params = swipekb::SwapParams::forLanguage(rules.language.toStdString());
    // WHY 2: the bar shows the typed word's two other places (three with the other spelling), as before.
    const auto list = tidySwapList(first, cands, params, 2, rules, false);
    if (!list.isEmpty()) out.candidates = afterSwipeCandidates(first, list, otherSpelling);
    out.pending.tapped = first;
    out.pending.cands = std::move(cands);
    out.pending.params = params;
    out.pending.limit = 2;
    out.pending.dress = false;
    out.pending.otherSpelling = otherSpelling;
    out.pending.sessionSerial = sessionSerial;
    return out;
}

AfterSwipeList buildAfterSwipe(swipekb::LanguageEngine &engine, const QString &first, const RecentSwipes::Keys &keys,
                               const QStringList &context, const RecentSwipes::Trace &trace, const QString &otherSpelling,
                               quint64 sessionSerial, const Rules &rules)
{
    const auto params = swipekb::SwapParams::forLanguage(rules.language.toStdString());
    return afterSwipeList(first, gather(engine, first, keys, context, {}, trace, params), otherSpelling, sessionSerial, rules);
}

ScoredList handleScores(PendingSwap &tapBack, PendingSwap &afterSwipe, bool tapBackOpen, bool afterSwipeOpen,
                        const PredictionStamp &stamp, const QStringList &words, const QVector<float> &scores,
                        const Rules &rules)
{
    ScoredList out;
    if (tapBack.waiting && stamp == tapBack.stamp) {
        if (!tapBackOpen) { tapBack.clear(); return out; }
        if (const auto list = tapBack.answer(stamp, words, scores, rules)) out = {ScoredList::TapBack, *list};
        return out;
    }
    if (afterSwipe.waiting && stamp == afterSwipe.stamp) {
        if (!afterSwipeOpen) { afterSwipe.clear(); return out; }
        const auto list = afterSwipe.answer(stamp, words, scores, rules);
        // WHY an empty answer keeps the bar: an emptier bar would be worse than the list already shown.
        if (list && !list->isEmpty())
            out = {ScoredList::AfterSwipe, afterSwipeCandidates(afterSwipe.tapped, *list, afterSwipe.otherSpelling)};
    }
    return out;
}

} // namespace swapflow
