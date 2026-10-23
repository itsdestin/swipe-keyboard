#pragma once
// Turns a verified language profile into the engine's settings. WHY a file of its own: the on-screen keyboard
// plugin and the Gaming Mode daemon must set the engine up in exactly the same way, or the same swipe would give
// different words in Desktop Mode and in Gaming Mode. Both call these functions; neither keeps its own copy.
#include "languageprofile.h"
#include "swipekb/recognizer.hpp"
#include <unordered_map>

namespace swipekb {
// Folder holding the language's model files: the installed model folder for English, the language-pack folder
// (SWIPEKB_LANGUAGE_PACKS, else <baseDir>/language-packs) for every other language.
QString languageModelRoot(const QString &locale, const QString &baseDir);
// Accented letters that have no key of their own are swiped through their base key (e.g. é through e).
std::unordered_map<char32_t, char32_t> popupAliasesFor(const QString &language);
// Fills the model paths and options the engine needs for this profile. False when the profile cannot swipe.
// The caller may still adjust the result (the plugin's SWIPEKB_NO_DECODER switch does).
bool configureRecognizer(const ProfileSnapshot &profile, ModelPaths &paths, RecognizerOptions &options);
} // namespace swipekb
