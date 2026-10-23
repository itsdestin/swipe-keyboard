#include "swipekb/prediction.hpp"
#include "swipekb/ngram.hpp"
#include "swipekb/ngram_predictor.hpp"
#include "swipekb/nwp_predictor.hpp"
#include "swipekb/language_engine.hpp"
#include <atomic>
#include <future>
#include <thread>
#include <QtTest>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

class PredictionTest : public QObject {
    Q_OBJECT
private slots:
    void filtersLexicalWordsAndCaseDuplicatesBeforeTruncation() {
        const auto words = swipekb::filterPredictedWords({{"123",1}, {".",1}, {"!",1}, {"word",1},
            {"Word",1}, {"can't",1}, {"-bad",1}, {"bad-",1}, {"<unk>",1}, {"third",1}}, 3);
        QCOMPARE(words, std::vector<std::string>({"word", "can't", "third"}));
    }
    void preservesSupportedLanguageOrthography() {
        const auto words = swipekb::filterPredictedWords({{"niño",1}, {"pingüino",1}, {"¿Cómo",1},
            {"école",1}, {"l’école",1}, {"d'accord",1}, {"œuvre",1}, {"Ärger",1},
            {"Straße",1}, {"Strasse",1}, {"123",1}, {"<unk>",1}}, 20);
        QCOMPARE(words, std::vector<std::string>({"niño", "pingüino", "école", "l’école",
            "d'accord", "œuvre", "Ärger", "Straße", "Strasse"}));
    }
    void bundledForeignContextModelsAreLanguageSpecificAndContextual() {
        const QString root=qEnvironmentVariable("SWIPEKB_TEST_MULTILINGUAL_MODELS");
        if(root.isEmpty()) QSKIP("Explicit verified multilingual model fixture not supplied");
        for(const auto &language:QStringList{"es","fr","de"}) {
            swipekb::Predictor predictor;
            const auto base=(root+"/"+language).toStdString();
            QVERIFY2(predictor.init(base+"/context.ngram",base+"/vocab.tsv"),qPrintable(language));
            QVERIFY(predictor.ready());
            const auto first=predictor.predict(language=="es" ? std::vector<std::string>{"me","gusta"} :
                language=="fr" ? std::vector<std::string>{"je","suis"} : std::vector<std::string>{"ich","bin"},5);
            const auto second=predictor.predict(language=="es" ? std::vector<std::string>{"por","favor"} :
                language=="fr" ? std::vector<std::string>{"de","la"} : std::vector<std::string>{"die","Zeit"},5);
            QVERIFY(!first.empty()); QVERIFY(!second.empty()); QVERIFY(first!=second);
            for(const auto &word:first) QVERIFY(!word.empty());
        }
        swipekb::Predictor unavailable;
        QVERIFY(!unavailable.init((root+"/es/corrupt.ngram").toStdString(),(root+"/es/vocab.tsv").toStdString()));
        QVERIFY(!unavailable.ready());
    }
    void completionKeepsTypedLettersAndRanksBySentence() {
        // Words 3.. = yo, soy, sol, estoy. Alone, "sol" beats "soy"; after "yo", "soy" is far likelier.
        using swipekb::NgramLM;
        swipekb::NgramModelData d;
        d.words = {"yo", "soy", "sol", "estoy"};
        d.uni_lp = {-2.f, -99.f, -1.5f, -1.0f, -1.6f, -1.2f, -1.3f};
        d.uni_bo = std::vector<float>(7, 0.f);
        d.bi_keys = {NgramLM::key(NgramLM::kBos, 3), NgramLM::key(3, 4)};
        d.bi_lp = {-0.1f, -0.1f};
        d.bi_bo = {0.f, 0.f};
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto model = dir.filePath("es.ngram").toStdString(), vocab = dir.filePath("es.vocab.tsv").toStdString();
        QVERIFY(NgramLM::write(model, d));
        QFile v(QString::fromStdString(vocab));
        QVERIFY(v.open(QIODevice::WriteOnly));
        v.write("yo\t200\tyo\nsoy\t180\tsoy\nsol\t170\tsol\nestoy\t160\testoy\n");
        v.close();
        swipekb::Predictor predictor;
        QVERIFY(predictor.init(model, vocab));
        QCOMPARE(predictor.predict({"yo"}, 3, "so"), std::vector<std::string>({"soy", "sol"}));
        QCOMPARE(predictor.predict({}, 3, "so"), std::vector<std::string>({"sol", "soy"}));
        QCOMPARE(predictor.predict({"yo"}, 3, "So"), std::vector<std::string>({"soy", "sol"}));
        QVERIFY(predictor.predict({"yo"}, 3, "x").empty());
        QVERIFY(predictor.predict({"yo"}, 3, "'").empty());
        QCOMPARE(predictor.predict({"yo"}, 1).front(), std::string("soy"));
    }
    void fastNextWordMatchesScoringEveryWord() {
        // WHY every word starts with "a": the completion path (prefix "a") scores every word, so it is the
        // reference the next-word shortcut (continuations + most frequent others) must reproduce exactly.
        using swipekb::NgramLM;
        swipekb::NgramModelData d;
        // WHY 200 words with rare bigram targets: more than the 4 x 12 leaders, so a shortcut that skipped the
        // continuations would miss them.
        for (int i = 0; i < 200; ++i)   // letters only: the model drops digits, so "a1"/"a2" would be one word
            d.words.push_back(std::string("a") + char('a' + i / 26) + char('a' + i % 26));
        const size_t V = d.words.size() + 3;
        d.uni_lp.resize(V); d.uni_bo.assign(V, -0.3f);
        for (size_t w = 0; w < V; ++w) d.uni_lp[w] = -1.f - float((w * 7) % 13) * 0.2f;   // many ties
        for (uint32_t w : {90u, 150u, 199u}) d.uni_lp[w] = -6.f;
        for (uint32_t w : {90u, 150u, 199u}) { d.bi_keys.push_back(NgramLM::key(3, w)); d.bi_lp.push_back(-0.2f); d.bi_bo.push_back(-0.1f); }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto model = dir.filePath("t.ngram").toStdString(), vocab = dir.filePath("t.vocab.tsv").toStdString();
        QVERIFY(NgramLM::write(model, d));
        QFile v(QString::fromStdString(vocab));
        QVERIFY(v.open(QIODevice::WriteOnly));
        for (const auto &w : d.words) v.write((w + "\t100\t" + w + "\n").c_str());
        v.write("azzunknown\t100\tazzunknown\n");   // not in the model: the dictionary fallback path
        v.close();
        swipekb::Predictor predictor;
        QVERIFY(predictor.init(model, vocab));
        for (const auto &context : std::vector<std::vector<std::string>>{{}, {d.words[0]}, {d.words[5]}, {"zzz"}})
            for (int k : {1, 3, 12})
                QCOMPARE(predictor.predict(context, k), predictor.predict(context, k, "a"));
    }
    // Card #170: the keyboard's Predictor (what the plugin's worker runs) must use LANG.nwp.bin when it sits next to
    // LANG.ngram, give exactly what the hybrid gives, and behave as the plain n-gram when the file is not there.
    void neuralModelNextToNgramIsUsedAutomatically() {

        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QStringList words = {"the", "of", "and", "a", "to", "in", "is", "you", "that", "it", "he", "was", "for", "on",
            "are", "as", "with", "his", "they", "at", "be", "this", "have", "from", "or", "one", "had", "by", "word", "but"};
        swipekb::NgramModelData d;
        d.uni_lp = {-2.f, -99.f, -1.5f}; d.uni_bo = {0.f, 0.f, 0.f};
        QByteArray vocabText;
        for (int i = 0; i < words.size(); ++i) {
            d.words.push_back(words[i].toStdString()); d.uni_lp.push_back(-1.f - 0.1f * float(i)); d.uni_bo.push_back(0.f);
            vocabText += words[i].toUtf8() + "\t100\t" + words[i].toUtf8() + "\n";
        }
        const auto model = dir.filePath("xx.ngram").toStdString(), vocab = dir.filePath("xx.vocab.tsv").toStdString();
        QVERIFY(swipekb::NgramLM::write(model, d));
        QFile v(QString::fromStdString(vocab)); QVERIFY(v.open(QIODevice::WriteOnly)); v.write(vocabText); v.close();
        const std::vector<std::string> context = {"to", "it", "the", "is"};

        swipekb::Predictor plain; QVERIFY(plain.init(model, vocab));
        swipekb::NgramPredictor ngramOnly; QVERIFY(ngramOnly.load(model, vocab));
        QCOMPARE(plain.predict(context, 3), ngramOnly.predict(context, 3));

        QVERIFY(QFile::copy(QStringLiteral(SWIPEKB_TEST_DATA "/nwp-tiny.nwp.bin"), dir.filePath("xx.nwp.bin")));
        swipekb::Predictor hybrid; QVERIFY(hybrid.init(model, vocab));
        swipekb::NwpPredictor expected; QVERIFY(expected.load(model, vocab)); QVERIFY(expected.hasLm());
        QCOMPARE(hybrid.predict(context, 3), expected.predict(context, 3));
        QVERIFY(hybrid.predict(context, 3) != plain.predict(context, 3));   // the neural model really changed the answer
        for (const auto &w : hybrid.predict(context, 5, "t")) QVERIFY(w[0] == 't');

        // Card #179: the keyboard decides which neural file is used. With the neighbour fallback off and no verified
        // path, an .nwp.bin lying beside the n-gram is ignored (n-gram answers); an explicit path is used even from
        // another folder.
        swipekb::Predictor unverified; QVERIFY(unverified.init(model, vocab, {}, false));
        QCOMPARE(unverified.predict(context, 3), plain.predict(context, 3));
        QTemporaryDir other; QVERIFY(other.isValid());
        const QString elsewhere = other.filePath("verified.nwp.bin");
        QVERIFY(QFile::copy(dir.filePath("xx.nwp.bin"), elsewhere));
        QVERIFY(QFile::remove(dir.filePath("xx.nwp.bin")));
        swipekb::Predictor explicitPath; QVERIFY(explicitPath.init(model, vocab, elsewhere.toStdString(), false));
        QCOMPARE(explicitPath.predict(context, 3), hybrid.predict(context, 3));
    }
    void absentModelIsUnavailable() {
        swipekb::Predictor p;
        QVERIFY(!p.init("/missing/model", "/missing/vocab"));
        QVERIFY(!p.ready()); QVERIFY(p.predict({"hello"}, 3).empty());
    }
    void independentScorerDoesNotChangeConcurrentSwipeResults() {
        const auto root = qEnvironmentVariable("SWIPEKB_TEST_ASSETS").toStdString();
        QVERIFY(!root.empty());
        swipekb::LanguageEngineSetup setup;
        setup.dictionary = root + "/dict/en_swipe.combined";
        setup.ngram = root + "/language-packs/en.ngram";
        setup.swipeModel = root + "/language-packs/swipe-model.bin";
        swipekb::LanguageEngine recognizer; QVERIFY(recognizer.init(setup));
        std::vector<swipekb::KeyRect> keys;
        const std::vector<std::string> rows = {"qwertyuiop","asdfghjkl","zxcvbnm"};
        for (int r = 0; r < 3; ++r)
            for (size_t i = 0; i < rows[r].size(); ++i)
                keys.push_back({rows[r][i],float(i*100 + (r==1 ? 50 : r==2 ? 150 : 0)),float(r*100),100,100});
        const auto center = [&](char c) {
            for (const auto &k : keys) if (k.letter == c) return QPointF(k.x+50,k.y+50);
            return QPointF();
        };
        std::vector<swipekb::TracePoint> trace;
        const std::string word = "hello";
        for (size_t i = 0; i + 1 < word.size(); ++i) {
            const auto a = center(word[i]), b = center(word[i+1]);
            for (int step = 0; step < 12; ++step) {
                const auto p = a + (b-a)*float(step)/12;
                trace.push_back({float(p.x()),float(p.y()),float(trace.size()*8)});
            }
        }
        auto baseline = recognizer.recognize(trace,keys,{"say"},3); QVERIFY(!baseline.empty());
        QElapsedTimer baselineTimer; baselineTimer.start();
        for (int i = 0; i < 10; ++i) QVERIFY(!recognizer.recognize(trace,keys,{"say"},3).empty());
        const auto baselineMs = baselineTimer.elapsed();
        std::atomic<bool> stop = false;
        std::promise<bool> loaded;
        auto ready = loaded.get_future();
        auto work = std::async(std::launch::async,[&] {
            swipekb::Predictor predictor;
            const bool ok = predictor.init(root+"/language-packs/en.ngram",root+"/language-packs/en.vocab.tsv");
            int predictions = ok && !predictor.predict({"hello","how","are"},3).empty() ? 1 : 0;
            loaded.set_value(ok && predictions > 0);
            while (ok && !stop.load()) { if (!predictor.predict({"hello","how","are"},3).empty()) ++predictions; }
            return predictions;
        });
        const bool ok = ready.get();
        QElapsedTimer elapsed; elapsed.start();
        bool unchanged = ok;
        for (int i = 0; i < 10; ++i) {
            const auto actual = recognizer.recognize(trace,keys,{"say"},3);
            unchanged = unchanged && actual.size() == baseline.size();
            for (size_t j = 0; j < std::min(actual.size(),baseline.size()); ++j)
                unchanged = unchanged && actual[j].word == baseline[j].word;
        }
        stop = true; const int predictions = work.get();
        QVERIFY(unchanged); QVERIFY(predictions > 0);
        qInfo() << "ten baseline swipe elapsed ms" << baselineMs << "concurrent elapsed ms" << elapsed.elapsed()
                << "predictions completed" << predictions;
    }
    void realModelPredictsBoundedWords() {
        const QString assets = qEnvironmentVariable("SWIPEKB_TEST_ASSETS");
        QVERIFY2(!assets.isEmpty(), "Set SWIPEKB_TEST_ASSETS to an explicit read-only asset directory");
        swipekb::Predictor p;
        QElapsedTimer clock; clock.start();
        QVERIFY(p.init((assets + "/language-packs/en.ngram").toStdString(),
                       (assets + "/language-packs/en.vocab.tsv").toStdString()));
        const auto cold = clock.elapsed();
        QVERIFY(p.ready());
        clock.restart();
        const auto words = p.predict({"hello"}, 3);
        QVERIFY(!words.empty()); QVERIFY(words.size() <= 3);
        for (const auto &w : words) QVERIFY(!w.empty());
        const auto first = clock.elapsed(); clock.restart();
        QVERIFY(!p.predict({"hello", "how", "are"}, 3).empty());
        qInfo() << "model load ms" << cold << "first prediction ms" << first << "warm ms" << clock.elapsed();
        QVERIFY(p.predict({"hello"}, 0).empty());
        QVERIFY(!p.init("/missing", "/missing")); QVERIFY(!p.ready());
    }
};
QTEST_GUILESS_MAIN(PredictionTest)
#include "test_prediction.moc"
