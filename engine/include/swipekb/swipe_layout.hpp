// Key geometry for swipe decoding: which keys exist, where they are, and which key a typed character belongs to.
//
// WHY a plain list of rectangles: nothing in the swipe engine is QWERTY-specific. AZERTY, QWERTZ, Spanish (with
// its own n-tilde key) and so on are different lists; accented letters in a word list fold onto their base key
// unless the layout has a key for them. Written without reference to FUTO code.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace swipekb {

struct SwipeKey {
    char32_t label;          // lower-case character printed on the key
    float cx, cy, hw, hh;    // centre and half size, 0..1 of the letter area (x right, y down)
};

class SwipeLayout {
public:
    SwipeLayout() = default;
    explicit SwipeLayout(std::vector<SwipeKey> keys);
    // Reads "letter<TAB>cx<TAB>cy<TAB>half_w<TAB>half_h" lines (first line is a header). False if unreadable/empty.
    bool loadTsv(const std::string &path);

    const std::vector<SwipeKey> &keys() const { return keys_; }
    size_t size() const { return keys_.size(); }
    // 1 / key width, in layout units: multiply layout x by this to measure in key widths (10 on a 10-key row).
    float unit() const { return unit_; }
    // Key index for a character (accents fold to the base key when this layout has no key for the accent), or -1.
    int keyOf(char32_t c) const;
    // Key sequence of a UTF-8 word with consecutive repeats merged ("see" -> s,e) and unmapped characters
    // (apostrophes, digits, hyphens) dropped. `rawLen` receives the number of mapped letters before merging.
    std::vector<int> wordKeys(std::string_view utf8, int *rawLen = nullptr) const;

private:
    std::vector<SwipeKey> keys_;
    float unit_ = 10.f;
    std::vector<int> byCodepoint_;   // index by code point < 0x180, -1 if none
};

// UTF-8 decoding helpers shared by the swipe code.
std::u32string decodeUtf8(std::string_view s);
// Lower-case + strip the accent when the result is a plain Latin letter ("É" -> 'e'); other characters lower-cased.
char32_t foldChar(char32_t c);
// Lower-case a UTF-8 string keeping accents (ASCII and Latin-1 letters): the identity used to dedupe a word list.
std::string lowerUtf8(std::string_view s);

}
