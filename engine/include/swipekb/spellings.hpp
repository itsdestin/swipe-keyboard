// Spellings — the dictionary's real spellings of a swipe shape ("its" / "it's", "lets" / "let's").
//
// WHY: a swipe never carries apostrophes or hyphens, so one finger path can mean several dictionary words. The guess
// bar offers the other spelling of the best guess (one tap to switch), and tap-to-swap lists it too.
// Capital letters are never added ("I'm" comes out as "i'm") except in German, where noun capitals are part of the
// spelling: the keyboard's own capitalization decides the rest.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace swipekb {

class Spellings {
public:
    // Reads a `.combined` dictionary. Words marked not-a-word (f=0, "nonword", "not_a_word") are ignored, as are
    // spellings with anything but letters and inner ' or - ("us$", "states'"). German keeps the supplied case.
    static Spellings fromCombinedFile(const std::string &path, const std::string &language = "en");
    static Spellings fromCombinedText(const std::string &text, const std::string &language = "en");   // the same, from text (tests)
    // Physical-key alternatives only (œ drawn through o, or as "oe"); the dictionary spelling stays the shown one.
    static std::vector<std::string> recognitionAliases(const std::string &word, const std::string &language);

    // The usual spelling of a letters-only form: its most frequent spelling ("lets" -> "let's",
    // "its" -> "its"); the input itself when the dictionary knows no other.
    std::string spell(const std::string &letters) const;
    // The same swipe's other real spellings worth offering in the guess bar, most frequent first ("its" ->
    // {"it's"}, "let's" -> {"lets"}). Excludes `word` itself; empty for most words.
    std::vector<std::string> others(const std::string &word) const;
    bool empty() const { return byLetters_.empty(); }

private:
    // letters-only, lower case -> real spellings, most frequent first (only where some spelling differs).
    std::unordered_map<std::string, std::vector<std::string>> byLetters_;
    std::unordered_map<std::string, std::string> otherKeys_;
};

} // namespace swipekb
