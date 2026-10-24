// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "languageevaluation.h"
#include "languagetextrules.h"
namespace swipekb {
QVariantMap scoreLanguageCase(const QString &target,const QStringList &candidates,
                             const QSet<QString> &dictionary,const QString &language)
{
    const auto exact=displayForm(target), lookup=lookupForm(target,language);
    QSet<QString> lookups;
    for(const auto &word:dictionary) lookups.insert(lookupForm(word,language));
    QStringList normalized, folded;
    int wrong=0;
    for(const auto &word:candidates) {
        normalized.append(displayForm(word)); folded.append(lookupForm(word,language));
        if(!lookups.contains(folded.last())) ++wrong;
    }
    // WHY absence is a miss, never a denominator filter. Exact case and lookup/diacritic identity are separate.
    return {{"status","evaluated"},{"targetInDictionary",lookups.contains(lookup)},
        {"top1",!normalized.isEmpty() && normalized.first()==exact},
        {"top3",normalized.mid(0,3).contains(exact)},
        {"lookupTop1",!folded.isEmpty() && folded.first()==lookup},
        {"lookupTop3",folded.mid(0,3).contains(lookup)},
        {"wrongDictionaryOutputs",wrong}};
}
QVariantMap summarizeLanguageCases(const QList<QVariantMap> &cases)
{
    QVariantMap out{{"total",cases.size()},{"evaluated",0},{"unavailable",0},{"invalid",0},
        {"geometryCovered",0},{"targetInDictionary",0},{"top1",0},{"top3",0},
        {"lookupTop1",0},{"lookupTop3",0},{"wrongDictionaryOutputs",0}};
    for(const auto &c:cases) {
        const auto status=c.value("status").toString();
        const auto bucket=status=="evaluated" || status=="unavailable" ? status : QString("invalid");
        out[bucket]=out[bucket].toInt()+1;
        for(const auto &key:{"geometryCovered","targetInDictionary","top1","top3","lookupTop1","lookupTop3"})
            out[key]=out[key].toInt()+int(c.value(key).toBool());
        out["wrongDictionaryOutputs"]=out["wrongDictionaryOutputs"].toInt()+c.value("wrongDictionaryOutputs").toInt();
    }
    return out;
}
}
