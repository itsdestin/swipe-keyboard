#pragma once
#include <QString>
#include <QStringList>
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
bool offeredUnprompted(const QString &word, const QString &language);
QStringList rankRegionalCandidates(const QStringList &words, const QString &locale, int limit,
                                   const QString &rawShape={});
} // namespace swipekb
