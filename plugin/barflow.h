// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Bar flow: how the guess bar's next-word and completion lists are put together from the prediction worker's answer,
// shared by BOTH front ends (plugin/swipeinputmethod.cpp and frontend-plasma69/src/swipecontroller.cpp) and by the
// typing bench (tools/swipekb-typing-bench), so the bar the bench grades is the bar a typist sees. The three slots
// are then chosen by barrules::nextWordShown / barrules::typedShown (nextwordsession.h).
// No keyboard state here: each front end passes its own personal-words step.
#pragma once

#include <QString>
#include <QStringList>
#include <functional>

namespace barflow {

// The front end's personalize(list, prev, prefix, true, 12) for this request (drops forgotten words, lifts the person's
// own); empty = no learning (the list is kept as it is).
using Personalize = std::function<QStringList(const QStringList &)>;
// Has the person typed this word themselves (a swear word they use may then be offered)? Empty = never.
using TypedByPerson = std::function<bool(const QString &)>;

// Next word: the worker's words (already in their capitals) -> the list for NextWordSession::accept.
// Personal words first, then no swear word out of the blue, then the regional spelling order, at most 12.
QStringList nextWordOffer(const QStringList &workerWords, const QString &language, const QString &locale,
                          const Personalize &personalize, const TypedByPerson &typedByPerson);

// Completion of typed letters: the sentence-ranked words (`ranked`, the worker's answer; empty = not arrived or none)
// followed by the dictionary's completions (`dictionary`, LanguageEngine::complete) -> the list for
// TypedWordSession::suggest. WHY one function for both moments: the dictionary list shows at once
// (refreshTypedSuggestions, `ranked` empty) and is replaced by the joined list when the worker answers.
QStringList completionOffer(const QStringList &ranked, const QStringList &dictionary, const QString &locale,
                            const Personalize &personalize);

} // namespace barflow
