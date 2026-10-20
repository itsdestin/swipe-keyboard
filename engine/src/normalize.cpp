#include "swipekb/recognizer.hpp"

#include <algorithm>
#include <QString>
#include <QList>
#include <QChar>
#include <cmath>
#include <unordered_map>
#include <limits>

namespace swipekb {

std::optional<NormalizedInput> normalize(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &keys, const std::string &alphabet,
                                         const std::unordered_map<char32_t,char32_t> &popupAliases)
{
    if (trace.size() < 2 || trace.size()>2048 || keys.size()>64)
        return std::nullopt;
    const auto symbols=QString::fromStdString(alphabet).toUcs4();
    if (symbols.isEmpty() || symbols.size()>48) return std::nullopt;

    // Letter area = bounding box of all letter keys. FUTO's coordinates are relative to it, so the
    // number row, space bar etc. never stretch the space.
    float minX = std::numeric_limits<float>::max(), minY = minX;
    float maxX = std::numeric_limits<float>::lowest(), maxY = maxX;
    NormalizedInput out;
    out.letters = alphabet;
    out.key_cx.assign(symbols.size(), -1.f);
    out.key_cy.assign(symbols.size(), -1.f);
    std::vector<const KeyRect *> byLetter(symbols.size(), nullptr);
    std::unordered_map<char32_t,size_t> indices;
    for (qsizetype i=0;i<symbols.size();++i) {
        if (!QChar::isLetter(symbols[i]) || indices.contains(symbols[i])) return std::nullopt;
        indices[symbols[i]]=size_t(i);
    }
    for (const KeyRect &k : keys) {
        // WHY validate before arithmetic: NaN or duplicate geometry must not poison FUTO's spatial head.
        if (!std::isfinite(k.x) || !std::isfinite(k.y) || !std::isfinite(k.w) || !std::isfinite(k.h) ||
            k.w<=0 || k.h<=0 || !std::isfinite(k.x+k.w) || !std::isfinite(k.y+k.h)) return std::nullopt;
        const auto it=indices.find(QChar::toLower(k.letter));
        if (it==indices.end()) continue;
        if (byLetter[it->second]) return std::nullopt;
        byLetter[it->second] = &k;
        minX = std::min(minX, k.x);
        minY = std::min(minY, k.y);
        maxX = std::max(maxX, k.x + k.w);
        maxY = std::max(maxY, k.y + k.h);
    }
    // WHY aliases reuse only a measured declared base key; they never invent centers or move physical glyphs.
    for (const auto &[symbol,base]:popupAliases) {
        const auto a=indices.find(symbol), b=indices.find(base);
        if (a!=indices.end() && b!=indices.end() && !byLetter[a->second]) byLetter[a->second]=byLetter[b->second];
    }
    if (std::find(byLetter.begin(), byLetter.end(), nullptr) != byLetter.end())
        return std::nullopt;
    const float w = maxX - minX, h = maxY - minY;
    if (w <= 0 || h <= 0 || !std::isfinite(w) || !std::isfinite(h))
        return std::nullopt;

    for (size_t i = 0; i < byLetter.size(); ++i) {
        out.key_cx[i] = (byLetter[i]->x + byLetter[i]->w / 2 - minX) / w;
        out.key_cy[i] = (byLetter[i]->y + byLetter[i]->h / 2 - minY) / h;
    }
    out.x.reserve(trace.size());
    out.y.reserve(trace.size());
    out.t.reserve(trace.size());
    float previous=-1;
    for (const TracePoint &p : trace) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.t_ms) || p.t_ms<previous || p.t_ms<0 || p.t_ms>10000)
            return std::nullopt;
        previous=p.t_ms;
        const float x=(p.x-minX)/w, y=(p.y-minY)/h;
        // WHY finite inputs can still overflow when subtracting/dividing extreme coordinates.
        if (!std::isfinite(x) || !std::isfinite(y)) return std::nullopt;
        out.x.push_back(x);
        out.y.push_back(y);
        out.t.push_back(p.t_ms);
    }
    return out;
}

} // namespace swipekb
