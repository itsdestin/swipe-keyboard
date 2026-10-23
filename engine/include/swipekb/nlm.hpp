// swipekb neural word model — "how likely is this word, given the whole sentence so far", from a small
// neural language model (a GGUF file run by llama.cpp on the CPU).
//
// WHY on top of the word-pair/triple model (ngram.hpp): that model only sees the last two words. Many of the
// remaining "right word was second or third" misses need the whole sentence ("Bach, who was cantor ..." vs
// "back"). Running a network is far slower than a table lookup, so it only ever looks at the handful of guesses
// the cheap ranking already put on top (see nlmRerank in recognizer.hpp).
//
// WHY the model code lives in its own library (libswipekb_nlm.so) that is opened only when needed: if
// llama.cpp is ever uninstalled, a hard link to it would stop the whole keyboard from starting. Opened on
// demand, a missing library just means "no neural model", and swiping works as before.
//
// No llama.cpp types here: callers (the recognizer, tools, tests) never compile against llama.cpp.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace swipekb {

// One word's score from the neural model.
struct WordScore {
    float lp = 0;        // natural log P(the word's pieces | sentence so far)
    float word_end = 0;  // natural log P(the word ends there: the next piece starts with a space or punctuation)
    int tokens = 1;      // how many pieces (tokens) the model spelled the word with
};

// What the re-ranking needs from a neural model. WHY an interface: the ranking rules are tested with a
// made-up scorer (no model file, no llama.cpp), and a bigger model can be swapped in later.
class WordScorer {
public:
    virtual ~WordScorer() = default;
    // One score per word, for the text continuing with that word after `context` (the sentence so far, oldest
    // word first; empty = start of a sentence). Empty on failure.
    virtual std::vector<WordScore> score(const std::vector<std::string> &context,
                                         const std::vector<std::string> &words) = 0;
};

struct NeuralLMOptions {
    // WHY 4 threads: the keyboard shares the machine with whatever the user is doing. Measured on the busy
    // test machine (step1b-nlm-report.md): 1 thread ~89 ms, 2 ~57, 4 ~32-35, 8 ~21-23 ms per swipe (median) —
    // more threads help, but each one taken from other programs while the user types.
    int threads = 4;
    // How many sentence pieces (tokens) of context the model reads at most. WHY a cap: cost grows with it,
    // and words far back barely change which of a few guesses fits.
    int max_context_tokens = 48;
    // Keep the last context in the model's memory and only run the new part next time. WHY switchable: the
    // speed test measures both the typing case (context grows a word at a time) and the worst case.
    bool reuse_context = true;
};

// Opens libswipekb_nlm.so (next to the engine library) and loads the model file on the CPU. nullptr when the
// file or the library is missing or unusable; the reason goes to stderr. Never throws.
std::unique_ptr<WordScorer> loadNeuralLM(const std::string &modelPath, const NeuralLMOptions &options = {});

} // namespace swipekb
