/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "swipekb/spellings.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <functional>
#include <string>
#include <utility>
#include <vector>
namespace swipekb {
inline constexpr auto TextRulesVersion = "unicode-latin-v1";
inline constexpr auto RegionalPolicyVersion = "en-regional-v1";
QString displayForm(const QString &text);
QString lookupForm(const QString &text, const QString &language);
QString applyTextCase(const QString &text, bool upper);
char32_t popupBase(char32_t symbol, const QString &language);
QString languageAlphabet(const QString &language);
QStringList textContextWords(const QString &text, int maxWords=20);
// False for swear words (packaging/blocked-words): never offered as a next word before anything is typed.
// Swiping them and completing them from typed letters stay allowed.
// `typedByPerson` (optional) lets a swear word the person has typed or swiped themselves pass: with "learn from my
// typing" on, a word they use often is offered like any other (owner request: "i swipe 'fuck' often").
bool offeredUnprompted(const QString &word, const QString &language,
                       const std::function<bool(const QString &)> &typedByPerson = {});
// Texting abbreviations and slang (packaging/texting-words/LANG.tsv, built into the program like the blocked words).
// `strength` is the dictionary's 0..255 score; `swear` marks a swear-word abbreviation (never offered unprompted).
struct TextingWord { QString word; int strength = 0; bool swear = false; };
// Reads one list. WHY tolerant: a damaged or hand-edited file must never stop the keyboard; every bad line (wrong
// column count, strength outside 1..255, a word with anything but letters, a very long word) is skipped and the
// good lines still count. A repeated word keeps its strongest line. At most 2000 words are read.
std::vector<TextingWord> parseTextingWords(const QByteArray &tsv, const QString &language);
// The list for a language ("en", "es", "fr", "de"; anything else gives an empty list). Test hook: when the
// environment variable SWIPEKB_TEXTING_WORDS names a folder holding LANG.tsv, that file is used INSTEAD of the built-in
// one, and an unreadable file there gives an empty list (never the built-in one, so a test can prove "ignored").
std::vector<TextingWord> textingWords(const QString &language);
// The same list as the (word, strength) pairs the engines' word-insert functions take.
std::vector<std::pair<std::string, int>> textingWordPairs(const QString &language);
// The built-in list of words shown with a capital letter (packaging/capitals/LANG.tsv, for swipekb::Capitals::fromTsv)
// for en, es and fr; empty for German (the dictionary keeps its own capitals) or any other language.
QByteArray capitalsList(const QString &language);
// capitalsList(language) as the engine's table, built once per language (nullptr for German and unknown languages).
// WHY inline here: only programs that also link the engine (the keyboards, Gaming Mode, the tools) call it, so the
// language-rules library itself needs no engine code.
inline std::shared_ptr<const Capitals> capitalsFor(const QString &language)
{
    static std::mutex lock;
    static std::map<QString, std::shared_ptr<const Capitals>> loaded;
    std::lock_guard<std::mutex> guard(lock);
    auto it = loaded.find(language);
    if (it == loaded.end()) it = loaded.emplace(language, Capitals::fromTsv(capitalsList(language).toStdString())).first;
    return it->second;
}
QStringList rankRegionalCandidates(const QStringList &words, const QString &locale, int limit,
                                   const QString &rawShape={});
} // namespace swipekb
