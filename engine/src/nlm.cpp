#include "swipekb/nlm.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string_view>

#include <ggml-backend.h>
#include <llama.h>

namespace swipekb {

namespace {

// WHY quiet: llama.cpp prints pages of model details on every load into the keyboard's log. Only its errors
// are worth keeping there.
void quietLog(ggml_log_level level, const char *text, void *)
{
    if (level == GGML_LOG_LEVEL_ERROR)
        std::fputs(text, stderr);
}

// WHY once per process: setting up llama.cpp and finding the CPU code for this processor is global state.
// WHY load the backends at all: this llama.cpp build keeps its CPU code in separate per-processor libraries
// (/usr/lib/ggml/libggml-cpu-*.so) and picks the best one for this CPU at run time. Only CPU libraries are
// installed here; and even if a GPU one were added later, the model is told to use no GPU (see load()).
void initLlamaOnce()
{
    static std::once_flag once;
    std::call_once(once, [] {
        llama_log_set(quietLog, nullptr);
        if (ggml_backend_dev_count() == 0)
            ggml_backend_load_all();
        llama_backend_init();
    });
}

float logSumExp(const float *x, int n)
{
    float m = x[0];
    for (int i = 1; i < n; ++i)
        m = std::max(m, x[i]);
    double s = 0;
    for (int i = 0; i < n; ++i)
        s += std::exp(double(x[i] - m));
    return m + float(std::log(s));
}

// WHY these sizes: the model's working memory grows with them (the per-piece output alone is ~200 KB for a
// 50k-piece vocabulary). The re-ranking asks about at most 8 words of a few pieces each after <= 48 context
// pieces, so 16 words and 256 pieces leave ample room while keeping the memory small.
constexpr int kMaxSeq = 16;     // most words scored in one call
constexpr int kCtxTokens = 256; // room for context + every word's pieces
constexpr int kMaxOutputs = 160; // most pieces whose next-piece chances are read in one call

// The real scorer. Only this library (libswipekb_nlm.so) sees llama.cpp.
class LlamaScorer final : public WordScorer {
public:
    bool load(const std::string &path, const NeuralLMOptions &options);
    std::vector<WordScore> score(const std::vector<std::string> &context, const std::vector<std::string> &words) override;
    ~LlamaScorer() override
    {
        if (ctx) llama_free(ctx);
        if (model) llama_model_free(model);
    }

private:
    llama_model *model = nullptr;
    llama_context *ctx = nullptr;
    const llama_vocab *vocab = nullptr;
    NeuralLMOptions options;
    int n_vocab = 0;
    llama_token start = LLAMA_TOKEN_NULL;   // "beginning of text" marker put before a sentence's first word
    std::vector<llama_token> boundary;      // pieces that start a new word (space, punctuation, end of text)
    std::vector<llama_token> cached;        // context pieces currently held in the model's memory

