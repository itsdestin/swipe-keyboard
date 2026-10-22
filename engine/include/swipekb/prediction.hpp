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
    bool init(const std::string &model, const std::string &vocab);
    bool ready() const;
    // prefix: letters already typed of the current word (completion); empty = next word. Only the n-gram
    // packs complete; the English neural model returns nothing for a prefix, keeping dictionary completion.
    std::vector<std::string> predict(const std::vector<std::string> &context, int topK, const std::string &prefix = {});
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
