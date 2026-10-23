// swipekb n-gram language model — "how likely is this word after the previous one or two words".
//
// WHY our own small model instead of FUTO's context model: FUTO's model squeezes the whole sentence into 16
// numbers and guesses the next word from that; it is weak at exactly the cases that decide a swipe ("of the"
// vs "of thee", "I am" vs "I an"). Word-pair and word-triple statistics from a large text collection
// (a 3-gram model, scripts/lm/) know those. The model is one read-only file, mapped into memory, so loading it
// is instant and several keyboard processes share one copy.
//
// No FUTO types here: the scoreboard tools and tests use this directly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace swipekb {

// Everything the model file holds, in plain vectors. Only the build tool (scripts/lm/ngram-build.cpp) and
// tests fill one of these; the keyboard only reads the file.
struct NgramModelData {
    // Word number = index + 3. 0 = <unk> (any word not listed), 1 = <s> (sentence start), 2 = </s>.
    std::vector<std::string> words;
    // log10 P(word) and the back-off weight (log10) used when a bigram starting with this word is missing.
    // Size words.size() + 3.
    std::vector<float> uni_lp, uni_bo;
    // Sorted keys (see NgramLM::key), with log10 P(w | v) and, for bigrams, the back-off weight (log10) used
    // when a trigram starting with this pair is missing.
    std::vector<uint64_t> bi_keys, tri_keys;
    std::vector<float> bi_lp, bi_bo, tri_lp;
    // Fallback for a word the model has never heard of (e.g. a dictionary swapped after the model was built):
    // log10 P ~= calib_a + calib_b * f, where f is the dictionary's 0..255 frequency score, never below floor.
    float calib_a = -7.f, calib_b = 0.f, floor = -9.f;
};

class NgramLM {
public:
    static constexpr uint32_t kUnk = 0, kBos = 1, kEos = 2;
    static constexpr int kIdBits = 18;   // up to 262,143 words
    static uint64_t key(uint32_t v, uint32_t w) { return (uint64_t(v) << kIdBits) | w; }
    static uint64_t key(uint32_t u, uint32_t v, uint32_t w) { return (uint64_t(u) << (2 * kIdBits)) | key(v, w); }

    // Lower case, letters only: the form FUTO's dictionary stores words in, and the form the model is built in.
    // ASCII is lower-cased; other letters are kept as they are (English dictionary).
    static std::string alphaForm(std::string_view word);

    NgramLM() = default;
    ~NgramLM();
    NgramLM(const NgramLM &) = delete;
    NgramLM &operator=(const NgramLM &) = delete;

    // Maps the file. False (and stays unloaded) when missing, truncated or not a model file.
    bool load(const std::string &path);
    bool loaded() const { return base_ != nullptr; }
    int order() const { return n_tri_ ? 3 : 2; }
    size_t fileBytes() const { return size_; }

    // Word number of a letters-only word; kUnk when the model doesn't list it.
    uint32_t id(std::string_view alpha) const;

    // The previous words, prepared once per swipe so scoring each guess is a couple of small lookups.
    struct Context {
        uint32_t u = kBos, v = kBos;   // the two previous words (u before v)
        bool has_u = false;            // false for the first word of a sentence (only <s> before it)
        uint32_t bi_lo = 0, bi_hi = 0;   // bigrams starting with v
        uint32_t tri_lo = 0, tri_hi = 0; // trigrams starting with u v
        float bo2 = 0, bo3 = 0;        // back-off weights (log10)
    };
    // `words` = swipekb::contextWords() output (oldest first, current sentence only); empty = sentence start.
    Context context(const std::vector<std::string> &words) const;

    // log10 P(word | context). `alpha` must be letters-only (alphaForm). `dictFreq` = the dictionary's f score,
    // only used if the model doesn't know the word.
    float logProb(const Context &c, std::string_view alpha, float dictFreq) const;
    float logProb(const Context &c, uint32_t w) const;
    // Words the model has seen right after this context (trigram, then bigram continuations; may repeat).
    // Every other known word scores max(floor, bo3 + bo2 + unigram), i.e. in unigram order.
    void continuations(const Context &c, std::vector<uint32_t> &out) const;
    float unigramLogProb(uint32_t w) const { return loaded() && w < vocab_ ? uni_lp_[w] : floor_; }
    float floorLogProb() const { return floor_; }

    // Writes `d` as a model file (atomically: temp file + rename). False on any I/O error.
    static bool write(const std::string &path, const NgramModelData &d);

private:
    void unload();
    const char *base_ = nullptr;
    size_t size_ = 0;
    uint32_t vocab_ = 0;
    uint64_t n_bi_ = 0, n_tri_ = 0;
    float calib_a_ = 0, calib_b_ = 0, floor_ = -9;
    const float *uni_lp_ = nullptr, *uni_bo_ = nullptr;
    const uint32_t *bi_off_ = nullptr, *bi_w_ = nullptr, *bi_tri_ = nullptr, *tri_w_ = nullptr;
    const int16_t *bi_lp_ = nullptr, *bi_bo_ = nullptr, *tri_lp_ = nullptr;
    std::unordered_map<std::string_view, uint32_t> ids_;
};

} // namespace swipekb
