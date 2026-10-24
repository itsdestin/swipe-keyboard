// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

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
    byUnigram_.clear(); unknownBySpelling_.clear(); indicesOfId_.clear();
    for (uint32_t i = 0; i < vocabulary_.size(); ++i) {
        if (wordIds_[i] == NgramLM::kUnk) unknownBySpelling_.push_back(i);
        else { byUnigram_.push_back(i); indicesOfId_[wordIds_[i]].push_back(i); }
    }
    std::sort(byUnigram_.begin(), byUnigram_.end(), [this](uint32_t a, uint32_t b) {
        const float x = ngram_.unigramLogProb(wordIds_[a]), y = ngram_.unigramLogProb(wordIds_[b]);
        return x == y ? vocabulary_[a] < vocabulary_[b] : x > y;
    });
    std::sort(unknownBySpelling_.begin(), unknownBySpelling_.end(),
              [this](uint32_t a, uint32_t b) { return vocabulary_[a] < vocabulary_[b]; });
    return true;
}

float NgramPredictor::score(const NgramLM::Context &state, uint32_t i) const
{
    return wordIds_[i] == NgramLM::kUnk ? ngram_.logProb(state, alpha_[i], 0) : ngram_.logProb(state, wordIds_[i]);
}

std::vector<std::string> NgramPredictor::predict(const std::vector<std::string> &context, int topK,
                                                 const std::string &prefix) const
{
    if (!ready() || topK <= 0) return {};
    topK = std::min(topK, 64);
    // WHY want = topK*4: filterPredictedWords drops duplicates/odd tokens, so a few spares are ranked.
    const auto ranked = rank(context, size_t(topK) * 4, prefix);
    std::vector<std::pair<std::string, float>> shown;
    shown.reserve(ranked.top.size());
    for (const auto &[lp, index] : ranked.top) shown.emplace_back(vocabulary_[index], lp);
    return filterPredictedWords(shown, topK);
}

NgramPredictor::Ranked NgramPredictor::rank(const std::vector<std::string> &context, size_t want,
                                            const std::string &prefix) const
{
    Ranked result;
    if (!ready() || want == 0) return result;
    // WHY letters-only: "l'" or "Stra" must match "l'homme"/"straße" the way the model spells words.
    const std::string typed = prefix.empty() ? std::string() : NgramLM::alphaForm(prefix);
    if (!prefix.empty() && typed.empty()) return result;
    want = std::min<size_t>(want, 256);
    std::vector<std::string> history;
    for (const auto &word : boundedContext(context))
        history.push_back(QString::fromUtf8(word.data(), int(word.size())).toLower().toUtf8().toStdString());
    const auto state = ngram_.context(history);
    result.state = state;
    // WHY (score, index) pairs: sorting numbers instead of copying 100k strings per request; ties still
    // break on the spelling, exactly as before.
    std::vector<std::pair<float, uint32_t>> scored;
    const auto score = [&](uint32_t i) { return this->score(state, i); };
    bool shortcut = typed.empty();
    if (shortcut) {
        // WHY exact: a word never seen after this context scores max(floor, constant + its unigram), so the
        // best of them are the first in unigram order; unknown words all tie and sort by spelling. Scoring
        // the continuations plus those leaders gives the same top list as scoring every word.
        std::vector<uint32_t> ids;
        ngram_.continuations(state, ids);
        std::unordered_set<uint32_t> seen;
        for (uint32_t id : ids)
            if (const auto it = indicesOfId_.find(id); it != indicesOfId_.end())
                for (uint32_t i : it->second) if (seen.insert(i).second) scored.emplace_back(score(i), i);
        size_t others = 0;
        float lastOther = 0;
        for (uint32_t i : byUnigram_) {
            if (others == want) break;
            if (seen.count(i)) continue;
            scored.emplace_back(lastOther = score(i), i);
            ++others;
        }
        for (size_t k = 0; k < std::min(want, unknownBySpelling_.size()); ++k)
            scored.emplace_back(score(unknownBySpelling_[k]), unknownBySpelling_[k]);
        // WHY fall back: if the leaders already sit on the floor, many words tie there and spelling, not
        // frequency, decides between them; only a full pass reproduces that order.
        if (others == want && lastOther <= ngram_.floorLogProb()) { scored.clear(); shortcut = false; }
    }
    if (!shortcut) {
        scored.reserve(vocabulary_.size());
        for (uint32_t i = 0; i < vocabulary_.size(); ++i) {
            if (!typed.empty() && alpha_[i].compare(0, typed.size(), typed) != 0) continue;
            scored.emplace_back(score(i), i);
        }
    }
    const size_t keep = std::min<size_t>(scored.size(), want);
    std::partial_sort(scored.begin(), scored.begin() + keep, scored.end(), [this](const auto &a, const auto &b) {
        return a.first == b.first ? vocabulary_[a.second] < vocabulary_[b.second] : a.first > b.first;
    });
    result.top.assign(scored.begin(), scored.begin() + keep);
    return result;
}
}
