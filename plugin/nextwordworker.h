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
#include <string>
#include <vector>

#include "swipekb/language_engine.hpp"

namespace swipekb { class Predictor; }

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
// Serial inference with one latest pending request; a changed identity reloads on run(). The loaded model is used by
// this thread and, for one quick call per swipe, by the input thread (scoreForSwipe), under a lock.
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
    // The neural re-scoring of a swipe (joint-model step 2, LanguageEngine::setContextRescore): the score of each of
    // `words` as the next word after `context`, from the model this worker has loaded, asked from the CALLING thread
    // (the swipe, on the input thread) and answered at once. Empty - the swipe then ranks exactly as without it - when
    // the loaded model is not the one `like` names (another language, or not loaded yet), has no neural part, or is
    // busy for longer than `maxWaitMs` (a load never makes it wait: models are loaded outside the lock).
    // `waitedMs` (optional) gets how long the call waited for the worker.
    // WHY the worker's own model under a lock and not a second copy on the input thread: a second next-word model
    // costs ~51 MB for English (step 1); sharing costs nothing, and the worker holds the lock only for one next-word,
    // completion or score call, a few milliseconds (measured in the step 2 report). WHY not a queued job: the swipe's
    // word is typed at once, it cannot wait for an answer that comes back later through the event loop.
    std::vector<float> scoreForSwipe(const PredictionJob &like, const std::vector<std::string> &context,
                                     const std::vector<std::string> &words, int maxWaitMs, double *waitedMs = nullptr);
    // The model files and checksums a job names, as one string: the same string means the same loaded model.
    static QString identityOf(const PredictionJob &job);
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
    // WHY its own lock, apart from m_mutex: the swipe waits only for a running model call, never for job queueing.
    // Created, loaded and destroyed on the worker thread; read by scoreForSwipe only while m_modelMutex is held.
    QMutex m_modelMutex;
    // Held by a swipe while it waits for and uses the model; the worker passes through it before each model call.
    // WHY: QMutex is not fair, so a busy worker could take the model again and again ahead of a waiting swipe; with
    // the gate a swipe waits for at most the one call already running.
    QMutex m_swipeGate;
    std::unique_ptr<swipekb::Predictor> m_model;
    QString m_modelIdentity;
};

// How the swipe re-scoring went, for diagnostics and tests (counts and times only, never words).
struct SwipeRescoreStats {
    quint64 asked = 0;      // swipes with words before that asked the worker's model
    quint64 scored = 0;     // ... and got its scores (the others kept the n-gram ranking)
    double lastWaitMs = 0;  // how long the last one waited for the worker
    double maxWaitMs = 0;   // the longest wait so far
};
// The longest a swipe waits for the worker's running model call before it keeps the n-gram ranking.
// WHY 5 ms: a swipe takes ~5 ms (step 1 report), the re-scoring ~1 ms, so even a full wait keeps the whole swipe under
// the 15 ms budget; the worker's longest call measured on this laptop is in the step 2 report.
inline constexpr int kSwipeRescoreMaxWaitMs = 5;
// The neural re-scoring of swipes for one language engine (LanguageEngine::setContextRescore), shared by both Plasma
// front ends: the weights tuned for `language` (ContextRescore::forLanguage), scored by `worker`'s model through
// scoreForSwipe. `like` names the profile's verified next-word files exactly as the front end's next-word jobs do.
// Off (an empty ContextRescore) when the profile has no verified neural file: swiping then ranks as before.
// `worker` must outlive the engine (both front ends own the engine and the worker and destroy the worker first, after
// the last swipe).
swipekb::ContextRescore swipeContextRescore(NextWordWorker *worker, const PredictionJob &like, const QString &language,
                                            std::shared_ptr<SwipeRescoreStats> stats = {});
