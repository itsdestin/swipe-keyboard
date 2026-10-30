// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#pragma once
#include <memory>
#include <string>
#include <vector>
#include <utility>
namespace swipekb {
std::vector<std::string> filterPredictedWords(const std::vector<std::pair<std::string, float>> &, int topK);
// Not thread-safe: one thread at a time (the plugin's next-word worker owns it; the swipe borrows it under the worker's
// lock, NextWordWorker::scoreForSwipe).
class Predictor {
public:
    Predictor();
    ~Predictor();
    Predictor(const Predictor &) = delete;
    Predictor &operator=(const Predictor &) = delete;
    // `nwp`: the verified neural next-word file for an n-gram pack. Default (both args omitted) keeps the file next
    // to the n-gram if there is one; the keyboard passes siblingFallback=false so only a lock-verified file is used.
    bool init(const std::string &model, const std::string &vocab, const std::string &nwp = {}, bool siblingFallback = true);
    bool ready() const;
    // True when the verified neural next-word file is loaded and used (not only the n-gram). WHY: the swipe re-scoring
    // (ContextRescore) was tuned on the neural score; with the n-gram alone the swipe keeps its own n-gram prior.
    bool hasNeural() const;
    // prefix: letters already typed of the current word (completion); empty = next word. Only n-gram packs
    // ("SWKBNG2") load; init() refuses any other file.
    std::vector<std::string> predict(const std::vector<std::string> &context, int topK, const std::string &prefix = {});
    // log10 score of each word as the next word after `context`: the same blend predict() ranks by (neural model and
    // n-gram). Words the vocabulary lacks get the n-gram floor. Same length and order as `words`. Empty when not ready.
    // WHY: tap-to-swap ranks given alternatives, so it needs their scores, not a top list.
    // `markUnknown`: a word the vocabulary lacks gets NaN ("no opinion") instead of the floor; the swipe re-scoring
    // (ContextRescore) then keeps that word's own n-gram prior, so a learned word is not pushed down.
    std::vector<float> scoreWords(const std::vector<std::string> &context, const std::vector<std::string> &words,
                                  bool markUnknown = false);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
