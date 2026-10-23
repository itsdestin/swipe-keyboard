// Offline authored/supplied fixtures only. Never opens a recorder, editor or runtime keyboard.
#include "languageevaluation.h"
#include "languageprofile.h"
#include "languagetextrules.h"
#include "languageruntime.h"
#include "swipekb/language_engine.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QCommandLineParser>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>
#include <stdexcept>

namespace {
void require(bool ok) { if(!ok) throw std::runtime_error("Malformed fixture or unsupported geometry/trace"); }
double number(const QJsonObject &o,const char *key) {
    const auto v=o[key]; require(v.isDouble() && std::isfinite(v.toDouble()) && std::abs(v.toDouble())<1e7); return v.toDouble();
}
QString text(const QJsonObject &o,const char *key) { require(o[key].isString() && !o[key].toString().isEmpty()); return o[key].toString(); }
std::vector<swipekb::KeyRect> keysFor(const QJsonObject &c) {
    require(c["keys"].isArray());
    std::vector<swipekb::KeyRect> keys;
    for(const auto &v:c["keys"].toArray()) {
        const auto k=v.toObject(); const auto letter=text(k,"letter").toUcs4(); require(letter.size()==1);
        keys.push_back({char32_t(letter[0]),float(number(k,"x")),float(number(k,"y")),float(number(k,"w")),float(number(k,"h"))});
    }
    require(keys.size()>=26 && keys.size()<=100);
    // WHY same ordered ASCII signature as SwipeCapture: never evaluate a QWERTY trace as AZERTY.
    auto sorted=keys;
    std::sort(sorted.begin(),sorted.end(),[](const auto &a,const auto &b) { return a.y==b.y ? a.x<b.x : a.y<b.y; });
    QString signature;
    for(const auto &k:sorted) if(k.letter>=U'a' && k.letter<=U'z') signature+=QChar(k.letter);
    const auto layout=text(c,"layout");
    require((layout=="qwerty" && signature=="qwertyuiopasdfghjklzxcvbnm") ||
            (layout=="qwertz" && signature=="qwertzuiopasdfghjklyxcvbnm") ||
            (layout=="azerty" && signature=="azertyuiopqsdfghjklmwxcvbn"));
    return keys;
}
std::vector<swipekb::TracePoint> traceFor(const QJsonObject &c,const std::vector<swipekb::KeyRect> &keys) {
    std::vector<swipekb::TracePoint> trace;
    const auto provenance=text(c,"provenance");
    // WHY synthetic-noisy-v1 shares the explicit-points path: its traces come from a noise model
    // (bench/languages/make-noisy-swipes.py) but are replayed exactly like supplied points.
    // It is never human, so the nativeReviewed check below rejects any claim of review.
    if(provenance=="supplied-human" || provenance=="synthetic-noisy-v1") {
        require(provenance!="synthetic-noisy-v1" || (c["nativeReviewed"].isBool() && !c["nativeReviewed"].toBool()));
        require(c["points"].isArray() && !c.contains("traceKeys"));
        double last=-1;
        for(const auto &v:c["points"].toArray()) {
            const auto p=v.toObject(); const auto t=number(p,"t"); require(t>=0 && t>=last); last=t;
            trace.push_back({float(number(p,"x")),float(number(p,"y")),float(t)});
        }
    } else {
        require(provenance=="synthetic-centered-v1" && !c.contains("points"));
        const auto letters=text(c,"traceKeys").toUcs4(); require(letters.size()<=100);
        for(char32_t letter:letters) {
            auto key=std::find_if(keys.begin(),keys.end(),[&](const auto &k) { return k.letter==letter; });
            require(key!=keys.end());
            const float x=key->x+key->w/2,y=key->y+key->h/2;
            if(trace.empty()) trace.push_back({x,y,0});
            else {
                const auto previous=trace.back();
                for(int i=1;i<=8;++i) trace.push_back({previous.x+(x-previous.x)*i/8,previous.y+(y-previous.y)*i/8,previous.t_ms+i*16});
            }
        }
    }
    require(trace.size()>=2 && trace.size()<=10000);
    return trace;
}
QSet<QString> dictionaryFor(const QString &path) {
    QFile f(path); require(f.open(QIODevice::ReadOnly)); QSet<QString> words;
    for(const auto &line:QString::fromUtf8(f.readAll()).split('\n')) if(line.startsWith(" word=")) {
        const auto end=line.indexOf(",f="); if(end>6) words.insert(line.mid(6,end-6));
    }
    require(!words.isEmpty()); return words;
}
struct Runtime { swipekb::ProfileSnapshot profile; std::unique_ptr<swipekb::LanguageEngine> engine; QSet<QString> dictionary; };
}
int main(int argc,char **argv)
{
    QCoreApplication app(argc,argv); QCommandLineParser parser; parser.addHelpOption();
    for(const auto &name:{"dataset","model-root","packs-root"}) parser.addOption(QCommandLineOption(name,name,"value"));
    // WHY inspect the existing beam without changing the default fixture or lexical matching policy.
    parser.addOption(QCommandLineOption("candidates","Bounded beam inspection (default evaluation unchanged)","count","10"));
    parser.process(app);
    bool validCount=false;
    const int candidateCount=parser.value("candidates").toInt(&validCount);
    if (!validCount || candidateCount<1 || candidateCount>100) return 2;
    for(const auto &name:{"dataset","model-root","packs-root"}) if(!parser.isSet(name)) parser.showHelp(2);
    QFile file(parser.value("dataset"));
    if(!file.open(QIODevice::ReadOnly) || file.size()>16*1024*1024) return 2;
    const auto bytes=file.readAll();
    QJsonParseError error; const auto doc=QJsonDocument::fromJson(bytes,&error); const auto data=doc.object();
    const QJsonObject matchingPolicy{{"display","NFC-case-sensitive"},
        {"lookup","shared-lookup-lowercase-NFC-no-diacritic-folding"},{"denominator","all-input-cases"}};
    if(error.error!=QJsonParseError::NoError || data["schema"].toInt()!=1 || !data["cases"].isArray() || data["cases"].toArray().isEmpty()) return 2;
    // WHY matching is not a tunable accuracy switch; an incompatible declared policy must not be ignored.
    if(data.contains("matchingPolicy") && data["matchingPolicy"].toObject()!=matchingPolicy) return 2;
    QMap<QString,std::shared_ptr<Runtime>> runtimes;
    QMap<QString,QList<QVariantMap>> groups;
    QSet<QString> ids; QVariantList results; QList<QVariantMap> all;
    for(const auto &value:data["cases"].toArray()) {
        const auto c=value.toObject(); QVariantMap result; QString group="invalid";
        try {
            const auto id=text(c,"id"), locale=text(c,"locale"), layout=text(c,"layout"), target=text(c,"target");
            require(!ids.contains(id)); ids.insert(id);
            require(!swipekb::canonicalLocale(locale).isEmpty() && target.size()<=100 && c["nativeReviewed"].isBool());
            group=swipekb::canonicalLocale(locale)+":"+layout;
            result={{"id",id},{"locale",locale},{"layout",layout},{"target",target},
                {"provenance",text(c,"provenance")},{"nativeReviewed",c["nativeReviewed"].toBool()}};
            const auto keys=keysFor(c); const auto trace=traceFor(c,keys);
            const auto language=swipekb::canonicalLocale(locale).section('-',0,0);
            require(swipekb::normalize(trace,keys,swipekb::languageAlphabet(language).toStdString(),swipekb::popupAliasesFor(language)).has_value());
            result["geometryCovered"]=true;
            if(!runtimes.contains(group)) {
                auto runtime=std::make_shared<Runtime>();
                runtime->profile=swipekb::resolveProfile(locale,layout,"Latin",parser.value(language=="en" ? "model-root" : "packs-root"));
                if(runtime->profile.capabilities.swipe) {
                    const auto &p=runtime->profile;
                    // WHY the plugin's own setup function and texting words: the bench measures what the keyboard does.
                    swipekb::LanguageEngineSetup setup;
                    runtime->dictionary=dictionaryFor(p.assets.value("dictionary").path);
                    runtime->engine=std::make_unique<swipekb::LanguageEngine>();
                    if(!swipekb::configureLanguageEngine(p,setup) || !runtime->engine->init(setup)) runtime->engine.reset();
                    else runtime->engine->liftWords(swipekb::textingWordPairs(language));
                }
                runtimes[group]=runtime;
            }
            const auto runtime=runtimes[group];
            if(!runtime->engine) {
                result["status"]="unavailable";
                result["reason"]=runtime->profile.unavailableReason.isEmpty() ? "Engine initialization failed" : runtime->profile.unavailableReason;
            } else {
                const auto start=std::clock();
                const auto candidates=runtime->engine->recognize(trace,keys,{},candidateCount);
                result["cpuMs"]=1000.0*(std::clock()-start)/CLOCKS_PER_SEC;
                QStringList words; for(const auto &candidate:candidates) words.append(QString::fromStdString(candidate.word));
                words=swipekb::rankRegionalCandidates(words,locale,candidateCount,QString::fromStdString(runtime->engine->bestShapeWord()));
                const auto score=swipekb::scoreLanguageCase(target,words,runtime->dictionary,language);
                for(auto it=score.begin();it!=score.end();++it) result[it.key()]=it.value();
                result["candidates"]=words; result["packId"]=runtime->profile.packId; result["packVersion"]=runtime->profile.packVersion;
                result["dictionarySha256"]=runtime->profile.assets.value("dictionary").sha256;
            }
        } catch(const std::exception &) { result["status"]="invalid"; result["reason"]="Malformed fixture or unsupported geometry/trace"; }
        // WHY even invalid/unavailable cases remain visible in overall and group accounting.
        groups[group].append(result); all.append(result); results.append(result);
    }
    QVariantMap summaries;
    for(auto it=groups.begin();it!=groups.end();++it) summaries[it.key()]=swipekb::summarizeLanguageCases(it.value());
    const QVariantMap report{{"schema",1},{"scope","offline-fixture-evaluation-not-runtime-status"},
        {"qualityClaim","Synthetic cases are not human/native-reviewed accuracy; no pass threshold"},
        {"textRulesVersion",swipekb::TextRulesVersion},{"regionalPolicyVersion",swipekb::RegionalPolicyVersion},
        {"matchingPolicy",matchingPolicy.toVariantMap()},
        {"datasetSha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())},
        {"totals",swipekb::summarizeLanguageCases(all)},{"profiles",summaries},{"cases",results}};
    QTextStream(stdout)<<QJsonDocument::fromVariant(report).toJson();
    return 0; // Metrics are results, not acceptance thresholds.
}
