// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include "swipekb/ngram.hpp"
#include <QString>
#include <QChar>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace swipekb {

namespace {

// File layout (little-endian, every section starts on an 8-byte boundary; V words, N2 bigrams, N3 trigrams):
//   Header | word list ("word\n" for ids 3..) | float uni_lp[V], uni_bo[V] | uint32 bi_off[V+1]
//   | uint32 bi_w[N2] | int16 bi_lp[N2] | int16 bi_bo[N2] | uint32 bi_tri[N2+1] | uint32 tri_w[N3] | int16 tri_lp[N3]
// Bigrams are grouped by first word v (bi_off[v]..bi_off[v+1]), sorted by second word; the trigrams that
// continue bigram i are tri_w[bi_tri[i]..bi_tri[i+1]], sorted by last word.
// WHY grouped instead of storing whole word triples: each entry then only names its last word, which makes
// the file ~40% smaller (6 bytes per trigram) for the same model.
// WHY 16-bit numbers for the scores: thousandths of a log10 step are far finer than anything that changes a
// ranking.
constexpr char kMagic[8] = {'S', 'W', 'K', 'B', 'N', 'G', '2', '\0'};
constexpr float kQuant = 1000.f;

struct Header {
    char magic[8];
    uint32_t version, vocab;   // vocab includes the 3 special words
    uint64_t n_bi, n_tri, vocab_bytes;
    float calib_a, calib_b, floor, pad;
};
static_assert(sizeof(Header) == 56);

uint64_t align8(uint64_t n) { return (n + 7) & ~uint64_t(7); }

int16_t quant(float x)
{
    const float q = std::round(x * kQuant);
    return int16_t(std::clamp(q, -32767.f, 32767.f));
}

// Position of `w` in a[lo, hi) (sorted), or hi if absent.
uint32_t find(const uint32_t *a, uint32_t lo, uint32_t hi, uint32_t w)
{
    const uint32_t *p = std::lower_bound(a + lo, a + hi, w);
    return (p != a + hi && *p == w) ? uint32_t(p - a) : hi;
}

// Section sizes, shared by load() and write() so the two can't disagree.
struct Sizes {
    uint64_t words, uni, off, bi, tri, total;
    Sizes(uint64_t V, uint64_t n2, uint64_t n3, uint64_t vocabBytes)
    {
        words = align8(sizeof(Header) + vocabBytes);
        uni = align8(V * 8);
        off = align8((V + 1) * 4);
        bi = align8(n2 * 8) + align8((n2 + 1) * 4);
        tri = align8(n3 * 6);
        total = words + uni + off + bi + tri;
    }
};

} // namespace

std::string NgramLM::alphaForm(std::string_view word)
{
    // WHY Qt's Unicode lowercase mapping and letter categories: accented words, ß and non-Latin
    // punctuation must normalize identically in training, typed context and dictionary candidates.
    const auto lower=QString::fromUtf8(word.data(),int(word.size())).toLower();
    QString alpha;
    alpha.reserve(lower.size());
    for (const QChar c:lower) if (c.isLetter()) alpha.append(c);
    return alpha.toUtf8().toStdString();
}

NgramLM::~NgramLM() { unload(); }

void NgramLM::unload()
{
    if (base_)
        munmap(const_cast<char *>(base_), size_);
    base_ = nullptr;
    size_ = 0;
    n_bi_ = n_tri_ = 0;
    vocab_ = 0;
    calib_a_ = calib_b_ = 0;
    floor_ = -9;
    ids_.clear();
}

