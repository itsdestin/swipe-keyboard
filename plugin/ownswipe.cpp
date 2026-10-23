#include "ownswipe.h"

#include <QFile>
#include <QString>
#include <QTemporaryFile>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <exception>

#include "swipekb/ngram.hpp"
#include "swipekb/swipe_engine.hpp"
#include "swipekb/swipe_lm.hpp"

namespace swipekb {

struct OwnSwipe::Impl {
    OwnSwipeSetup setup;
    SwipeEngine engine;
    NgramLM lm;
    bool lmLoaded = false, failed = false, built = false;
    std::unique_ptr<NgramWordPrior> prior;
    std::u32string letters;               // sorted letters of the layout the lexicon was built for
    std::vector<SwipeKey> keys;           // the geometry currently set on the engine
    std::string wordsTsv;                 // filtered dictionary, kept so a new letter set can rebuild the lexicon

    // Reads the verified `.combined` file into "word<TAB>freq" lines. Same acceptance rules as the FUTO path's
    // spelling table: skip f=0, "nonword"/not_a_word, and anything that is not letters with inner ' or -.
    bool readWords() {
        std::ifstream f(setup.dictionary, std::ios::binary);
        if (!f) return false;
        std::ostringstream tsv;
        std::string line;
        size_t n = 0;
        while (std::getline(f, line)) {
            if (line.compare(0, 6, " word=") != 0) continue;
            const size_t e = line.find(',', 6);
            if (e == std::string::npos) continue;
            const size_t fp = line.find(",f=", e);
            if (fp == std::string::npos) continue;
            const int freq = std::atoi(line.c_str() + fp + 3);
            if (freq <= 0 || line.find("not_a_word=true") != std::string::npos || line.find("nonword") != std::string::npos) continue;
            QString w = QString::fromStdString(line.substr(6, e - 6)).normalized(QString::NormalizationForm_C);
            bool ok = !w.isEmpty() && w.front().isLetter() && w.back().isLetter();
            for (int i = 0; ok && i < w.size(); ++i) {
                const QChar c = w[i];
                ok = c.isLetter() || ((c == QLatin1Char('\'') || c == QChar(0x2019) || c == QLatin1Char('-')) && i > 0 && i + 1 < w.size()
                                      && w[i + 1].isLetter());
            }
            if (!ok) continue;
            if (!setup.keepCase) w = w.toLower();
            tsv << w.toStdString() << '\t' << freq << '\n';
            ++n;
        }
        wordsTsv = tsv.str();
        return n > 0;
    }

    // Makes the engine match these key rectangles. False if the geometry is unusable.
    bool prepare(const std::vector<KeyRect> &rects, float &x0, float &y0, float &W, float &H) {
        if (rects.size() < 2) return false;
        x0 = y0 = 1e30f; float x1 = -1e30f, y1 = -1e30f;
        for (const auto &r : rects) {
            if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.w) || !std::isfinite(r.h) || r.w <= 0 || r.h <= 0) return false;
            x0 = std::min(x0, r.x); y0 = std::min(y0, r.y); x1 = std::max(x1, r.x + r.w); y1 = std::max(y1, r.y + r.h);
        }
        W = x1 - x0; H = y1 - y0;
        if (!(W > 0) || !(H > 0)) return false;
        // WHY sorted by letter: the lexicon stores key *numbers*, so the same letters must always get the same
        // numbers; then a window resize only moves keys and the (slow) word-list build is skipped.
        std::vector<SwipeKey> ks;
        std::u32string seen;
        std::vector<KeyRect> sorted(rects);
        std::stable_sort(sorted.begin(), sorted.end(), [](const KeyRect &a, const KeyRect &b) { return a.letter < b.letter; });
        for (const auto &r : sorted) {
            if (seen.find(r.letter) != std::u32string::npos) continue;   // a second key with the same letter adds nothing
            seen.push_back(r.letter);
            ks.push_back({r.letter, (r.x + r.w / 2 - x0) / W, (r.y + r.h / 2 - y0) / H, r.w / 2 / W, r.h / 2 / H});
        }
        if (!built || seen != letters) {
            engine.setLayout(SwipeLayout(ks));
            QTemporaryFile tmp;   // WHY a file: the engine reads its word list from a path
            if (!tmp.open() || tmp.write(wordsTsv.data(), qint64(wordsTsv.size())) != qint64(wordsTsv.size())) return false;
            tmp.flush();
            if (engine.loadWords(tmp.fileName().toStdString()) == 0 || !engine.ready()) return false;
            prior.reset();
            if (lmLoaded) prior = std::make_unique<NgramWordPrior>(lm, engine.lexicon());
            letters = seen; built = true;
        } else {
            bool same = ks.size() == keys.size();
            for (size_t i = 0; same && i < ks.size(); ++i)
                same = std::fabs(ks[i].cx - keys[i].cx) < 1e-4f && std::fabs(ks[i].cy - keys[i].cy) < 1e-4f
                    && std::fabs(ks[i].hw - keys[i].hw) < 1e-4f && std::fabs(ks[i].hh - keys[i].hh) < 1e-4f;
            if (!same) engine.setLayout(SwipeLayout(ks));
        }
        keys = std::move(ks);
        return true;
    }
};

