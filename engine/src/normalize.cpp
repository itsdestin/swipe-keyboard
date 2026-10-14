#include "swipekb/recognizer.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace swipekb {

std::optional<NormalizedInput> normalize(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &keys)
{
    if (trace.size() < 2)
        return std::nullopt;

    // Letter area = bounding box of all letter keys. FUTO's coordinates are relative to it, so the
    // number row, space bar etc. never stretch the space.
    float minX = std::numeric_limits<float>::max(), minY = minX;
    float maxX = std::numeric_limits<float>::lowest(), maxY = maxX;
    NormalizedInput out;
    out.letters = "abcdefghijklmnopqrstuvwxyz";
    out.key_cx.assign(26, -1.f);
    out.key_cy.assign(26, -1.f);
    std::vector<const KeyRect *> byLetter(26, nullptr);
    for (const KeyRect &k : keys) {
        const int c = std::tolower(static_cast<unsigned char>(k.letter));
        if (c < 'a' || c > 'z')
            continue;
        byLetter[c - 'a'] = &k;
        minX = std::min(minX, k.x);
        minY = std::min(minY, k.y);
        maxX = std::max(maxX, k.x + k.w);
        maxY = std::max(maxY, k.y + k.h);
    }
    if (std::find(byLetter.begin(), byLetter.end(), nullptr) != byLetter.end())
        return std::nullopt;
    const float w = maxX - minX, h = maxY - minY;
    if (w <= 0 || h <= 0)
        return std::nullopt;

    for (int i = 0; i < 26; ++i) {
        out.key_cx[i] = (byLetter[i]->x + byLetter[i]->w / 2 - minX) / w;
        out.key_cy[i] = (byLetter[i]->y + byLetter[i]->h / 2 - minY) / h;
    }
    out.x.reserve(trace.size());
    out.y.reserve(trace.size());
    out.t.reserve(trace.size());
    for (const TracePoint &p : trace) {
        out.x.push_back((p.x - minX) / w);
        out.y.push_back((p.y - minY) / h);
        out.t.push_back(p.t_ms);
    }
    return out;
}

} // namespace swipekb
