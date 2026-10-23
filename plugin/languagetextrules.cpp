#include "languagetextrules.h"
#include "languageprofile.h"
#include "swipekb/recognizer.hpp"
#include <QFile>
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
    // including interior punctuation and symbols in English. context.cpp has no FUTO/model dependency.
    QStringList out;
    for (const auto &word:contextWords(displayForm(text).toStdString(),maxWords)) out.append(QString::fromStdString(word));
    return out;
}
bool offeredUnprompted(const QString &word,const QString &language)
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
        it=blocked.insert(language,words);
    }
    const auto key=word.normalized(QString::NormalizationForm_C).toLower().replace(QChar(0x2019),QLatin1Char('\''));
    // WHY the part before an apostrophe too: "dick's" or "fuck'n" must not slip past a list of bare words.
    const auto apostrophe=key.indexOf(QLatin1Char('\''));
    return !it->contains(key) && (apostrophe<=0 || !it->contains(key.left(apostrophe)));
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
