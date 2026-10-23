#include "swipekb/recognizer.hpp"
#include "swipekb/ngram.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>
#include <numeric>
#include <mutex>
#include <QChar>
#include <QString>
#include <QList>
#include <functional>
#include <unordered_map>

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
    // Apostrophes back into the typed guesses ("lets" -> "let's", "im" -> "i'm"). See Spellings.
    Spellings spellings;
    std::string bestShape;
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
std::string Recognizer::bestShapeWord() const { return impl_->spellings.spell(impl_->bestShape); }

bool Recognizer::init(const ModelPaths &p, const RecognizerOptions &options)
{
    impl_ = std::make_unique<Impl>();
    try {
        // WHY defense in depth: foreign profiles may use only their authenticated language-specific n-gram;
        // English neural/context/decoder refinements and an absent or unrequested n-gram fail closed.
        if (options.language!="en" && (!p.decoder.empty() || !p.lm_model.empty() || !p.lm_vocab.empty() ||
            (!p.ngram.empty() && !options.ngram))) return false;
        if (!fileExists(p.encoder) || !fileExists(p.dictionary)) {
            std::fprintf(stderr, "swipekb: missing encoder or dictionary (%s, %s)\n", p.encoder.c_str(), p.dictionary.c_str());
            return false;
        }
        impl_->options = options;
        // WHY before FUTO's engine: if the n-gram file is missing or damaged we simply keep the old ranking
        // (FUTO's context model); nothing else depends on it.
        if (options.ngram && fileExists(p.ngram)) {
            if (!impl_->ngram.load(p.ngram)) {
                if (options.language!="en") return false;
                std::fprintf(stderr, "swipekb: could not read n-gram model %s; using FUTO's ranking\n", p.ngram.c_str());
            }
        } else if (options.language!="en" && options.ngram) return false;
        // WHY install once before any trie: FUTO otherwise only lowercases ASCII on desktop.
        static std::once_flag caseTable;
        std::call_once(caseTable, [] {
            std::vector<int> from,to;
            for (char32_t c=128;c<0x10000;++c) {
                const auto lower=QChar::toLower(c);
                if (c!=lower) { from.push_back(int(c)); to.push_back(int(lower)); }
            }
            swipe_decoder::set_case_mapping(from.data(),to.data(),from.size());
        });
        // WHY the default trie is ASCII: construct its Unicode alphabet explicitly for experimental packs.
        swipe_decoder::ParsedCombined combined;
        if (!swipe_decoder::parse_combined_vocab(p.dictionary,"f",combined)) return false;
        impl_->dict.emplace(options.alphabet);
        swipe_decoder::apply_parsed_to_trie(combined,*impl_->dict);
        // WHY expose the same bounded physical aliases that Spellings restores. These are
        // dictionary-derived keys, not English fallback or relaxed lexical evaluation.
        for (const auto &entry:combined.alphas)
            for (const auto &alias:Spellings::recognitionAliases(entry.alpha,options.language))
                impl_->dict->insert(alias,entry.max_freq,true);
        // WHY always (not an option): FUTO's search drops apostrophes, so without this every contraction was
        // typed wrong ("im", "dont", "lets"). Costs one more read of the dictionary file at start-up.
        impl_->spellings = Spellings::fromCombinedFile(p.dictionary, options.language);
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

std::vector<Candidate> Recognizer::complete(const std::string &prefix, int top_k) const
{
    if(!impl_->ready || !impl_->dict || top_k<=0) return {};
    const auto text=QString::fromStdString(prefix).normalized(QString::NormalizationForm_C).toLower();
    if(text.isEmpty() || text.size()>64) return {};
    const auto cps=text.toUcs4();
    std::vector<int> wanted;
    for(auto cp:cps) {
        if(cp=='\'' || cp==0x2019 || cp=='-') continue;
        int c=impl_->dict->lookup_codepoint(cp);if(c<0)return {};wanted.push_back(c);
    }
    if(wanted.empty())return {};
    std::vector<Candidate> pool;
    size_t visited=0;
    // WHY bounded edit-distance DP: traverse the existing dictionary, not a second
    // vocabulary/cache or a new model. Exact prefix descendants keep frequency order;
    // one-edit corrections become eligible only for a meaningful (3+ letter) token.
    const int edits=wanted.size()>=3?1:0;
    std::vector<int> initial(wanted.size()+1);std::iota(initial.begin(),initial.end(),0);
    std::function<void(uint32_t,const std::vector<int>&,int)> walk;
    walk=[&](uint32_t index,const std::vector<int>&row,int prefixDistance) {
        if(++visited>60000)return;
        const auto *node=impl_->dict->get_node(index);if(!node || node->depth>64)return;
        int match=prefixDistance;
        if(row.back()==0)match=0;
        const int terminalDistance=std::min(match,row.back());
        // WHY no shorter corrections: dropping one of the person's letters (hel -> he) is never a
        // completion, and a frequent short word would otherwise take the centre slot.
        if(node->is_word && terminalDistance<=edits && (terminalDistance==0 || node->depth>=wanted.size())) {
            auto alpha=impl_->dict->reconstruct_word(index);
            auto word=impl_->spellings.spell(alpha);
            // Same-length substitutions are less specific than an omitted letter;
            // retain both, but avoid a frequent unrelated held/help burying helo->hello.
            const float substitution=terminalDistance && node->depth==wanted.size()?40.0f:0.0f;
            pool.push_back({word,node->log_frequency-float(terminalDistance)*1000.0f-substitution});
        }
        if(match>edits && *std::min_element(row.begin(),row.end())>edits)return;
        for(int c=0;c<impl_->dict->num_chars();++c)if(node->children[c]) {
            std::vector<int> next(row.size());next[0]=row[0]+1;
            for(size_t j=1;j<next.size();++j) next[j]=std::min({next[j-1]+1,row[j]+1,row[j-1]+(wanted[j-1]!=c)});
            walk(node->children[c],next,match);
        }
    };
    walk(0,initial,edits+1);
    float bestExact=-10000,bestCorrection=-10000;
    for(const auto &candidate:pool) {
        if(candidate.score>-500)bestExact=std::max(bestExact,candidate.score);
        else bestCorrection=std::max(bestCorrection,candidate.score+1000);
    }
    // WHY rare accidental dictionary prefixes (helo -> helots) must not bury a
    // common one-edit word. Strong exact prefixes retain their completion beam.
    if(bestCorrection-20>bestExact)for(auto &candidate:pool)if(candidate.score<-500)candidate.score+=980;
    std::stable_sort(pool.begin(),pool.end(),[](const auto&a,const auto&b){return a.score>b.score;});
    std::vector<Candidate> out;
    for(const auto &candidate:pool) {
        if(std::none_of(out.begin(),out.end(),[&](const auto &x){return x.word==candidate.word;}))out.push_back(candidate);
        if(int(out.size())>=std::min(top_k,12))break;
    }
    return out;
}

std::vector<Candidate> Recognizer::similar(const std::string &word, int top_k) const
{
    if(!impl_->ready || !impl_->dict || top_k<=0) return {};
    const auto text=QString::fromStdString(word).normalized(QString::NormalizationForm_C).toLower();
    if(text.size()<2 || text.size()>64) return {};
    std::vector<int> wanted;
    for(auto cp:text.toUcs4()) {
        if(cp=='\'' || cp==0x2019 || cp=='-') continue;
        int c=impl_->dict->lookup_codepoint(cp);if(c<0)return {};wanted.push_back(c);
    }
    if(wanted.size()<2)return {};
    // WHY a separate walk from complete(): complete() ranks words that merely START with the letters, so for
    // "sea" the top twelve are search/season/seat and the real neighbours (see, sew) never surface. Here only
    // WHOLE words one edit away count: a swapped, dropped, added or replaced letter (Damerau, "teh" -> "the").
    std::vector<Candidate> pool;
    size_t visited=0;
    const size_t n=wanted.size();
    std::vector<int> initial(n+1);std::iota(initial.begin(),initial.end(),0);
    std::function<void(uint32_t,const std::vector<int>&,const std::vector<int>&,int)> walk;
    walk=[&](uint32_t index,const std::vector<int>&row,const std::vector<int>&prevRow,int lastChar) {
        if(++visited>60000)return;
        const auto *node=impl_->dict->get_node(index);if(!node || node->depth>64)return;
        // distance 0 is the tapped word itself, never an alternative
        if(node->is_word && row.back()==1) {
            auto display=impl_->spellings.spell(impl_->dict->reconstruct_word(index));
            pool.push_back({display,node->log_frequency});
        }
        // WHY prune at 2: nothing below this node can come back within one edit.
        if(*std::min_element(row.begin(),row.end())>1)return;
        for(int c=0;c<impl_->dict->num_chars();++c)if(node->children[c]) {
            std::vector<int> next(row.size());next[0]=row[0]+1;
            for(size_t j=1;j<next.size();++j) {
                next[j]=std::min({next[j-1]+1,row[j]+1,row[j-1]+(wanted[j-1]!=c)});
                if(j>1 && lastChar>=0 && c==wanted[j-2] && lastChar==wanted[j-1]) next[j]=std::min(next[j],prevRow[j-2]+1);
            }
            walk(node->children[c],next,row,c);
        }
    };
    walk(0,initial,initial,-1);
    std::stable_sort(pool.begin(),pool.end(),[](const auto&a,const auto&b){return a.score>b.score;});
    std::vector<Candidate> out;
    for(const auto &candidate:pool) {
        if(QString::fromStdString(candidate.word).toLower()==text)continue;
        if(std::none_of(out.begin(),out.end(),[&](const auto &x){return x.word==candidate.word;}))out.push_back(candidate);
        if(int(out.size())>=std::min(top_k,24))break;
    }
    return out;
}

bool Recognizer::ready() const { return impl_->ready; }
bool Recognizer::usingNgram() const { return impl_->ready && impl_->ngram.loaded(); }

std::vector<std::string> Recognizer::otherSpellings(const std::string &word) const
{
    return impl_->ready ? impl_->spellings.others(word) : std::vector<std::string>{};
}

std::vector<Candidate> Recognizer::recognize(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &keys,
                                             const std::vector<std::string> &context, int top_k)
{
    // WHY spelled last: every ranking step (n-gram, the raw-shape guard) works on FUTO's letters-only
    // words; only what leaves the engine gets its apostrophes back. Two guesses can never end up spelled the
    // same, because each letters-only word has exactly one usual spelling.
    std::vector<Candidate> out = recognizeLetters(trace, keys, context, top_k);
    for (Candidate &c : out)
        c.word = impl_->spellings.spell(c.word);
    return out;
}

std::vector<Candidate> Recognizer::recognizeLetters(const std::vector<TracePoint> &trace,
                                                    const std::vector<KeyRect> &keys,
                                                    const std::vector<std::string> &context, int top_k)
{
    impl_->bestShape.clear();
    if (!impl_->ready || top_k <= 0)
        return {};
    const auto n = normalize(trace, keys, impl_->options.alphabet, impl_->options.popupAliases);
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
            if (!words.empty()) impl_->bestShape=std::max_element(words.begin(),words.end(),[](const auto &a,const auto &b) { return a.ctc_score<b.ctc_score; })->word;
            return rerankCandidates(toRaw(impl_->ngram, words, context), impl_->options.weights, top_k);
        }
        const auto words = impl_->engine.recognize(n->x.data(), n->y.data(), n->t.data(), n->x.size(), context, top_k);
        if (!words.empty()) impl_->bestShape=std::max_element(words.begin(),words.end(),[](const auto &a,const auto &b) { return a.ctc_score<b.ctc_score; })->word;
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
    const auto n = normalize(trace, keys, impl_->options.alphabet, impl_->options.popupAliases);
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