bool NgramLM::load(const std::string &path)
{
    unload();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st{};
    if (fstat(fd, &st) != 0 || size_t(st.st_size) < sizeof(Header)) {
        ::close(fd);
        return false;
    }
    // WHY a read-only shared mapping: the file is never modified, the kernel pages in only what lookups touch,
    // and every process that maps it shares the same memory.
    void *m = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED)
        return false;
    base_ = static_cast<const char *>(m);
    size_ = size_t(st.st_size);
    // WHY random access: each swipe looks at a few scattered entries. Without this hint the kernel reads ahead
    // ~128 KB around every touched spot, which pulled most of the file into memory for no benefit.
    madvise(m, size_, MADV_RANDOM);

    Header h;
    std::memcpy(&h, base_, sizeof h);
    // WHY every size is checked against the file length: a truncated download or an older format must make
    // load() fail (keyboard falls back to its previous ranking), never read past the end of the mapping.
    const uint64_t V = h.vocab;
    if (std::memcmp(h.magic, kMagic, 8) != 0 || h.version != 2 || V < 3 || V > (1u << kIdBits) ||
        h.n_bi > (uint64_t(1) << 40) || h.n_tri > (uint64_t(1) << 40) || h.vocab_bytes > size_) {
        unload();
        return false;
    }
    const Sizes sz(V, h.n_bi, h.n_tri, h.vocab_bytes);
    if (h.n_bi >= UINT32_MAX || h.n_tri >= UINT32_MAX || sz.total > size_) {
        unload();
        return false;
    }
    vocab_ = uint32_t(V);
    n_bi_ = h.n_bi;
    n_tri_ = h.n_tri;
    calib_a_ = h.calib_a;
    calib_b_ = h.calib_b;
    floor_ = h.floor;

    // Word list: one word per line, word number = line index + 3.
    ids_.reserve(V);
    const char *p = base_ + sizeof h, *end = p + h.vocab_bytes;
    uint32_t next = 3;
    while (p < end && next < V) {
        const char *nl = static_cast<const char *>(std::memchr(p, '\n', size_t(end - p)));
        if (!nl) break;
        ids_.emplace(std::string_view(p, size_t(nl - p)), next++);
        p = nl + 1;
    }
    size_t off = sz.words;
    uni_lp_ = reinterpret_cast<const float *>(base_ + off);
    uni_bo_ = uni_lp_ + V;
    off += sz.uni;
    bi_off_ = reinterpret_cast<const uint32_t *>(base_ + off);
    off += sz.off;
    bi_w_ = reinterpret_cast<const uint32_t *>(base_ + off);
    bi_lp_ = reinterpret_cast<const int16_t *>(bi_w_ + n_bi_);
    bi_bo_ = bi_lp_ + n_bi_;
    bi_tri_ = reinterpret_cast<const uint32_t *>(base_ + off + align8(n_bi_ * 8));
    off += sz.bi;
    tri_w_ = reinterpret_cast<const uint32_t *>(base_ + off);
    tri_lp_ = reinterpret_cast<const int16_t *>(tri_w_ + n_tri_);
    // WHY only the end markers here: checking every index entry would read (and keep in memory) the whole
    // file at start-up. Each lookup clamps its own range instead (context()), so even a damaged file can't
    // make a lookup read outside the mapping.
    if (next != V || bi_off_[V] != n_bi_ || bi_tri_[n_bi_] != n_tri_) {
        unload();
        return false;
    }
    return true;
}

uint32_t NgramLM::id(std::string_view alpha) const
{
    const auto it = ids_.find(alpha);
    return it == ids_.end() ? kUnk : it->second;
}

NgramLM::Context NgramLM::context(const std::vector<std::string> &words) const
{
    Context c;
    if (!loaded())
        return c;
    // WHY <s> for an empty context: the model knows which words start sentences ("The", "I", "This"), which
    // is the only hint the first word of a sentence has.
    if (!words.empty()) {
        c.v = id(alphaForm(words.back()));
        c.has_u = true;
        c.u = words.size() >= 2 ? id(alphaForm(words[words.size() - 2])) : kBos;
    }
    // Clamped so a damaged index can never point outside its table.
    auto span = [](uint32_t lo, uint32_t hi, uint64_t n) {
        hi = uint32_t(std::min<uint64_t>(hi, n));
        return std::pair<uint32_t, uint32_t>(std::min(lo, hi), hi);
    };
    std::tie(c.bi_lo, c.bi_hi) = span(bi_off_[c.v], bi_off_[c.v + 1], n_bi_);
    c.bo2 = uni_bo_[c.v];
    if (c.has_u) {
        const auto [lo, hi] = span(bi_off_[c.u], bi_off_[c.u + 1], n_bi_);
        const uint32_t i = find(bi_w_, lo, hi, c.v);
        if (i < hi) {
            c.bo3 = bi_bo_[i] / kQuant;
            std::tie(c.tri_lo, c.tri_hi) = span(bi_tri_[i], bi_tri_[i + 1], n_tri_);
        }   // else: pair never seen, back-off weight 1 (log 0) and no trigrams
    }
    return c;
}

float NgramLM::logProb(const Context &c, uint32_t w) const
{
    if (!loaded() || w >= vocab_)
        return floor_;
    // Standard back-off: use the longest word sequence the model holds; each step down multiplies in the
    // back-off weight of the context that was dropped, so the probabilities still add up to one.
    if (c.tri_hi > c.tri_lo) {
        const uint32_t i = find(tri_w_, c.tri_lo, c.tri_hi, w);
        if (i < c.tri_hi)
            return tri_lp_[i] / kQuant;
    }
    const uint32_t j = find(bi_w_, c.bi_lo, c.bi_hi, w);
    if (j < c.bi_hi)
        return c.bo3 + bi_lp_[j] / kQuant;
    return std::max(floor_, c.bo3 + c.bo2 + uni_lp_[w]);
}

