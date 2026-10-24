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
// Thread-confined scorer. Construct/init/predict/destroy on its owning worker.
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
    // prefix: letters already typed of the current word (completion); empty = next word. Only n-gram packs
    // ("SWKBNG2") load; init() refuses any other file.
    std::vector<std::string> predict(const std::vector<std::string> &context, int topK, const std::string &prefix = {});
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
