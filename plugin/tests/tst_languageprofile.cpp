#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include "languageprofile.h"
#include "languagetextrules.h"
#include "languagefixtures.h"
using namespace swipekb;
class LanguageProfileTest : public QObject {
    Q_OBJECT
private slots:
    void canonicalizesLocale() {
        QCOMPARE(canonicalLocale("EN_gb"), QString("en-GB"));
        QCOMPARE(canonicalLocale("fr_CA"), QString("fr-CA"));
        QCOMPARE(canonicalLocale("EN_latn_gb"),QString("en-GB"));
        QCOMPARE(canonicalLocale("es_419"),QString("es-419"));
        QVERIFY(canonicalLocale("garbage!").isEmpty());
    }
    void regionalPreference() {
        const QStringList words{"color", "colour", "organize", "organise", "rare"};
        auto uk = rankRegionalCandidates(words, "en-GB", 5);
        QVERIFY(uk.indexOf("colour") < uk.indexOf("color"));
        QCOMPARE(uk.size(), words.size());
        QVERIFY(uk.contains("organize")); // Oxford -ize remains valid, not blanket rewritten.
        QCOMPARE(rankRegionalCandidates({"colour", "color"}, "en-US", 2).first(), QString("color"));
        QCOMPARE(rankRegionalCandidates(words, "fr-FR", 5), words);
        QVERIFY(rankRegionalCandidates(words, "en-GB", 3, "rare").contains("rare"));
        const auto truncated=rankRegionalCandidates({"color","shape","third","colour"},"en-GB",3,"shape");
        QVERIFY(truncated.contains("colour")); QVERIFY(truncated.contains("color")); QVERIFY(truncated.contains("shape"));
        const auto guarded=rankRegionalCandidates({"first","color","third","colour","raw"},"en-GB",3,"raw");
        QCOMPARE(guarded,QStringList({"colour","color","raw"}));
        QCOMPARE(rankRegionalCandidates({"first","second","color","raw"},"en-GB",3,"raw"),QStringList({"first","second","raw"}));
        QCOMPARE(rankRegionalCandidates({"color","color","colour","raw"},"en-GB",3,"raw"),QStringList({"colour","color","raw"}));
    }
    void swearWordsAreNeverOfferedUnprompted() {
        QVERIFY(!offeredUnprompted("fuck", "en"));
        QVERIFY(!offeredUnprompted("Fucking", "en"));
        QVERIFY(!offeredUnprompted("mierda", "es"));
        QVERIFY(!offeredUnprompted("putain", "fr"));
        QVERIFY(!offeredUnprompted("Scheiße", "de"));
        QVERIFY(!offeredUnprompted("scheisse", "de"));
        QVERIFY(!offeredUnprompted("dick's", "en"));
        QVERIFY(!offeredUnprompted("fuck’n", "en"));
        // Ordinary words, including ones the source list over-reaches on, stay suggestible.
        for (const auto &[word, language] : std::initializer_list<std::pair<const char *, const char *>>{
                 {"hello", "en"}, {"sexual", "en"}, {"don't", "en"}, {"l'homme", "fr"}, {"infierno", "es"}, {"concha", "es"}, {"folle", "fr"},
                 {"nackt", "de"}, {"word", "xx"}})
            QVERIFY2(offeredUnprompted(word, language), word);
    }
    void failsClosed() {
        QTemporaryDir dir;
        auto p = resolveProfile("es_ES", "qwerty", "Latin", dir.path(), 2);
        QVERIFY(!p.capabilities.swipe);
        QVERIFY(p.unavailableReason.contains("manifest"));
        QCOMPARE(p.requestedLocale, QString("es-ES"));
        QCOMPARE(p.layoutId, QString("qwerty"));
        QVERIFY(resolveProfile("en-Cyrl-US","qwerty","Latin",dir.path()).unavailableReason.contains("script"));
        for (const auto &lang : {"es", "fr", "de", "pt"}) {
            QVERIFY(!resolveProfile(lang,"azerty","Latin",dir.path()).capabilities.swipe);
            p = resolveProfile(lang, "azerty", "Latin", dir.path(), 3);
            QVERIFY(!p.capabilities.swipe);
            QVERIFY(p.assets.isEmpty());
            QVERIFY(!p.capabilities.nextWord);
        }
    }
    void packs() {
        QTemporaryDir dir;
        auto write = [&](const QString &name, const QByteArray &data) {
            QFile f(dir.filePath(name)); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(data), data.size());
        };
        if (!languagefixtures::available()) QSKIP("Explicit pinned English assets and CC license fixtures not supplied");
        QJsonObject manifest;
        QVERIFY(languagefixtures::populate(dir.path(),manifest));
        const auto data=languagefixtures::read(dir.filePath("dict.combined"));
        const auto metadata=languagefixtures::read(dir.filePath("metadata.json"));
        const auto notice=languagefixtures::read(dir.filePath("NOTICE"));
        auto asset = [&](const QString &path, const QString &compatibility) {
            const auto bytes=languagefixtures::read(dir.filePath(path));
            return QJsonObject{{"path",path},{"sha256",languagefixtures::hash(bytes)},{"size",bytes.size()},{"compatibility",compatibility}};
        };
        auto resolve = [&] {
            write("fr.json", QJsonDocument(manifest).toJson());
            return resolveProfile("fr_CA", "azerty", "Latin", dir.path(), 42);
        };
        auto p = resolve();
        QVERIFY2(p.capabilities.swipe,qPrintable(p.unavailableReason));
        QCOMPARE(p.epoch, quint64(42)); QCOMPARE(p.effectiveLanguage, QString("fr"));
        QVERIFY(!p.capabilities.contextRanking); QVERIFY(!p.capabilities.nextWord);
        QCOMPARE(p.assets.size(),2); QVERIFY(!p.assets.contains("decoder"));
        QVERIFY(!resolveProfile("fr", "qwertz", "Latin", dir.path()).capabilities.swipe);
        auto original = manifest;
        QVERIFY(!resolveProfile("fr","azerty","Numeric",dir.path()).capabilities.swipe);
        manifest["scripts"]=QJsonArray{"Cyrl"}; QVERIFY(!resolve().capabilities.swipe); manifest=original;
        auto source=manifest["source"].toObject(); source["sha256"]="wrong"; manifest["source"]=source;
        QVERIFY(!resolve().capabilities.swipe); manifest=original;
        write("NOTICE","tampered"); QVERIFY(!resolve().capabilities.swipe); write("NOTICE",notice);
        write("metadata.json","tampered"); QVERIFY(!resolve().capabilities.swipe); write("metadata.json",metadata);
        QTemporaryDir outside;
        QFile external(outside.filePath("dictionary")); QVERIFY(external.open(QIODevice::WriteOnly)); external.write(data); external.close();
        auto traversed=manifest["assets"].toObject(); auto escaped=traversed["dictionary"].toObject();
        escaped["path"]=QDir(dir.path()).relativeFilePath(external.fileName()); traversed["dictionary"]=escaped;
        manifest["assets"]=traversed; QVERIFY(!resolve().capabilities.swipe); manifest=original;
        manifest["compatibility"] = "wrong"; QVERIFY(!resolve().capabilities.swipe); manifest = original;
        manifest.remove("license"); QVERIFY(!resolve().capabilities.swipe); manifest = original;
        auto assets = manifest["assets"].toObject();
        assets["decoder"] = asset("encoder.pte","en:qwerty:26"); manifest["assets"] = assets;
        QVERIFY(!resolve().capabilities.swipe); manifest = original;
        assets = manifest["assets"].toObject(); assets["contextModel"] = asset("encoder.pte","fr:context-v1");
        manifest["assets"] = assets; QVERIFY(!resolve().capabilities.swipe); manifest = original;
        write("dict.combined", "corrupt"); QVERIFY(!resolve().capabilities.swipe);
        QFile::remove(dir.filePath("encoder.pte")); QVERIFY(!resolve().capabilities.swipe);
    }
    void rejectsSelfAttestedFixedAssets_data() {
        QTest::addColumn<QString>("role");
        for (const auto &role:{"encoder","metadata","encoderLicense","dataTerms"}) QTest::newRow(role)<<QString(role);
    }
    void rejectsSelfAttestedFixedAssets() {
        QFETCH(QString,role);
        if (!languagefixtures::available()) QSKIP("Explicit pinned English assets and CC license fixtures not supplied");
        QTemporaryDir dir; QJsonObject manifest; QVERIFY(languagefixtures::populate(dir.path(),manifest));
        auto resolve=[&] {
            if (!languagefixtures::write(dir.path(),"fr.json",QJsonDocument(manifest).toJson())) return ProfileSnapshot{};
            return resolveProfile("fr","azerty","Latin",dir.path());
        };
        QVERIFY2(resolve().capabilities.swipe,qPrintable(resolve().unavailableReason));
        const QByteArray incompatible=role=="metadata" ? QByteArray("{\"kind\":\"decoder\",\"layout\":\"en_qwerty\"}") : QByteArray("incompatible but self-consistent replacement");
        const auto replacementHash=languagefixtures::hash(incompatible);
        auto assets=manifest["assets"].toObject(); auto encoder=assets["encoder"].toObject();
        QString file;
        if (role=="encoder") { file="encoder.pte"; encoder["sha256"]=replacementHash; encoder["size"]=incompatible.size(); }
        else if (role=="metadata") { file="metadata.json"; encoder["metadataSha256"]=replacementHash; }
        else if (role=="encoderLicense") {
            file="FUTO-LICENSE"; auto license=manifest["encoderLicense"].toObject(); license["sha256"]=replacementHash; manifest["encoderLicense"]=license;
        } else {
            file="CC-BY-SA-4.0.txt"; auto license=manifest["license"].toObject(); license["termsSha256"]=replacementHash; manifest["license"]=license;
        }
        assets["encoder"]=encoder; manifest["assets"]=assets;
        QVERIFY(languagefixtures::write(dir.path(),file,incompatible));
        const auto rejected=resolve();
        QVERIFY2(!rejected.capabilities.swipe,qPrintable("Self-attested "+role+" must not replace a repository pin"));
        QVERIFY(rejected.assets.isEmpty());
    }
    void switchEpochs() {
        ProfileEpoch state;
        auto old = state.invalidate("en-US", "qwerty", "Latin");
        auto current = state.invalidate("de-DE", "qwertz", "Latin");
        QVERIFY(current > old); QVERIFY(!state.snapshot().capabilities.swipe);
        ProfileSnapshot result; result.epoch = old; result.capabilities.swipe = true;
        QVERIFY(!state.publish(result));
        result.epoch = current; QVERIFY(state.publish(result));
        QCOMPARE(state.snapshot().epoch, current);
    }
};
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv); LanguageProfileTest test;
    const int result=QTest::qExec(&test,argc,argv);
    // WHY missing fixtures must be CTest-skipped, but an actual failure always takes precedence.
    return result ? result : (languagefixtures::available() ? 0 : 77);
}
#include "tst_languageprofile.moc"