void NgramLM::continuations(const Context &c, std::vector<uint32_t> &out) const
{
    if (!loaded()) return;
    for (uint32_t i = c.tri_lo; i < c.tri_hi; ++i) out.push_back(tri_w_[i]);
    for (uint32_t i = c.bi_lo; i < c.bi_hi; ++i) out.push_back(bi_w_[i]);
}

float NgramLM::logProb(const Context &c, std::string_view alpha, float dictFreq) const
{
    if (!loaded())
        return floor_;
    const uint32_t w = id(alpha);
    if (w != kUnk)
        return logProb(c, w);
    // WHY the dictionary fallback: a word the model never saw (dictionary changed after the build) still gets
    // a sensible, frequency-based score instead of the <unk> probability, which belongs to "any unknown word".
    return std::max(floor_, calib_a_ + calib_b_ * dictFreq);
}

bool NgramLM::write(const std::string &path, const NgramModelData &d)
{
    const uint64_t V = d.words.size() + 3, n2 = d.bi_keys.size(), n3 = d.tri_keys.size();
    if (V > (1u << kIdBits) || d.uni_lp.size() != V || d.uni_bo.size() != V || d.bi_lp.size() != n2 ||
        d.bi_bo.size() != n2 || d.tri_lp.size() != n3 || n2 >= UINT32_MAX || n3 >= UINT32_MAX ||
        !std::is_sorted(d.bi_keys.begin(), d.bi_keys.end()) || !std::is_sorted(d.tri_keys.begin(), d.tri_keys.end()))
        return false;
    const uint64_t mask = (uint64_t(1) << kIdBits) - 1;
    // Group bigrams by first word, and trigrams under the bigram they continue (which must exist).
    std::vector<uint32_t> off(V + 1, 0), bw(n2), tri(n2 + 1, 0), tw(n3);
    for (uint64_t i = 0; i < n2; ++i) {
        if ((d.bi_keys[i] >> kIdBits) >= V || (d.bi_keys[i] & mask) >= V) return false;
        ++off[(d.bi_keys[i] >> kIdBits) + 1];
        bw[i] = uint32_t(d.bi_keys[i] & mask);
    }
    for (uint64_t v = 0; v < V; ++v) off[v + 1] += off[v];
    uint64_t b = 0;
    for (uint64_t t = 0; t < n3; ++t) {
        const uint64_t ctx = d.tri_keys[t] >> kIdBits;
        while (b < n2 && d.bi_keys[b] < ctx) tri[++b] = uint32_t(t);
        if (b == n2 || d.bi_keys[b] != ctx || (d.tri_keys[t] & mask) >= V) return false;
        tw[t] = uint32_t(d.tri_keys[t] & mask);
    }
    while (b < n2) tri[++b] = uint32_t(n3);

    std::string words;
    for (const auto &w : d.words)
        words += w + '\n';
    Header h{};
    std::memcpy(h.magic, kMagic, 8);
    h.version = 2;
    h.vocab = uint32_t(V);
    h.n_bi = n2;
    h.n_tri = n3;
    h.vocab_bytes = words.size();
    h.calib_a = d.calib_a;
    h.calib_b = d.calib_b;
    h.floor = d.floor;

    const std::string tmp = path + ".part";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    bool ok = true;
    uint64_t pos = 0;
    auto put = [&](const void *p, uint64_t n) {
        if (n && std::fwrite(p, 1, n, f) != n) ok = false;
        pos += n;
    };
    auto pad = [&] {
        static const char z[8] = {};
        put(z, align8(pos) - pos);
    };
    auto put16 = [&](const std::vector<float> &v) {
        std::vector<int16_t> q(v.size());
        std::transform(v.begin(), v.end(), q.begin(), quant);
        put(q.data(), q.size() * 2);
    };
    put(&h, sizeof h);
    put(words.data(), words.size());
    pad();
    put(d.uni_lp.data(), V * 4);
    put(d.uni_bo.data(), V * 4);
    pad();
    put(off.data(), (V + 1) * 4);
    pad();
    put(bw.data(), n2 * 4);
    put16(d.bi_lp);
    put16(d.bi_bo);
    pad();
    put(tri.data(), (n2 + 1) * 4);
    pad();
    put(tw.data(), n3 * 4);
    put16(d.tri_lp);
    pad();
    ok = (std::fclose(f) == 0) && ok && pos == Sizes(V, n2, n3, words.size()).total;
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace swipekb
