#include "swipekb/prediction.hpp"
#include "swipe_decoder/context_lm.hpp"
#include "swipekb/ngram_predictor.hpp"
#include <algorithm>
#include <fstream>

namespace swipekb {
// WHY the n-gram path and filterPredictedWords live in ngram_predictor.cpp: the offline scorer
// runs that exact code without FUTO's library; only the English ExecuTorch path stays here.
struct Predictor::Impl {
    swipe_decoder::ContextLMScorer neural;
    NgramPredictor ngram;
    bool useNgram = false;
};
Predictor::Predictor() = default;
Predictor::~Predictor() = default;
bool Predictor::ready() const { return m_impl && (m_impl->useNgram ? m_impl->ngram.ready() : m_impl->neural.is_loaded()); }
bool Predictor::init(const std::string &model, const std::string &vocab)
{
    m_impl.reset();
    if (model.empty() || vocab.empty() || !std::ifstream(model).good() || !std::ifstream(vocab).good()) return false;
    try {
        auto impl = std::make_unique<Impl>();
        std::ifstream probe(model, std::ios::binary);
        char magic[8]{}; probe.read(magic, sizeof magic);
        if (probe.gcount() == sizeof magic && std::string(magic, sizeof magic) == std::string("SWKBNG2\0", 8)) {
            if (!impl->ngram.load(model, vocab)) return false;
            impl->useNgram = true;
        } else {
            // WHY preserve the existing English ExecuTorch path byte-for-byte for its established model.
            // WHY zero: do not reset ExecuTorch's global threadpool while swipe inference is active.
            if (!impl->neural.load(model, vocab, 0)) return false;
        }
        m_impl = std::move(impl); return true;
    } catch (...) { return false; }
}
std::vector<std::string> Predictor::predict(const std::vector<std::string> &context, int topK, const std::string &prefix)
{
    if (!ready() || topK <= 0) return {};
    try {
        topK = std::min(topK, 64);
        if (m_impl->useNgram) return m_impl->ngram.predict(context, topK, prefix);
        if (!prefix.empty()) return {};
        return filterPredictedWords(m_impl->neural.predict_topk(boundedContext(context), std::min(128, topK * 4)), topK);
    } catch (...) { return {}; }
}
}
