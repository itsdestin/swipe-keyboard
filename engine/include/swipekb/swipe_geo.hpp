// Geometric swipe decoder: the fallback used when no network file is installed (and a sanity check for tests).
//
// WHY keep it: swiping must never break because a model file is missing, damaged or from a newer version. This
// decoder needs only the key positions and the word list. Method (published ideas: SHARK2 location channel,
// dynamic time warping): compare the finger path with each candidate word's path through its key centres by banded
// DTW (touches inside a key's core cost nothing), add a "every letter was visited" term and a frequency prior.
// Measured on real English swipes: ~82% first guess / ~90% top 3. Written for this project from its design doc, without reference to other swipe engines' code.
#pragma once

#include <vector>

#include "swipekb/swipe_beam.hpp"
#include "swipekb/swipe_layout.hpp"
#include "swipekb/swipe_net.hpp"

namespace swipekb {

struct GeoParams {
    int steps = 40;            // equal-length resample points for path and templates
    int band = 8;              // DTW half band (in steps)
    float radius = 0.45f;      // key widths: distance inside this costs nothing
    float coverRadius = 0.4f;  // key widths: letter counts as visited inside this
    float coverWeight = 0.01f; // per key width of a missed letter
    float freqWeight = 0.0005f;// per frequency point (0..255)
    int endKeys = 6;           // nearest keys to the first / last touch that may start / end a word
    float lenLo = 0.55f, lenHi = 1.8f;   // allowed word-path length relative to the swipe length (with slack)
};

// A swipe prepared once so many candidate words can be costed against it cheaply.
struct GeoPoint { float x, y; };
struct GeoPrepared {
    std::vector<GeoPoint> path;   // resampled finger path, key-width units
    std::vector<float> miss;      // per key: how far the path stayed from it, beyond the cover radius
    float length = 0;
    bool valid = false;
};

class GeoDecoder {
public:
    // Both must outlive the decoder. Builds the (first key, last key) word index once.
    GeoDecoder(const SwipeLexicon &lex, const SwipeLayout &layout);
    // Ranked words, best first; score = -(geometric cost), higher is better. Empty for an empty trace.
    std::vector<SwipeCandidate> decode(const std::vector<SwipePoint> &trace, float aspect, int topN, const GeoParams &p = {}) const;

    // Cost (lower is better) of one lexicon word against a prepared swipe: DTW + coverage, no frequency term. Used to
    // sanity-check the network's candidates (a blend of both is more robust to swipers unlike the training data).
    GeoPrepared prepare(const std::vector<SwipePoint> &trace, float aspect, const GeoParams &p = {}) const;
    float cost(const GeoPrepared &swipe, uint32_t word, float aspect, const GeoParams &p = {}) const;

private:
    const SwipeLexicon &lex_;
    const SwipeLayout &layout_;
    std::vector<std::vector<uint32_t>> bucket_;   // words by first * keyCount + last
};

}
