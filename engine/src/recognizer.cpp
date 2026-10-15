#include "swipekb/recognizer.hpp"
#include "swipekb/ngram.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>

#include "swipe_decoder/engine.hpp"
#include "swipe_decoder/layout.hpp"
#include "swipe_decoder/trie.hpp"

namespace swipekb {

struct Recognizer::Impl {
    // Declared before `engine` so it is destroyed after it: the engine keeps a pointer into the dictionary.
    std::optional<swipe_decoder::Trie> dict;
    swipe_decoder::SwipeEngine engine;
    swipe_decoder::KeyboardLayout layout; // what the engine currently uses
    NgramLM ngram;          // loaded only when options.ngram is on and the file is there
    RecognizerOptions options;
    bool ready = false;
};

// WHY the whole beam: the search keeps at most beam_width partial words, so asking for that many returns every
// word it found. The n-gram model then gets to rank all of them, not just FUTO's first few.
static int poolSize(const RecognizerOptions &o) { return std::max(100, o.beam_width); }

std::vector<Candidate> rerankCandidates(const std::vector<RawCandidate> &pool, const RerankWeights &w, int top_k)
{
    if (pool.empty() || top_k <= 0)
        return {};
    std::vector<std::pair<float, size_t>> scored;
    scored.reserve(pool.size());
    for (size_t i = 0; i < pool.size(); ++i) {
        const RawCandidate &c = pool[i];
        // Same "path fit" formula as FUTO's search (beam_search.cpp), plus the n-gram term.
        const float L = std::max(c.length, 1.0f);
        const float s = c.ctc / std::pow(L, w.gamma) + w.beta * c.length + w.lambda * c.freq + w.alpha * c.lm;
        scored.push_back({s, i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    const size_t best_ctc = size_t(std::max_element(pool.begin(), pool.end(), [](const auto &a, const auto &b) {
                                       return a.ctc < b.ctc;
                                   }) - pool.begin());
    std::vector<Candidate> out;
    bool shown = false;
    for (size_t k = 0; k < scored.size() && int(out.size()) < top_k; ++k) {
        out.push_back({pool[scored[k].second].word, scored[k].first});
        shown = shown || scored[k].second == best_ctc;
    }
    // The raw-shape guard (FUTO does the same after its own re-ranking, engine.cpp): if the word the path
    // matches best was pushed out by the word model, it takes the last visible slot.
    if (!shown) {
        const auto it = std::find_if(scored.begin(), scored.end(), [&](const auto &x) { return x.second == best_ctc; });
        out.back() = {pool[best_ctc].word, it->first};
    }
    return out;
}

static bool fileExists(const std::string &p) { return !p.empty() && std::ifstream(p).good(); }

// WHY compare: the keyboard re-measures its keys on every swipe; only re-configure FUTO's engine when
// the geometry actually changed (window resize, rotation), not on every swipe.
static bool sameGeometry(const swipe_decoder::KeyboardLayout &l, const NormalizedInput &n)
{
    if (l.key_cx.size() != n.key_cx.size() || l.letters != n.letters)
        return false;
    for (size_t i = 0; i < n.key_cx.size(); ++i)
        if (std::fabs(l.key_cx[i] - n.key_cx[i]) > 1e-4f || std::fabs(l.key_cy[i] - n.key_cy[i]) > 1e-4f)
            return false;
    return true;
}

// Re-configures FUTO's engine only when the key geometry changed (see sameGeometry). Shared by recognize()
// and recognizeDetailed().
static bool updateGeometry(swipe_decoder::SwipeEngine &engine, swipe_decoder::KeyboardLayout &current,
                           const NormalizedInput &n)
{
    if (sameGeometry(current, n))
        return true;
    auto layout = swipe_decoder::make_layout(n.letters, n.key_cx.data(), n.key_cy.data(), static_cast<int>(n.key_cx.size()));
    swipe_decoder::SwipeEngine::Mode mode;
    mode.layout = layout;
    if (!engine.setMode(mode))
        return false;
    current = layout;
    return true;
}

// FUTO's guesses plus our model's log10 P(word | previous words), computed once per swipe for the context.
static std::vector<RawCandidate> toRaw(const NgramLM &ngram, const std::vector<swipe_decoder::DecodedWord> &words,
                                       const std::vector<std::string> &context)
{
    std::vector<RawCandidate> raw;
    raw.reserve(words.size());
    const NgramLM::Context ctx = ngram.context(context);
    for (const auto &w : words) {
        RawCandidate c{w.word, w.ctc_score, w.word_length, w.frequency, 0.f, w.lm_score};
        if (ngram.loaded())
            c.lm = ngram.logProb(ctx, NgramLM::alphaForm(w.word), w.frequency);
        raw.push_back(std::move(c));
    }
    return raw;
}

Recognizer::Recognizer() : impl_(std::make_unique<Impl>()) {}
Recognizer::~Recognizer() = default;

bool Recognizer::init(const ModelPaths &p, const RecognizerOptions &options)
{
    impl_ = std::make_unique<Impl>();
    try {
        if (!fileExists(p.encoder) || !fileExists(p.dictionary)) {
            std::fprintf(stderr, "swipekb: missing encoder or dictionary (%s, %s)\n", p.encoder.c_str(), p.dictionary.c_str());
            return false;
        }
        impl_->options = options;
        // WHY before FUTO's engine: if the n-gram file is missing or damaged we simply keep the old ranking
        // (FUTO's context model); nothing else depends on it.
        if (options.ngram && fileExists(p.ngram)) {
            if (!impl_->ngram.load(p.ngram))
                std::fprintf(stderr, "swipekb: could not read n-gram model %s; using FUTO's ranking\n", p.ngram.c_str());
        }
        impl_->dict = swipe_decoder::load_trie_simple(p.dictionary);
        if (!impl_->dict) {
            std::fprintf(stderr, "swipekb: could not read dictionary %s\n", p.dictionary.c_str());
            return false;
        }
        swipe_decoder::SwipeEngine::Config cfg;
        cfg.encoder_path = p.encoder;
        // WHY only when set: 0 keeps FUTO's tuned default, so the live keyboard is unchanged.
        if (options.beam_width > 0)
            cfg.beam_width = options.beam_width;
        if (fileExists(p.decoder))
            cfg.decoder_path = p.decoder;
        // WHY FUTO's context model is skipped when ours loaded: it would never be used (ours ranks instead),
        // and loading it costs start-up time and memory. It stays the fallback when ours is missing.
        if (!impl_->ngram.loaded() && fileExists(p.lm_model) && fileExists(p.lm_vocab)) {
            cfg.lm_model_path = p.lm_model;
            cfg.lm_vocab_path = p.lm_vocab;
        }
        cfg.layout = swipe_decoder::default_en_qwerty();
        cfg.dictionaries = {impl_->dict->itrie()};
        if (!impl_->engine.init(cfg)) {
            std::fprintf(stderr, "swipekb: FUTO engine init failed\n");
            impl_ = std::make_unique<Impl>();
            return false;
        }
        impl_->layout = cfg.layout;
        impl_->ready = true;
        return true;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "swipekb: init failed: %s\n", e.what());
        impl_ = std::make_unique<Impl>();
        return false;
    } catch (...) {
        // WHY: the model library could throw something that isn't a std::exception; an escaped throw would
        // crash the whole on-screen keyboard. Fail the same way: swiping off, tapping unaffected.
        std::fprintf(stderr, "swipekb: init failed: unknown error\n");
        impl_ = std::make_unique<Impl>();
        return false;
    }
}

bool Recognizer::ready() const { return impl_->ready; }
bool Recognizer::usingNgram() const { return impl_->ready && impl_->ngram.loaded(); }

std::vector<Candidate> Recognizer::recognize(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &keys,
                                             const std::vector<std::string> &context, int top_k)
{
    if (!impl_->ready || top_k <= 0)
        return {};
    const auto n = normalize(trace, keys);
    if (!n)
        return {};
    try {
        if (!updateGeometry(impl_->engine, impl_->layout, *n))
            return {};
        if (impl_->ngram.loaded()) {
            // WHY no context for FUTO here: an empty context switches FUTO's own context model off (its weaker
            // re-ranking), so what comes back is the pure path-fit score; our model adds the context instead.
            // The search itself prunes the same way with or without context (same pruning numbers in FUTO's
            // scoring.json), so no word is lost by this.
            const auto words = impl_->engine.recognize(n->x.data(), n->y.data(), n->t.data(), n->x.size(), {},
                                                       poolSize(impl_->options));
            return rerankCandidates(toRaw(impl_->ngram, words, context), impl_->options.weights, top_k);
        }
        const auto words = impl_->engine.recognize(n->x.data(), n->y.data(), n->t.data(), n->x.size(), context, top_k);
        std::vector<Candidate> out;
        out.reserve(words.size());
        for (const auto &w : words)
            out.push_back({w.word, w.final_score});
        return out;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "swipekb: recognize failed: %s\n", e.what());
        return {};
    } catch (...) {
        // WHY: same as init: a non-standard throw must cost this one swipe (no guess), never the keyboard.
        std::fprintf(stderr, "swipekb: recognize failed: unknown error\n");
        return {};
    }
}

std::vector<RawCandidate> Recognizer::recognizeDetailed(const std::vector<TracePoint> &trace,
                                                        const std::vector<KeyRect> &keys,
                                                        const std::vector<std::string> &context)
{
    if (!impl_->ready)
        return {};
    const auto n = normalize(trace, keys);
    if (!n)
        return {};
    try {
        if (!updateGeometry(impl_->engine, impl_->layout, *n))
            return {};
        // With the context, so FUTO's own context model also scores each word (futo_lm); the pool covers the
        // whole beam, so FUTO's own cut and re-ordering drop nothing.
        const auto words = impl_->engine.recognize(n->x.data(), n->y.data(), n->t.data(), n->x.size(), context,
                                                   poolSize(impl_->options));
        return toRaw(impl_->ngram, words, context);
    } catch (...) {
        return {};
    }
}

} // namespace swipekb
