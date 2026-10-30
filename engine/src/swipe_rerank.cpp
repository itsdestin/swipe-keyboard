// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// SwipeReranker (see swipe_rerank.hpp). Written for this project; no code from other keyboard engines.
#include "swipekb/swipe_rerank.hpp"

#include <cmath>
#include <fstream>
#include <map>
#include <utility>
#include <sstream>

namespace swipekb {

namespace {
// The weights compiled into the engine, per language (the training script's output, unchanged). An entry "*" is used
// for every language without its own. Empty = no built-in weights (the fixed ContextRescore weights apply).
#include "swipe_rerank_weights.inc"
}

const char *rerankFeatureName(int feature)
{
    static const char *const names[kRrCount] = {"score", "path", "prior", "geo", "neural", "first", "rank_log", "freq",
                                                "letters", "length_fit", "start", "end", "apostrophe", "first_gap",
                                                "path_rank", "neural_rank", "swipe_length"};
    return feature >= 0 && feature < kRrCount ? names[feature] : "";
}

bool SwipeReranker::parse(const std::string &text)
{
    ready_ = false;
    sets_ = {};
    std::istringstream in(text);
    std::string tok;
    int version = 0, features = 0;
    if (!(in >> tok) || tok != "swipekb-rerank" || !(in >> version) || version != 1) return false;
    if (!(in >> tok) || tok != "features" || !(in >> features) || features != kRrCount) return false;
    // WHY every number is checked: a damaged file must switch the model off, never put NaN into a swipe's ranking.
    const auto readFloats = [&](float *out, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            if (!(in >> out[i]) || !std::isfinite(out[i])) return false;
        }
        return true;
    };
    while (in >> tok) {
        if (tok == "end") break;
        if (tok != "set") return false;
        std::string name, h;
        int hidden = -1;
        if (!(in >> name >> h >> hidden) || h != "hidden" || hidden < 0 || hidden > 64) return false;
        const int idx = name == "sentence" ? WithSentence : name == "nosentence" ? NoSentence : -1;
        if (idx < 0) return false;
        Model &m = sets_[idx];
        m.hidden = hidden;
        m.w1.assign(size_t(hidden) * kRrCount, 0.f);
        m.b1.assign(size_t(hidden), 0.f);
        m.out.assign(size_t(hidden), 0.f);
        const auto field = [&](const char *want, float *out, size_t n) { return (in >> tok) && tok == want && readFloats(out, n); };
        if (!field("mean", m.mean.data(), kRrCount) || !field("scale", m.scale.data(), kRrCount) ||
            !field("lin", m.lin.data(), kRrCount))
            return false;
        if (hidden > 0 && (!field("w1", m.w1.data(), m.w1.size()) || !field("b1", m.b1.data(), m.b1.size()) ||
                           !field("out", m.out.data(), m.out.size())))
            return false;
        for (float s : m.scale) if (!(s > 0.f)) return false;
        m.present = true;
    }
    ready_ = sets_[WithSentence].present || sets_[NoSentence].present;
    return ready_;
}

bool SwipeReranker::load(const std::string &path)
{
    std::ifstream f(path);
    if (!f) { ready_ = false; return false; }
    std::ostringstream s;
    s << f.rdbuf();
    return parse(s.str());
}

float SwipeReranker::score(Set set, const RerankRow &f) const
{
    const Model &m = sets_[set];
    std::array<float, kRrCount> z{};
    float s = 0.f;
    for (int i = 0; i < kRrCount; ++i) {
        // WHY a non-finite value counts as the mean (z = 0): that feature then says nothing either way.
        z[size_t(i)] = std::isfinite(f[size_t(i)]) ? (f[size_t(i)] - m.mean[size_t(i)]) / m.scale[size_t(i)] : 0.f;
        s += m.lin[size_t(i)] * z[size_t(i)];
    }
    for (int j = 0; j < m.hidden; ++j) {
        float a = m.b1[size_t(j)];
        const float *row = m.w1.data() + size_t(j) * kRrCount;
        for (int i = 0; i < kRrCount; ++i) a += row[i] * z[size_t(i)];
        if (a > 0.f) s += m.out[size_t(j)] * a;
    }
    return s;
}

std::shared_ptr<const SwipeReranker> SwipeReranker::builtIn(const std::string &language)
{
    // WHY parsed once per language and shared: every engine of that language uses the same read-only weights.
    static const auto table = [] {
        std::map<std::string, std::shared_ptr<const SwipeReranker>> t;
        for (const auto &[lang, text] : kBuiltInRerankers) {
            auto r = std::make_shared<SwipeReranker>();
            if (r->parse(text)) t[lang] = std::move(r);
        }
        return t;
    }();
    const std::string base = language.substr(0, 2);
    if (const auto it = table.find(base); it != table.end()) return it->second;
    if (const auto it = table.find("*"); it != table.end()) return it->second;
    return nullptr;
}

} // namespace swipekb
