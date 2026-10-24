#include "swipekb/prediction.hpp"
#include "swipekb/nwp_predictor.hpp"
#include <algorithm>
#include <fstream>

namespace swipekb {
// WHY only the project's own n-gram pack ("SWKBNG2", plus LANG.nwp.bin when it sits beside it): it predicts the next
// word and completes half-typed words for every language. Any other file is refused, so a stale or foreign model
// can never be loaded; the keyboard then simply shows no next-word suggestions.
struct Predictor::Impl {
    NwpPredictor ngram;
};
Predictor::Predictor() = default;
Predictor::~Predictor() = default;
bool Predictor::ready() const { return m_impl && m_impl->ngram.ready(); }
bool Predictor::init(const std::string &model, const std::string &vocab, const std::string &nwp, bool siblingFallback)
{
    m_impl.reset();
    if (model.empty() || vocab.empty() || !std::ifstream(model).good() || !std::ifstream(vocab).good()) return false;
    try {
        std::ifstream probe(model, std::ios::binary);
        char magic[8]{}; probe.read(magic, sizeof magic);
        if (probe.gcount() != sizeof magic || std::string(magic, sizeof magic) != std::string("SWKBNG2\0", 8)) return false;
        auto impl = std::make_unique<Impl>();
        if (!impl->ngram.load(model, vocab, nwp, siblingFallback)) return false;
        m_impl = std::move(impl); return true;
    } catch (...) { return false; }
}
std::vector<std::string> Predictor::predict(const std::vector<std::string> &context, int topK, const std::string &prefix)
{
    if (!ready() || topK <= 0) return {};
    try {
        return m_impl->ngram.predict(context, std::min(topK, 64), prefix);
    } catch (...) { return {}; }
}
}
