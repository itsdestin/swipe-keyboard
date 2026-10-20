#include "swipekb/prediction.hpp"
#include "swipe_decoder/context_lm.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_set>

namespace swipekb {
std::vector<std::string> filterPredictedWords(const std::vector<std::pair<std::string, float>> &raw, int topK)
{
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    const auto letter = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    for (const auto &[word, score] : raw) {
        if (topK <= 0 || word.empty() || word.size() > 64 || !std::isfinite(score)) continue;
        // WHY English-only lexical policy: control/punctuation/numeric tokens must
        // not consume the candidate budget before the UI gets a chance to filter.
        bool valid = letter(word.front()) && letter(word.back());
        std::string folded;
        for (size_t i = 0; i < word.size(); ++i) {
            const unsigned char c = word[i];
            valid = valid && (letter(c) || ((c == '\'' || c == '-') && i > 0 && i + 1 < word.size()
                && letter(word[i - 1]) && letter(word[i + 1])));
            folded.push_back(c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : char(c));
        }
        if (!valid || !seen.insert(folded).second) continue;
        out.push_back(word);
        if (int(out.size()) == std::min(topK, 64)) break;
    }
    return out;
}

struct Predictor::Impl { swipe_decoder::ContextLMScorer scorer; };
Predictor::Predictor() = default;
Predictor::~Predictor() = default;
bool Predictor::ready() const { return m_impl && m_impl->scorer.is_loaded(); }
bool Predictor::init(const std::string &model, const std::string &vocab)
{
    m_impl.reset();
    if (model.empty() || vocab.empty() || !std::ifstream(model).good() || !std::ifstream(vocab).good()) return false;
    try {
        auto impl = std::make_unique<Impl>();
        // WHY zero: do not reset ExecuTorch's global threadpool while swipe inference is active.
        if (!impl->scorer.load(model, vocab, 0)) return false;
        m_impl = std::move(impl); return true;
    } catch (...) { return false; }
}
std::vector<std::string> Predictor::predict(const std::vector<std::string> &context, int topK)
{
    if (!ready() || topK <= 0) return {};
    try {
        std::vector<std::string> bounded;
        for (size_t i = context.size() > 20 ? context.size() - 20 : 0; i < context.size(); ++i)
            bounded.push_back(context[i].substr(0, 256));
        topK = std::min(topK, 64);
        return filterPredictedWords(m_impl->scorer.predict_topk(bounded, std::min(128, topK * 4)), topK);
    } catch (...) { return {}; }
}
}
