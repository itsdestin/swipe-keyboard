// Scores the network + lexicon search on real swipes: first-guess / top-3 accuracy and milliseconds per swipe.
//   swipekb-swipenet-bench --model model.bin --words words.tsv --layout qwerty.tsv swipes.jsonl [options]
// Timing is for one thread (--jobs 1, the default); more jobs only speed up parameter sweeps.
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <thread>

#include "swipekb/swipe_beam.hpp"
#include "swipekb/ngram.hpp"
#include "swipekb/swipe_layout.hpp"
#include "swipekb/swipe_lm.hpp"
#include "swipekb/swipe_net.hpp"

using namespace swipekb;
using Clock = std::chrono::steady_clock;

namespace {
struct Swipe { std::string word; float aspect; std::vector<SwipePoint> pts; };

std::string lettersOnly(const std::string &s) {   // a-z only, lower case: how the benchmark compares words
    std::string o;
    for (unsigned char c : s) if (std::isalpha(c)) o.push_back(char(std::tolower(c)));
    return o;
}
}

int main(int argc, char **argv) {
    std::string model, words, layoutPath, input, ngramPath;
    float lmWeight = 1.0f, freqWeight2 = 0.0f;
    BeamParams bp;
    int jobs = 1, topN = 5; size_t limit = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--model") model = val(); else if (a == "--words") words = val(); else if (a == "--layout") layoutPath = val();
        else if (a == "--beam") bp.beam = std::atoi(val()); else if (a == "--keys-per-step") bp.keysPerStep = std::atoi(val());
        else if (a == "--lookahead") bp.lookahead = float(std::atof(val())); else if (a == "--freq-weight") bp.freqWeight = float(std::atof(val()));
        else if (a == "--jobs") jobs = std::atoi(val()); else if (a == "--limit") limit = size_t(std::atoll(val()));
        else if (a == "--top") topN = std::atoi(val());
        else if (a == "--ngram") ngramPath = val(); else if (a == "--lm-weight") lmWeight = float(std::atof(val()));
        else if (a == "--lm-freq-weight") freqWeight2 = float(std::atof(val()));
        else if (a[0] != '-') input = a; else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (model.empty() || words.empty() || layoutPath.empty() || input.empty()) {
        std::fprintf(stderr, "usage: %s --model m.bin --words words.tsv --layout keys.tsv [--beam N --keys-per-step N --lookahead x --freq-weight x --jobs N --limit N] swipes.jsonl\n", argv[0]);
        return 2;
    }
    SwipeLayout layout; SwipeNet net; SwipeLexicon lex;
    if (!layout.loadTsv(layoutPath)) { std::fprintf(stderr, "cannot read layout\n"); return 1; }
    if (!net.load(model)) { std::fprintf(stderr, "cannot load model\n"); return 1; }
    auto t0 = Clock::now();
    if (!lex.loadTsv(words, layout)) { std::fprintf(stderr, "cannot read words\n"); return 1; }
    lex.finish();
    std::fprintf(stderr, "lexicon: %zu words, %zu trie nodes, built in %.0f ms; model %zu parameters\n", lex.wordCount(), lex.nodeCount(),
                 std::chrono::duration<double, std::milli>(Clock::now() - t0).count(), net.parameterCount());

    std::vector<Swipe> swipes;
    QFile f(QString::fromStdString(input));
    if (!f.open(QIODevice::ReadOnly)) { std::fprintf(stderr, "cannot read swipes\n"); return 1; }
    while (!f.atEnd() && (!limit || swipes.size() < limit)) {
        const auto o = QJsonDocument::fromJson(f.readLine()).object();
        Swipe s;
        s.word = o["word"].toString().toStdString();
        s.aspect = float(o["canvas_height"].toDouble() / o["canvas_width"].toDouble());
        for (const auto &p : o["data"].toArray()) {
            const auto po = p.toObject();
            s.pts.push_back({float(po["x"].toDouble()), float(po["y"].toDouble()), po["t"].toDouble()});
        }
        if (!s.pts.empty()) swipes.push_back(std::move(s));
    }
    // Optional n-gram prior. Benchmark swipes are single words, so the model's plain word frequency is used
    // (no previous words); the sentence-context path is exercised by the unit tests.
    NgramLM lm; std::unique_ptr<NgramWordPrior> prior;
    if (!ngramPath.empty()) {
        if (!lm.load(ngramPath)) { std::fprintf(stderr, "cannot load n-gram file\n"); return 1; }
        prior = std::make_unique<NgramWordPrior>(lm, lex);
        prior->unigramOnly = true; prior->lmWeight = lmWeight; prior->freqWeight = freqWeight2;
    }
    struct Out { std::vector<std::string> top; double netMs = 0, beamMs = 0; };
    std::vector<Out> out(swipes.size());
    std::atomic<size_t> next{0};
    auto work = [&]() {
        SwipeBeamSearch search(lex);
        SwipeInput in; std::vector<float> lp;
        for (size_t i; (i = next++) < swipes.size();) {
            const auto a = Clock::now();
            net.buildInput(swipes[i].pts, layout, swipes[i].aspect, in);
            net.forward(in, lp);
            const auto b = Clock::now();
            const auto res = prior ? search.decode(lp.data(), in.steps, in.keyCount, bp, topN, [&](uint32_t w) { return (*prior)(w); })
                                         : search.decode(lp.data(), in.steps, in.keyCount, bp, topN);
            const auto c = Clock::now();
            for (const auto &r : res) out[i].top.push_back(lettersOnly(lex.text(r.word)));
            out[i].netMs = std::chrono::duration<double, std::milli>(b - a).count();
            out[i].beamMs = std::chrono::duration<double, std::milli>(c - b).count();
        }
    };
    const auto start = Clock::now();
    std::vector<std::thread> th;
    for (int j = 1; j < jobs; ++j) th.emplace_back(work);
    work();
    for (auto &t : th) t.join();
    const double wall = std::chrono::duration<double, std::milli>(Clock::now() - start).count();

    size_t top1 = 0, top3 = 0, inLex = 0, topN_ = 0;
    double net_ms = 0, beam_ms = 0;
    std::map<std::string, bool> known;
    for (size_t i = 0; i < swipes.size(); ++i) {
        const std::string truth = lettersOnly(swipes[i].word);
        const auto &t = out[i].top;
        if (!t.empty() && t[0] == truth) ++top1;
        for (size_t k = 0; k < t.size() && k < 3; ++k) if (t[k] == truth) { ++top3; break; }
        for (const auto &w : t) if (w == truth) { ++topN_; break; }
        net_ms += out[i].netMs; beam_ms += out[i].beamMs;
        (void)inLex;
    }
    const double n = double(swipes.size());
    std::printf("swipes %zu  first-guess %.2f%%  top-3 %.2f%%  top-%d %.2f%%  | ms/swipe: net %.2f + search %.2f = %.2f (jobs %d, wall %.1f s)\n",
                swipes.size(), 100 * top1 / n, 100 * top3 / n, topN, 100 * topN_ / n, net_ms / n, beam_ms / n, (net_ms + beam_ms) / n, jobs, wall / 1000);
    return 0;
}
