// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Our own small swipe network and its plain-C++ runner (no ML runtime): trace -> per-step key probabilities.
//
// WHY this shape: the standard published design for gesture typing is a small neural encoder that gives, for each
// moment of the swipe, the probability of each letter (trained with CTC, Graves et al. 2006), followed by a
// lexicon search (swipe_beam.hpp). Keys are given as positions, not as a fixed alphabet, so one network serves
// any layout. Written for this project without reference to other swipe engines' code;
// trained only on openly licensed swipes (scripts/swipe/).
//
// Model file ("SKNN" v1, little endian) is produced by scripts/swipe/swipelib.py:export_model; its layout is read
// in swipe_net.cpp. Input building must match scripts/swipe/swipelib.py:trace_to_input exactly (tested).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "swipekb/swipe_layout.hpp"

namespace swipekb {

struct SwipePoint {
    float x, y;     // 0..1 of the letter area (may fall slightly outside)
    double t;       // milliseconds
};

// What the network sees for one swipe (all float32, row-major).
struct SwipeInput {
    int steps = 0, keyCount = 0;
    std::vector<float> feats;   // steps x 8
    std::vector<float> pos;     // steps x 2, key-width units
    std::vector<float> keys;    // keyCount x 4: centre x, centre y, width, height in key-width units
};

class SwipeNet {
public:
    static constexpr int kInFeatures = 8, kKeyFeatures = 4;
    // False (net stays empty) when missing, truncated or an unknown version; the keyboard then uses the geometric
    // decoder, so a bad file never breaks swiping.
    bool load(const std::string &path);
    bool loadBytes(const std::vector<uint8_t> &bytes);
    bool ready() const { return steps_ > 0; }
    int steps() const { return steps_; }
    size_t parameterCount() const { return params_; }

    // Resample to `steps()` points by arc length and describe each (position, motion, time spent). aspect = canvas
    // height / width. Returns false for an empty trace or layout.
    bool buildInput(const std::vector<SwipePoint> &trace, const SwipeLayout &layout, float aspect, SwipeInput &out) const;
    // Log-probabilities, steps x (1 + keyCount): index 0 is the CTC blank, key k is index k + 1.
    bool forward(const SwipeInput &in, std::vector<float> &logProbs) const;

private:
    struct Conv { int cin, cout; std::vector<float> w /* [k][cin][cout] */, b; };
    struct Dir { std::vector<float> wih /* [in][3H] */, whh /* [H][3H] */, bih, bhh; };
    int steps_ = 0, kernel_ = 0, hidden_ = 0, layers_ = 0, pointer_ = 0;
    size_t params_ = 0;
    std::vector<Conv> convs_;
    std::vector<Dir> dirs_;   // layer-major: layer0 forward, layer0 reverse, layer1 forward, ...
    std::vector<float> A_ /* [2H][P] */, Ab_, B_ /* [4][P] */, Bb_, w_, wb_, blankW_, blankB_;
};

}
