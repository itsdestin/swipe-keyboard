// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "typingsim.h"

#include <QChar>
#include <QRegularExpression>
#include <algorithm>
#include <cstdio>

#include "swipekb/prediction.hpp"
#include "swipekb/text.hpp"

namespace typingsim {
namespace {
// WHY case and apostrophe folded: capitals are the keyboard's display (Shift, sentence starts), and "c’est" offered
// for "c'est" is the same word to the person (the same rule as swipekb-nextword-eval's Gboard comparison).
QString fold(const QString &w) { return w.toLower().replace(QChar(0x2019), QLatin1Char('\'')); }
bool onBar(const QStringList &bar, const QString &word)
{
    const QString want = fold(word);
    return std::any_of(bar.begin(), bar.end(), [&](const QString &x) { return fold(x) == want; });
}
}

QString targetOf(const QString &piece)
{
    QString chunk = piece;
    // WHY trailing . ! ? removed first: contextWords starts a new sentence after them, which would otherwise drop the
    // sentence's last word.
    while (!chunk.isEmpty() && QStringLiteral(".!?").contains(chunk.back())) chunk.chop(1);
    const auto words = swipekb::contextWords(chunk.toStdString(), 1);
    if (words.empty() || swipekb::filterPredictedWords({{words.front(), 0.f}}, 1).empty()) return {};
    return QString::fromStdString(words.front());
}

void typeLine(const QString &line, const Bar &bar, Tally &t)
{
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    const QStringList pieces = line.split(spaces, Qt::SkipEmptyParts);
    if (pieces.isEmpty()) return;
    ++t.lines;
    QString text;   // the field so far: every earlier piece as written, each followed by a space
    for (const auto &piece : pieces) {
        const QString word = targetOf(piece);
        if (!word.isEmpty()) {
            const qsizetype len = word.size();
            ++t.words;
            t.baseline += size_t(len) + 1;
            bool done = false;
            if (bar.nextWord && onBar(bar.nextWord(text), word)) {
                ++t.taps;   // one tap on the suggestion; the keyboard adds the space
                ++t.fromBar[0];
                done = true;
            }
            // Letter by letter; after each one the bar may offer the word. WHY not after the last letter: the word is
            // then written, and tapping it (the typed letters' own slot) is the same one tap as Space.
            for (qsizetype i = 1; !done && i < len; ++i) {
                ++t.taps;   // letter i
                if (bar.completion && onBar(bar.completion(text, word.left(i)), word)) {
                    ++t.taps;
                    ++t.fromBar[std::min<qsizetype>(i, 3)];
                    done = true;
                }
            }
            if (!done) {
                t.taps += 2;   // the last letter and Space
                ++t.typedOut;
            }
        }
        text += piece + QLatin1Char(' ');
    }
}

std::string Tally::format() const
{
    char buf[240];
    std::snprintf(buf, sizeof buf, "typing: lines=%zu words=%zu baseline=%zu taps=%zu bar0=%zu bar1=%zu bar2=%zu bar3plus=%zu typedout=%zu\n",
                  lines, words, baseline, taps, fromBar[0], fromBar[1], fromBar[2], fromBar[3], typedOut);
    return buf;
}

} // namespace typingsim
