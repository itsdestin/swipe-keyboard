// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "barflow.h"

#include "languagetextrules.h"

namespace barflow {

QStringList nextWordOffer(const QStringList &workerWords, const QString &language, const QString &locale,
                          const Personalize &personalize, const TypedByPerson &typedByPerson)
{
    // WHY learned pairs join here: a word the person usually types after this one can lead the list, even when the
    // n-gram file did not list it. Forgotten words are dropped inside personalize().
    const QStringList personal = personalize ? personalize(workerWords) : workerWords;
    // WHY next words only: a swear word is never offered out of the blue, but swiping or completing one from typed
    // letters still works (Destin, 2026-10-22); one the person typed or swiped themselves passes.
    QStringList offered;
    for (const auto &word : personal)
        if (swipekb::offeredUnprompted(word, language, typedByPerson)) offered << word;
    return swipekb::rankRegionalCandidates(offered, locale, 12);
}

QStringList completionOffer(const QStringList &ranked, const QStringList &dictionary, const QString &locale,
                            const Personalize &personalize)
{
    // WHY context-ranked words first, dictionary after: the sentence picks among words that start with the typed
    // letters; the dictionary still offers one-letter-off corrections when nothing matches.
    QStringList words = ranked + dictionary;
    // WHY after both lists are joined: the person's own words (names, favourites after this word) are weighed against
    // the n-gram's order and the dictionary's, never replacing them wholesale.
    if (personalize) words = personalize(words);
    return swipekb::rankRegionalCandidates(words, locale, 3);
}

} // namespace barflow
