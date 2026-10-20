// swipekb engine — turns a finger path over the on-screen keyboard into ranked word guesses.
// Wraps FUTO Swipe; this header deliberately exposes no FUTO or ExecuTorch types so callers (the Qt
// plugin) never compile against them.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "swipekb/nlm.hpp"

namespace swipekb {

struct TracePoint {
    float x, y;   // pixels, keyboard coordinates
    float t_ms;   // milliseconds since the swipe started
};

struct KeyRect {
    char32_t letter;      // Unicode scalar, lower-cased without changing the measured key center
    float x, y, w, h;     // top-left corner and size, same pixel space as the trace
};

struct Candidate {
    std::string word;
    float score;
};

// FUTO's models expect key centers and trace points in 0..1 coordinates of the letter area
// (left edge of the leftmost letter key = 0, right edge of the rightmost = 1; same for rows).
struct NormalizedInput {
    std::string letters;                 // UTF-8 profile alphabet (one index per Unicode scalar)
    std::vector<float> key_cx, key_cy;   // [alphabet codepoint count], never UTF-8 byte count
    std::vector<float> x, y, t;          // trace, same length
};

// nullopt unless every alphabet symbol has measured geometry (or an explicit measured popup base),
// rectangles/trace are finite and well formed, and the trace has at least 2 points.
std::optional<NormalizedInput> normalize(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &keys,
                                         const std::string &alphabet = "abcdefghijklmnopqrstuvwxyz",
                                         const std::unordered_map<char32_t,char32_t> &popupAliases = {});

// The words before the cursor that the context model should see: only the current sentence (a . ! or ?
// starts a fresh one), split on whitespace, punctuation trimmed off both ends of each word, last `max` kept,
// oldest first. Input and output are UTF-8.
// WHY here and not in the plugin: the live keyboard and the scoreboard (tools/swipekb-bench) must build
// context with the very same rules, or the scoreboard stops measuring what ships.
std::vector<std::string> contextWords(const std::string &textBeforeCursor, int max = 20);

struct ModelPaths {
    std::string encoder, decoder, lm_model, lm_vocab, dictionary;
    // Our word-sequence model (scripts/lm/build-lm.sh). Optional: when the file is missing or unreadable the
    // engine ranks guesses the old way, with FUTO's context model.
    std::string ngram;
    // The neural word model (a small GGUF language model, nlm.hpp). Optional, like the n-gram file.
    std::string nlm;
    // Standard layout of assets/ and of the installed ~/.local/share/swipe-keyboard.
    static ModelPaths fromDir(const std::string &dir)
    {
        return {dir + "/models/honorable_sturgeon/model_fp32.pte",
                dir + "/models/magic_macaw/model_fp32.pte",
                dir + "/models/hungry_jellyfish/context_lm.pte",
                dir + "/models/hungry_jellyfish/vocab.txt",
                // WHY en_swipe: AOSP's word list plus wordfreq words (scripts/dict/build-en-swipe.sh); it scored
                // higher than AOSP alone on the scoreboard.
                dir + "/dict/en_swipe.combined",
                dir + "/lm/en_ngram.bin",
                dir + "/nlm/en_nlm.gguf"};
    }
};

// One guess straight out of FUTO's search, before ranking. WHY public: the re-ranking below is a plain
// function of these numbers, so it can be tested with made-up guesses and tuned offline (tools/swipekb-bench
// --dump writes these).
struct RawCandidate {
    std::string word;      // letters-only, lower case (the form FUTO's dictionary returns)
    float ctc = 0;         // how well the finger path matches the word's letters (log probability, <= 0)
    float length = 0;      // number of letters
    float freq = 0;        // the dictionary's 0..255 frequency score
    float lm = 0;          // our n-gram model: log10 P(word | previous words)
    float futo_lm = 0;     // FUTO's context model score; 0 unless that model is loaded (n-gram ranking off)
};

// How the scores are combined:  shape / length^gamma + beta * length + lambda * freq + alpha * lm.
// The first three terms are FUTO's own "how well does the path fit" score; alpha weighs the n-gram model.
// WHY gamma/beta/lambda are FUTO's own numbers (its scoring.json row for "with a context model"): letting a
// search re-tune all four weights on 2,000 swipes over-fitted (+1.2 points there, -0.1 on the scoreboard); on
// 10,000 swipes it beat these fixed numbers by only 0.09. So only alpha is ours: tuned on FUTO's validation
// split (bench/tune-rerank.py --pin lm), never on the scoreboard's test slice; best at 1.75, and anywhere in
// 1.5-2.25 is within 0.15 points.
struct RerankWeights {
    float gamma = 0.1126f, beta = 2.2138f, lambda = 0.0060f, alpha = 1.75f;
};

// Ranks `pool` best-first and keeps `top_k`. The best raw path match (highest ctc) always stays in the list
// (in the last slot if it wasn't already shown), so a clearly swiped rare word can never be hidden by the
// word model. Ties keep FUTO's order.
std::vector<Candidate> rerankCandidates(const std::vector<RawCandidate> &pool, const RerankWeights &w, int top_k);

// How the neural model re-orders the first pass's best few guesses (step 1b,
// .superpowers/sdd/step1b-nlm-report.md):  new score = first-pass score + weight * neural score, where the
// neural score is the natural-log chance of the word (and of the word ending there) given the sentence so far.
// Only the first `n` guesses are re-ordered; everything below them keeps its place.
// WHY the defaults: tuned on FUTO's validation swipes only (bench/tune-nlm.py on tune-10000 and devhold-5000,
// 13k swipes; never on the scoreboard's bench-2000). Weight 0.4 sits in the middle of the plateau (0.3-0.5 are
// within ~0.1 point on the pooled dev swipes; 0.6+ starts breaking more words than it fixes). Re-ordering 5
// guesses gains as much as 8 at less cost; 3 is cheaper but helps the top-3 less. Dividing by the piece count
// and counting the word-end chance were each slightly better on both dev slices.
struct NlmWeights {
    int n = 5;                 // how many of the first pass's best guesses the neural model may re-order
    float beta = 0.4f;         // weight of the neural score
    // Weight for words of 3 letters or fewer; negative = same as beta. WHY: the n-gram attempt's breakages
    // were mostly short words (in/of/too/off/now/had); this lets them be tuned separately.
    float beta_short = -1.f;
    bool per_token = true;     // divide the neural score by how many pieces the model spelled the word with
    bool word_end = true;      // include the chance that the word ends there (WordScore::word_end)
    // When the first pass's best guess leads the second by more than this, change nothing. Negative = off.
    // WHY: a clear, well-swiped word should not be overruled by the sentence model.
    float margin = -1.f;
};

// Pure ranking rule, testable without any model: `base` = first-pass scores, best first; `letters` = each
// guess's letter count; `scores` = neural scores for the first min(n, size) guesses. Returns the new order of
// those first guesses as indices into `base` (ties keep the first pass's order). The caller appends the rest.
std::vector<size_t> nlmOrder(const std::vector<float> &base, const std::vector<int> &letters,
                             const std::vector<WordScore> &scores, const NlmWeights &w);

// Tuning knobs. WHY a struct: the replay tool (tools/swipekb-replay) compares settings on recorded swipes;
// the keyboard itself uses the defaults.
struct RecognizerOptions {
    std::string language = "en";
    std::string alphabet = "abcdefghijklmnopqrstuvwxyz";
    std::unordered_map<char32_t,char32_t> popupAliases;
    int beam_width = 0;   // how many partial words the search keeps alive; 0 = FUTO's default (100)
    // Rank guesses with our n-gram model (when ModelPaths::ngram loads) instead of FUTO's context model.
    // WHY off by default: the ship rule is "+0.5 points on the scoreboard's test slice"; it measured +0.10
    // there (93.42 vs 93.32), although +0.51 on 10,000 other test swipes and +0.89 on held-out validation
    // swipes. The owner decides; the keyboard turns it on with the `use-ngram` switch file (plugin).
    bool ngram = false;
    RerankWeights weights;
    // Re-order the best few guesses with the neural word model (when ModelPaths::nlm and its library load).
    // WHY off by default: the ship rules were not all met (.superpowers/sdd/step1b-nlm-report.md). On the
    // scoreboard it gains +0.57 first-guess points over FUTO's ranking but loses one top-3 hit (96.51 vs 96.56);
    // combined with the n-gram ranking it gains +1.18 / +0.15 but the owner replay loses one first guess (as with
    // the n-gram alone); and it adds ~25-40 ms per swipe on this (busy) machine, over the 20 ms budget.
    // The keyboard turns it on with the `use-nlm` switch file (plugin).
    bool nlm = false;
    NlmWeights nlm_weights;
    int nlm_threads = 4;
};

// Puts apostrophes (and hyphens) back into guesses. WHY: FUTO's search works on letters only, so it returns
// "lets", "im", "dont" even though the dictionary spells them "let's", "I'm", "don't". Without this the
// keyboard typed "im" and "lets". Built from the same dictionary file the search uses.
// Capital letters are never added ("I'm" comes out as "i'm"): the keyboard's own capitalization decides them,
// and the owner chose not to auto-capitalize a lone "i".
class Spellings {
public:
    // German preserves supplied noun case; other profiles retain the existing lowercase policy.
    // Reads a `.combined` dictionary. Words marked not-a-word (f=0, "nonword", "not_a_word") are ignored, as
    // are spellings with anything but letters and inner ' or - ("us$", "states'").
    static Spellings fromCombinedFile(const std::string &path, const std::string &language = "en");
    static Spellings fromCombinedText(const std::string &text, const std::string &language = "en");   // the same, from text (tests)
    // Physical-key alternatives only; emitted display/lexical identity remains the verified spelling.
    static std::vector<std::string> recognitionAliases(const std::string &word, const std::string &language);

