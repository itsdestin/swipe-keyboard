// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Spellings — the dictionary's real spellings of a swipe shape ("its" / "it's", "lets" / "let's").
//
// WHY: a swipe never carries apostrophes or hyphens, so one finger path can mean several dictionary words. The guess
// bar offers the other spelling of the best guess (one tap to switch), and tap-to-swap lists it too.
// Spellings are lower case except in German, where noun capitals are part of the spelling; Capitals (below) puts
// back the capitals a word nearly always has ("I'm", "Paris") in the other languages.
#pragma once

#include <memory>
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

// Capitals — which words are shown with a capital letter in English, Spanish and French (packaging/capitals/LANG.tsv:
// "i\tI", "paris\tParis", "iphone\tiPhone"; counted from web text by scripts/lm/capitals.py).
//
// WHY a list of words nearly always capitalised, not the dictionary's most frequent form: web text capitalises many
// ordinary words inside names ("Banco Santander", "Hall of Fame"), so "Banco" can be a word's most common form while
// someone typing "el banco" wants lower case. Sentence-start and Shift capitals stay the keyboard's job.
class Capitals {
public:
    // "word<TAB>Form[<TAB>...]" lines; '#' lines are comments. A Form whose letters differ from the word is skipped.
    // nullptr when no line is usable (an empty list).
    static std::shared_ptr<const Capitals> fromTsv(const std::string &text);
    // The listed form of `word` ("i'm" or "I'M" -> "I'm"), otherwise `word` in lower case. Keeps the word's own
    // apostrophe (' or ’).
    std::string apply(const std::string &word) const;
    size_t size() const { return forms_.size(); }

private:
    std::unordered_map<std::string, std::string> forms_;   // lower case, ' apostrophes -> form
};

} // namespace swipekb
