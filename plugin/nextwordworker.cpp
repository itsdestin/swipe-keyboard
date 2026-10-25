// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "nextwordworker.h"
#include "swipekb/prediction.hpp"

NextWordWorker::NextWordWorker(Inference inference, Scorer scorer, QObject *parent)
    : QThread(parent), m_inference(std::move(inference)), m_scorer(std::move(scorer))
{
    qRegisterMetaType<PredictionStamp>();
    // WHY: scores cross to the input thread through a queued signal, which needs the type registered.
    qRegisterMetaType<QVector<float>>();
    start();
}
NextWordWorker::~NextWordWorker() { stop(); }
void NextWordWorker::submit(PredictionJob job)
{
    QMutexLocker lock(&m_mutex);
    if (m_stopping) return;
    ++m_serial;
    job.context = job.context.mid(qMax(0, job.context.size() - 20));
    for (auto &word : job.context) word = word.left(256);
    m_pending = std::move(job); m_wake.wakeOne();
}
void NextWordWorker::invalidate()
{
    QMutexLocker lock(&m_mutex);
    ++m_serial; m_pending.reset();
}
void NextWordWorker::requestStop()
{
    QMutexLocker lock(&m_mutex); m_stopping = true; ++m_serial; m_pending.reset(); m_wake.wakeOne();
}
void NextWordWorker::stop()
{
    requestStop();
    wait();
}
void NextWordWorker::run()
{
    // WHY local: creation, loading, inference AND destruction occur on this thread;
    // the recognizer's own mutable score buffers are never shared with predictions.
    swipekb::Predictor predictor;
    QString loadedIdentity;
    for (;;) {
        PredictionJob job; quint64 serial;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_stopping && !m_pending) m_wake.wait(&m_mutex);
            if (m_stopping) return;
            job = std::move(*m_pending); m_pending.reset(); serial = m_serial;
        }
        QStringList words;
        QVector<float> scores;
        const bool scoring = !job.scoreWords.isEmpty();
        bool modelLoaded=false;
        try {
            const bool hooked = scoring ? bool(m_scorer) : bool(m_inference);
            if (hooked) {
                if (scoring) scores = m_scorer(job); else words = m_inference(job);
                modelLoaded=true;
            } else {
                const auto identity = job.modelIdentity + '\n' + job.model + '\n' + job.vocab + '\n' + job.nwp;
                if (loadedIdentity != identity || !predictor.ready()) {
                    predictor.init(job.model.toStdString(), job.vocab.toStdString(), job.nwp.toStdString(), false);
                    loadedIdentity = identity;
                }
                modelLoaded=predictor.ready();
                std::vector<std::string> context;
                for (const auto &word : job.context) context.push_back(word.toStdString());
                if (scoring) {
                    // WHY the same loaded model and reload rule as a next-word job: the swap list ranks words by the
                    // very sentence model the guess bar uses, and a language switch reloads it here first.
                    std::vector<std::string> asked;
                    for (const auto &w : job.scoreWords) asked.push_back(w.toStdString());
                    for (float v : predictor.scoreWords(context, asked)) scores << v;
                } else {
                    for (const auto &word : predictor.predict(context, job.topK, job.prefix.toStdString()))
                        words << QString::fromStdString(word);
                }
            }
            // WHY after both paths: whatever produced the words, the bar shows the same capitals as a swipe.
            if (job.capitals)
                for (auto &w : words) w = QString::fromStdString(job.capitals->apply(w.toStdString()));
            // WHY: a score list that does not line up word for word with the question is worthless; send none.
            if (scoring && !scores.isEmpty() && scores.size() != job.scoreWords.size()) scores.clear();
        } catch (...) { words.clear(); scores.clear(); }
        {
            QMutexLocker lock(&m_mutex);
            if (!m_stopping && serial == m_serial) {
                // WHY no model path/context in status: only the current profile's
                // loaded bit may cross into support diagnostics.
                emit modelStateChanged(job.stamp.profileEpoch,modelLoaded);
                if (scoring) emit wordScoresReady(job.stamp, job.scoreWords, scores);
                else if (job.prefix.isEmpty()) emit resultReady(job.stamp, words);
                else emit completionReady(job.stamp, job.prefix, words);
            }
        }
        // WHY completion follows delivery even for revoked work: callers can
        // observe asynchronous drain without blocking the GUI or exposing words.
        emit requestSettled();
    }
}