    std::vector<llama_token> tokenize(const std::string &text) const
    {
        if (text.empty())
            return {};
        int n = -llama_tokenize(vocab, text.data(), int32_t(text.size()), nullptr, 0, false, false);
        std::vector<llama_token> out(std::max(n, 0));
        if (n <= 0 || llama_tokenize(vocab, text.data(), int32_t(text.size()), out.data(), n, false, false) != n)
            return {};
        return out;
    }
};

bool LlamaScorer::load(const std::string &path, const NeuralLMOptions &options)
{
    if (path.empty() || !std::ifstream(path).good())
        return false;
    initLlamaOnce();
    LlamaScorer *im = this;
    im->options = options;
    llama_model_params mp = llama_model_default_params();
    // WHY CPU only: the keyboard must never wake or hold the graphics chip. An empty device list plus zero
    // GPU layers keeps every weight and every calculation on the CPU.
    static ggml_backend_dev_t noDevices[] = {nullptr};
    mp.devices = noDevices;
    mp.n_gpu_layers = 0;
    // WHY mapped: the file's pages are shared and clean, so the kernel can drop them under memory pressure.
    mp.load_mode = LLAMA_LOAD_MODE_MMAP;
    im->model = llama_model_load_from_file(path.c_str(), mp);
    if (!im->model)
        return false;
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = kCtxTokens;
    cp.n_batch = kCtxTokens;
    cp.n_ubatch = kCtxTokens;
    cp.n_seq_max = kMaxSeq;
    cp.n_outputs_max = kMaxOutputs;
    // WHY one shared memory for all candidate words: they all continue the SAME context, so the context is
    // run through the network once and every candidate reads it (instead of once per candidate).
    cp.kv_unified = true;
    cp.n_threads = cp.n_threads_batch = std::clamp(options.threads, 1, 8);
    cp.no_perf = true;
    im->ctx = llama_init_from_model(im->model, cp);
    if (!im->ctx)
        return false;
    im->vocab = llama_model_get_vocab(im->model);
    im->n_vocab = llama_vocab_n_tokens(im->vocab);
    im->start = llama_vocab_bos(im->vocab);
    if (im->start == LLAMA_TOKEN_NULL)
        im->start = llama_vocab_eos(im->vocab);
    // The pieces after which a word is finished: anything starting with whitespace or with sentence
    // punctuation, and the end-of-text marker. WHY not apostrophes or hyphens: "don" + "'t" and "well" +
    // "-known" continue the same word.
    char buf[64];
    for (llama_token t = 0; t < im->n_vocab; ++t) {
        if (llama_vocab_is_eog(im->vocab, t)) {
            im->boundary.push_back(t);
            continue;
        }
        const int n = llama_token_to_piece(im->vocab, t, buf, sizeof buf, 0, false);
        if (n <= 0)
            continue;
        const unsigned char c = buf[0];
        if (std::isspace(c) || std::string_view(".,!?;:\")]}").find(char(c)) != std::string_view::npos)
            im->boundary.push_back(t);
    }
    return true;
}

std::vector<WordScore> LlamaScorer::score(const std::vector<std::string> &context, const std::vector<std::string> &words)
{
    LlamaScorer &m = *this;
    if (!m.ctx || words.empty() || int(words.size()) > kMaxSeq)
        return {};

    // The sentence so far, as plain text. At the start of a sentence the model sees only "beginning of text",
    // and the word is scored capitalised, the way a sentence's first word is written.
    std::string text;
    for (const auto &w : context)
        text += (text.empty() ? "" : " ") + w;
    std::vector<llama_token> ctxTok = m.tokenize(text);
    if (int(ctxTok.size()) >= m.options.max_context_tokens)
        ctxTok.erase(ctxTok.begin(), ctxTok.end() - m.options.max_context_tokens);
    else
        ctxTok.insert(ctxTok.begin(), m.start);
    std::vector<std::vector<llama_token>> wordTok;
    int total = int(ctxTok.size());
    for (const auto &w : words) {
        std::string piece = w;
        if (context.empty() && !piece.empty())
            piece[0] = char(std::toupper(static_cast<unsigned char>(piece[0])));
        else
            piece = " " + piece;
        wordTok.push_back(m.tokenize(piece));
        if (wordTok.back().empty())
            return {};
        total += int(wordTok.back().size());
    }
    if (total > kCtxTokens)
        return {};

    // WHY keep the previous context in memory: while typing a sentence the context only grows by a word each
    // time, so only the new pieces need running. Everything after the part both contexts share is dropped
    // (including last time's candidate words). At least the last context piece is re-run, because its output
    // is what scores each candidate's first piece.
    llama_memory_t mem = llama_get_memory(m.ctx);
    size_t keep = 0;
    while (m.options.reuse_context && keep < m.cached.size() && keep < ctxTok.size() && m.cached[keep] == ctxTok[keep])
        ++keep;
    keep = std::min(keep, ctxTok.size() - 1);
    if (!llama_memory_seq_rm(mem, -1, llama_pos(keep), -1)) {
        llama_memory_clear(mem, true);
        keep = 0;
    }
    m.cached.assign(ctxTok.begin(), ctxTok.begin() + keep);

    llama_batch batch = llama_batch_init(kCtxTokens, 0, kMaxSeq);
    auto add = [&](llama_token t, llama_pos pos, int seqFirst, int seqCount, bool logits) {
        const int i = batch.n_tokens++;
        batch.token[i] = t;
        batch.pos[i] = pos;
        batch.n_seq_id[i] = seqCount;
        for (int s = 0; s < seqCount; ++s)
            batch.seq_id[i][s] = seqFirst + s;
        batch.logits[i] = logits;
        return i;
    };
    // Context pieces belong to every candidate's sequence; each candidate's own pieces to its sequence only.
    int lastCtx = -1;
    for (size_t p = keep; p < ctxTok.size(); ++p)
        lastCtx = add(ctxTok[p], llama_pos(p), 0, kMaxSeq, p + 1 == ctxTok.size());
    std::vector<int> firstOut(words.size());
    for (size_t w = 0; w < words.size(); ++w) {
        for (size_t j = 0; j < wordTok[w].size(); ++j) {
            const int i = add(wordTok[w][j], llama_pos(ctxTok.size() + j), int(w), 1, true);
            if (j == 0) firstOut[w] = i;
        }
    }
    const int rc = llama_decode(m.ctx, batch);
    llama_batch_free(batch);
    if (rc != 0) {
        llama_memory_clear(mem, true);
        m.cached.clear();
        return {};
    }
    m.cached = ctxTok;

    // log P(piece | everything before it) = its raw score minus the log of the sum over all pieces.
    auto logProb = [&](int out, llama_token t) {
        const float *l = llama_get_logits_ith(m.ctx, out);
        return l[t] - logSumExp(l, m.n_vocab);
    };
    std::vector<WordScore> res(words.size());
    std::vector<float> tmp;
    for (size_t w = 0; w < words.size(); ++w) {
        const auto &toks = wordTok[w];
        float lp = logProb(lastCtx, toks[0]);
        for (size_t j = 1; j < toks.size(); ++j)
            lp += logProb(firstOut[w] + int(j) - 1, toks[j]);
        // The chance that the word is complete here, read from the output after its last piece.
        const float *l = llama_get_logits_ith(m.ctx, firstOut[w] + int(toks.size()) - 1);
        tmp.clear();
        for (llama_token b : m.boundary)
            tmp.push_back(l[b]);
        res[w] = {lp, logSumExp(tmp.data(), int(tmp.size())) - logSumExp(l, m.n_vocab), int(toks.size())};
    }
    // The candidates' pieces are not kept: next time only the context is shared.
    llama_memory_seq_rm(mem, -1, llama_pos(ctxTok.size()), -1);
    return res;
}

} // namespace

} // namespace swipekb

// The one entry point the engine looks up after opening this library (see nlm_loader.cpp). WHY plain C: a
// name that is easy to find with dlsym. Returns nullptr when the model cannot be loaded; never throws.
extern "C" __attribute__((visibility("default"))) swipekb::WordScorer *
swipekb_nlm_create(const char *path, const swipekb::NeuralLMOptions *options)
{
    try {
        auto s = std::make_unique<swipekb::LlamaScorer>();
        if (!s->load(path ? path : "", *options))
            return nullptr;
        return s.release();
    } catch (...) {
        return nullptr;
    }
}
