// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QtTest>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QFile>
#include <atomic>
#include "nextwordworker.h"

class NextWordWorkerTest : public QObject {
    Q_OBJECT
private slots:
    void onlyNewestQueuedRequestSurvives() {
        QSemaphore started, finish;
        NextWordWorker worker([&](const PredictionJob &j) {
            if (j.stamp.requestId == 1) { started.release(); finish.acquire(); }
            return QStringList{QString::number(j.stamp.requestId)};
        });
        QSignalSpy spy(&worker, &NextWordWorker::resultReady);
        worker.submit({{1, 1, 1}, "a", "b", {"hello"}});
        QVERIFY(started.tryAcquire(1, 15000));
        worker.submit({{1, 1, 2}, "a", "b", {"hello"}});
        worker.submit({{1, 1, 3}, "a", "b", {"hello"}});
        finish.release();
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 15000);
        QCOMPARE(qvariant_cast<PredictionStamp>(spy.at(0).at(0)).requestId, quint64(3));
    }
    void completionJobAnswersOnItsOwnSignal() {
        NextWordWorker worker([](const PredictionJob &j) { return QStringList{j.prefix + "a"}; });
        QSignalSpy next(&worker, &NextWordWorker::resultReady), completion(&worker, &NextWordWorker::completionReady);
        PredictionJob job{{1, 1, 1}, "a", "b", {"hola"}};
        job.prefix = "cas";
        worker.submit(job);
        QTRY_COMPARE_WITH_TIMEOUT(completion.size(), 1, 15000);
        QCOMPARE(completion.at(0).at(1).toString(), QStringLiteral("cas"));
        QCOMPARE(completion.at(0).at(2).toStringList(), QStringList{"casa"});
        QCOMPARE(next.size(), 0);
    }
    void invalidationSuppressesRunningResult() {
        QSemaphore started, finish;
        NextWordWorker worker([&](const PredictionJob &) { started.release(); finish.acquire(); return QStringList{"world"}; });
        QSignalSpy spy(&worker, &NextWordWorker::resultReady);
        worker.submit({{1, 1, 1}, "a", "b", {"hello"}});
        QVERIFY(started.tryAcquire(1, 15000)); worker.invalidate(); finish.release();
        worker.stop(); // joins completed inference: no sleep/poll can fake a suppressed result
        QCoreApplication::sendPostedEvents(); QCoreApplication::processEvents();
        QCOMPARE(spy.size(), 0);
    }
    void shutdownDropsQueuedWorkWhileInferenceDrains() {
        QSemaphore started, finish;
        std::atomic<int> calls = 0;
        NextWordWorker worker([&](const PredictionJob &) { ++calls; started.release(); finish.acquire(); return QStringList{"world"}; });
        QSignalSpy spy(&worker, &NextWordWorker::resultReady);
        worker.submit({{1,1,1}, "a", "b", {"hello"}});
        QVERIFY(started.tryAcquire(1,15000));
        worker.submit({{2,2,2}, "c", "d", {"bonjour"}});
        worker.requestStop(); finish.release(); worker.stop();
        QCoreApplication::processEvents();
        QCOMPARE(calls.load(),1); QCOMPARE(spy.size(),0);
    }
    void realScorerRecoversAndReconfigures() {
        QTemporaryDir data;
        const QString assets = qEnvironmentVariable("SWIPEKB_TEST_ASSETS");
        QVERIFY(!assets.isEmpty());
        const QString model = data.path() + "/model.ngram", vocab = data.path() + "/vocab.tsv";
        NextWordWorker worker;
        QSignalSpy modelState(&worker, &NextWordWorker::modelStateChanged);
        QSignalSpy spy(&worker, &NextWordWorker::resultReady);
        worker.submit({{1,1,1},model,vocab,{"hello"},"same-identity"});
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(),1,15000); QVERIFY(spy.at(0).at(1).toStringList().isEmpty());
        QVERIFY(QFile::link(assets + "/language-packs/en.ngram",model));
        QVERIFY(QFile::link(assets + "/language-packs/en.vocab.tsv",vocab));
        worker.submit({{2,2,2},model,vocab,{"hello"},"same-identity"});
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(),2,15000); QVERIFY(!spy.at(1).at(1).toStringList().isEmpty());
        worker.submit({{3,3,3},"/missing","/missing",{"hello"},"different-profile"});
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(),3,15000); QVERIFY(spy.at(2).at(1).toStringList().isEmpty());
        worker.submit({{4,4,4},model,vocab,{"hello"},"same-identity"});
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(),4,15000); QVERIFY(!spy.at(3).at(1).toStringList().isEmpty());
        QCOMPARE(modelState.size(),4);
        for (int i=0;i<4;++i) {
            QCOMPARE(modelState.at(i).at(0).toULongLong(),quint64(i+1));
            QCOMPARE(modelState.at(i).at(1).toBool(),i==1 || i==3);
        }
    }
    void inferenceFailureIsEmptyResult() {
        NextWordWorker worker([](const PredictionJob &) -> QStringList { throw std::runtime_error("failure"); });
        QSignalSpy spy(&worker, &NextWordWorker::resultReady);
        worker.submit({{1, 1, 1}, "a", "b", {"hello"}});
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 15000);
        QVERIFY(spy.at(0).at(1).toStringList().isEmpty());
    }
    // Tap-to-swap (one brain): a "score these words" job answers with one score per word on its own signal, from the
    // same loaded next-word model, and never with a word list.
    void scoreJobAnswersWithOneScorePerWord() {
        const QString assets = qEnvironmentVariable("SWIPEKB_TEST_ASSETS");
        QVERIFY(!assets.isEmpty());
        NextWordWorker worker;
        QSignalSpy scores(&worker, &NextWordWorker::wordScoresReady), next(&worker, &NextWordWorker::resultReady),
            completion(&worker, &NextWordWorker::completionReady);
        PredictionJob job{{1, 1, 1}, assets + "/language-packs/en.ngram", assets + "/language-packs/en.vocab.tsv",
                          {"i", "can"}, "en"};
        job.scoreWords = {"see", "sea", "sew"};
        worker.submit(job);
        QTRY_COMPARE_WITH_TIMEOUT(scores.size(), 1, 15000);
        QCOMPARE(qvariant_cast<PredictionStamp>(scores.at(0).at(0)).requestId, quint64(1));
        QCOMPARE(scores.at(0).at(1).toStringList(), (QStringList{"see", "sea", "sew"}));
        const auto got = qvariant_cast<QVector<float>>(scores.at(0).at(2));
        QCOMPARE(got.size(), 3);
        QVERIFY2(got[0] > got[1], "after \"i can\", \"see\" must score above \"sea\"");
        // No model: the job still answers on its own signal, with no scores.
        job.stamp = {1, 1, 2};
        job.model = job.vocab = "/missing";
        job.modelIdentity = "missing";
        worker.submit(job);
        QTRY_COMPARE_WITH_TIMEOUT(scores.size(), 2, 15000);
        QVERIFY(qvariant_cast<QVector<float>>(scores.at(1).at(2)).isEmpty());
        QCOMPARE(next.size() + completion.size(), 0);
    }
    void onlyNewestScoreJobSurvives() {
        QSemaphore started, finish;
        NextWordWorker worker({}, [&](const PredictionJob &j) {
            if (j.stamp.requestId == 1) { started.release(); finish.acquire(); }
            return QVector<float>(j.scoreWords.size(), float(j.stamp.requestId));
        });
        QSignalSpy scores(&worker, &NextWordWorker::wordScoresReady);
        PredictionJob job{{1, 1, 1}, "a", "b", {"hello"}};
        job.scoreWords = {"x", "y"};
        worker.submit(job);
        QVERIFY(started.tryAcquire(1, 15000));
        job.stamp = {1, 1, 2}; worker.submit(job);
        job.stamp = {1, 1, 3}; worker.submit(job);
        finish.release();
        QTRY_COMPARE_WITH_TIMEOUT(scores.size(), 1, 15000);
        QCOMPARE(qvariant_cast<PredictionStamp>(scores.at(0).at(0)).requestId, quint64(3));
        QCOMPARE(qvariant_cast<QVector<float>>(scores.at(0).at(2)), (QVector<float>{3.f, 3.f}));
    }
    // WHY: the next-word files spell some words as web text does ("Banco", "Hall"); the capitals list must decide what
    // the bar shows, as for a swipe ("I", "Paris"), and no list (German) leaves the words as they are.
    void capitalsListDecidesHowWordsAreShown() {
        NextWordWorker worker([](const PredictionJob &) { return QStringList{"i", "Hall", "paris", "Haus"}; });
        QSignalSpy spy(&worker, &NextWordWorker::resultReady);
        PredictionJob job{{1, 1, 1}, "a", "b", {"hello"}};
        job.capitals = swipekb::Capitals::fromTsv("i\tI\nparis\tParis\n");
        worker.submit(job);
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 15000);
        QCOMPARE(spy.at(0).at(1).toStringList(), QStringList({"I", "hall", "Paris", "haus"}));
        job.stamp = {1, 1, 2};
        job.capitals.reset();
        worker.submit(job);
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 2, 15000);
        QCOMPARE(spy.at(1).at(1).toStringList(), QStringList({"i", "Hall", "paris", "Haus"}));
    }
};
QTEST_GUILESS_MAIN(NextWordWorkerTest)
#include "tst_nextwordworker.moc"
