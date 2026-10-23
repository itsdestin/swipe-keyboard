#include <QtTest>
#include <QFile>
#include <QTextStream>
#include <QTemporaryDir>
#include "languagefixtures.h"
#include "languageprofile.h"
#include "languagetextrules.h"
#include "swipekb/recognizer.hpp"
using namespace swipekb;
class LanguagePacksTest : public QObject {
    Q_OBJECT
private slots:
    void realEnglishLock() {
        const QString root=qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS");
        if (root.isEmpty()) QSKIP("Explicit read-only English asset fixture not supplied");
        auto en=resolveProfile("en_US","qwerty","Latin",root);
        QVERIFY2(en.capabilities.swipe,qPrintable(en.unavailableReason));
        QVERIFY(en.assets.contains("decoder"));
        auto uk=resolveProfile("en_GB","qwertz","Latin",root);
        QVERIFY2(uk.capabilities.swipe,qPrintable(uk.unavailableReason));
        QCOMPARE(en.assets["dictionary"].sha256,uk.assets["dictionary"].sha256);
        QVERIFY(!uk.assets.contains("decoder"));
    }
    void englishOptionalRolesAndLegacyDictionaryFailClosed() {
        const auto source=qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS");
        if(source.isEmpty()) QSKIP("Explicit locked English asset fixture required");
        QTemporaryDir dir;
        const QStringList paths{"models/FUTO-MODEL-LICENSE.md","models/honorable_sturgeon/model_fp32.pte",
            "models/honorable_sturgeon/metadata.json","models/magic_macaw/model_fp32.pte","models/magic_macaw/metadata.json",
            "models/hungry_jellyfish/context_lm.pte","models/hungry_jellyfish/vocab.txt","models/hungry_jellyfish/metadata.json","dict/en_swipe.combined"};
        for(const auto &path:paths) {
            QVERIFY(QDir().mkpath(QFileInfo(dir.filePath(path)).absolutePath()));
            QVERIFY2(QFile::copy(QDir(source).filePath(path),dir.filePath(path)),qPrintable(path));
        }
        auto resolve=[&] { return resolveProfile("en-GB","qwerty","Latin",dir.path()); };
        auto p=resolve(); QVERIFY2(p.capabilities.swipe,qPrintable(p.unavailableReason));
        QVERIFY(p.capabilities.contextRanking); QVERIFY(p.capabilities.nextWord);
        const auto dictionary=languagefixtures::read(dir.filePath("dict/en_swipe.combined"));
        QVERIFY(dictionary.contains("date=0"));
        auto dated=dictionary; dated.replace("date=0","date=1792036347");
        QVERIFY(languagefixtures::write(dir.path(),"dict/en_swipe.combined",dated));
        QVERIFY(!resolve().capabilities.swipe);
        QVERIFY(languagefixtures::write(dir.path(),"dict/en_swipe.combined",dictionary)); QVERIFY(resolve().capabilities.swipe);
        const QString decoder="models/magic_macaw/model_fp32.pte", model="models/hungry_jellyfish/context_lm.pte",
            vocab="models/hungry_jellyfish/vocab.txt", metadata="models/hungry_jellyfish/metadata.json";
        for (const auto &path:QStringList{decoder,model,vocab,metadata}) {
            const auto original=languagefixtures::read(dir.filePath(path));
            QVERIFY(languagefixtures::write(dir.path(),path,"corrupt optional asset"));
            QVERIFY2(!resolve().capabilities.swipe,qPrintable(path));
            if(path==decoder) QVERIFY(resolveProfile("en-GB","azerty","Latin",dir.path()).capabilities.swipe);
            QVERIFY(languagefixtures::write(dir.path(),path,original)); QVERIFY(resolve().capabilities.swipe);
        }
        const auto vocabulary=languagefixtures::read(dir.filePath(vocab));
        QVERIFY(QFile::remove(dir.filePath(vocab))); QVERIFY(resolve().unavailableReason.contains("paired"));
        QVERIFY(languagefixtures::write(dir.path(),vocab,vocabulary));
        QVERIFY(QFile::remove(dir.filePath(model))); QVERIFY(resolve().unavailableReason.contains("paired"));
        QVERIFY(QFile::remove(dir.filePath(vocab))); QVERIFY(QFile::remove(dir.filePath(decoder)));
        p=resolve(); QVERIFY(p.capabilities.swipe); QVERIFY(!p.capabilities.contextRanking); QVERIFY(!p.capabilities.nextWord);
        QVERIFY(!p.assets.contains("decoder")); QVERIFY(!p.assets.contains("contextModel")); QVERIFY(!p.assets.contains("contextVocab"));
    }
    void multilingualContextRolesAreLocaleSharedAndFailClosedWhenDamaged() {
        const QString source=qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS");
        if(source.isEmpty()) QSKIP("Verified multilingual pack fixture required");
        auto spain=resolveProfile("es_ES","qwerty","Latin",source);
        auto mexico=resolveProfile("es_MX","qwerty","Latin",source);
        QVERIFY2(spain.capabilities.nextWord,qPrintable(spain.unavailableReason));
        QVERIFY(mexico.capabilities.nextWord); QCOMPARE(spain.requestedLocale,QString("es-ES"));
        QCOMPARE(mexico.requestedLocale,QString("es-MX"));
        QCOMPARE(spain.assets["contextModel"].sha256,mexico.assets["contextModel"].sha256);
        QCOMPARE(spain.assets["contextModel"].compatibility,QString("es:context-ngram-v1"));
        QTemporaryDir copy; QVERIFY(copy.isValid());
        for(const auto &name:QDir(source).entryList(QDir::Files))
            QVERIFY2(QFile::copy(QDir(source).filePath(name),copy.filePath(name)),qPrintable(name));
        const QString model=spain.assets["contextModel"].path.section('/',-1);
        const QString vocab=spain.assets["contextVocab"].path.section('/',-1);
        auto bytes=languagefixtures::read(copy.filePath(model)); QVERIFY(!bytes.isEmpty());
        // WHY a damaged or missing next-word file only removes prediction: the language still swipes and taps,
        // and the damaged bytes are never offered to the engine (no contextModel/contextVocab role survives).
        auto expectSwipeWithoutPrediction=[&](const char *what) {
            auto broken=resolveProfile("es-MX","qwerty","Latin",copy.path());
            QVERIFY2(broken.capabilities.swipe,what); QVERIFY2(!broken.capabilities.nextWord,what);
            QVERIFY2(!broken.capabilities.contextRanking,what);
            QVERIFY2(!broken.assets.contains("contextModel") && !broken.assets.contains("contextVocab"),what);
            QVERIFY2(broken.assets.contains("encoder") && broken.assets.contains("dictionary"),what);
        };
        QVERIFY(languagefixtures::write(copy.path(),model,bytes+"damage"));
        expectSwipeWithoutPrediction("damaged n-gram");
        QVERIFY(languagefixtures::write(copy.path(),model,bytes));
        QVERIFY(resolveProfile("es-MX","qwerty","Latin",copy.path()).capabilities.nextWord);
        const auto vocabBytes=languagefixtures::read(copy.filePath(vocab));
        QVERIFY(QFile::remove(copy.filePath(vocab)));
        expectSwipeWithoutPrediction("missing word list");
        QVERIFY(languagefixtures::write(copy.path(),vocab,vocabBytes));
        QVERIFY(QFile::remove(copy.filePath(model)));
        expectSwipeWithoutPrediction("missing n-gram");
        QVERIFY(languagefixtures::write(copy.path(),model,bytes));
        // Every credit/licence text that must travel with the files is verified too.
        for(const QString &name:{QString("ODC-By-1.0.txt"),QString("LANGUAGE-FILES-NOTICE.md"),QString("TATOEBA-CC-BY-2.0-FR.txt"),QString("es-CONTRIBUTORS.txt")}) {
            const auto original=languagefixtures::read(copy.filePath(name)); QVERIFY2(!original.isEmpty(),qPrintable(name));
            QVERIFY(languagefixtures::write(copy.path(),name,original+"tampered"));
            // WHY the notice and both licence texts take the whole language down but the credit list only prediction:
            // the swipe dictionary is itself counted from FineWeb (ODC-By) and Tatoeba (CC BY 2.0 FR) text, so, like the
            // CC BY-SA text before them, its licence texts are part of the swipe-level pins; authors are context-only.
            if(name.endsWith("CONTRIBUTORS.txt")) expectSwipeWithoutPrediction(qPrintable(name));
            else QVERIFY2(!resolveProfile("es-MX","qwerty","Latin",copy.path()).capabilities.swipe,qPrintable(name));
            QVERIFY(languagefixtures::write(copy.path(),name,original));
            QVERIFY2(resolveProfile("es-MX","qwerty","Latin",copy.path()).capabilities.nextWord,qPrintable(name));
        }
        // The swipe encoder itself stays strictly required: damage there still makes the language unavailable.
        const auto encoder=languagefixtures::read(copy.filePath("encoder.pte"));
        QVERIFY(languagefixtures::write(copy.path(),"encoder.pte",encoder+"x"));
        QVERIFY(!resolveProfile("es-MX","qwerty","Latin",copy.path()).capabilities.swipe);
    }
    // Lays out a model root like a new install: English swipe files from the explicit fixture, English next-word
    // files (and the texts that must accompany them) from the verified language-pack fixture.
    static bool englishRoot(const QString &root,bool withNgram,QString &why) {
        const auto source=qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS"), packs=qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS");
        const QStringList paths{"models/FUTO-MODEL-LICENSE.md","models/honorable_sturgeon/model_fp32.pte",
            "models/honorable_sturgeon/metadata.json","models/magic_macaw/model_fp32.pte","models/magic_macaw/metadata.json",
            "models/hungry_jellyfish/context_lm.pte","models/hungry_jellyfish/vocab.txt","models/hungry_jellyfish/metadata.json","dict/en_swipe.combined"};
        for(const auto &path:paths) {
            if(!QDir().mkpath(QFileInfo(QDir(root).filePath(path)).absolutePath()) || !QFile::copy(QDir(source).filePath(path),QDir(root).filePath(path))) { why=path; return false; }
        }
        if(!withNgram) return true;
        if(!QDir().mkpath(QDir(root).filePath("language-packs"))) return false;
        for(const QString &name:{QString("en.ngram"),QString("en.vocab.tsv"),QString("en-CONTRIBUTORS.txt"),QString("TATOEBA-CC-BY-2.0-FR.txt"),
                QString("ODC-By-1.0.txt"),QString("LANGUAGE-FILES-NOTICE.md"),QString("Apache-2.0.txt")}) {
            if(!QFile::copy(QDir(packs).filePath(name),QDir(root).filePath("language-packs/"+name))) { why=name; return false; }
        }
        return true;
    }
    void englishUsesTheOpenNgramWhenInstalled() {
        if(qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS").isEmpty() || !QFileInfo::exists(qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS")+"/en.ngram"))
            QSKIP("English assets and an installed English n-gram fixture required");
        QTemporaryDir dir; QString why; QVERIFY2(englishRoot(dir.path(),true,why),qPrintable(why));
        for(const char *locale:{"en_US","en_GB","en-CA"}) {
            auto p=resolveProfile(locale,"qwerty","Latin",dir.path());
            QVERIFY2(p.capabilities.swipe,qPrintable(p.unavailableReason));
            QVERIFY(p.capabilities.nextWord); QVERIFY(p.capabilities.contextRanking);
            QCOMPARE(p.assets["contextModel"].compatibility,QString("en:context-ngram-v1"));
            QCOMPARE(p.assets["contextVocab"].compatibility,QString("en:context-ngram-v1"));
            QVERIFY(p.assets["contextModel"].path.endsWith("language-packs/en.ngram"));
            QVERIFY(p.assets["contextVocab"].path.endsWith("language-packs/en.vocab.tsv"));
        }
        auto p=resolveProfile("en_US","qwerty","Latin",dir.path());
        ModelPaths paths; paths.encoder=p.assets["encoder"].path.toStdString(); paths.dictionary=p.assets["dictionary"].path.toStdString();
        paths.decoder=p.assets["decoder"].path.toStdString(); paths.ngram=p.assets["contextModel"].path.toStdString();
        RecognizerOptions options; options.language="en"; options.ngram=true; options.alphabet=languageAlphabet("en").toStdString();
        Recognizer engine; QVERIFY(engine.init(paths,options));
        QVERIFY2(engine.usingNgram(),"English must rank with the open n-gram, not FUTO's model");
    }
    void englishFallsBackToFutoWhenNgramFilesAreMissingOrDamaged() {
        if(qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS").isEmpty() || !QFileInfo::exists(qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS")+"/en.ngram"))
            QSKIP("English assets and an installed English n-gram fixture required");
        QTemporaryDir dir; QString why; QVERIFY2(englishRoot(dir.path(),true,why),qPrintable(why));
        auto resolve=[&] { return resolveProfile("en_US","qwerty","Latin",dir.path()); };
        auto expectFuto=[&](const char *what) {
            auto p=resolve();
            QVERIFY2(p.capabilities.swipe,what); QVERIFY2(p.capabilities.nextWord,what);
            QCOMPARE(p.assets["contextModel"].compatibility,QString("en:context-v1"));
            QVERIFY2(p.assets["contextModel"].path.endsWith("hungry_jellyfish/context_lm.pte"),what);
        };
        QCOMPARE(resolve().assets["contextModel"].compatibility,QString("en:context-ngram-v1"));
        // Anything wrong with the English files (missing, damaged, or a missing/changed licence text) means FUTO's
        // model keeps working exactly as before: no crash, no blank keyboard.
        for(const QString &name:{QString("en.ngram"),QString("en.vocab.tsv"),QString("en-CONTRIBUTORS.txt"),QString("ODC-By-1.0.txt"),
                QString("LANGUAGE-FILES-NOTICE.md"),QString("TATOEBA-CC-BY-2.0-FR.txt"),QString("Apache-2.0.txt")}) {
            const QString relative="language-packs/"+name; const auto original=languagefixtures::read(dir.filePath(relative));
            QVERIFY2(!original.isEmpty(),qPrintable(name));
            QVERIFY(languagefixtures::write(dir.path(),relative,original+"damage")); expectFuto(qPrintable("damaged "+name));
            QVERIFY(QFile::remove(dir.filePath(relative))); expectFuto(qPrintable("missing "+name));
            QVERIFY(languagefixtures::write(dir.path(),relative,original));
            QCOMPARE(resolve().assets["contextModel"].compatibility,QString("en:context-ngram-v1"));
        }
        // No English pack folder at all (today's install): identical to the previous behaviour.
        QTemporaryDir plain; QVERIFY2(englishRoot(plain.path(),false,why),qPrintable(why));
        auto p=resolveProfile("en_US","qwerty","Latin",plain.path());
        QVERIFY(p.capabilities.swipe); QCOMPARE(p.assets["contextModel"].compatibility,QString("en:context-v1"));
        // And with neither the open n-gram nor FUTO's model: swipes, no prediction, no error.
        QVERIFY(QFile::remove(plain.filePath("models/hungry_jellyfish/context_lm.pte"))); QVERIFY(QFile::remove(plain.filePath("models/hungry_jellyfish/vocab.txt")));
        p=resolveProfile("en_US","qwerty","Latin",plain.path());
        QVERIFY(p.capabilities.swipe); QVERIFY(!p.capabilities.nextWord); QVERIFY(!p.assets.contains("contextModel"));
    }
    void syntheticLanguageMechanics_data() {
        QTest::addColumn<QString>("language"); QTest::addColumn<QString>("layout"); QTest::addColumn<QString>("word");
        QTest::newRow("Spanish") << QString("es") << QString("qwerty") << QString("casa");
        QTest::newRow("French") << QString("fr") << QString("azerty") << QString("école");
        QTest::newRow("German") << QString("de") << QString("qwertz") << QString("schön");
    }
    void syntheticLanguageMechanics() {
        QFETCH(QString,language); QFETCH(QString,layout); QFETCH(QString,word);
        const QString root=qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS");
        if (root.isEmpty()) QSKIP("Generated pack fixture not supplied");
        auto p=resolveProfile(language,layout,"Latin",root); // Normal selection needs no opt-in.
        QVERIFY2(p.capabilities.swipe,qPrintable(p.unavailableReason));
        QVERIFY(p.capabilities.nextWord); QVERIFY(p.capabilities.contextRanking); QCOMPARE(p.assets.size(),4);
        ModelPaths paths; paths.encoder=p.assets["encoder"].path.toStdString(); paths.dictionary=p.assets["dictionary"].path.toStdString();
        paths.ngram=p.assets["contextModel"].path.toStdString();
        RecognizerOptions options; options.language=language.toStdString(); options.ngram=true; options.alphabet=languageAlphabet(language).toStdString();
        std::vector<KeyRect> keys;
        QStringList rows=layout=="azerty" ? QStringList{"azertyuiop","qsdfghjklm","wxcvbn"} :
            layout=="qwertz" ? QStringList{"qwertzuiop","asdfghjkl","yxcvbnm"} : QStringList{"qwertyuiop","asdfghjkl","zxcvbnm"};
        for (int r=0;r<rows.size();++r)
            for(int i=0;i<rows[r].size();++i) keys.push_back({char32_t(rows[r][i].unicode()),float(i)*100,float(r)*100,100,100});
        if (language=="es") keys.push_back({U'ñ',900,100,100,100});
        for (char32_t c:languageAlphabet(language).toUcs4()) {
            const auto base=popupBase(c,language); if (base!=c) options.popupAliases[c]=base;
        }
        std::vector<TracePoint> trace;
        float t=0;
        for (char32_t c:word.toUcs4()) {
            const auto base=popupBase(c,language);
            const auto key=std::find_if(keys.begin(),keys.end(),[&](const auto &k){return k.letter==base;});
            QVERIFY(key!=keys.end());
            if (!trace.empty()) {
                const auto previous=trace.back();
                for(int n=1;n<=5;++n) trace.push_back({previous.x+(key->x+50-previous.x)*n/5,previous.y+(key->y+50-previous.y)*n/5,t+=20});
            } else trace.push_back({key->x+50,key->y+50,t});
        }
        QVERIFY(normalize(trace,keys,options.alphabet,options.popupAliases));
        Recognizer engine;
        ModelPaths wrong=paths;
        wrong.decoder=qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS").toStdString()+"/models/magic_macaw/model_fp32.pte";
        QVERIFY(!engine.init(wrong,options));
        QVERIFY(engine.init(paths,options));
        const auto candidates=engine.recognize(trace,keys,{},10);
        QVERIFY2(!candidates.empty(),"Synthetic trace produced no candidates (not an accuracy threshold)");
        QFile dict(p.assets["dictionary"].path); QVERIFY(dict.open(QIODevice::ReadOnly));
        const QString data=QString::fromUtf8(dict.readAll());
        // WHY compare lower-cased: the open word lists keep real capitals (names, German nouns, "Ecole"), while the
        // engine reports the lower-case search form of a dictionary word; a candidate must still be a dictionary word.
        const QString lowerData=data.toLower();
        for (const auto &c:candidates) {
            const auto spelling=QString::fromStdString(c.word);
            QVERIFY2(lowerData.contains(" word="+spelling.toLower()+","),qPrintable(spelling));
        }
        QVERIFY(!engine.bestShapeWord().empty());
    }
};
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv); LanguagePacksTest test;
    const int result=QTest::qExec(&test,argc,argv);
    return result ? result : (languagefixtures::runtimeAvailable() ? 0 : 77);
}
#include "tst_languagepacks.moc"