    // The usual spelling of a letters-only guess: its most frequent spelling ("lets" -> "let's",
    // "its" -> "its"); the guess itself when the dictionary knows no other.
    std::string spell(const std::string &letters) const;
    // The same swipe's other real spellings worth offering in the guess bar, most frequent first ("its" ->
    // {"it's"}, "let's" -> {"lets"}). Excludes `word` itself; empty for most words.
    std::vector<std::string> others(const std::string &word) const;
    bool empty() const { return byLetters_.empty(); }

private:
    // letters-only, lower case -> real spellings, most frequent first (only where some spelling differs).
    std::unordered_map<std::string, std::vector<std::string>> byLetters_;
    std::unordered_map<std::string, std::string> otherKeys_;
};

class Recognizer {
public:
    Recognizer();
    ~Recognizer();
    Recognizer(const Recognizer &) = delete;
    Recognizer &operator=(const Recognizer &) = delete;

    // Loads models + dictionary. Encoder and dictionary are required; decoder and context LM are used
    // when their files exist. Returns false (and stays not-ready) on any failure.
    bool init(const ModelPaths &paths, const RecognizerOptions &options = {});
    bool ready() const;

    // Best-first guesses, at most top_k. Empty when not ready, input invalid, or anything fails.
    // `context` = up to the last few words before the cursor, oldest first.
    std::vector<Candidate> recognize(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &keys,
                                     const std::vector<std::string> &context, int top_k);

    // The same swipe's other spellings for the guess bar (Spellings::others); empty when not ready.
    std::vector<std::string> otherSpellings(const std::string &word) const;

    // True when guesses are ranked by the n-gram model (option on and the model file loaded).
    bool usingNgram() const;
    // Display spelling of the most recent recognition's best raw path match; no second inference needed.
    std::string bestShapeWord() const;
    // True when the neural model re-orders the best guesses (option on, model and library loaded).
    bool usingNlm() const;
    // For tests: re-order with this scorer instead of the model file (nullptr = none). Takes ownership.
    void setWordScorerForTesting(std::unique_ptr<WordScorer> scorer);

    // Tuning aid: every word FUTO's search kept (not cut to a top-k), with the raw scores, the n-gram score
    // (when loaded) and FUTO's context-model score (when loaded). Unranked. Empty when not ready or invalid.
    std::vector<RawCandidate> recognizeDetailed(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &keys,
                                                const std::vector<std::string> &context);

private:
    // recognize() before the apostrophes go back in: FUTO's letters-only words.
    std::vector<Candidate> recognizeLetters(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &keys,
                                            const std::vector<std::string> &context, int top_k);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace swipekb
