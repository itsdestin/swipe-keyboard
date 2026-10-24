// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// ngram-build — counts word triples in tokenised text and turns the counts into the keyboard's n-gram model.
//
// WHY our own tool and not KenLM: KenLM's estimator needs the Boost libraries' headers to build, which this
// machine doesn't have and we may not install. The maths here is the same textbook method KenLM uses
// (interpolated modified Kneser-Ney), followed by count pruning and re-normalised back-off weights, written
// out in the small binary format of engine/src/ngram.cpp.
//
//   ngram-build count OUT.run IDS.u32...        word-number streams (scripts/lm/tokenize.py) -> sorted
//                                               (trigram, count) run for one shard
//   ngram-build stats RUN...                    how many n-grams survive each count threshold (pick sizes)
//   ngram-build build --vocab V.tsv --out M.bin [--order 3] [--t2 N] [--t3 N] RUN...
//
// Kneser-Ney in one paragraph: a word's chance after "u v" is its discounted count after "u v", plus a share
// (proportional to how much was discounted) of its chance after just "v". The chance after "v" is computed the
// same way but from how many DIFFERENT words precede "v w" rather than raw counts, so "Francisco" (always after
// "San") isn't judged likely after other words. The single-word level does the same once more.
#include "swipekb/ngram.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#ifdef _GLIBCXX_PARALLEL
#include <parallel/algorithm>
#define PSORT __gnu_parallel::sort
#else
#define PSORT std::sort
#endif

using swipekb::NgramLM;
using swipekb::NgramModelData;

namespace {

constexpr int B = NgramLM::kIdBits;
constexpr uint64_t M = (uint64_t(1) << B) - 1;
constexpr uint32_t UNK = NgramLM::kUnk, BOS = NgramLM::kBos, EOS = NgramLM::kEos;

#pragma pack(push, 1)
struct Rec {
    uint64_t key;
    uint32_t count;
};
#pragma pack(pop)

[[noreturn]] void die(const char *msg, const char *arg = "")
{
    std::fprintf(stderr, "ngram-build: %s%s\n", msg, arg);
    std::exit(1);
}

// ---- count -------------------------------------------------------------------------------------------------

int cmdCount(int argc, char **argv)
{
    if (argc < 4) die("usage: count OUT.run IDS.u32...");
    std::vector<uint64_t> keys;
    {
        // WHY reserve up front: the key list is several GB; growing it file by file would copy it many times.
        size_t total = 0;
        for (int i = 3; i < argc; ++i) total += size_t(std::ifstream(argv[i], std::ios::binary | std::ios::ate).tellg()) / 4;
        keys.reserve(total);
    }
    for (int i = 3; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary | std::ios::ate);
        if (!in) die("cannot read ", argv[i]);
        const size_t n = size_t(in.tellg()) / 4;
        std::vector<uint32_t> ids(n);
        in.seekg(0);
        in.read(reinterpret_cast<char *>(ids.data()), std::streamsize(n * 4));
        // Each sentence is <s> w1 .. wn </s>; every word after the first (and </s>) ends one trigram.
        uint32_t a = BOS, b = BOS;
        bool first = true;
        for (uint32_t w : ids) {
            if (w > M) die("word number out of range in ", argv[i]);
            if (first) {
                a = BOS;
                b = w;
                first = false;
            } else {
                keys.push_back(NgramLM::key(a, b, w));
                a = b;
                b = w;
            }
            if (w == EOS) first = true;
        }
    }
    PSORT(keys.begin(), keys.end());
    std::vector<Rec> out;
    for (size_t i = 0; i < keys.size();) {
        size_t j = i;
        while (j < keys.size() && keys[j] == keys[i]) ++j;
        out.push_back({keys[i], uint32_t(std::min<size_t>(j - i, UINT32_MAX))});
        i = j;
    }
    FILE *f = std::fopen(argv[2], "wb");
    if (!f || std::fwrite(out.data(), sizeof(Rec), out.size(), f) != out.size() || std::fclose(f) != 0)
        die("cannot write ", argv[2]);
    std::printf("%s: %zu trigram tokens, %zu distinct\n", argv[2], keys.size(), out.size());
    return 0;
}

// ---- merge the sorted runs into memory ---------------------------------------------------------------------

