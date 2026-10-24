// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Which language the daemon serves and how (plan Sections 13 and 15 decision 7). Pure logic with no sockets and no
// models, so every rule is unit-tested (tests/gaming/test_languages.cpp). The heavy lifting (checking language packs,
// building the engine) is the plugin's own code in plugin/languageprofile.* and plugin/languageruntime.*.
#pragma once

#include <QString>
#include <QStringList>
#include <optional>
#include <string>
#include <vector>

#include "swipekb/text.hpp"

namespace gamingd {

struct ProfileChoice {
    QString language;         // "en", "es", "fr" or "de"
    QString physicalLayout;   // the plugin's layout id: "qwerty", "azerty" or "qwertz"
};

// Steam's keyboard layout id (its own names: "qwerty" is English US, "spanish", "french", "german") -> the language
// profile it needs. nullopt for every other layout: other scripts (CJK, Cyrillic, Greek, ...) and Latin layouts we
// have no language pack for. WHY a whitelist and not a blacklist: a layout Valve adds tomorrow must leave the layer off.
std::optional<ProfileChoice> mapSteamLayout(const std::string &steamLayoutId);

// The languages the user enabled in the desktop keyboard's settings, canonical tags ("es-ES"), in the order they
// are stored. WHY this file: Plasma Keyboard's settings page writes `enabledLocales` under [General] in
// `plasmakeyboardrc`; the daemon runs inside the same Flatpak as that page, so the file is in the app's own config
// folder, which the sandbox already gives it (no extra permission). Empty when the file or key is absent.
QStringList readEnabledLocales(const QString &settingsFile);
QString defaultSettingsFile();   // <config home>/plasmakeyboardrc

// What the desktop keyboard does when nothing is enabled: it offers only the system language ("C" counts as en_US).
QStringList effectiveLocales(const QStringList &enabled, const QString &systemLocale);
QString systemLocaleName();

// The first enabled locale of `language`, or an empty string when the user has not enabled that language.
QString pickLocale(const QStringList &enabledCanonical, const QString &language);

// True when the key table can carry the language: every letter of its alphabet has a key, or an accented letter's
// base key (the very check the plugin does before it takes over a swipe). Stops Cyrillic/CJK tables early.
bool geometryCoversLanguage(const std::vector<swipekb::KeyRect> &keys, const QString &language);

} // namespace gamingd
