// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The Plasma 6.9 front end's ONLY door into the language engine (dictionary, swipe, completion, next word).
//
// WHY this file exists: a parallel branch (feature/179-drop-futo) is replacing swipekb::Recognizer with a FUTO-free
// facade, swipekb::LanguageEngine (engine/include/swipekb/language_engine.hpp), plus a FUTO-free resolveProfile() and
// configureLanguageEngine(). Until that lands, this header declares the SAME API in namespace swipekb69 and implements
// it over the clean pieces that already exist (OwnSwipe, Spellings, the dictionary file, the lock-pinned n-gram).
// Switching to the facade is then this one file pair: replace the class with
// `using LanguageEngine = swipekb::LanguageEngine;` and resolveOpenLanguage() with resolveProfile() +
// configureLanguageEngine(). Nothing in here links or loads FUTO code or FUTO models.
#pragma once

#include <QString>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "swipekb/recognizer.hpp" // TracePoint, KeyRect, Candidate only (header types; no FUTO symbol is used)

namespace swipekb69
{
using swipekb::Candidate;
using swipekb::KeyRect;
using swipekb::TracePoint;

// Same fields as the facade's swipekb::LanguageEngineSetup.
struct LanguageEngineSetup {
    std::string language = "en";
    std::string dictionary; // verified .combined
    std::string ngram; // optional SWKBNG2 pack: sentence prior; "" = dictionary frequency
    std::string swipeModel; // optional swipe network; "" or damaged = geometry only
};

// Same API as the facade's swipekb::LanguageEngine. Thread rule: one instance per thread, called from its owner only.
class LanguageEngine
{
public:
    LanguageEngine();
    ~LanguageEngine();
    LanguageEngine(const LanguageEngine &) = delete;
    LanguageEngine &operator=(const LanguageEngine &) = delete;

    bool init(const LanguageEngineSetup &setup);
    bool ready() const;
    bool hasModel() const;
    bool usingNgram() const;
    const char *mode() const; // "own" | "own-geometry"

    // Display spellings, best first.
    std::vector<Candidate> recognize(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &keys, const std::vector<std::string> &context, int topK);
    std::string bestShapeWord() const;
    std::vector<std::string> otherSpellings(const std::string &word) const; // its -> it's
    std::vector<Candidate> complete(const std::string &typedPrefix, int topK = 3) const; // prefix + one-edit corrections
    std::vector<Candidate> similar(const std::string &word, int topK = 12) const; // tap-to-swap neighbours
    bool hasWord(const std::string &word) const;
    bool isOffensive(const std::string &word) const;
    void setLearnedWords(const std::vector<std::pair<std::string, int>> &words); // REPLACES the learned set
    void liftWords(const std::vector<std::pair<std::string, int>> &words); // texting words: only raises

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// What the front end needs to know about one language, resolved from verified files only.
// (Interim stand-in for ProfileSnapshot + configureLanguageEngine; see the top comment.)
struct OpenLanguage {
    QString requestedLocale; // canonical, e.g. "en-US"
    QString language; // "en", "es", "fr", "de"
    bool swipe = false; // dictionary verified: swiping, completion and tap-to-swap work
    bool nextWord = false; // n-gram pack verified: next-word suggestions and context ranking work
    LanguageEngineSetup setup;
    QString nextModel, nextVocab, nextNwp; // next-word worker inputs (empty when !nextWord; nwp optional)
    QString modelIdentity; // changes when any next-word file changes, so the worker reloads
    QString unavailableReason;
};

// `modelRoot` is the installed data folder (SWIPEKB_MODEL_DATA, e.g. /usr/share/swipe-keyboard).
OpenLanguage resolveOpenLanguage(const QString &locale, const QString &modelRoot);

} // namespace swipekb69