struct Reader {
    FILE *f = nullptr;
    std::vector<Rec> buf;
    size_t pos = 0, len = 0;
    bool next(Rec &r)
    {
        if (pos == len) {
            len = std::fread(buf.data(), sizeof(Rec), buf.size(), f);
            pos = 0;
            if (len == 0) return false;
        }
        r = buf[pos++];
        return true;
    }
};

void mergeRuns(const std::vector<std::string> &runs, std::vector<uint64_t> &K, std::vector<uint32_t> &C)
{
    std::vector<std::unique_ptr<Reader>> rd;
    using Item = std::pair<uint64_t, size_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> pq;
    std::vector<Rec> cur(runs.size());
    size_t total = 0;
    for (const auto &r : runs) total += size_t(std::ifstream(r, std::ios::binary | std::ios::ate).tellg()) / sizeof(Rec);
    K.reserve(total);
    C.reserve(total);
    for (size_t i = 0; i < runs.size(); ++i) {
        auto r = std::make_unique<Reader>();
        r->f = std::fopen(runs[i].c_str(), "rb");
        if (!r->f) die("cannot read ", runs[i].c_str());
        r->buf.resize(1 << 20);
        if (r->next(cur[i])) pq.push({cur[i].key, i});
        rd.push_back(std::move(r));
    }
    while (!pq.empty()) {
        const auto [k, i] = pq.top();
        pq.pop();
        if (!K.empty() && K.back() == k) C.back() = uint32_t(std::min<uint64_t>(uint64_t(C.back()) + cur[i].count, UINT32_MAX));
        else {
            K.push_back(k);
            C.push_back(cur[i].count);
        }
        if (rd[i]->next(cur[i])) pq.push({cur[i].key, i});
    }
    for (auto &r : rd) std::fclose(r->f);
}

// Modified Kneser-Ney discounts for counts 1, 2 and 3+, from how many n-grams occur exactly 1..4 times.
struct Discount {
    double d[4] = {0, 0.5, 1.0, 1.5};
    double operator()(uint64_t c) const { return c == 0 ? 0 : d[std::min<uint64_t>(c, 3)]; }
};

Discount discounts(const uint64_t n[5], const char *name)
{
    Discount D;
    if (n[1] && n[2] && n[3] && n[4]) {
        const double Y = double(n[1]) / (n[1] + 2.0 * n[2]);
        for (int k = 1; k <= 3; ++k)
            D.d[k] = std::clamp(k - (k + 1) * Y * double(n[k + 1]) / double(n[k]), 0.0, double(k));
    }
    std::printf("%s discounts: %.3f %.3f %.3f\n", name, D.d[1], D.d[2], D.d[3]);
    return D;
}

void countOfCounts(uint64_t n[5], uint64_t c)
{
    if (c >= 1 && c <= 4) ++n[c];
}

// ---- stats -------------------------------------------------------------------------------------------------

int cmdStats(int argc, char **argv)
{
    std::vector<std::string> runs(argv + 2, argv + argc);
    std::vector<uint64_t> K;
    std::vector<uint32_t> C;
    mergeRuns(runs, K, C);
    // Raw bigram counts = sums over trigrams with the same first two words (every bigram that isn't at a
    // sentence end is followed by something).
    std::vector<uint64_t> tri(41), bi(41);
    for (size_t i = 0; i < K.size();) {
        size_t j = i;
        uint64_t s = 0;
        while (j < K.size() && (K[j] >> B) == (K[i] >> B)) {
            ++tri[std::min<uint32_t>(C[j], 40)];
            s += C[j++];
        }
        ++bi[std::min<uint64_t>(s, 40)];
        i = j;
    }
    uint64_t t = 0, b = 0;
    std::printf("distinct trigrams %zu\n min-count  trigrams>=  bigram-prefixes>=\n", K.size());
    for (int c = 40; c >= 1; --c) {
        t += tri[c];
        b += bi[c];
        tri[c] = t;
        bi[c] = b;
    }
    for (int c = 1; c <= 40; ++c) std::printf(" %3d  %12zu  %12zu\n", c, size_t(tri[c]), size_t(bi[c]));
    return 0;
}

// ---- build -------------------------------------------------------------------------------------------------

