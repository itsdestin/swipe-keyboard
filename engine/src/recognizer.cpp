#include "swipekb/recognizer.hpp"

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
    bool ready = false;
};

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
        if (fileExists(p.lm_model) && fileExists(p.lm_vocab)) {
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

std::vector<Candidate> Recognizer::recognize(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &keys,
                                             const std::vector<std::string> &context, int top_k)
{
    if (!impl_->ready || top_k <= 0)
        return {};
    const auto n = normalize(trace, keys);
    if (!n)
        return {};
    try {
        if (!sameGeometry(impl_->layout, *n)) {
            auto layout = swipe_decoder::make_layout(n->letters, n->key_cx.data(), n->key_cy.data(),
                                                     static_cast<int>(n->key_cx.size()));
            swipe_decoder::SwipeEngine::Mode mode;
            mode.layout = layout;
            if (!impl_->engine.setMode(mode))
                return {};
            impl_->layout = layout;
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

} // namespace swipekb
