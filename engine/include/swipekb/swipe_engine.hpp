// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// The swipe engine: trace in, ranked words out. Network + lexicon search + (optional) sentence prior + geometric
// cross-check, with the geometric decoder as the fallback when no usable model file is installed.
//
// WHY one entry point: the keyboard plugin should not know which parts are present. Swiping must never break, so a
// missing/damaged model, or a search that finds nothing, silently falls back to the geometry-only decoder.
// Written for this project from its design doc, without reference to other swipe engines' code.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "swipekb/swipe_beam.hpp"
#include "swipekb/swipe_geo.hpp"
#include "swipekb/swipe_layout.hpp"
#include "swipekb/swipe_net.hpp"

namespace swipekb {

struct EngineParams {
    BeamParams beam;
    GeoParams geo;
    // How much the geometric cost is subtracted from the network score (nats per cost unit). 0 = network only. 40 was best on clean validation (flat between 30 and 45).
    float geoWeight = 40.f;
    // The network's best N words are re-scored with the geometric cost (only matters when geoWeight > 0).
    int rerank = 20;
    // Dictionary frequency prior used by the beam search when no sentence prior is set (nats per point).
    float freqWeight = 0.06f;
    // The network's key probabilities are flattened by this factor before the search (log-probabilities divided by
    // it, then renormalised per step). 1 = as trained. WHY: tap-to-swap searches a made-up, perfectly clean path
    // through the typed word's keys; the network is far surer about such a path than about a real swipe, so words a
    // real swipe could have produced fall out. Used only by LanguageEngine::swapCandidates (SwapParams).
    float temperature = 1.f;
};

class SwipeEngine {
public:
    SwipeEngine() = default;
    // Engines made by copying share the (read-only) layout, lexicon and network; each copy has its own search
    // scratch, so give every thread its own copy.
    SwipeEngine(const SwipeEngine &other);
    SwipeEngine &operator=(const SwipeEngine &) = delete;

    bool loadLayout(const std::string &tsv);
    void setLayout(SwipeLayout layout);
    // "word<TAB>freq" lines; returns the number of lines used (0 = unreadable). Builds the lexicon.
    size_t loadWords(const std::string &tsv);
    // The same from memory (word, 0..255 frequency); returns the number of words offered. WHY: the language engine
    // builds its word list from the parsed dictionary plus learned words, with no temporary file.
    size_t setWords(const std::vector<std::pair<std::string, int>> &words);
    // False leaves the engine in geometric-only mode.
    bool loadModel(const std::string &path);
    bool hasModel() const { return d_ && d_->net.ready(); }
    bool ready() const { return d_ && d_->lex.wordCount() > 0 && d_->layout.size() > 0; }

    const SwipeLexicon &lexicon() const { return d_->lex; }
    const SwipeLayout &layout() const { return d_->layout; }
    EngineParams params;

    // Natural-log prior of a lexicon word in the current sentence (replaces the plain frequency prior), plus the
    // words the context favours. Pass empty functions to go back to frequency.
    void setPrior(std::function<float(uint32_t)> prior, std::vector<uint32_t> hints = {});

    // Best first; score is higher-is-better. Network + search when a model is loaded, else geometry.
    std::vector<SwipeCandidate> decode(const std::vector<SwipePoint> &trace, float aspect, int topN);
    // The path fit (ctc, natural log, the same scale as SwipeCandidate::ctc) of each given word against the network
    // output of the last decode(), whether or not the search found the word. Empty when the last decode() did not use
    // the network (no model, or geometry fallback); -1e30 for a word with no keys on this layout.
    // WHY: tap-to-swap adds words the search did not reach (one-edit neighbours) and must place them on the same
    // path scale as the words it did reach. It is the exact sum over all alignments; the search only lets a step's
    // best `beam.keysPerStep` keys start a letter and prunes, so a word it found scores the same or a little higher
    // here, and a word it could never reach still gets a (low) finite value.
    std::vector<float> pathScores(const std::vector<std::string> &words) const;

private:
    struct Data {
        SwipeLayout layout;
        SwipeLexicon lex;
        SwipeNet net;
        std::unique_ptr<GeoDecoder> geo;
    };
    void rebuild();
    std::shared_ptr<Data> d_;
    std::unique_ptr<SwipeBeamSearch> search_;
    std::function<float(uint32_t)> prior_;
    std::vector<uint32_t> hints_;
    SwipeInput in_;
    std::vector<float> lp_;
    bool lastUsedNet_ = false;   // lp_ / in_ hold the last decode()'s network output
};

}
