/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once
// Turns a verified language profile into the engine's settings. WHY a file of its own: the on-screen keyboard
// plugin and the Gaming Mode daemon must set the engine up in exactly the same way, or the same swipe would give
// different words in Desktop Mode and in Gaming Mode. Both call these functions; neither keeps its own copy.
#include "languageprofile.h"
#include "swipekb/language_engine.hpp"
#include <unordered_map>

namespace swipekb {
// Folder holding the language's files: the installed data folder for English, the language-pack folder
// (SWIPEKB_LANGUAGE_PACKS, else <baseDir>/language-packs) for every other language.
QString languageModelRoot(const QString &locale, const QString &baseDir);
// Accented letters that have no key of their own are swiped through their base key (e.g. é through e).
std::unordered_map<char32_t, char32_t> popupAliasesFor(const QString &language);
// Fills the engine setup for this profile from its verified files (dictionary, n-gram pack, swipe model).
// False when the profile cannot swipe. SWIPEKB_SWIPE_MODEL (manual trials and tests only) replaces the swipe model.
bool configureLanguageEngine(const ProfileSnapshot &profile, LanguageEngineSetup &setup);
} // namespace swipekb
