#include "languageprofile.h"
#include "languagetextrules.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QResource>

static void initLanguageAssets() { Q_INIT_RESOURCE(language_assets); }
namespace swipekb {
QString canonicalLocale(const QString &locale)
{
    QStringList parts = QString(locale).replace('_', '-').split('-');
    static const QRegularExpression language("^[A-Za-z]{2,3}$"), region("^([A-Za-z]{2}|[0-9]{3})$"), script("^[A-Za-z]{4}$");
    if (parts.isEmpty() || !language.match(parts[0]).hasMatch()) return {};
    parts[0] = parts[0].toLower();
    int i=1;
    if (i<parts.size() && script.match(parts[i]).hasMatch()) {
        parts[i] = parts[i].toLower(); parts[i][0] = parts[i][0].toUpper(); ++i;
    }
    if (i<parts.size() && region.match(parts[i]).hasMatch()) { parts[i] = parts[i].toUpper(); ++i; }
    // WHY reject unknown suffixes rather than accidentally selecting an unrelated language model.
    if (i != parts.size()) return {};
    // WHY these profiles declare Latin only; BCP47 suppress-script removes redundant Latn without losing region.
    if (parts.size()>1 && parts[1]=="Latn" && QStringList{"en","es","fr","de"}.contains(parts[0])) parts.removeAt(1);
    return parts.join('-');
}
static QString localPath(const QString &root, const QString &relative)
{
    if (relative.isEmpty() || QDir::isAbsolutePath(relative)) return {};
    const QString base = QFileInfo(root).canonicalFilePath();
    const QString path = QFileInfo(QDir(root).filePath(relative)).canonicalFilePath();
    // WHY canonical: reject symlink/traversal escapes even when the manifest's relative spelling looks safe.
    if (base.isEmpty() || !path.startsWith(base + '/') || !QFileInfo(path).isFile()) return {};
    return path;
}
static QString digest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}
static QJsonObject readObject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size()>1024*1024) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
static bool contains(const QJsonValue &v, const QString &value) { return v.toArray().contains(value); }
// WHY one checker for every language: English (no manifest) and es/fr/de (manifest) must accept exactly the same
// next-word files. Everything is judged against the repository lock, never against what a manifest claims:
// the n-gram + word list, the credit file and all licence/notice texts that must travel with them.
struct ContextFiles { QString model, vocab, nwp; };
static bool contextFilesVerified(const QString &language, const QString &dir, const QJsonObject &lock, ContextFiles &out)
{
    const auto pins=lock["multilingual_context_models"].toObject()[language].toObject();
    const auto shared=lock["language_files"].toObject();
    if (pins.isEmpty() || shared.isEmpty() || dir.isEmpty()) return false;
    auto check=[&](const QString &name, const QString &sha, qint64 bytes, QString *path=nullptr) {
        const auto found=localPath(dir,name);
        if (found.isEmpty() || sha.size()!=64 || (bytes>=0 && QFileInfo(found).size()!=bytes) || digest(found)!=sha) return false;
        if (path) *path=found;
        return true;
    };
    ContextFiles files;
    if (!check(language+".ngram",pins["modelSha256"].toString(),pins["modelBytes"].toInteger(-1),&files.model) ||
        !check(language+".vocab.tsv",pins["vocabSha256"].toString(),pins["vocabBytes"].toInteger(-1),&files.vocab) ||
        !check(language+"-CONTRIBUTORS.txt",pins["contributorsSha256"].toString(),-1) ||
        !check("TATOEBA-CC-BY-2.0-FR.txt",lock["tatoeba_license_notice_sha256"].toString(),-1) ||
        !check(shared["odcTerms"].toObject()["path"].toString(),shared["odcTerms"].toObject()["sha256"].toString(),-1) ||
        !check(shared["notice"].toObject()["path"].toString(),shared["notice"].toObject()["sha256"].toString(),-1))
        return false;
    // WHY Apache-2.0 only for English: its dictionary keeps AOSP's word list (Apache-2.0), whose text must travel with
    // the files; the other languages' word lists come only from FineWeb/Tatoeba (ODC-By + CC BY 2.0 FR, checked above).
    const auto apache=shared["apacheTerms"].toObject();
    if (language=="en" && !check(apache["path"].toString(),apache["sha256"].toString(),-1)) return false;
    // WHY optional and judged only by the lock: the neural next-word file (LANG.nwp.bin) sits beside the n-gram and
    // improves its suggestions. A missing, truncated or swapped file leaves `files.nwp` empty, so only that model is
    // switched off: the n-gram, the word list and the language itself stay exactly as they were.
    const auto nwpPin=lock["trained_models"].toObject()["nextWord"].toObject()[language].toObject();
    if (!nwpPin.isEmpty() && nwpPin["path"].toString()==language+".nwp.bin")
        check(nwpPin["path"].toString(),nwpPin["sha256"].toString(),nwpPin["bytes"].toInteger(-1),&files.nwp);
    out=files; return true;
}
// WHY the manifest is only cross-checked: it may describe the context files, but it cannot widen what the lock allows.
static bool manifestContextMatchesLock(const QString &language, const QJsonObject &m, const QJsonObject &lock)
{
    const auto pins=lock["multilingual_context_models"].toObject()[language].toObject();
    const auto shared=lock["language_files"].toObject();
    const auto odc=shared["odcTerms"].toObject(), notice=shared["notice"].toObject();
    const auto assets=m["assets"].toObject(), terms=m["contextLicense"].toObject();
    const auto model=assets["contextModel"].toObject(), vocab=assets["contextVocab"].toObject();
    return !pins.isEmpty() && !shared.isEmpty() && m["schema"].toInt()==3 && m["contextFormat"].toString()=="swk-ngram-v2" &&
        model["path"].toString()==language+".ngram" && model["sha256"].toString()==pins["modelSha256"].toString() &&
        model["size"].toInteger(-1)==pins["modelBytes"].toInteger() &&
        vocab["path"].toString()==language+".vocab.tsv" && vocab["sha256"].toString()==pins["vocabSha256"].toString() &&
        vocab["size"].toInteger(-1)==pins["vocabBytes"].toInteger() &&
        // WHY both sources named: the files contain FineWeb web text (ODC-By) and Tatoeba sentences (CC BY 2.0 FR).
        terms["id"].toString()=="ODC-By-1.0 AND CC-BY-2.0-FR" &&
        terms["attributionPath"].toString()==language+"-CONTRIBUTORS.txt" &&
        terms["attributionSha256"].toString()==pins["contributorsSha256"].toString() &&
        terms["termsPath"].toString()=="TATOEBA-CC-BY-2.0-FR.txt" &&
        terms["termsSha256"].toString()==lock["tatoeba_license_notice_sha256"].toString() &&
        terms["webTermsPath"].toString()==odc["path"].toString() && terms["webTermsSha256"].toString()==odc["sha256"].toString() &&
        terms["noticePath"].toString()==notice["path"].toString() && terms["noticeSha256"].toString()==notice["sha256"].toString() &&
        terms["sourceUrl"].toString()==lock["language_files"].toObject()["baseUrl"].toString()+language+".ngram" &&
        !terms["attribution"].toString().isEmpty();
}
static ProfileSnapshot unavailable(ProfileSnapshot p, const QString &reason)
{
    p.assets.clear(); p.capabilities = {}; p.unavailableReason = reason; return p;
}
// The lock-pinned swipe model, when it sits in one of these folders (the first folder that holds a file decides).
// WHY optional: without it the engine decodes swipes by geometry, so a missing or altered model never stops swiping.
static void addSwipeModel(ProfileSnapshot &p, const QStringList &dirs)
{
    for (const auto &dir:dirs) {
        if (!QFileInfo::exists(QDir(dir).filePath("swipe-model.bin"))) continue;
        const auto path=verifiedSwipeModel(dir);
        if (!path.isEmpty()) p.assets["swipeModel"]={path,digest(path),"swipe-model-v1"};
        return;
    }
}
static ProfileSnapshot english(ProfileSnapshot p, const QString &root)
{
    initLanguageAssets();
    const auto lock = readObject(":/swipekb/assets.lock.json");
    // WHY only the dictionary is required: English swipes and completes from it alone; the swipe model and the
    // n-gram pack add accuracy and next-word suggestions when they are installed and verified.
    const auto expected=lock["language_dictionaries"].toObject()["en"].toObject()["sha256"].toString();
    const auto dictionary=localPath(root,"dict/en_swipe.combined");
    if (dictionary.isEmpty() || expected.size()!=64 || digest(dictionary)!=expected)
        return unavailable(p,"Bundled English dictionary missing or checksum mismatch");
    p.assets["dictionary"]={dictionary,expected,"en"};
    p.packId="bundled-en"; p.packVersion=lock["language_files"].toObject()["release"].toString();
    p.capabilities={true,false,false};
    const QStringList dirs{QDir(root).filePath("language-packs"),root};
    addSwipeModel(p,dirs);
    // WHY missing or damaged next-word files are not an error: English then swipes and completes from the
    // dictionary, without next-word suggestions.
    ContextFiles ngram;
    for (const auto &dir:dirs) {
        if (!QFileInfo::exists(QDir(dir).filePath("en.ngram"))) continue;
        if (contextFilesVerified("en",dir,lock,ngram)) {
            const auto pins=lock["multilingual_context_models"].toObject()["en"].toObject();
            p.assets["contextModel"]={ngram.model,pins["modelSha256"].toString(),"en:context-ngram-v1"};
            p.assets["contextVocab"]={ngram.vocab,pins["vocabSha256"].toString(),"en:context-ngram-v1"};
            if (!ngram.nwp.isEmpty()) p.assets["contextNwp"]={ngram.nwp,lock["trained_models"].toObject()["nextWord"].toObject()["en"].toObject()["sha256"].toString(),"en:context-nwp-v1"};
            p.capabilities={true,true,true};
        }
        break;
    }
    return p;
}
QString verifiedSwipeModel(const QString &modelRoot)
{
    initLanguageAssets();
    const auto pin=readObject(":/swipekb/assets.lock.json")["trained_models"].toObject()["swipe"].toObject();
    const auto sha=pin["sha256"].toString();
    // WHY the file name only: the lock names its download place (language-files/swipe-model.bin); installed, it sits
    // in the language-pack folder beside every language's files.
    const auto file=QFileInfo(pin["path"].toString()).fileName();
    if (sha.size()!=64 || file.isEmpty()) return {};
    const auto path=localPath(modelRoot,file);
    if (path.isEmpty() || QFileInfo(path).size()!=pin["bytes"].toInteger(-1) || digest(path)!=sha) return {};
    return path;
}
ProfileSnapshot resolveProfile(const QString &locale, const QString &layoutId, const QString &inputMode,
                               const QString &modelRoot, quint64 epoch)
{
    ProfileSnapshot p;
    p.requestedLocale=canonicalLocale(locale); p.effectiveLanguage=p.requestedLocale.section('-',0,0);
    p.layoutId=layoutId; p.inputMode=inputMode; p.epoch=epoch;
    p.profileId=p.requestedLocale+":"+layoutId+":"+inputMode;
    p.textRulesVersion=TextRulesVersion;
    if (p.requestedLocale.isEmpty()) return unavailable(p,"Unsupported or malformed locale tag");
    const auto script=p.requestedLocale.section('-',1,1);
    if (script.size()==4) return unavailable(p,"Unsupported script; these profiles declare Latin only");
    if (inputMode!="Latin" || !QStringList{"qwerty","azerty","qwertz"}.contains(layoutId))
        return unavailable(p,"Unsupported physical layout or input mode");
    // WHY accept the installed model root as well as an explicit pack root, without changing layout identity.
    const QString root=p.effectiveLanguage=="en" || QFileInfo::exists(QDir(modelRoot).filePath(p.effectiveLanguage+".json"))
        ? modelRoot : QDir(modelRoot).filePath("language-packs");
    if (p.effectiveLanguage=="en") {
        p.regionalPolicyVersion=RegionalPolicyVersion;
        return english(p,root);
    }
    if (!QStringList{"es","fr","de"}.contains(p.effectiveLanguage)) return unavailable(p,"Unsupported language");
    // WHY no opt-in: the existing Qt language selection is the owner's approved selection contract.
    const auto m=readObject(QDir(root).filePath(p.effectiveLanguage+".json"));
    if (m.isEmpty()) return unavailable(p,"Language pack manifest missing or invalid");
    // WHY schema 3 / "swipekb-open-v1" only: older packs were built for another swipe engine and carry files this
    // keyboard no longer reads; they are refused (the language is then shown as unavailable), never half-used.
    if (m["schema"].toInt()!=3 || m["id"].toString().isEmpty() || m["version"].toString().isEmpty() ||
        m["language"].toString()!=p.effectiveLanguage ||
        m["compatibility"].toString()!="swipekb-open-v1" || !contains(m["scripts"],"Latn") ||
        m["textRulesVersion"].toString()!=TextRulesVersion ||
        // WHY a single accepted transformation: the word lists are counted from FineWeb/Tatoeba (scripts/lm/count-words.py);
        // any other transform version fails closed and every role hash/size remains verified.
        m["transformationVersion"].toString()!="fineweb-counts-v1") return unavailable(p,"Pack schema/version/compatibility mismatch");
    const bool localeOK=contains(m["locales"],p.requestedLocale) ||
        contains(m["locales"],p.effectiveLanguage+"-*") || (p.requestedLocale==p.effectiveLanguage && contains(m["locales"],p.effectiveLanguage));
    if (!localeOK || !contains(m["layouts"],layoutId) || !contains(m["modes"],inputMode))
        return unavailable(p,"Pack does not declare requested locale/layout/mode");
    initLanguageAssets();
    const auto lock=readObject(":/swipekb/assets.lock.json");
    // WHY the word list's terms are ODC-By 1.0 (FineWeb text) AND CC BY 2.0 FR (Tatoeba), both pinned in the lock.
    const auto dataTermsHash=lock["language_files"].toObject()["odcTerms"].toObject()["sha256"].toString();
    const auto tatoebaTermsHash=lock["tatoeba_license_notice_sha256"].toString();
    const auto noticeHash=lock["language_files"].toObject()["notice"].toObject()["sha256"].toString();
    const auto dictionaryPin=lock["language_dictionaries"].toObject()[p.effectiveLanguage].toObject();
    if (noticeHash.size()!=64 || dataTermsHash.size()!=64 || tatoebaTermsHash.size()!=64 || dictionaryPin["sha256"].toString().size()!=64)
        return unavailable(p,"Embedded fixed asset/license pins unavailable");
    const auto license=m["license"].toObject();
    const auto licensePath=localPath(root,license["path"].toString());
    const auto dataTerms=localPath(root,license["termsPath"].toString());
    const auto tatoebaTerms=localPath(root,license["tatoebaTermsPath"].toString());
    // WHY a manifest cannot authenticate itself: every file identity comes from the embedded repository lock.
    if (license["id"].toString()!="ODC-By-1.0 AND CC-BY-2.0-FR" || license["attribution"].toString().isEmpty() ||
        licensePath.isEmpty() || QFileInfo(licensePath).size()==0 || license["sha256"].toString()!=noticeHash ||
        digest(licensePath)!=noticeHash ||
        dataTerms.isEmpty() || license["termsSha256"].toString()!=dataTermsHash || digest(dataTerms)!=dataTermsHash ||
        tatoebaTerms.isEmpty() || license["tatoebaTermsSha256"].toString()!=tatoebaTermsHash || digest(tatoebaTerms)!=tatoebaTermsHash)
        return unavailable(p,"Pack license/attribution missing or repository pin mismatch");
    if (m["source"].toObject()!=lock["word_list_source"].toObject())
        return unavailable(p,"Pack source/model revision mismatch");
    const auto assets=m["assets"].toObject();
    if (!assets.contains("dictionary")) return unavailable(p,"Required dictionary missing");
    if (assets.contains("contextModel") != assets.contains("contextVocab"))
        return unavailable(p,"Context model/vocabulary must be paired");
    const bool declaresContext=assets.contains("contextModel");
    // WHY a context problem (missing/damaged file, pin or licence mismatch) only drops next-word suggestions:
    // the dictionary is verified separately, and nothing unverified is ever loaded.
    bool hasContext=false;
    ContextFiles contextFiles;
    if (declaresContext && manifestContextMatchesLock(p.effectiveLanguage,m,lock))
        hasContext=contextFilesVerified(p.effectiveLanguage,root,lock,contextFiles);
    for (auto it=assets.begin(); it!=assets.end(); ++it) {
        if (!QStringList{"dictionary","contextModel","contextVocab"}.contains(it.key()))
            return unavailable(p,"Unknown asset role");
        // Context roles were judged above against the lock; when that failed they are simply left out.
        if (it.key().startsWith("context") && !hasContext) continue;
        const auto a=it.value().toObject(); const auto path=localPath(root,a["path"].toString());
        const auto hash=a["sha256"].toString(), compat=a["compatibility"].toString();
        const QString expectedCompat=it.key().startsWith("context") ? p.effectiveLanguage+":context-ngram-v1" : p.effectiveLanguage;
        if (compat!=expectedCompat) return unavailable(p,"Asset language/layout compatibility mismatch");
        if (path.isEmpty() || a["size"].toInteger(-1)<=0 || QFileInfo(path).size()!=a["size"].toInteger() ||
            hash.size()!=64 || digest(path)!=hash) return unavailable(p,"Asset missing, corrupt or checksum/size mismatch");
        if (it.key()=="dictionary" && (hash!=dictionaryPin["sha256"].toString() || a["path"].toString()!=dictionaryPin["path"].toString()))
            return unavailable(p,"Dictionary does not match the repository pin");
        p.assets[it.key()]={path,hash,compat};
    }
    // The neural next-word file was verified against the lock by contextFilesVerified(); the manifest cannot add it.
    if (hasContext && !contextFiles.nwp.isEmpty())
        p.assets["contextNwp"]={contextFiles.nwp,lock["trained_models"].toObject()["nextWord"].toObject()[p.effectiveLanguage].toObject()["sha256"].toString(),
                                p.effectiveLanguage+":context-nwp-v1"};
    addSwipeModel(p,{root});
    p.packId=m["id"].toString(); p.packVersion=m["version"].toString();
    p.capabilities={true,hasContext,hasContext};
    return p;
}
quint64 ProfileEpoch::invalidate(const QString &locale,const QString &layout,const QString &mode)
{
    const quint64 epoch=current_.epoch+1;
    current_={}; current_.epoch=epoch; current_.requestedLocale=canonicalLocale(locale);
    current_.layoutId=layout; current_.inputMode=mode; current_.unavailableReason="Profile loading";
    return epoch;
}
bool ProfileEpoch::publish(const ProfileSnapshot &result)
{
    if (result.epoch!=current_.epoch) return false;
    current_=result; return true;
}
} // namespace swipekb
