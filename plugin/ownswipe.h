// OwnSwipe — glue between the keyboard plugin and the project's own swipe engine (engine/include/swipekb/swipe_*.hpp).
//
// WHY a separate class: the plugin talks to FUTO's recogniser through swipekb::Recognizer. The clean engine needs
// different inputs (key layout in 0..1 of the letter area, a word list, an optional n-gram prior, a model file), so
// this class turns what the plugin already has (live key rectangles, the verified dictionary, the profile's n-gram
// pack, the context words) into those inputs. It is opt-in; FUTO stays the default (see requested()).
//
// WHY nothing here throws or blocks swiping: every failure returns "no answer" and the plugin falls back to FUTO.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "swipekb/recognizer.hpp"   // TracePoint, KeyRect

namespace swipekb {

struct OwnSwipeSetup {
    std::string dictionary;   // the language's verified `.combined` dictionary
    std::string ngram;        // the profile's n-gram pack ("" = none: dictionary frequency is the prior)
    std::string model;        // swipe-model.bin ("" or unreadable/damaged = geometry-only)
    bool keepCase = false;    // German keeps noun capitals; every other language is lower-cased like FUTO's path
};

class OwnSwipe {
public:
    OwnSwipe();
    ~OwnSwipe();
    OwnSwipe(const OwnSwipe &) = delete;
    OwnSwipe &operator=(const OwnSwipe &) = delete;

    // Opt-in switch: an empty file `own-engine` in the keyboard's data dir, or SWIPEKB_ENGINE=own (tests).
    static bool requested(const std::string &dataDir);

    // Cheap: remembers paths, opens the n-gram file, loads the model. False = nothing usable (caller uses FUTO).
    bool init(const OwnSwipeSetup &setup);
    bool hasModel() const;      // false = geometry-only
    bool usingNgram() const;
    // "own" (network + search) or "own-geometry" (no usable model file).
    const char *mode() const { return hasModel() ? "own" : "own-geometry"; }

    struct Result {
        std::vector<std::string> words;   // best first, display spelling (apostrophes/accents from the dictionary)
        std::string bestShape;            // the word whose path fits the swipe best, before the sentence prior
    };
    // False (and `out` empty) when it cannot answer for any reason, including bad geometry or any exception.
    bool recognize(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &keys,
                   const std::vector<std::string> &context, int topN, Result &out);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}
