// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "languagetextrules.h"
#include "languageprofile.h"
#include "swipekb/text.hpp"
#include <QFile>
#include <QDir>
#include <QRegularExpression>
#include <QHash>
#include <QSet>
#include <algorithm>
namespace swipekb {
QString displayForm(const QString &text) { return text.normalized(QString::NormalizationForm_C); }
QString lookupForm(const QString &text,const QString &language)
{
    QString result=displayForm(text).toLower();
    if (language=="fr") result.replace(QChar(0x2019), QChar('\''));
    // WHY never strip diacritics/expand ß here: lexical identity is distinct from a popup's physical key.
    return result;
}
QString applyTextCase(const QString &text,bool upper) { return upper ? displayForm(text).toUpper() : displayForm(text); }
char32_t popupBase(char32_t c,const QString &language)
{
    const QString symbol=QString::fromUcs4(&c,1);
    const QString alphabet=languageAlphabet(language);
    if (!alphabet.contains(symbol)) return c;
    // WHY declared tables, not a global accent remover: ñ is a separate Spanish physical key.
    const QStringList groups=language=="es" ? QStringList{"aá","eé","ií","oó","uúü"} :
        language=="fr" ? QStringList{"aàâæ","eéèêë","iîï","oôœ","uùûü","yÿ","cç"} :
        language=="de" ? QStringList{"aä","oö","uü","sß"} : QStringList{};
    for (const auto &group:groups) if (group.contains(symbol)) return group.front().unicode();
    return c;
}
QString languageAlphabet(const QString &language)
{
    const QString latin="abcdefghijklmnopqrstuvwxyz";
    if (language=="es") return latin+"áéíóúüñ";
    if (language=="fr") return latin+"àâéèêëîïôùûüÿçœæ";
    if (language=="de") return latin+"äöüß";
    return latin;
}
QStringList textContextWords(const QString &text,int maxWords)
{
    // WHY share the engine's pure context implementation: live swiping, prediction and replay must agree,
    // including interior punctuation and symbols in English. context.cpp has no model dependency.
    QStringList out;
    for (const auto &word:contextWords(displayForm(text).toStdString(),maxWords)) out.append(QString::fromStdString(word));
    return out;
}
std::vector<TextingWord> parseTextingWords(const QByteArray &tsv,const QString &language)
{
    std::vector<TextingWord> out;
    QHash<QString,int> at;
    // WHY letters of the language only: a swipe cannot draw digits or apostrophes, and the engines skip such words.
    static const QRegularExpression letters(QStringLiteral("^\\p{L}+$"));
    for (const auto &raw:QString::fromUtf8(tsv).split('\n')) {
        if (out.size()>=2000) break;
        const QString line=raw.trimmed().isEmpty() || raw.startsWith('#') ? QString() : raw;
        if (line.isEmpty()) continue;
        const auto cols=line.split('\t');
        if (cols.size()<3) continue;
        bool ok=false;
        const int strength=cols[1].trimmed().toInt(&ok);
        const QString word=displayForm(cols[0].trimmed()).toLower();
        const QString tag=cols[2].trimmed();
        if (!ok || strength<1 || strength>255 || word.isEmpty() || word.size()>24 || !letters.match(word).hasMatch()) continue;
        if (tag!=QLatin1String("-") && tag!=QLatin1String("swear")) continue;
        bool inAlphabet=true;
        // WHY the alphabet check: a word the language's keys cannot spell would only be skipped later, silently.
        if (!language.isEmpty()) {
            const auto alphabet=languageAlphabet(language);
            for (const QChar c:word) inAlphabet=inAlphabet && alphabet.contains(c);
        }
        if (!inAlphabet) continue;
        const bool swear=tag==QLatin1String("swear");
        const auto it=at.find(word);
        if (it==at.end()) { at.insert(word,int(out.size())); out.push_back({word,strength,swear}); }
        else {
            auto &e=out[size_t(it.value())];
            e.strength=std::max(e.strength,strength); e.swear=e.swear||swear;
        }
    }
    return out;
}
std::vector<TextingWord> textingWords(const QString &language)
{
    static const QSet<QString> supported{QStringLiteral("en"),QStringLiteral("es"),QStringLiteral("fr"),QStringLiteral("de")};
    if (!supported.contains(language)) return {};
    const QString override=qEnvironmentVariable("SWIPEKB_TEXTING_WORDS");
    // WHY not cached when overridden: tests change the folder between cases; the built-in lists are tiny anyway.
    QFile f(override.isEmpty() ? QStringLiteral(":/swipekb/texting/%1.tsv").arg(language)
                               : QDir(override).filePath(language+QStringLiteral(".tsv")));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return parseTextingWords(f.read(1<<20),language);
}
std::vector<std::pair<std::string,int>> textingWordPairs(const QString &language)
{
    std::vector<std::pair<std::string,int>> out;
    for (const auto &w:textingWords(language)) out.emplace_back(w.word.toStdString(),w.strength);
    return out;
}
bool offeredUnprompted(const QString &word,const QString &language,const std::function<bool(const QString &)> &typedByPerson)
{
    // WHY a per-language cache filled once: the lists are small built-in resources and this runs per suggestion.
    static QHash<QString,QSet<QString>> blocked;
    auto it=blocked.find(language);
    if (it==blocked.end()) {
        QSet<QString> words;
        QFile f(QStringLiteral(":/swipekb/blocked/%1.txt").arg(language));
        if (f.open(QIODevice::ReadOnly|QIODevice::Text))
            // WHY ß and ss both: German packs spell some words with ss (scheisse), the source list with ß.
            for (const auto &line:QString::fromUtf8(f.readAll()).split('\n',Qt::SkipEmptyParts))
                words << line.trimmed() << line.trimmed().replace(QChar(0xDF),QStringLiteral("ss"));
        // WHY the built-in texting list too: swear acronyms (wtf, stfu, ntm) are tagged there, never in the shared
        // word list; they can be swiped or completed but must not appear out of the blue. Read from the built-in
        // file only (not the test override), because this set is cached for the life of the process.
        QFile t(QStringLiteral(":/swipekb/texting/%1.tsv").arg(language));
        if (t.open(QIODevice::ReadOnly))
            for (const auto &w:parseTextingWords(t.readAll(),language)) if (w.swear) words << w.word;
        it=blocked.insert(language,words);
    }
    const auto key=word.normalized(QString::NormalizationForm_C).toLower().replace(QChar(0x2019),QLatin1Char('\''));
    // WHY the part before an apostrophe too: "dick's" or "fuck'n" must not slip past a list of bare words.
    const auto apostrophe=key.indexOf(QLatin1Char('\''));
    const bool blockedWord=it->contains(key) || (apostrophe>0 && it->contains(key.left(apostrophe)));
    // WHY the person's own word passes: they chose to type it, so seeing it offered is what they asked for.
    // Only the exact word counts, never a different word that merely resembles it.
    return !blockedWord || (typedByPerson && typedByPerson(key));
}
QStringList rankRegionalCandidates(const QStringList &words,const QString &locale,int limit,const QString &rawShape)
{
    if (limit<=0) return {};
    QStringList out=words;
    out.removeDuplicates();
    const auto region=canonicalLocale(locale);
    // WHY curated exact pairs only: UK Oxford -ize forms are valid; suffix rewriting destroys words.
    // Policy en-regional-v1 prefers local spelling only when both candidates survived shape scoring.
    static const std::pair<const char *, const char *> pairs[]={{"color","colour"},{"favorite","favourite"},
        {"center","centre"},{"theater","theatre"},{"honor","honour"},{"neighbor","neighbour"},
        {"traveling","travelling"},{"canceled","cancelled"},{"gray","grey"}};
    if (region=="en-US" || region=="en-GB") {
        for (const auto &[us,uk]:pairs) {
            const QString preferred=region=="en-GB" ? uk : us, other=region=="en-GB" ? us : uk;
            int a=-1,b=-1;
            for (int i=0;i<out.size();++i) {
                if (out[i].toLower()==preferred) a=i;
                if (out[i].toLower()==other) b=i;
            }
            // WHY insert rather than swap: swapping a distant preferred spelling would hide its alternative at truncation.
            if (a>b && b>=0) out.move(a,b);
        }
    }
    const auto ranked=out;
    QStringList protectedWords;
    if (!rawShape.isEmpty() && words.contains(rawShape)) protectedWords.append(rawShape);
    // WHY protect both safeguards together: replacing the last slot with rawShape can otherwise evict
    // the alternate spelling. Sacrifice unrelated candidates when capacity permits the pair plus raw.
    if (region=="en-US" || region=="en-GB") {
        for (const auto &word:ranked.mid(0,limit)) {
            for (const auto &[us,uk]:pairs) {
                if (word.toLower()!=us && word.toLower()!=uk) continue;
                const bool hasUS=std::any_of(ranked.begin(),ranked.end(),[&](const auto &c) { return c.toLower()==us; });
                const bool hasUK=std::any_of(ranked.begin(),ranked.end(),[&](const auto &c) { return c.toLower()==uk; });
                if (!hasUS || !hasUK) continue;
                QStringList proposed=protectedWords;
                for (const auto &candidate:ranked)
                    if ((candidate.toLower()==us || candidate.toLower()==uk) && !proposed.contains(candidate)) proposed.append(candidate);
                if (proposed.size()<=limit) protectedWords=proposed;
            }
        }
    }
    out=ranked.mid(0,limit);
    for (const auto &word:protectedWords) if (!out.contains(word)) out.append(word);
    while (out.size()>limit) {
        for (qsizetype i=out.size();i-->0;)
            if (!protectedWords.contains(out[i])) { out.removeAt(i); break; }
    }
    std::stable_sort(out.begin(),out.end(),[&](const auto &a,const auto &b) { return ranked.indexOf(a)<ranked.indexOf(b); });
    return out;
}
} // namespace swipekb
