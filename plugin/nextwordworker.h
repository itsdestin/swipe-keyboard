#pragma once
#include "nextwordsession.h"
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <functional>

Q_DECLARE_METATYPE(PredictionStamp)
struct PredictionJob {
    PredictionStamp stamp;
    QString model, vocab;
    QStringList context;
    QString modelIdentity;
    // Letters typed so far of the current word: non-empty makes this a completion job, answered by
    // completionReady instead of resultReady.
    QString prefix;
};
// Serial, thread-confined inference with one latest pending request. No GUI-side
// inference lock or synchronous profile drain; a changed identity reloads on run().
class NextWordWorker : public QThread {
    Q_OBJECT
public:
    using Inference = std::function<QStringList(const PredictionJob &)>;
    explicit NextWordWorker(Inference inference = {}, QObject *parent = nullptr);
    ~NextWordWorker() override;
    void submit(PredictionJob);
    void invalidate();
    void requestStop();
    void stop();
signals:
    void resultReady(PredictionStamp, QStringList);
    void completionReady(PredictionStamp, QString prefix, QStringList);
    void requestSettled();
    void modelStateChanged(quint64 profileEpoch, bool loaded);
protected:
    void run() override;
private:
    QMutex m_mutex;
    QWaitCondition m_wake;
    std::optional<PredictionJob> m_pending;
    quint64 m_serial = 0;
    bool m_stopping = false;
    Inference m_inference;
};
