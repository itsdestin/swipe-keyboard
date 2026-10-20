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
        QVERIFY(!p.capabilities.nextWord); QVERIFY(!p.capabilities.contextRanking); QCOMPARE(p.assets.size(),2);
        ModelPaths paths; paths.encoder=p.assets["encoder"].path.toStdString(); paths.dictionary=p.assets["dictionary"].path.toStdString();
        RecognizerOptions options; options.language=language.toStdString(); options.alphabet=languageAlphabet(language).toStdString();
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
        for (const auto &c:candidates) {
            const auto spelling=QString::fromStdString(c.word);
            QVERIFY2(data.contains(" word="+spelling+",") || data.contains(" word="+spelling.toLower()+","),qPrintable(spelling));
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
