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
    return true;
}

std::vector<std::string> NgramPredictor::predict(const std::vector<std::string> &context, int topK) const
{
    if (!ready() || topK <= 0) return {};
    topK = std::min(topK, 64);
    std::vector<std::string> history;
    for (const auto &word : boundedContext(context))
        history.push_back(QString::fromUtf8(word.data(), int(word.size())).toLower().toUtf8().toStdString());
    const auto state = ngram_.context(history);
    std::vector<std::pair<std::string, float>> ranked;
    ranked.reserve(vocabulary_.size());
    for (const auto &word : vocabulary_)
        ranked.emplace_back(word, ngram_.logProb(state, ngram_.alphaForm(word), 0));
    std::partial_sort(ranked.begin(), ranked.begin() + std::min<size_t>(ranked.size(), size_t(topK) * 4), ranked.end(),
        [](const auto &a, const auto &b) { return a.second == b.second ? a.first < b.first : a.second > b.second; });
    if (ranked.size() > size_t(topK) * 4) ranked.resize(size_t(topK) * 4);
    return filterPredictedWords(ranked, topK);
}
}
