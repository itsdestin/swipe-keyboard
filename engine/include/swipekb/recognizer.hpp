// swipekb engine — turns a finger path over the on-screen keyboard into ranked word guesses.
// Wraps FUTO Swipe; this header deliberately exposes no FUTO or ExecuTorch types so callers (the Qt
// plugin) never compile against them.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace swipekb {

struct TracePoint {
    float x, y;   // pixels, keyboard coordinates
    float t_ms;   // milliseconds since the swipe started
};

struct KeyRect {
    char letter;          // 'a'..'z' (upper case accepted, lower-cased)
    float x, y, w, h;     // top-left corner and size, same pixel space as the trace
};

struct Candidate {
    std::string word;
    float score;
};

// FUTO's models expect key centers and trace points in 0..1 coordinates of the letter area
// (left edge of the leftmost letter key = 0, right edge of the rightmost = 1; same for rows).
struct NormalizedInput {
    std::string letters;                 // always "abcdefghijklmnopqrstuvwxyz"
    std::vector<float> key_cx, key_cy;   // [26], indexed like `letters`
    std::vector<float> x, y, t;          // trace, same length
};

// nullopt unless all 26 letters are present and the trace has at least 2 points.
std::optional<NormalizedInput> normalize(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &keys);

struct ModelPaths {
    std::string encoder, decoder, lm_model, lm_vocab, dictionary;
    // Standard layout of assets/ and of the installed ~/.local/share/swipe-keyboard.
    static ModelPaths fromDir(const std::string &dir)
    {
        return {dir + "/models/honorable_sturgeon/model_fp32.pte",
                dir + "/models/magic_macaw/model_fp32.pte",
                dir + "/models/hungry_jellyfish/context_lm.pte",
                dir + "/models/hungry_jellyfish/vocab.txt",
                dir + "/dict/en_US_wordlist.combined"};
    }
};

// Tuning knobs. WHY a struct: the replay tool (tools/swipekb-replay) compares settings on recorded swipes;
// the keyboard itself uses the defaults.
struct RecognizerOptions {
    int beam_width = 0;   // how many partial words the search keeps alive; 0 = FUTO's default (100)
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace swipekb
