// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QtTest>
#include <QFile>
#include <QTextStream>
#include <QTemporaryDir>
#include "languagefixtures.h"
#include "languageprofile.h"
#include "languagetextrules.h"
#include "languageruntime.h"
#include "swipekb/language_engine.hpp"
using namespace swipekb;
class LanguagePacksTest : public QObject {
    Q_OBJECT
private slots:
    void realEnglishLock() {
        const QString root=qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS");
        if (root.isEmpty()) QSKIP("Explicit read-only English asset fixture not supplied");
        auto en=resolveProfile("en_US","qwerty","Latin",root);
        QVERIFY2(en.capabilities.swipe,qPrintable(en.unavailableReason));
        auto uk=resolveProfile("en_GB","qwertz","Latin",root);
        QVERIFY2(uk.capabilities.swipe,qPrintable(uk.unavailableReason));
        QCOMPARE(en.assets["dictionary"].sha256,uk.assets["dictionary"].sha256);
        // The swipe model is shared by every language and found beside the packs.
        if (QFileInfo::exists(root+"/language-packs/swipe-model.bin")) QVERIFY(en.assets["swipeModel"].path.endsWith("language-packs/swipe-model.bin"));
    }
    // Lays out a model root like a new install: the English dictionary, plus (optionally) the English next-word files
    // and the texts that must accompany them, from the verified language-pack fixture.
    static bool englishRoot(const QString &root,bool withNgram,QString &why) {
        const auto source=qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS"), packs=qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS");
        if(!QDir().mkpath(QDir(root).filePath("dict")) || !QFile::copy(source+"/dict/en_swipe.combined",QDir(root).filePath("dict/en_swipe.combined"))) { why="dict"; return false; }
        if(!withNgram) return true;
        if(!QDir().mkpath(QDir(root).filePath("language-packs"))) return false;
        for(const QString &name:{QString("en.ngram"),QString("en.vocab.tsv"),QString("en-CONTRIBUTORS.txt"),QString("TATOEBA-CC-BY-2.0-FR.txt"),
                QString("ODC-By-1.0.txt"),QString("LANGUAGE-FILES-NOTICE.md"),QString("Apache-2.0.txt")}) {
            if(!QFile::copy(QDir(packs).filePath(name),QDir(root).filePath("language-packs/"+name))) { why=name; return false; }
        }
        return true;
    }
    void englishNeedsOnlyItsPinnedDictionary() {
        if(qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS").isEmpty()) QSKIP("Explicit locked English asset fixture required");
        QTemporaryDir dir; QString why; QVERIFY2(englishRoot(dir.path(),false,why),qPrintable(why));
        auto resolve=[&] { return resolveProfile("en-GB","qwerty","Latin",dir.path()); };
        auto p=resolve(); QVERIFY2(p.capabilities.swipe,qPrintable(p.unavailableReason));
        // Without next-word files English swipes and completes, without prediction; no swipe model = geometry.
        QVERIFY(!p.capabilities.contextRanking); QVERIFY(!p.capabilities.nextWord);
        QCOMPARE(p.assets.keys(),QStringList{"dictionary"});
        const auto dictionary=languagefixtures::read(dir.filePath("dict/en_swipe.combined"));
        QVERIFY(dictionary.contains("date=0"));
        auto dated=dictionary; dated.replace("date=0","date=1792036347");
        QVERIFY(languagefixtures::write(dir.path(),"dict/en_swipe.combined",dated));
        QCOMPARE(resolve().unavailableReason,QString("Bundled English dictionary missing or checksum mismatch"));
        QVERIFY(QFile::remove(dir.filePath("dict/en_swipe.combined")));
        QVERIFY(!resolve().capabilities.swipe);
        QVERIFY(languagefixtures::write(dir.path(),"dict/en_swipe.combined",dictionary)); QVERIFY(resolve().capabilities.swipe);
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
            QVERIFY2(broken.assets.contains("dictionary"),what);
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
            // the swipe dictionary is itself counted from FineWeb (ODC-By) and Tatoeba (CC BY 2.0 FR) text, so its
            // licence texts are part of the swipe-level pins; authors are context-only.
            if(name.endsWith("CONTRIBUTORS.txt")) expectSwipeWithoutPrediction(qPrintable(name));
            else QVERIFY2(!resolveProfile("es-MX","qwerty","Latin",copy.path()).capabilities.swipe,qPrintable(name));
            QVERIFY(languagefixtures::write(copy.path(),name,original));
            QVERIFY2(resolveProfile("es-MX","qwerty","Latin",copy.path()).capabilities.nextWord,qPrintable(name));
        }
        // The word list itself stays strictly required: damage there makes the language unavailable.
        const auto dictionary=languagefixtures::read(copy.filePath("es.combined"));
        QVERIFY(languagefixtures::write(copy.path(),"es.combined",dictionary+"x"));
        QVERIFY(!resolveProfile("es-MX","qwerty","Latin",copy.path()).capabilities.swipe);
        QVERIFY(languagefixtures::write(copy.path(),"es.combined",dictionary));
        // A damaged shared swipe model only removes the model (geometry decoding), never the language.
        if (QFileInfo::exists(copy.filePath("swipe-model.bin"))) {
            QVERIFY(resolveProfile("es-MX","qwerty","Latin",copy.path()).assets.contains("swipeModel"));
            const auto model=languagefixtures::read(copy.filePath("swipe-model.bin"));
            QVERIFY(languagefixtures::write(copy.path(),"swipe-model.bin",model+"x"));
            const auto q=resolveProfile("es-MX","qwerty","Latin",copy.path());
            QVERIFY(q.capabilities.swipe); QVERIFY(q.capabilities.nextWord); QVERIFY(!q.assets.contains("swipeModel"));
        }
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
        LanguageEngineSetup setup; QVERIFY(configureLanguageEngine(p,setup));
        LanguageEngine engine; QVERIFY(engine.init(setup));
        QVERIFY2(engine.usingNgram(),"English must rank swipes with the open n-gram");
    }
    void englishKeepsSwipingWhenNgramFilesAreMissingOrDamaged() {
        if(qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS").isEmpty() || !QFileInfo::exists(qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS")+"/en.ngram"))
            QSKIP("English assets and an installed English n-gram fixture required");
        QTemporaryDir dir; QString why; QVERIFY2(englishRoot(dir.path(),true,why),qPrintable(why));
        auto resolve=[&] { return resolveProfile("en_US","qwerty","Latin",dir.path()); };
        auto expectPlain=[&](const char *what) {
            auto p=resolve();
            QVERIFY2(p.capabilities.swipe,what); QVERIFY2(!p.capabilities.nextWord,what);
            QVERIFY2(!p.assets.contains("contextModel"),what);
        };
        QCOMPARE(resolve().assets["contextModel"].compatibility,QString("en:context-ngram-v1"));
        // Anything wrong with the English next-word files (missing, damaged, or a missing/changed licence text) only
        // removes next-word suggestions: no crash, no blank keyboard.
        for(const QString &name:{QString("en.ngram"),QString("en.vocab.tsv"),QString("en-CONTRIBUTORS.txt"),QString("ODC-By-1.0.txt"),
                QString("LANGUAGE-FILES-NOTICE.md"),QString("TATOEBA-CC-BY-2.0-FR.txt"),QString("Apache-2.0.txt")}) {
            const QString relative="language-packs/"+name; const auto original=languagefixtures::read(dir.filePath(relative));
            QVERIFY2(!original.isEmpty(),qPrintable(name));
            QVERIFY(languagefixtures::write(dir.path(),relative,original+"damage")); expectPlain(qPrintable("damaged "+name));
            QVERIFY(QFile::remove(dir.filePath(relative))); expectPlain(qPrintable("missing "+name));
            QVERIFY(languagefixtures::write(dir.path(),relative,original));
            QCOMPARE(resolve().assets["contextModel"].compatibility,QString("en:context-ngram-v1"));
        }
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
        QVERIFY(p.capabilities.nextWord); QVERIFY(p.capabilities.contextRanking);
        // dictionary, n-gram, word list; plus the neural next-word model and the shared swipe model when present.
        QCOMPARE(p.assets.size(),3+int(QFileInfo::exists(root+"/"+language+".nwp.bin"))+int(QFileInfo::exists(root+"/swipe-model.bin")));
        LanguageEngineSetup setup; QVERIFY(configureLanguageEngine(p,setup));
        std::vector<KeyRect> keys;
        QStringList rows=layout=="azerty" ? QStringList{"azertyuiop","qsdfghjklm","wxcvbn"} :
            layout=="qwertz" ? QStringList{"qwertzuiop","asdfghjkl","yxcvbnm"} : QStringList{"qwertyuiop","asdfghjkl","zxcvbnm"};
        for (int r=0;r<rows.size();++r)
            for(int i=0;i<rows[r].size();++i) keys.push_back({char32_t(rows[r][i].unicode()),float(i)*100,float(r)*100,100,100});
        if (language=="es") keys.push_back({U'ñ',900,100,100,100});
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
        QVERIFY(normalize(trace,keys,languageAlphabet(language).toStdString(),popupAliasesFor(language)));
        LanguageEngine engine;
        QVERIFY(engine.init(setup));
        const auto candidates=engine.recognize(trace,keys,{},10);
        QVERIFY2(!candidates.empty(),"Synthetic trace produced no candidates (not an accuracy threshold)");
        QFile dict(p.assets["dictionary"].path); QVERIFY(dict.open(QIODevice::ReadOnly));
        const QString data=QString::fromUtf8(dict.readAll());
        // WHY compare lower-cased: the open word lists keep real capitals (names, German nouns, "Ecole"); a candidate
        // must still be a dictionary word.
        const QString lowerData=data.toLower();
        for (const auto &c:candidates) {
            const auto spelling=QString::fromStdString(c.word);
            QVERIFY2(lowerData.contains(" word="+spelling.toLower()+","),qPrintable(spelling));
        }
        QVERIFY(!engine.bestShapeWord().empty());
    }
    // Card #179: the neural next-word model (LANG.nwp.bin) is offered to the engine only when its size and SHA-256 equal the
    // lock. Any other state (absent, damaged, swapped) removes only that role: the n-gram, the word list and the language stay.
    void neuralNextWordModelIsOfferedOnlyWhenVerified() {
        const QString source=qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS");
        if(source.isEmpty() || !QFileInfo::exists(source+"/es.nwp.bin")) QSKIP("Verified pack fixture with es.nwp.bin required");
        QTemporaryDir copy; QVERIFY(copy.isValid());
        for(const auto &name:QDir(source).entryList(QDir::Files))
            QVERIFY2(QFile::copy(QDir(source).filePath(name),copy.filePath(name)),qPrintable(name));
        auto resolve=[&] { return resolveProfile("es-MX","qwerty","Latin",copy.path()); };
        auto p=resolve();
        QVERIFY2(p.capabilities.nextWord,qPrintable(p.unavailableReason));
        QVERIFY(p.assets.contains("contextNwp"));
        QVERIFY(p.assets["contextNwp"].path.endsWith("/es.nwp.bin"));
        QCOMPARE(p.assets["contextNwp"].compatibility,QString("es:context-nwp-v1"));
        const auto good=languagefixtures::read(copy.filePath("es.nwp.bin")); QVERIFY(!good.isEmpty());
        auto expectNgramOnly=[&](const char *what) {
            auto q=resolve();
            QVERIFY2(q.capabilities.swipe && q.capabilities.nextWord,what);
            QVERIFY2(q.assets.contains("contextModel") && q.assets.contains("contextVocab"),what);
            QVERIFY2(!q.assets.contains("contextNwp"),what);
        };
        QVERIFY(languagefixtures::write(copy.path(),"es.nwp.bin",good+"x"));   // longer
        expectNgramOnly("longer file");
        auto flipped=good; flipped[flipped.size()/2]=char(flipped[flipped.size()/2]^0x01);   // same size, one bit changed
        QVERIFY(languagefixtures::write(copy.path(),"es.nwp.bin",flipped));
        expectNgramOnly("one changed bit");
        QVERIFY(QFile::remove(copy.filePath("es.nwp.bin")));
        expectNgramOnly("absent file");
        QVERIFY(languagefixtures::write(copy.path(),"es.nwp.bin",good));
        QVERIFY(resolve().assets.contains("contextNwp"));
        // A model that is valid for another language must not pass as this one's (the lock pins one file per language).
        QVERIFY(QFile::copy(source+"/fr.nwp.bin",copy.filePath("fr.nwp.bin.other")));
        QVERIFY(QFile::remove(copy.filePath("es.nwp.bin")));
        QVERIFY(QFile::copy(copy.filePath("fr.nwp.bin.other"),copy.filePath("es.nwp.bin")));
        expectNgramOnly("French model under the Spanish name");
    }
    void englishNeuralNextWordFollowsTheSameRule() {
        if(qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS").isEmpty() || !QFileInfo::exists(qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS")+"/en.nwp.bin"))
            QSKIP("English assets and an installed en.nwp.bin fixture required");
        QTemporaryDir dir; QString why; QVERIFY2(englishRoot(dir.path(),true,why),qPrintable(why));
        const auto packs=qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS");
        QVERIFY(QFile::copy(packs+"/en.nwp.bin",dir.filePath("language-packs/en.nwp.bin")));
        auto p=resolveProfile("en-US","qwerty","Latin",dir.path());
        QVERIFY2(p.assets.contains("contextNwp"),qPrintable(p.unavailableReason));
        QCOMPARE(p.assets["contextModel"].compatibility,QString("en:context-ngram-v1"));
        const auto good=languagefixtures::read(dir.filePath("language-packs/en.nwp.bin"));
        QVERIFY(languagefixtures::write(dir.path(),"language-packs/en.nwp.bin",good+"x"));
        p=resolveProfile("en-US","qwerty","Latin",dir.path());
        QVERIFY(!p.assets.contains("contextNwp")); QCOMPARE(p.assets["contextModel"].compatibility,QString("en:context-ngram-v1"));
    }
    // The swipe model is used only when it is byte-identical to the pinned file (else geometry-only).
    void swipeModelIsVerifiedAgainstTheLock() {
        const auto source=qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS")+"/swipe-model.bin";
        if(qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS").isEmpty() || !QFileInfo::exists(source)) QSKIP("Pinned swipe-model.bin fixture required");
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QVERIFY(verifiedSwipeModel(dir.path()).isEmpty());   // absent
        QVERIFY(QFile::copy(source,dir.filePath("swipe-model.bin")));
        QVERIFY(verifiedSwipeModel(dir.path()).endsWith("/swipe-model.bin"));
        auto bytes=languagefixtures::read(dir.filePath("swipe-model.bin"));
        auto flipped=bytes; flipped[100]=char(flipped[100]^0x01);
        QVERIFY(languagefixtures::write(dir.path(),"swipe-model.bin",flipped));
        QVERIFY(verifiedSwipeModel(dir.path()).isEmpty());   // same size, different bytes
        QVERIFY(languagefixtures::write(dir.path(),"swipe-model.bin",bytes+"x"));
        QVERIFY(verifiedSwipeModel(dir.path()).isEmpty());   // wrong size
    }
};
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv); LanguagePacksTest test;
    const int result=QTest::qExec(&test,argc,argv);
    return result ? result : (languagefixtures::runtimeAvailable() ? 0 : 77);
}
#include "tst_languagepacks.moc"
