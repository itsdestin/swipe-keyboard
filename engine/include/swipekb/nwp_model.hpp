// swipekb next-word neural model (NWP LM): a one-layer LSTM with a projection and tied word vectors, the size
// class of Google's published Gboard next-word models (arXiv 2305.18465). Plain C++ - no ML runtime.
//
// WHY this exists next to the n-gram: a 3-gram only sees the two previous words ("I have" -> any verb), the
// LSTM carries the whole sentence in its state ("yesterday we ... " -> past tense, "she and her sisters" ->
// plural). The file is produced by scripts/lm/nwp/export.py; its layout is documented there.
//
// One instance is single-threaded: step() and logits() reuse scratch buffers (the Predictor already lives
// on one worker thread).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace swipekb {

class NwpModel {
public:
    static constexpr uint32_t kUnk = 0, kBos = 1, kEos = 2;   // token numbers; words start at 3

    struct State {
        std::vector<float> h, c;   // h: projected output (dim), c: cell (hidden)
    };

    // Reads and verifies the whole file (magic, version, exact size, checksum). False, and stays unloaded,
    // when it is missing, truncated, damaged or from another version.
    bool load(const std::string &path);
    bool loaded() const { return vocab_ != 0; }
    size_t fileBytes() const { return fileBytes_; }
    uint32_t vocabSize() const { return vocab_; }     // including the three special tokens
    uint32_t dim() const { return dim_; }
    uint32_t hidden() const { return hidden_; }
    // How much to trust the model against the n-gram, and the score bonus of words the model lacks (log10);
    // tuned per language when the file is written.
    float lambda() const { return lambda_; }
    float oovBonus() const { return oovBonus_; }

    // Token of a letters-only word; kUnk when the model does not list it.
    uint32_t token(std::string_view alpha) const;
    // The letters-only word of a token >= 3.
    const std::string &word(uint32_t token) const { return words_[token - 3]; }

    State start() const;                          // all zeros: nothing read yet
    void step(State &s, uint32_t token) const;    // read one token (the input word)
    // Scores of every token as the next one: out[t] = natural-log probability (log-softmax), size vocabSize().
    void logProbs(const State &s, std::vector<float> &out) const;

private:
    struct Matrix {
        const int8_t *q = nullptr;     // int8 rows (null when the file stores float)
        const float *f = nullptr;      // float rows (null when int8)
        const float *scale = nullptr;  // per-row scale for int8
        uint32_t rows = 0, cols = 0;
    };
    void matvec(const Matrix &m, const float *x, float *out, bool add) const;
    void embedRow(uint32_t token, float *out) const;

    std::vector<uint8_t> blob_;
    size_t fileBytes_ = 0;
    uint32_t vocab_ = 0, dim_ = 0, hidden_ = 0;
    float lambda_ = 0.5f, oovBonus_ = 0.f;
    bool int8_ = false;
    Matrix emb_, wih_, whh_, proj_;
    const float *outBias_ = nullptr, *bias_ = nullptr;
    std::vector<std::string> words_;
    std::unordered_map<std::string_view, uint32_t> ids_;
    mutable std::vector<float> gates_, x_, hfull_, hnew_;
};

} // namespace swipekb