OwnSwipe::OwnSwipe() : d_(std::make_unique<Impl>()) {}
OwnSwipe::~OwnSwipe() = default;

bool OwnSwipe::requested(const std::string &dataDir)
{
    // WHY read at profile resolve, not per swipe: switching engines mid-sentence would change the feel of typing.
    if (const char *e = std::getenv("SWIPEKB_ENGINE")) return std::string(e) == "own";
    return QFile::exists(QString::fromStdString(dataDir + "/own-engine"));
}

bool OwnSwipe::init(const OwnSwipeSetup &setup)
{
    try {
        d_->setup = setup;
        if (setup.dictionary.empty() || !QFile::exists(QString::fromStdString(setup.dictionary))) return false;
        // WHY EngineParams spelled out: these are the values the engine was tuned and scored with (spec section 6).
        d_->engine.params.beam.beam = 48;
        d_->engine.params.geoWeight = 40.f;
        d_->engine.params.freqWeight = 0.06f;
        // WHY a missing/damaged model is not an error: loadModel() just leaves the engine geometry-only.
        if (!setup.model.empty() && QFile::exists(QString::fromStdString(setup.model))) d_->engine.loadModel(setup.model);
        if (!setup.ngram.empty()) d_->lmLoaded = d_->lm.load(setup.ngram);
        return true;
    } catch (...) {
        return false;
    }
}

bool OwnSwipe::hasModel() const { return d_->engine.hasModel(); }
bool OwnSwipe::usingNgram() const { return d_->lmLoaded; }

bool OwnSwipe::recognize(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &rects,
                         const std::vector<std::string> &context, int topN, Result &out)
{
    out = {};
    if (d_->failed || trace.size() < 2 || topN <= 0) return false;
    try {
        if (d_->wordsTsv.empty() && !d_->readWords()) { d_->failed = true; return false; }
        float x0, y0, W, H;
        if (!d_->prepare(rects, x0, y0, W, H)) return false;
        std::vector<SwipePoint> pts;
        pts.reserve(trace.size());
        for (const auto &p : trace) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
            pts.push_back({(p.x - x0) / W, (p.y - y0) / H, double(p.t_ms)});
        }
        if (d_->prior) {
            // WHY alphaForm: the n-gram file stores lower-case letters-only words, like the FUTO path feeds it.
            std::vector<std::string> prev;
            for (const auto &w : context) { const auto a = NgramLM::alphaForm(w); if (!a.empty()) prev.push_back(a); }
            d_->prior->setContext(prev);
            NgramWordPrior *pr = d_->prior.get();
            d_->engine.setPrior([pr](uint32_t w) { return (*pr)(w); }, d_->prior->hints());
        } else {
            d_->engine.setPrior({}, {});
        }
        const auto res = d_->engine.decode(pts, H / W, topN);
        float bestCtc = -1e30f;
        for (const auto &c : res) {
            const std::string &w = d_->engine.lexicon().text(c.word);
            if (w.empty()) continue;
            out.words.push_back(w);
            if (c.ctc > bestCtc) { bestCtc = c.ctc; out.bestShape = w; }
        }
        if (out.words.empty()) return false;
        return true;
    } catch (...) {
        out = {};
        return false;
    }
}

}
