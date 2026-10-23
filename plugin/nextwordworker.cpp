#include "nextwordworker.h"
#include "swipekb/prediction.hpp"

NextWordWorker::NextWordWorker(Inference inference, QObject *parent)
    : QThread(parent), m_inference(std::move(inference))
{
    qRegisterMetaType<PredictionStamp>();
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
        bool modelLoaded=false;
        try {
            if (m_inference) { words = m_inference(job); modelLoaded=true; }
            else {
                const auto identity = job.modelIdentity + '\n' + job.model + '\n' + job.vocab + '\n' + job.nwp;
                if (loadedIdentity != identity || !predictor.ready()) {
                    predictor.init(job.model.toStdString(), job.vocab.toStdString(), job.nwp.toStdString(), false);
                    loadedIdentity = identity;
                }
                modelLoaded=predictor.ready();
                std::vector<std::string> context;
                for (const auto &word : job.context) context.push_back(word.toStdString());
                for (const auto &word : predictor.predict(context, job.topK, job.prefix.toStdString()))
                    words << QString::fromStdString(word);
            }
        } catch (...) { words.clear(); }
        {
            QMutexLocker lock(&m_mutex);
            if (!m_stopping && serial == m_serial) {
                // WHY no model path/context in status: only the current profile's
                // loaded bit may cross into support diagnostics.
                emit modelStateChanged(job.stamp.profileEpoch,modelLoaded);
                if (job.alternatives) emit alternativesReady(job.stamp, words);
                else if (job.prefix.isEmpty()) emit resultReady(job.stamp, words);
                else emit completionReady(job.stamp, job.prefix, words);
            }
        }
        // WHY completion follows delivery even for revoked work: callers can
        // observe asynchronous drain without blocking the GUI or exposing words.
        emit requestSettled();
    }
}
