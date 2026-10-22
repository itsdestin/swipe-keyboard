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
static ProfileSnapshot unavailable(ProfileSnapshot p, const QString &reason)
{
    p.assets.clear(); p.capabilities = {}; p.unavailableReason = reason; return p;
}
static ProfileSnapshot english(ProfileSnapshot p, const QString &root)
{
    initLanguageAssets();
    const auto lock = readObject(":/swipekb/assets.lock.json");
    QHash<QString, QString> hashes;
    for (const auto &v : lock["assets"].toArray()) {
        const auto a=v.toObject(); hashes[a["path"].toString()]=a["sha256"].toString();
    }
    hashes["dict/en_swipe.combined"]=lock["dictionary_sha256"].toString();
    auto verify = [&](const QString &relative, const QString &role, const QString &compat) {
        const auto path=localPath(root,relative), expected=hashes.value(relative);
        if (path.isEmpty() || expected.size()!=64 || digest(path)!=expected) return false;
        if (!role.isEmpty()) p.assets[role]={path,expected,compat};
        return true;
    };
    if (!verify("models/FUTO-MODEL-LICENSE.md", {}, {}) ||
        !verify("models/honorable_sturgeon/model_fp32.pte", "encoder", "layout-agnostic") ||
        !verify("dict/en_swipe.combined", "dictionary", "en"))
        return unavailable(p,"Bundled English assets/license missing or checksum mismatch");
    // WHY optional means absent, not substituted. Corrupt present roles invalidate the entire snapshot.
    if (!verify("models/honorable_sturgeon/metadata.json",{},{})) return unavailable(p,"English encoder metadata checksum mismatch");
    const QString decoder="models/magic_macaw/model_fp32.pte";
    if (p.layoutId=="qwerty" && QFileInfo::exists(QDir(root).filePath(decoder)) &&
        (!verify(decoder,"decoder","en:qwerty:26") || !verify("models/magic_macaw/metadata.json",{},{}))) return unavailable(p,"English decoder checksum mismatch");
    const QString model="models/hungry_jellyfish/context_lm.pte", vocab="models/hungry_jellyfish/vocab.txt";
    const bool hasModel=QFileInfo::exists(QDir(root).filePath(model)), hasVocab=QFileInfo::exists(QDir(root).filePath(vocab));
    if (hasModel != hasVocab) return unavailable(p,"Context model/vocabulary must be paired");
    if (hasModel && (!verify(model,"contextModel","en:context-v1") || !verify(vocab,"contextVocab","en:context-v1") || !verify("models/hungry_jellyfish/metadata.json",{},{})))
        return unavailable(p,"English context checksum mismatch");
    p.packId="bundled-en"; p.packVersion=lock["model_revision"].toString();
    p.capabilities={true,hasModel,hasModel}; return p;
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
    if (!QStringList{"1","2"}.contains(m["schema"].toVariant().toString()) || m["id"].toString().isEmpty() || m["version"].toString().isEmpty() ||
        m["language"].toString()!=p.effectiveLanguage ||
        m["compatibility"].toString()!="swipekb-futo-v1" || !contains(m["scripts"],"Latn") ||
        m["futoRevision"].toString()!="1b13f2c85d6b347f6ea3fbc4b3aaf01fce42429a" ||
        m["executorchRevision"].toString()!="0b0e2c5cdd67c8b4396a46ea1d1aa72ffb0128d7" ||
        m["textRulesVersion"].toString()!=TextRulesVersion ||
        // WHY retain the authenticated v1 format while accepting the explicit v2 orthography erratum;
        // arbitrary transform versions still fail closed and every role hash/size remains verified.
        !QStringList{"wordfreq-latin-v1","wordfreq-latin-v2"}.contains(m["transformationVersion"].toString())) return unavailable(p,"Pack schema/version/compatibility mismatch");
    const bool localeOK=contains(m["locales"],p.requestedLocale) ||
        contains(m["locales"],p.effectiveLanguage+"-*") || (p.requestedLocale==p.effectiveLanguage && contains(m["locales"],p.effectiveLanguage));
    if (!localeOK || !contains(m["layouts"],layoutId) || !contains(m["modes"],inputMode))
        return unavailable(p,"Pack does not declare requested locale/layout/mode");
    initLanguageAssets();
    const auto lock=readObject(":/swipekb/assets.lock.json");
    QHash<QString,QString> fixedHashes;
    for (const auto &value:lock["assets"].toArray()) {
        const auto entry=value.toObject(); fixedHashes[entry["path"].toString()]=entry["sha256"].toString();
    }
    const auto encoderHash=fixedHashes.value("models/honorable_sturgeon/model_fp32.pte");
    const auto metadataHash=fixedHashes.value("models/honorable_sturgeon/metadata.json");
    const auto encoderTermsHash=fixedHashes.value("models/FUTO-MODEL-LICENSE.md");
    const auto dataTermsHash=lock["cc_license_source"].toObject()["sha256"].toString();
    if (encoderHash.size()!=64 || metadataHash.size()!=64 || encoderTermsHash.size()!=64 || dataTermsHash.size()!=64)
        return unavailable(p,"Embedded fixed asset/license pins unavailable");
    const auto license=m["license"].toObject();
    const auto licensePath=localPath(root,license["path"].toString());
    const auto encoderLicense=m["encoderLicense"].toObject();
    const auto encoderTerms=localPath(root,encoderLicense["path"].toString());
    const auto dataTerms=localPath(root,license["termsPath"].toString());
    // WHY a manifest cannot authenticate itself: fixed model/metadata/license identities come only from
    // the embedded repository lock. The derived dictionary/attribution hashes remain local integrity checks.
    if (license["id"].toString()!="CC-BY-SA-4.0" || license["attribution"].toString().isEmpty() ||
        licensePath.isEmpty() || QFileInfo(licensePath).size()==0 || license["sha256"].toString().size()!=64 ||
        digest(licensePath)!=license["sha256"].toString() || encoderTerms.isEmpty() ||
        encoderLicense["sha256"].toString()!=encoderTermsHash || digest(encoderTerms)!=encoderTermsHash ||
        dataTerms.isEmpty() || license["termsSha256"].toString()!=dataTermsHash || digest(dataTerms)!=dataTermsHash)
        return unavailable(p,"Pack license/attribution missing or repository pin mismatch");
    const auto source=m["source"].toObject();
    if (source!=lock["wordfreq_wheel"].toObject() || m["modelRevision"]!=lock["model_revision"])
        return unavailable(p,"Pack source/model revision mismatch");
    const auto assets=m["assets"].toObject();
    if (!assets.contains("encoder") || !assets.contains("dictionary")) return unavailable(p,"Required encoder/dictionary missing");
    // WHY foreign context is enabled only by the paired, repository-pinned n-gram model/vocabulary;
    // English weights or a manifest's self-asserted model hash cannot confer capability.
    if (assets.contains("decoder") || assets.contains("contextModel") != assets.contains("contextVocab"))
        return unavailable(p,"Unsupported decoder or unpaired context model/vocabulary");
    const bool hasContext=assets.contains("contextModel");
    const auto contextPins=lock["multilingual_context_models"].toObject()[p.effectiveLanguage].toObject();
    if (hasContext && (m["schema"].toInt()!=2 || m["contextFormat"].toString()!="swk-ngram-v2" || contextPins.isEmpty() ||
        assets["contextModel"].toObject()["sha256"].toString()!=contextPins["modelSha256"].toString() ||
        assets["contextVocab"].toObject()["sha256"].toString()!=contextPins["vocabSha256"].toString()))
        return unavailable(p,"Context model format or repository pin mismatch");
    if (hasContext) {
        const auto terms=m["contextLicense"].toObject();
        const auto attribution=localPath(root,terms["attributionPath"].toString());
        const auto contextTerms=localPath(root,terms["termsPath"].toString());
        if (terms["id"].toString()!="CC-BY-2.0-FR" || terms["sourceSha256"].toString()!=contextPins["corpusSha256"].toString() ||
            terms["attributionSha256"].toString()!=contextPins["contributorsSha256"].toString() || attribution.isEmpty() ||
            digest(attribution)!=contextPins["contributorsSha256"].toString() || contextTerms.isEmpty() ||
            terms["termsSha256"].toString()!=lock["tatoeba_license_notice_sha256"].toString() ||
            digest(contextTerms)!=lock["tatoeba_license_notice_sha256"].toString() ||
            terms["sourceUrl"].toString()!=QString("https://downloads.tatoeba.org/exports/per_language/%1/%1_sentences.tsv.bz2").arg(p.effectiveLanguage=="es" ? "spa" : p.effectiveLanguage=="fr" ? "fra" : "deu"))
            return unavailable(p,"Context source or contributor attribution pin mismatch");
    }
    if (!hasContext && m["schema"].toInt()!=1) return unavailable(p,"Unsupported language pack schema");
    for (auto it=assets.begin(); it!=assets.end(); ++it) {
        if (!QStringList{"encoder","dictionary","contextModel","contextVocab"}.contains(it.key()))
            return unavailable(p,"Unknown asset role");
        const auto a=it.value().toObject(); const auto path=localPath(root,a["path"].toString());
        const auto hash=a["sha256"].toString(), compat=a["compatibility"].toString();
        const QString expectedCompat=it.key()=="encoder" ? QString("layout-agnostic") :
            it.key().startsWith("context") ? p.effectiveLanguage+":context-ngram-v1" : p.effectiveLanguage;
        if (compat!=expectedCompat) return unavailable(p,"Asset language/layout compatibility mismatch");
        if (path.isEmpty() || a["size"].toInteger(-1)<=0 || QFileInfo(path).size()!=a["size"].toInteger() ||
            hash.size()!=64 || digest(path)!=hash) return unavailable(p,"Asset missing, corrupt or checksum/size mismatch");
        if (it.key()=="encoder") {
            const auto metadata=localPath(root,QDir(root).relativeFilePath(QFileInfo(path).dir().filePath("metadata.json")));
            if (hash!=encoderHash || metadata.isEmpty() || a["metadataSha256"].toString()!=metadataHash || digest(metadata)!=metadataHash)
                return unavailable(p,"Encoder/model metadata does not match repository compatibility pins");
        }
        p.assets[it.key()]={path,hash,compat};
    }
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
