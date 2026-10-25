/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "nextwordsession.h"
#include <QThread>
#include <QVector>
#include <memory>
#include "swipekb/spellings.hpp"
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
    // How many words a next-word or completion job answers with.
    int topK = 12;
    // Tap-to-swap scoring (one brain): non-empty makes this a "score these words" job. The worker returns each
    // word's next-word score after `context` (log10, same order) on wordScoresReady, never a word list. WHY on the
    // worker: the model is loaded and used only on this thread, so the swap list borrows it rather than a copy.
    QStringList scoreWords;
    // The neural next-word file (LANG.nwp.bin) that the profile verified against the lock; empty = n-gram only.
    // WHY explicit: the engine must never pick up an unverified file that merely sits beside the n-gram.
    QString nwp;
    // Words shown with a capital (swipekb::Capitals); nullptr = shown as the model spells them (German).
    // WHY here: the next-word files spell a word the way web text most often does ("Banco", "Hall"); the same list
    // as the swipe engine keeps ordinary words lower case and gives "I", "Paris" their capital.
    std::shared_ptr<const swipekb::Capitals> capitals;
};
// Serial, thread-confined inference with one latest pending request. No GUI-side
// inference lock or synchronous profile drain; a changed identity reloads on run().
class NextWordWorker : public QThread {
    Q_OBJECT
public:
    using Inference = std::function<QStringList(const PredictionJob &)>;
    // Test hook for scoring jobs, like Inference for word lists; empty = the real model.
    using Scorer = std::function<QVector<float>(const PredictionJob &)>;
    explicit NextWordWorker(Inference inference = {}, Scorer scorer = {}, QObject *parent = nullptr);
    ~NextWordWorker() override;
    void submit(PredictionJob);
    void invalidate();
    void requestStop();
    void stop();
signals:
    void resultReady(PredictionStamp, QStringList);
    void completionReady(PredictionStamp, QString prefix, QStringList);
    // Answer to a scoreWords job: the words asked about and one score each (empty scores when no model is loaded).
    void wordScoresReady(PredictionStamp, QStringList words, QVector<float> scores);
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
    Scorer m_scorer;
};
