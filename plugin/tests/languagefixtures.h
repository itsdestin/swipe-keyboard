#pragma once
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>

namespace languagefixtures {
inline QByteArray read(const QString &path) {
    QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
inline bool write(const QString &root,const QString &name,const QByteArray &data) {
    QFile f(QDir(root).filePath(name)); return f.open(QIODevice::WriteOnly) && f.write(data)==data.size();
}
inline QString hash(const QByteArray &data) { return QString::fromLatin1(QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex()); }
inline bool runtimeAvailable() {
    return !qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS").isEmpty() && !qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS").isEmpty();
}
inline bool available() {
    return !qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS").isEmpty() && !qEnvironmentVariable("SWIPEKB_TEST_CC_LICENSE").isEmpty();
}
// WHY actual pinned bytes, never a production bypass: supplied-but-missing/bad fixtures fail the tests;
// only an explicitly absent fixture setting is skipped by the caller. No mutable upstream file is touched.
inline bool populate(const QString &root,QJsonObject &manifest) {
    const auto assets=qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS");
    const auto encoder=read(assets+"/models/honorable_sturgeon/model_fp32.pte");
    const auto metadata=read(assets+"/models/honorable_sturgeon/metadata.json");
    const auto terms=read(assets+"/models/FUTO-MODEL-LICENSE.md");
    const auto cc=read(qEnvironmentVariable("SWIPEKB_TEST_CC_LICENSE"));
    if (encoder.isEmpty() || metadata.isEmpty() || terms.isEmpty() || cc.isEmpty()) return false;
    if (!write(root,"encoder.pte",encoder) || !write(root,"metadata.json",metadata) || !write(root,"FUTO-LICENSE",terms) ||
        !write(root,"CC-BY-SA-4.0.txt",cc) || !write(root,"NOTICE","wordfreq 3.1.1 / Robyn Speer; fixture transformation") ||
        !write(root,"dict.combined","dictionary=fixture,locale=fr,version=1\n word=école,f=200\n")) return false;
    auto asset=[&](const QString &path,const QString &compat) {
        const auto bytes=read(QDir(root).filePath(path));
        return QJsonObject{{"path",path},{"sha256",hash(bytes)},{"size",bytes.size()},{"compatibility",compat}};
    };
    auto enc=asset("encoder.pte","layout-agnostic"); enc["metadataSha256"]=hash(metadata);
    manifest=QJsonObject{{"schema",1},{"id","fr-fixture"},{"version","1"},{"language","fr"},
        {"locales",QJsonArray{"fr","fr-*"}},{"layouts",QJsonArray{"azerty"}},{"modes",QJsonArray{"Latin"}},
        {"scripts",QJsonArray{"Latn"}},{"compatibility","swipekb-futo-v1"},
        {"textRulesVersion","unicode-latin-v1"},{"transformationVersion","wordfreq-latin-v1"},
        {"modelRevision","18328c3042b066952c0936b3771d492fe2ec289a"},
        {"futoRevision","1b13f2c85d6b347f6ea3fbc4b3aaf01fce42429a"},
        {"executorchRevision","0b0e2c5cdd67c8b4396a46ea1d1aa72ffb0128d7"},
        {"source",QJsonObject{{"url","https://files.pythonhosted.org/packages/24/61/62835c475d69872d30689f284497853fe33fe1d6dd18f57346d13305861d/wordfreq-3.1.1-py3-none-any.whl"},
            {"sha256","4b1c6ecffc6198be3396d5cf871c4423ca71c907c231348d352dd54d62b97473"}}},
        {"license",QJsonObject{{"id","CC-BY-SA-4.0"},{"path","NOTICE"},{"sha256",hash(read(QDir(root).filePath("NOTICE")))},
            {"termsPath","CC-BY-SA-4.0.txt"},{"termsSha256",hash(cc)},{"attribution","wordfreq 3.1.1 / Robyn Speer"}}},
        {"encoderLicense",QJsonObject{{"path","FUTO-LICENSE"},{"sha256",hash(terms)}}},
        {"assets",QJsonObject{{"encoder",enc},{"dictionary",asset("dict.combined","fr")}}}};
    return true;
}
}