int cmdBuild(int argc, char **argv)
{
    std::string vocabPath, outPath;
    int order = 3;
    uint32_t t2 = 2, t3 = 2;
    std::vector<std::string> runs;
    for (int i = 2; i < argc; ++i) {
        const std::string s = argv[i];
        auto val = [&] { if (i + 1 >= argc) die("missing value for ", s.c_str()); return std::string(argv[++i]); };
        if (s == "--vocab") vocabPath = val();
        else if (s == "--out") outPath = val();
        else if (s == "--order") order = std::atoi(val().c_str());
        else if (s == "--t2") t2 = uint32_t(std::atoi(val().c_str()));
        else if (s == "--t3") t3 = uint32_t(std::atoi(val().c_str()));
        else runs.push_back(s);
    }
    if (vocabPath.empty() || outPath.empty() || runs.empty() || (order != 2 && order != 3))
        die("usage: build --vocab V.tsv --out M.bin [--order 2|3] [--t2 N] [--t3 N] RUN...");

    NgramModelData d;
    std::vector<float> dictF;
    {
        std::ifstream in(vocabPath);
        std::string line;
        while (std::getline(in, line)) {
            const size_t tab = line.find('\t');
            d.words.push_back(line.substr(0, tab));
            dictF.push_back(tab == std::string::npos ? 0.f : float(std::atof(line.c_str() + tab + 1)));
        }
    }
    const size_t V = d.words.size() + 3;
    if (V > M) die("vocabulary too large");

    std::vector<uint64_t> K;
    std::vector<uint32_t> C;
    mergeRuns(runs, K, C);
    std::printf("%zu distinct trigrams\n", K.size());

    // Highest order: raw counts.
    uint64_t n3[5] = {};
    for (uint32_t c : C) countOfCounts(n3, c);
    const Discount D3 = discounts(n3, "trigram");

    // Bigram table. Adjusted count (KN) of "v w" = number of different words seen before it; raw count = sum.
    // Packed as (v w) << 28 | count so one 64-bit sort does it. WHY the 28-bit cap: no trigram of a few
    // billion words occurs 268 million times; the cap only guards against overflow.
    std::vector<uint64_t> BK;           // bigram keys, sorted
    std::vector<uint64_t> Badj, Braw;   // adjusted and raw counts
    {
        std::vector<uint64_t> s(K.size());
        for (size_t i = 0; i < K.size(); ++i)
            s[i] = ((K[i] & ((uint64_t(1) << (2 * B)) - 1)) << 28) | std::min<uint32_t>(C[i], (1u << 28) - 1);
        PSORT(s.begin(), s.end());
        for (size_t i = 0; i < s.size();) {
            const uint64_t k = s[i] >> 28;
            uint64_t adj = 0, raw = 0;
            while (i < s.size() && (s[i] >> 28) == k) {
                ++adj;
                raw += s[i++] & ((1u << 28) - 1);
            }
            BK.push_back(k);
            Badj.push_back(adj);
            Braw.push_back(raw);
        }
        // "<s> w": nothing comes before <s>, so its adjusted count is its raw count, the sum of trigrams
        // "<s> w x". Those keys (v = <s> = 1) sort between v = 0 and v = 2 blocks; insert them there.
        std::vector<uint64_t> sk, sc;
        for (size_t i = 0; i < K.size(); ++i)
            if ((K[i] >> (2 * B)) == BOS) {
                const uint64_t k = (K[i] >> B);
                if (!sk.empty() && sk.back() == k) sc.back() += C[i];
                else { sk.push_back(k); sc.push_back(C[i]); }
            }
        const size_t at = std::lower_bound(BK.begin(), BK.end(), NgramLM::key(BOS, 0)) - BK.begin();
        BK.insert(BK.begin() + at, sk.begin(), sk.end());
        Badj.insert(Badj.begin() + at, sc.begin(), sc.end());
        Braw.insert(Braw.begin() + at, sc.begin(), sc.end());
    }
    // WHY: a 2-gram model's top level must use raw counts, like the trigram level does in a 3-gram model.
    if (order == 2) Badj = Braw;
    std::printf("%zu distinct bigrams\n", BK.size());
    uint64_t n2[5] = {};
    for (uint64_t c : Badj) countOfCounts(n2, c);
    const Discount D2 = discounts(n2, "bigram");

    // Unigrams: number of different words seen before w.
    std::vector<uint64_t> a1(V, 0), raw1(V, 0);
    for (size_t i = 0; i < BK.size(); ++i) {
        ++a1[BK[i] & M];
        raw1[BK[i] & M] += Braw[i];
    }
    a1[BOS] = 0;
    uint64_t n1[5] = {}, S1 = 0, N1[4] = {};
    for (size_t w = 0; w < V; ++w) {
        countOfCounts(n1, a1[w]);
        S1 += a1[w];
        if (a1[w]) ++N1[std::min<uint64_t>(a1[w], 3)];
    }
    const Discount D1 = discounts(n1, "unigram");
    std::vector<double> p1(V, 0.0);
    {
        const double gamma = (D1.d[1] * N1[1] + D1.d[2] * N1[2] + D1.d[3] * N1[3]) / double(S1);
        for (size_t w = 0; w < V; ++w)
            if (w != BOS) p1[w] = std::max(0.0, a1[w] - D1(a1[w])) / double(S1) + gamma / double(V - 1);
        // Dictionary fallback: fit log10 p1 = a + b*f on words seen often enough to be reliable, then give
        // words the corpus never showed (a1 == 0) that estimate, capped at a once-seen word's probability.
        // WHY: an unseen word would otherwise get only a tiny uniform share, however common the dictionary
        // says it is (fresh slang, rare spellings) — it would then nearly never win a swipe.
        double sx = 0, sy = 0, sxx = 0, sxy = 0, n = 0, once = 0;
        for (size_t i = 0; i < d.words.size(); ++i) {
            const size_t w = i + 3;
            if (a1[w] == 1 && once == 0) once = p1[w];
            if (a1[w] < 5) continue;
            const double x = dictF[i], y = std::log10(p1[w]);
            sx += x; sy += y; sxx += x * x; sxy += x * y; n += 1;
        }
        const double b = n > 1 && (n * sxx - sx * sx) > 0 ? (n * sxy - sx * sy) / (n * sxx - sx * sx) : 0;
        const double a = n > 0 ? (sy - b * sx) / n : -7;
        d.calib_a = float(a);
        d.calib_b = float(b);
        size_t unseen = 0;
        for (size_t i = 0; i < d.words.size(); ++i)
            if (a1[i + 3] == 0) {
                const double est = std::pow(10.0, a + b * dictF[i]);
                p1[i + 3] = once > 0 ? std::min(est, once) : est;
                ++unseen;
            }
        double sum = 0;
        for (double p : p1) sum += p;
        for (double &p : p1) p /= sum;
        double mn = 1;
        for (size_t w = 0; w < V; ++w) if (w != BOS) mn = std::min(mn, p1[w]);
        d.floor = float(std::log10(mn)) - 1.f;
        std::printf("unigrams: %zu dictionary words never seen in the corpus; fallback log10 p = %.3f + %.4f f\n",
                    unseen, a, b);
    }

    // Full (unpruned) bigram probabilities, grouped by first word v.
    std::vector<float> p2(BK.size());
    std::vector<double> gamma2(V, 0.0);
    for (size_t i = 0; i < BK.size();) {
        const uint64_t v = BK[i] >> B;
        size_t j = i;
        double S = 0, N[4] = {};
        for (; j < BK.size() && (BK[j] >> B) == v; ++j) {
            S += Badj[j];
            ++N[std::min<uint64_t>(Badj[j], 3)];
        }
        const double g = (D2.d[1] * N[1] + D2.d[2] * N[2] + D2.d[3] * N[3]) / S;
        gamma2[v] = g;
        for (size_t k = i; k < j; ++k) p2[k] = float(std::max(0.0, Badj[k] - D2(Badj[k])) / S + g * p1[BK[k] & M]);
        i = j;
    }
    auto p2full = [&](uint64_t v, uint64_t w) {
        const auto it = std::lower_bound(BK.begin(), BK.end(), NgramLM::key(uint32_t(v), uint32_t(w)));
        return (it != BK.end() && *it == NgramLM::key(uint32_t(v), uint32_t(w))) ? double(p2[it - BK.begin()])
                                                                                 : gamma2[v] * p1[w];
    };

    // Which trigrams survive pruning, and which word pairs are their contexts (those pairs must stay).
    const bool tri = order == 3;
    std::vector<uint64_t> ctx;
    if (tri)
        for (size_t i = 0; i < K.size(); ++i)
            if (C[i] >= t3 && (ctx.empty() || ctx.back() != (K[i] >> B))) ctx.push_back(K[i] >> B);
    std::vector<char> keep2(BK.size(), 0), isCtx(BK.size(), 0);
    {
        size_t c = 0;
        for (size_t i = 0; i < BK.size(); ++i) {
            while (c < ctx.size() && ctx[c] < BK[i]) ++c;
            isCtx[i] = c < ctx.size() && ctx[c] == BK[i];
            keep2[i] = Braw[i] >= t2 || isCtx[i];
        }
    }
    // Back-off weight of context v: the probability left for pruned words, spread like the unigram model.
    std::vector<double> bo2(V, 1.0);
    for (size_t i = 0; i < BK.size();) {
        const uint64_t v = BK[i] >> B;
        double num = 1, den = 1;
        size_t j = i;
        for (; j < BK.size() && (BK[j] >> B) == v; ++j)
            if (keep2[j]) { num -= p2[j]; den -= p1[BK[j] & M]; }
        bo2[v] = (num > 1e-9 && den > 1e-9) ? num / den : 1e-9;
        i = j;
    }
    // Probability of w after v in the PRUNED bigram model.
    auto p2pruned = [&](uint64_t v, uint64_t w) {
        const auto it = std::lower_bound(BK.begin(), BK.end(), NgramLM::key(uint32_t(v), uint32_t(w)));
        if (it != BK.end() && *it == NgramLM::key(uint32_t(v), uint32_t(w)) && keep2[it - BK.begin()])
            return double(p2[it - BK.begin()]);
        return bo2[v] * p1[w];
    };

    // Trigram level: kept trigrams' full probabilities + back-off weight of each context pair.
    std::vector<double> bo3(BK.size(), 1.0);
    if (tri) {
        for (size_t i = 0; i < K.size();) {
            const uint64_t uv = K[i] >> B, v = uv & M;
            size_t j = i;
            double S = 0, N[4] = {};
            for (; j < K.size() && (K[j] >> B) == uv; ++j) {
                S += C[j];
                ++N[std::min<uint32_t>(C[j], 3)];
            }
            const double g = (D3.d[1] * N[1] + D3.d[2] * N[2] + D3.d[3] * N[3]) / S;
            double num = 1, den = 1;
            bool any = false;
            for (size_t k = i; k < j; ++k) {
                if (C[k] < t3) continue;
                const uint64_t w = K[k] & M;
                const double p = std::max(0.0, C[k] - D3(C[k])) / S + g * p2full(v, w);
                num -= p;
                den -= p2pruned(v, w);
                any = true;
                // WHY </s> and <unk> are not stored: the keyboard only ever asks about dictionary words.
                if (w != EOS && w != UNK) {
                    d.tri_keys.push_back(K[k]);
                    d.tri_lp.push_back(float(std::log10(p)));
                }
            }
            if (any) {
                const auto it = std::lower_bound(BK.begin(), BK.end(), uv);
                bo3[it - BK.begin()] = (num > 1e-9 && den > 1e-9) ? num / den : 1e-9;
            }
            i = j;
        }
    }
    for (size_t i = 0; i < BK.size(); ++i) {
        const uint64_t w = BK[i] & M;
        if (!keep2[i] || w == EOS || (w == UNK && !isCtx[i])) continue;
        d.bi_keys.push_back(BK[i]);
        d.bi_lp.push_back(float(std::log10(p2[i])));
        d.bi_bo.push_back(float(std::log10(bo3[i])));
    }
    d.uni_lp.resize(V);
    d.uni_bo.resize(V);
    for (size_t w = 0; w < V; ++w) {
        d.uni_lp[w] = w == BOS ? -99.f : float(std::log10(p1[w]));
        d.uni_bo[w] = float(std::log10(bo2[w]));
    }
    if (!NgramLM::write(outPath, d)) die("cannot write ", outPath.c_str());
    std::printf("wrote %s: order %d, %zu words, %zu bigrams (t2=%u), %zu trigrams (t3=%u)\n", outPath.c_str(), order,
                V, d.bi_keys.size(), t2, d.tri_keys.size(), t3);
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "count") return cmdCount(argc, argv);
    if (cmd == "stats") return cmdStats(argc, argv);
    if (cmd == "build") return cmdBuild(argc, argv);
    std::fprintf(stderr, "usage: ngram-build count|stats|build ... (see the comment at the top of the source)\n");
    return 2;
}
