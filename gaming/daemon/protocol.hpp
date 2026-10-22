// swipekb-gamingd wire protocol, version 1. Pure parsing and validation: no sockets, no models, so every
// rule here is unit-tested without starting the daemon (tests/gaming/test_protocol.cpp).
//
// FRAMING: each message is a 4-byte big-endian length, then that many bytes of UTF-8 JSON (one object).
// Both directions. A length of 0 or above kMaxFrame is fatal for the connection (we cannot resynchronise).
//
// REQUESTS ("v" must be 1, "op" names the call). The first request on a connection must be "hello".
//   hello      {token, mode?: "unknown"|"plain"}           -> {session}
//   layout     {keys:[{c:"q",x,y,w,h}, ...]}                -> {keys:N}
//   swipe      {points:[[x,y,t_ms], ...], context?:[word], top_k?:1..10}
//                                                           -> {req, candidates:[{id,word,score}]}
//   next_words {words?:[word], partial?:"te", top_k?}       -> {req, candidates:[{id,word,score}]}
//   commit     {req, cand}                                  -> {text}      (cand must be an id we issued for req)
//   reset      {mode?}                                      -> {}          (wipes layout + issued candidates)
//   close      {}                                           -> {}          (then the daemon closes the socket)
//   ping       {}                                           -> {}
//   status     {}                                           -> {recognizer_loaded, predictor_loaded, connections}
// RESPONSES: {"ok":true,"op":...,...} or {"ok":false,"error":CODE}. CODE is one of the kErr* strings below.
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

#include "swipekb/recognizer.hpp"

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
    *kErrVersion = "unsupported_version", *kErrTooMany = "too_many_connections";

enum class Mode { Unknown, Plain };

// Result of validating one field group: either a value or an error code (never both).
template <class T> struct Parsed {
    std::optional<T> value;
    const char *error = nullptr;
};

// Frame body -> JSON object. Error is kErrBadJson (or kErrVersion when "v" is not 1).
Parsed<QJsonObject> parseBody(const QByteArray &body);
Parsed<std::vector<swipekb::KeyRect>> parseLayout(const QJsonObject &req);

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
