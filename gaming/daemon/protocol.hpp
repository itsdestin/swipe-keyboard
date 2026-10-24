// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// swipekb-gamingd wire protocol, version 1. Pure parsing and validation: no sockets, no models, so every
// rule here is unit-tested without starting the daemon (tests/gaming/test_protocol.cpp).
//
// FRAMING: each message is a 4-byte big-endian length, then that many bytes of UTF-8 JSON (one object).
// Both directions. A length of 0 or above kMaxFrame is fatal for the connection (we cannot resynchronise).
//
// REQUESTS ("v" must be 1, "op" names the call). The first request on a connection must be "hello".
//   hello      {token, mode?: "unknown"|"plain"}           -> {session}
//   layout     {keys:[{c:"q",x,y,w,h}, ...], steam_layout?:"qwerty"}
//                                                           -> {keys:N, language, locale, layout}   (M7, see LANGUAGES)
//   swipe      {points:[[x,y,t_ms], ...], context?:[word], top_k?:1..10}
//                                                           -> {req, candidates:[{id,word,score,alt?}, ...]}
//   next_words {words?:[word], partial?:"te", top_k?}       -> {req, candidates:[{id,word,score}]}
//   commit     {req, cand}                                  -> {text}      (cand must be an id we issued for req)
//   reset      {mode?}                                      -> {}          (wipes layout + language + issued candidates)
//   close      {}                                           -> {}          (then the daemon closes the socket)
//   ping       {}                                           -> {}
//   status     {}                                           -> {recognizer_loaded, predictor_loaded, profiles_loaded, connections}
// RESPONSES: {"ok":true,"op":...,...} or {"ok":false,"error":CODE}. CODE is one of the kErr* strings below.
//
// LANGUAGES (plan Section 13, milestone M7). "layout" names Steam's own keyboard layout id in "steam_layout" (absent =
// "qwerty", for callers older than M7). The daemon serves four: qwerty (English US), spanish, french (AZERTY), german
// (QWERTZ); every other id is "unsupported". It then needs the user to have ENABLED that language in the desktop
// keyboard's settings (plasmakeyboardrc, key enabledLocales, re-read on every "layout"; none enabled = the system
// language, as Plasma Keyboard does), and a complete, checksum-verified language pack. A layout that fails any step is
// answered {"ok":false,"error":CODE} and the connection stays on that refusal: swipe and next_words answer the same
// CODE until the next accepted "layout". The layer must switch itself off on these three and leave stock typing alone:
//   unsupported            a layout we do not serve (CJK, Cyrillic, Greek, other Latin layouts), or a key table
//                          that lacks the language's letters (Spanish also needs its n-tilde key)
//   language_not_enabled   supported, but not among the languages the user enabled
//   language_unavailable   enabled, but its pack is missing or fails the plugin's own checks (never an English fallback)
// A successful "layout" also wipes every candidate id issued before it. "language" is en/es/fr/de, "locale" the enabled
// regional tag chosen (the first enabled of that language, e.g. en-GB), "layout" the plugin's physical layout id.
// Before any "layout" the daemon assumes English (qwerty) so next_words keeps working for older callers.
// Candidates are what the keyboard plugin shows for the same input: a wide search, the plugin's regional spelling
// preference (color/colour), cut to top_k, then ONE extra entry marked "alt":true placed second when the dictionary has
// another spelling of the best guess (its / it's). So a reply holds at most top_k entries plus that one. Words are
// exact Unicode text (n-tilde, e-acute, u-umlaut, eszett); input is taken in composed form, so a decomposed letter
// round-trips as the composed one. Capital letters are left to the caller.
//
// MODES (privacy, plan Section 8): "unknown" = we cannot tell what kind of text box this is; "plain" = the
// caller knows it is an ordinary text box. Neither mode ever stores or learns text. They differ only in how
// much context we accept and in a secret-looking-text filter that applies to "unknown" (see wordsAllowed).
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "swipekb/text.hpp"

namespace gamingd {

constexpr int kProtocolVersion = 1;
constexpr uint32_t kMaxFrame = 256 * 1024;   // WHY: a 2,000-point swipe is ~70 KiB of JSON; this leaves headroom.
constexpr size_t kMaxPoints = 2048, kMinPoints = 2, kMaxKeys = 128, kMaxContextWords = 20, kMaxWordBytes = 64;
constexpr size_t kMaxUnknownContextWords = 12;   // WHY smaller: in unknown mode we only want this session's own words.
constexpr int kMaxTopK = 10, kDefaultTopK = 3;
constexpr double kMaxCoord = 1e5, kMaxTimeMs = 1e7;

// Error codes. Literals only: the logger takes string literals and must never be handed typed text.
constexpr const char *kErrBadFrame = "bad_frame", *kErrBadJson = "bad_json", *kErrBadRequest = "bad_request",
    *kErrUnauthorized = "unauthorized", *kErrUnknownOp = "unknown_op", *kErrNoLayout = "no_layout",
    *kErrUnknownCandidate = "unknown_candidate", *kErrModelUnavailable = "model_unavailable",
    *kErrVersion = "unsupported_version", *kErrTooMany = "too_many_connections",
    // Language answers to a "layout" request (plan Section 13): the layer treats each as "stay off, stock typing".
    *kErrUnsupported = "unsupported",                  // a layout/script we do not serve (CJK, Cyrillic, other Latin layouts)
    *kErrLanguageOff = "language_not_enabled",         // supported, but the user has not enabled that language
    *kErrLanguageUnavailable = "language_unavailable"; // enabled, but its language pack is missing or fails its checks

enum class Mode { Unknown, Plain };

// Result of validating one field group: either a value or an error code (never both).
template <class T> struct Parsed {
    std::optional<T> value;
    const char *error = nullptr;
};

// Frame body -> JSON object. Error is kErrBadJson (or kErrVersion when "v" is not 1).
Parsed<QJsonObject> parseBody(const QByteArray &body);
Parsed<std::vector<swipekb::KeyRect>> parseLayout(const QJsonObject &req);
// Steam's keyboard layout id from a "layout" request ("qwerty", "french", "russian", ...). Absent means "qwerty" so
// callers written before M7 keep working. Only the shape is checked here; whether we serve it is languages.hpp's call.
Parsed<std::string> parseSteamLayout(const QJsonObject &req);

struct SwipeArgs {
    std::vector<swipekb::TracePoint> trace;
    std::vector<std::string> context;
    int topK = kDefaultTopK;
    bool contextDropped = false;   // unknown mode saw secret-looking context: it was discarded, swipe still runs
};
Parsed<SwipeArgs> parseSwipe(const QJsonObject &req, Mode mode);

struct NextWordsArgs {
    std::vector<std::string> words;
    std::string partial;
    int topK = kDefaultTopK;
    bool suppressed = false;   // unknown mode saw secret-looking text: answer with no suggestions at all
};
Parsed<NextWordsArgs> parseNextWords(const QJsonObject &req, Mode mode);

Parsed<Mode> parseMode(const QJsonObject &req, Mode fallback);

// True when `word` looks like a password or token rather than language: 8+ characters with at least three of
// lower case, upper case, digit, symbol; or 16+ characters with no vowels. WHY: Steam's keyboard over a game gives
// no field-type signal (plan Section 8), so in "unknown" mode a secret-looking run must not even be shown as
// context to a model. Heuristic, deliberately blunt: a false positive only hides a suggestion.
bool looksLikeSecret(const std::string &word);

// Big-endian length prefix + body; empty result if body exceeds kMaxFrame.
QByteArray encodeFrame(const QByteArray &body);

} // namespace gamingd
