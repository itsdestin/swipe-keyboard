#include "swipekb/prediction.hpp"
#include "swipe_decoder/context_lm.hpp"
#include "swipekb/ngram.hpp"
#include <QFile>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_set>
#include <QString>
#include <QChar>

namespace swipekb {
std::vector<std::string> filterPredictedWords(const std::vector<std::pair<std::string, float>> &raw, int topK)
{
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto &[word, score] : raw) {
        if (topK <= 0 || word.empty() || word.size() > 64 || !std::isfinite(score)) continue;
        // WHY Unicode categories rather than ASCII bytes: Spanish, French and German
        // words must remain intact, while punctuation/control tokens stay excluded.
        const auto text = QString::fromUtf8(word.data(), int(word.size()));
        if (text.toUtf8().toStdString() != word || text.isEmpty()) continue;
        bool valid = true;
        bool previousLetter = false;
        for (qsizetype i = 0; i < text.size(); ++i) {
            const QChar c = text[i];
            const bool letter = c.isLetter();
            const bool mark = c.category() == QChar::Mark_NonSpacing || c.category() == QChar::Mark_SpacingCombining || c.category() == QChar::Mark_Enclosing;
            const bool apostrophe = c == QLatin1Char('\'') || c == QChar(0x2019);
            const bool hyphen = c == QLatin1Char('-');
            if (letter) { previousLetter = true; continue; }
            if (mark && previousLetter) continue;
            const bool nextLetter = i + 1 < text.size() && text[i + 1].isLetter();
            if ((apostrophe || hyphen) && previousLetter && nextLetter) { previousLetter = false; continue; }
            valid = false; break;
        }
        if (!valid || !text.front().isLetter() || !text.back().isLetter()) continue;
        const std::string lower = text.toLower().toUtf8().toStdString();
        if (!seen.insert(lower).second) continue;
        out.push_back(word);
        if (int(out.size()) == std::min(topK, 64)) break;
    }
    return out;
}

struct Predictor::Impl {
    swipe_decoder::ContextLMScorer neural;
    NgramLM ngram;
    std::vector<std::string> vocabulary;
    bool useNgram = false;
};
Predictor::Predictor() = default;
Predictor::~Predictor() = default;
bool Predictor::ready() const { return m_impl && (m_impl->useNgram ? m_impl->ngram.loaded() : m_impl->neural.is_loaded()); }
bool Predictor::init(const std::string &model, const std::string &vocab)
{
    m_impl.reset();
    if (model.empty() || vocab.empty() || !std::ifstream(model).good() || !std::ifstream(vocab).good()) return false;
    try {
        auto impl = std::make_unique<Impl>();
        std::ifstream probe(model, std::ios::binary);
        char magic[8]{}; probe.read(magic, sizeof magic);
        if (probe.gcount() == sizeof magic && std::string(magic, sizeof magic) == std::string("SWKBNG2\0", 8)) {
            if (!impl->ngram.load(model)) return false;
            std::ifstream words(vocab);
            std::string line;
            while (std::getline(words, line)) {
                const auto tab = line.find('\t');
                const auto secondTab=tab==std::string::npos ? std::string::npos : line.find('\t',tab+1);
                const auto word = line.substr(0, tab);
                const auto display = secondTab==std::string::npos ?
                    (tab==std::string::npos ? word : line.substr(tab+1)) : line.substr(secondTab+1);
                if (!word.empty() && !display.empty()) impl->vocabulary.push_back(display);
            }
            if (!words.eof() || impl->vocabulary.empty()) return false;
            impl->useNgram = true;
        } else {
            // WHY preserve the existing English ExecuTorch path byte-for-byte for its established model.
            // WHY zero: do not reset ExecuTorch's global threadpool while swipe inference is active.
            if (!impl->neural.load(model, vocab, 0)) return false;
        }
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
        if (m_impl->useNgram) {
            std::vector<std::string> history;
            history.reserve(bounded.size());
            for (const auto &word : bounded)
                history.push_back(QString::fromUtf8(word.data(), int(word.size())).toLower().toUtf8().toStdString());
            const auto state = m_impl->ngram.context(history);
            std::vector<std::pair<std::string, float>> ranked;
            ranked.reserve(m_impl->vocabulary.size());
            for (const auto &word : m_impl->vocabulary)
                ranked.emplace_back(word, m_impl->ngram.logProb(state, m_impl->ngram.alphaForm(word), 0));
            std::partial_sort(ranked.begin(), ranked.begin() + std::min<size_t>(ranked.size(), size_t(topK) * 4), ranked.end(),
                [](const auto &a, const auto &b) { return a.second == b.second ? a.first < b.first : a.second > b.second; });
            if (ranked.size() > size_t(topK) * 4) ranked.resize(size_t(topK) * 4);
            return filterPredictedWords(ranked, topK);
        }
        return filterPredictedWords(m_impl->neural.predict_topk(bounded, std::min(128, topK * 4)), topK);
    } catch (...) { return {}; }
}
}
