#include "swipekb/prediction.hpp"
#include "swipekb/recognizer.hpp"
#include <atomic>
#include <future>
#include <thread>
#include <QtTest>
#include <QElapsedTimer>
#include <QFile>
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
    void absentModelIsUnavailable() {
        swipekb::Predictor p;
        QVERIFY(!p.init("/missing/model", "/missing/vocab"));
        QVERIFY(!p.ready()); QVERIFY(p.predict({"hello"}, 3).empty());
    }
    void independentScorerDoesNotChangeConcurrentSwipeResults() {
        const auto root = qEnvironmentVariable("SWIPEKB_TEST_ASSETS").toStdString();
        QVERIFY(!root.empty());
        swipekb::Recognizer recognizer; QVERIFY(recognizer.init(swipekb::ModelPaths::fromDir(root)));
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
            const bool ok = predictor.init(root+"/models/hungry_jellyfish/context_lm.pte",root+"/models/hungry_jellyfish/vocab.txt");
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
        QVERIFY(p.init((assets + "/models/hungry_jellyfish/context_lm.pte").toStdString(),
                       (assets + "/models/hungry_jellyfish/vocab.txt").toStdString()));
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
