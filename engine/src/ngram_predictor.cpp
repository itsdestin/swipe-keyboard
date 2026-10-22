#include "swipekb/ngram_predictor.hpp"
#include "swipekb/prediction.hpp"
#include <QChar>
#include <QString>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_set>

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

std::vector<std::string> boundedContext(const std::vector<std::string> &context)
{
    std::vector<std::string> bounded;
    for (size_t i = context.size() > 20 ? context.size() - 20 : 0; i < context.size(); ++i)
        bounded.push_back(context[i].substr(0, 256));
    return bounded;
}

bool NgramPredictor::load(const std::string &model, const std::string &vocab)
{
    vocabulary_.clear();
    if (!ngram_.load(model)) return false;
    std::ifstream words(vocab);
    std::string line;
    while (std::getline(words, line)) {
        const auto tab = line.find('\t');
        const auto secondTab=tab==std::string::npos ? std::string::npos : line.find('\t',tab+1);
        const auto word = line.substr(0, tab);
        const auto display = secondTab==std::string::npos ?
            (tab==std::string::npos ? word : line.substr(tab+1)) : line.substr(secondTab+1);
        if (!word.empty() && !display.empty()) vocabulary_.push_back(display);
    }
    if (!words.eof() || vocabulary_.empty()) { vocabulary_.clear(); return false; }
    // WHY resolve every word's model number once here: predict() used to lower-case and look up all
    // ~100k words on every request (~100 ms each on a fast laptop). The letters-only form is kept for every
    // word: unknown words score from it, and completion matches typed letters against it.
    wordIds_.clear(); alpha_.clear();
    wordIds_.reserve(vocabulary_.size()); alpha_.reserve(vocabulary_.size());
    for (size_t i = 0; i < vocabulary_.size(); ++i) {
        alpha_.push_back(NgramLM::alphaForm(vocabulary_[i]));
        wordIds_.push_back(ngram_.id(alpha_.back()));
    }
    return true;
}

std::vector<std::string> NgramPredictor::predict(const std::vector<std::string> &context, int topK,
                                                 const std::string &prefix) const
{
    if (!ready() || topK <= 0) return {};
    // WHY letters-only: "l'" or "Stra" must match "l'homme"/"straße" the way the model spells words.
    const std::string typed = prefix.empty() ? std::string() : NgramLM::alphaForm(prefix);
    if (!prefix.empty() && typed.empty()) return {};
    topK = std::min(topK, 64);
    std::vector<std::string> history;
    for (const auto &word : boundedContext(context))
        history.push_back(QString::fromUtf8(word.data(), int(word.size())).toLower().toUtf8().toStdString());
    const auto state = ngram_.context(history);
    // WHY (score, index) pairs: sorting numbers instead of copying 100k strings per request; ties still
    // break on the spelling, exactly as before.
    std::vector<std::pair<float, uint32_t>> scored;
    scored.reserve(vocabulary_.size());
    for (uint32_t i = 0; i < vocabulary_.size(); ++i) {
        if (!typed.empty() && alpha_[i].compare(0, typed.size(), typed) != 0) continue;
        scored.emplace_back(wordIds_[i] == NgramLM::kUnk ? ngram_.logProb(state, alpha_[i], 0)
                                                        : ngram_.logProb(state, wordIds_[i]), i);
    }
    const size_t keep = std::min<size_t>(scored.size(), size_t(topK) * 4);
    std::partial_sort(scored.begin(), scored.begin() + keep, scored.end(), [this](const auto &a, const auto &b) {
        return a.first == b.first ? vocabulary_[a.second] < vocabulary_[b.second] : a.first > b.first;
    });
    std::vector<std::pair<std::string, float>> ranked;
    ranked.reserve(keep);
    for (size_t i = 0; i < keep; ++i) ranked.emplace_back(vocabulary_[scored[i].second], scored[i].first);
    return filterPredictedWords(ranked, topK);
}
}
