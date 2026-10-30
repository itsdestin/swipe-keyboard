// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "nextwordworker.h"
#include "swipekb/prediction.hpp"
#include <QElapsedTimer>
#include <algorithm>

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
QString NextWordWorker::identityOf(const PredictionJob &job)
{
    return job.modelIdentity + '\n' + job.model + '\n' + job.vocab + '\n' + job.nwp;
}
std::vector<float> NextWordWorker::scoreForSwipe(const PredictionJob &like, const std::vector<std::string> &context,
                                                 const std::vector<std::string> &words, int maxWaitMs, double *waitedMs)
{
    if (waitedMs) *waitedMs = 0;
    if (words.empty()) return {};
    const QString identity = identityOf(like);
    QElapsedTimer waited;
    waited.start();
    QMutexLocker gate(&m_swipeGate);   // the worker starts no new model call until this swipe is done
    // WHY tryLock with a limit: the swipe must stay quick even if a model call runs long; past the limit the swipe
    // simply keeps its n-gram ranking (the word is still typed, only without the sentence model's opinion).
    const bool locked = m_modelMutex.tryLock(qMax(0, maxWaitMs));
    if (waitedMs) *waitedMs = double(waited.nsecsElapsed()) / 1e6;
    if (!locked) return {};
    std::vector<float> scores;
    try {
        // WHY the identity check: right after a language switch the worker may still hold the previous language's
        // model (it reloads on its next job); scoring French swipe words with the English model would be noise.
        // WHY only with the neural part: the weights were tuned on the neural score (ContextRescore::forLanguage);
        // the n-gram alone is what the swipe already uses.
        if (m_model && m_modelIdentity == identity && m_model->hasNeural()) {
            // The same bounds as a queued job (submit): the last 20 words, each at most 256 bytes.
            std::vector<std::string> bounded(context.end() - std::min<std::ptrdiff_t>(20, std::ptrdiff_t(context.size())), context.end());
            for (auto &w : bounded) if (w.size() > 256) w.resize(256);
            scores = m_model->scoreWords(bounded, words, true);   // NaN = no opinion (ContextRescore)
        }
    } catch (...) { scores.clear(); }
    m_modelMutex.unlock();
    // WHY: an answer that does not line up word for word is worthless (the engine would refuse it too).
    if (scores.size() != words.size()) scores.clear();
    return scores;
}
void NextWordWorker::run()
{
    // WHY: creation, loading and destruction of the model all happen on this thread; the input thread only ever
    // calls it through scoreForSwipe, under m_modelMutex. The guard drops it here when the thread ends.
    struct DropModel {
        NextWordWorker *w;
        ~DropModel()
        {
            std::unique_ptr<swipekb::Predictor> old;
            { QMutexLocker lock(&w->m_modelMutex); old = std::move(w->m_model); w->m_modelIdentity.clear(); }
        }
    } dropModel{this};
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
                const auto identity = identityOf(job);
                bool current = false;
                {
                    QMutexLocker lock(&m_modelMutex);
                    current = m_model && m_modelIdentity == identity && m_model->ready();
                }
                if (!current) {
                    // WHY the old model is dropped before the new one loads, and the load runs without the lock: a
                    // swipe meanwhile finds no model and keeps its n-gram ranking at once, instead of waiting for a
                    // load (hundreds of ms), and two models are never held at the same time.
                    std::unique_ptr<swipekb::Predictor> old;
                    { QMutexLocker lock(&m_modelMutex); old = std::move(m_model); m_modelIdentity.clear(); }
                    old.reset();
                    auto fresh = std::make_unique<swipekb::Predictor>();
                    fresh->init(job.model.toStdString(), job.vocab.toStdString(), job.nwp.toStdString(), false);
                    QMutexLocker lock(&m_modelMutex);
                    m_model = std::move(fresh);
                    m_modelIdentity = identity;
                }
                std::vector<std::string> context;
                for (const auto &word : job.context) context.push_back(word.toStdString());
                // WHY the lock only around the model call: that is the longest a swipe can wait for this thread.
                // WHY through the gate first: a swipe waiting for the model goes before this call (see m_swipeGate).
                { QMutexLocker gate(&m_swipeGate); }
                QMutexLocker lock(&m_modelMutex);
                modelLoaded=m_model->ready();
                if (scoring) {
                    // WHY the same loaded model and reload rule as a next-word job: the swap list ranks words by the
                    // very sentence model the guess bar uses, and a language switch reloads it here first.
                    std::vector<std::string> asked;
                    for (const auto &w : job.scoreWords) asked.push_back(w.toStdString());
                    for (float v : m_model->scoreWords(context, asked)) scores << v;
                } else {
                    for (const auto &word : m_model->predict(context, job.topK, job.prefix.toStdString()))
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

swipekb::ContextRescore swipeContextRescore(NextWordWorker *worker, const PredictionJob &like, const QString &language,
                                            std::shared_ptr<SwipeRescoreStats> stats)
{
    // WHY off without the verified neural file: the weights were tuned on its score; the n-gram alone is already the
    // swipe's own prior ("if the neural file is missing, swiping behaves exactly as today").
    if (!worker || like.nwp.isEmpty() || like.model.isEmpty() || like.vocab.isEmpty()) return {};
    auto r = swipekb::ContextRescore::forLanguage(language.toStdString());
    // WHY only the files and checksums are kept: the scorer runs on every swipe and must not hold anything else.
    PredictionJob files;
    files.model = like.model;
    files.vocab = like.vocab;
    files.nwp = like.nwp;
    files.modelIdentity = like.modelIdentity;
    r.scorer = [worker, files, stats](const std::vector<std::string> &context, const std::vector<std::string> &words) {
        double waited = 0;
        auto scores = worker->scoreForSwipe(files, context, words, kSwipeRescoreMaxWaitMs, &waited);
        if (stats) {
            ++stats->asked;
            if (!scores.empty()) ++stats->scored;
            stats->lastWaitMs = waited;
            stats->maxWaitMs = std::max(stats->maxWaitMs, waited);
        }
        return scores;
    };
    return r;
}
