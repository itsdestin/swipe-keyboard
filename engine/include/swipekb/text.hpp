// Plain value types shared by every part of the engine and its callers (keyboard plugin, Gaming Mode daemon,
// tools): a finger path, the on-screen key rectangles, a ranked guess, and the "previous words" rule.
//
// WHY a header of its own: callers that only pass swipes and words around (the session logic, the daemon's
// protocol) should not have to include the whole language engine.
#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace swipekb {

struct TracePoint {
    float x, y;   // pixels, keyboard coordinates
    float t_ms;   // milliseconds since the swipe started
};

struct KeyRect {
    char32_t letter;      // Unicode scalar, lower-cased without changing the measured key center
    float x, y, w, h;     // top-left corner and size, same pixel space as the trace
};

struct Candidate {
    std::string word;
    float score;          // higher is better; only comparable within one answer
};

// Key centres and trace in 0..1 coordinates of the letter area (left edge of the leftmost letter key = 0, right
// edge of the rightmost = 1; the same for rows).
struct NormalizedInput {
    std::string letters;                 // UTF-8 profile alphabet (one index per Unicode scalar)
    std::vector<float> key_cx, key_cy;   // [alphabet code point count], never UTF-8 byte count
    std::vector<float> x, y, t;          // trace, same length
};

// nullopt unless every alphabet symbol has measured geometry (or an explicit measured popup base),
// rectangles/trace are finite and well formed, and the trace has at least 2 points.
// WHY the keyboard uses it: before a stock key press is turned into a swipe, it checks that the layout really has
// every letter of the language, so an incomplete layout stays ordinary tapping.
std::optional<NormalizedInput> normalize(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &keys,
                                         const std::string &alphabet = "abcdefghijklmnopqrstuvwxyz",
                                         const std::unordered_map<char32_t, char32_t> &popupAliases = {});

// The words before the cursor that the sentence model should see: only the current sentence (a . ! or ?
// starts a fresh one), split on whitespace, punctuation trimmed off both ends of each word, last `max` kept,
// oldest first. Input and output are UTF-8.
// WHY in the engine and not in the plugin: the live keyboard and the benchmarks must build context with the very
// same rules, or the benchmarks stop measuring what ships.
std::vector<std::string> contextWords(const std::string &textBeforeCursor, int max = 20);

} // namespace swipekb
