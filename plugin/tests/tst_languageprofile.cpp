// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

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
    // ---- texting abbreviations and slang (packaging/texting-words) ----
    void textingListsLoadForEveryLanguage() {
        for (const auto &[language, minimum] : std::initializer_list<std::pair<const char *, size_t>>{{"en", 90}, {"es", 45}, {"fr", 45}, {"de", 40}}) {
            const auto words = textingWords(language);
            QVERIFY2(words.size() >= minimum, language);
            QSet<QString> seen;
            for (const auto &w : words) {
                QVERIFY2(!seen.contains(w.word), qPrintable(w.word));   // no repeated word
                seen.insert(w.word);
                QVERIFY2(w.strength >= 1 && w.strength <= 150, qPrintable(w.word));   // README: nothing above 150
                QCOMPARE(w.word, w.word.toLower());   // shown exactly as listed, lower-case
            }
        }
        const auto en = textingWordPairs("en");
        for (const char *w : {"wyd", "lmao", "rn", "pls", "ya", "brb", "smh", "ikr", "ty", "hbu"})
            QVERIFY2(std::any_of(en.begin(), en.end(), [&](const auto &e) { return e.first == w; }), w);
        QVERIFY(textingWords("pt").empty());   // a language without a list gets none
    }
    void textingParserSkipsBadLinesAndKeepsGoodOnes() {
        const QByteArray tsv =
            "# comment\n\n"
            "good\t100\t-\tnote\n"
            "tooweak\t0\t-\tx\n"            // strength below 1
            "toostrong\t256\t-\tx\n"        // above 255
            "text\tabc\t-\tx\n"             // not a number
            "two\twords\n"                    // too few columns
            "gn8\t50\t-\tx\n"               // digit: cannot be swiped
            "don't\t50\t-\tx\n"             // apostrophe: cannot be swiped
            "badtag\t50\tmaybe\tx\n"        // unknown tag
            "thisisafarfartoolongwordtoswipe\t50\t-\tx\n"
            "angry\t60\tswear\tx\n"
            "good\t120\t-\tagain\n"          // repeated: strongest line wins
            "\xff\xfe\t50\t-\tx\n";       // not valid text
        const auto words = parseTextingWords(tsv, "en");
        QCOMPARE(words.size(), size_t(2));
        QCOMPARE(words[0].word, QString("good"));
        QCOMPARE(words[0].strength, 120);
        QCOMPARE(words[1].word, QString("angry"));
        QVERIFY(words[1].swear);
        QVERIFY(parseTextingWords("\0\0\0 garbage without tabs", "en").empty());
        QVERIFY(parseTextingWords(QByteArray(), "en").empty());
        // A letter the language's keys cannot spell is skipped (a German umlaut in an English list).
        QVERIFY(parseTextingWords("l\xc3\xa4" "ch\t50\t-\tx\n", "en").empty());
        QCOMPARE(parseTextingWords("l\xc3\xa4" "ch\t50\t-\tx\n", "de").size(), size_t(1));
        // Never more than 2000 words, however long the file.
        QByteArray huge;
        for (int i = 0; i < 3000; ++i) huge += "w" + QByteArray(1, char('a' + i % 26)) + QByteArray(1, char('a' + i / 26 % 26)) + QByteArray(1, char('a' + i / 676)) + "\t50\t-\tx\n";
        QCOMPARE(parseTextingWords(huge, "en").size(), size_t(2000));
    }
    void aDamagedListFileIsIgnoredSafely() {
        QTemporaryDir dir;
        QFile f(dir.filePath("en.tsv"));
        QVERIFY(f.open(QIODevice::WriteOnly)); f.write("garbage\x01\x02 no tabs at all\nmore garbage\n"); f.close();
        qputenv("SWIPEKB_TEXTING_WORDS", dir.path().toUtf8());
        QVERIFY(textingWords("en").empty());     // nothing usable: ignored, no crash, NOT the built-in list
        QVERIFY(textingWords("es").empty());     // no file for this language in the folder: empty as well
        QFile::remove(dir.filePath("en.tsv"));
        QVERIFY(textingWords("en").empty());     // missing file
        qunsetenv("SWIPEKB_TEXTING_WORDS");
        QVERIFY(!textingWords("en").empty());    // back to the built-in list
    }
    void swearAcronymsAreNeverOfferedUnprompted() {
        // Tagged `swear` in the texting list: swipe/complete OK (the engines take them), never offered out of the blue.
        for (const auto &[word, language] : std::initializer_list<std::pair<const char *, const char *>>{
                 {"wtf", "en"}, {"stfu", "en"}, {"omfg", "en"}, {"ntm", "fr"}, {"nmms", "es"}, {"wtf", "de"}})
            QVERIFY2(!offeredUnprompted(word, language), word);
        // The person's own use passes, as for any swear word.
        QVERIFY(offeredUnprompted("wtf", "en", [](const QString &w) { return w == "wtf"; }));
        // Ordinary abbreviations are offered like any word.
        for (const auto &[word, language] : std::initializer_list<std::pair<const char *, const char *>>{
                 {"lol", "en"}, {"idk", "en"}, {"jaja", "es"}, {"mdr", "fr"}, {"vllt", "de"}})
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
        if (!languagefixtures::available()) QSKIP("Explicit pinned English assets and language pack fixtures not supplied");
        QJsonObject manifest;
        QVERIFY(languagefixtures::populate(dir.path(),manifest));
        const auto data=languagefixtures::read(dir.filePath("fr.combined"));
        const auto notice=languagefixtures::read(dir.filePath("LANGUAGE-FILES-NOTICE.md"));
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
        QCOMPARE(p.assets.size(),1); QVERIFY(p.assets.contains("dictionary"));
        QVERIFY(!resolveProfile("fr", "qwertz", "Latin", dir.path()).capabilities.swipe);
        auto original = manifest;
        QVERIFY(!resolveProfile("fr","azerty","Numeric",dir.path()).capabilities.swipe);
        manifest["scripts"]=QJsonArray{"Cyrl"}; QVERIFY(!resolve().capabilities.swipe); manifest=original;
        auto source=manifest["source"].toObject(); source["sha256"]="wrong"; manifest["source"]=source;
        QVERIFY(!resolve().capabilities.swipe); manifest=original;
        write("LANGUAGE-FILES-NOTICE.md","tampered"); QVERIFY(!resolve().capabilities.swipe); write("LANGUAGE-FILES-NOTICE.md",notice);
        QTemporaryDir outside;
        QFile external(outside.filePath("dictionary")); QVERIFY(external.open(QIODevice::WriteOnly)); external.write(data); external.close();
        auto traversed=manifest["assets"].toObject(); auto escaped=traversed["dictionary"].toObject();
        escaped["path"]=QDir(dir.path()).relativeFilePath(external.fileName()); traversed["dictionary"]=escaped;
        manifest["assets"]=traversed; QVERIFY(!resolve().capabilities.swipe); manifest=original;
        manifest["compatibility"] = "wrong"; QVERIFY(!resolve().capabilities.swipe); manifest = original;
        manifest.remove("license"); QVERIFY(!resolve().capabilities.swipe); manifest = original;
        // An unknown role (e.g. an old pack's swipe network) or an unpaired context file refuses the pack.
        write("encoder.bin","old engine file");
        auto assets = manifest["assets"].toObject();
        assets["encoder"] = asset("encoder.bin","layout-agnostic"); manifest["assets"] = assets;
        QVERIFY(!resolve().capabilities.swipe); manifest = original;
        assets = manifest["assets"].toObject(); assets["contextModel"] = asset("fr.combined","fr:context-ngram-v1");
        manifest["assets"] = assets; QVERIFY(!resolve().capabilities.swipe); manifest = original;
        // A self-consistent but unpinned word list is refused: the lock, not the manifest, decides.
        write("other.combined","dictionary=x\n word=autre,f=100\n");
        assets = manifest["assets"].toObject(); assets["dictionary"] = asset("other.combined","fr"); manifest["assets"] = assets;
        QCOMPARE(resolve().unavailableReason, QString("Dictionary does not match the repository pin")); manifest = original;
        write("fr.combined", "corrupt"); QVERIFY(!resolve().capabilities.swipe);
        QFile::remove(dir.filePath("fr.combined")); QVERIFY(!resolve().capabilities.swipe);
    }
    void rejectsSelfAttestedFixedAssets_data() {
        QTest::addColumn<QString>("role");
        for (const auto &role:{"dataTerms","tatoebaTerms","notice"}) QTest::newRow(role)<<QString(role);
    }
    void rejectsSelfAttestedFixedAssets() {
        QFETCH(QString,role);
        if (!languagefixtures::available()) QSKIP("Explicit pinned English assets and language pack fixtures not supplied");
        QTemporaryDir dir; QJsonObject manifest; QVERIFY(languagefixtures::populate(dir.path(),manifest));
        auto resolve=[&] {
            if (!languagefixtures::write(dir.path(),"fr.json",QJsonDocument(manifest).toJson())) return ProfileSnapshot{};
            return resolveProfile("fr","azerty","Latin",dir.path());
        };
        QVERIFY2(resolve().capabilities.swipe,qPrintable(resolve().unavailableReason));
        const QByteArray incompatible("incompatible but self-consistent replacement");
        const auto replacementHash=languagefixtures::hash(incompatible);
        QString file;
        if (role=="dataTerms") {
            file="ODC-By-1.0.txt"; auto license=manifest["license"].toObject(); license["termsSha256"]=replacementHash; manifest["license"]=license;
        } else if (role=="tatoebaTerms") {
            file="TATOEBA-CC-BY-2.0-FR.txt"; auto license=manifest["license"].toObject(); license["tatoebaTermsSha256"]=replacementHash; manifest["license"]=license;
        } else {
            file="LANGUAGE-FILES-NOTICE.md"; auto license=manifest["license"].toObject(); license["sha256"]=replacementHash; manifest["license"]=license;
        }
        QVERIFY(languagefixtures::write(dir.path(),file,incompatible));
        const auto rejected=resolve();
        QVERIFY2(!rejected.capabilities.swipe,qPrintable("Self-attested "+role+" must not replace a repository pin"));
        QVERIFY(rejected.assets.isEmpty());
    }
    void refusesOldFormatPacksCleanly() {
        // A pack made for the previous swipe engine (schema 2, its own compatibility tag and revision pins, encoder
        // files) must be refused with a reason, never half-used or crash, whatever else it holds.
        if (!languagefixtures::available()) QSKIP("Explicit pinned language pack fixtures not supplied");
        QTemporaryDir dir; QJsonObject manifest; QVERIFY(languagefixtures::populate(dir.path(),manifest));
        manifest["schema"]=2; manifest["compatibility"]="swipekb-legacy-v1";
        manifest["engineRevision"]="1b13f2c85d6b347f6ea3fbc4b3aaf01fce42429a";
        QVERIFY(languagefixtures::write(dir.path(),"fr.json",QJsonDocument(manifest).toJson()));
        auto p=resolveProfile("fr","azerty","Latin",dir.path());
        QVERIFY(!p.capabilities.swipe); QVERIFY(p.assets.isEmpty());
        QCOMPARE(p.unavailableReason,QString("Pack schema/version/compatibility mismatch"));
        manifest["schema"]=3; QVERIFY(languagefixtures::write(dir.path(),"fr.json",QJsonDocument(manifest).toJson()));
        QVERIFY(!resolveProfile("fr","azerty","Latin",dir.path()).capabilities.swipe);   // still the old compatibility
        QVERIFY(languagefixtures::write(dir.path(),"fr.json","{ not json"));
        QVERIFY(resolveProfile("fr","azerty","Latin",dir.path()).unavailableReason.contains("manifest"));
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
