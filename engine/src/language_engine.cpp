// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// LanguageEngine (see language_engine.hpp). Written for this project from its own design; no code from other
// keyboard engines.
#include "swipekb/language_engine.hpp"

#include <QList>
#include <QString>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "swipekb/ngram.hpp"
#include "swipekb/spellings.hpp"
#include "swipekb/swipe_engine.hpp"
#include "swipekb/swipe_lm.hpp"

namespace swipekb {

namespace {

// WHY 72 points per edit on the 0..255 frequency scale (roughly a factor of 15 in how often a word is used): an exact
// match of the typed letters wins unless a correction is a much more common word ("teh" -> "the"), and a correction
// never buries the real completions. A forgotten double letter is the commonest slip ("helo" -> "hello"), a swapped
// pair the next, so they cost less.
constexpr float kEditCost = 72.f;
constexpr float kDoubleLetter = 0.25f, kSwapped = 0.75f;
// WHY corrections start at 3 typed letters: after 1-2 letters almost every short word is one edit away, and the
// guess bar would fill with words the person did not start.
constexpr size_t kCorrectFrom = 3;
constexpr size_t kMaxLetters = 48;

QString nfc(const std::string &s) { return QString::fromStdString(s).normalized(QString::NormalizationForm_C); }

bool isJoiner(QChar c) { return c == QLatin1Char('\'') || c == QChar(0x2019) || c == QLatin1Char('-'); }

// Letters, with a single ' or - only between two letters ("don't", "full-time").
bool wellFormed(const QString &w)
{
    if (w.isEmpty() || !w.front().isLetter() || !w.back().isLetter()) return false;
    for (qsizetype i = 0; i < w.size(); ++i) {
        const QChar c = w[i];
        if (c.isLetter() || c.isMark()) continue;
        if (!isJoiner(c) || i + 1 >= w.size() || !w[i + 1].isLetter()) return false;
    }
    return true;
}

bool lettersOnly(const QString &w)
{
    if (w.isEmpty()) return false;
    for (const QChar c : w) if (!c.isLetter() && !c.isMark()) return false;
    return true;
}

// The comparison form of a word: lower case, accents kept, apostrophes and hyphens dropped (a swipe or a typed
// prefix never has them).
std::u32string keyOf(const QString &w)
{
    std::u32string out;
    for (char32_t c : w.toLower().toUcs4())
        if (!(c == U'\'' || c == 0x2019 || c == U'-')) out.push_back(c);
    return out;
}

std::string utf8(const std::u32string &s) { return QString::fromUcs4(s.data(), qsizetype(s.size())).toStdString(); }

} // namespace

struct LanguageEngine::Impl {
    struct Entry {
        std::string text;        // display spelling
        std::u32string key;      // keyOf(text)
        int base = 0;            // dictionary frequency; 0 = not a word (or not in the dictionary)
        int lift = 0;            // texting-word frequency (only raises)
        int learned = -1;        // learned frequency, overrides; -1 = none
        bool offensive = false;
        int freq() const { return learned >= 0 ? learned : std::max(base, lift); }
    };

    LanguageEngineSetup setup;
    bool keepCase = false, ok = false;
    std::vector<Entry> entries;
    std::unordered_map<std::string, uint32_t> byIdentity;     // lower-cased spelling -> entry
    std::unordered_map<std::u32string, std::vector<uint32_t>> byKey;
    std::vector<char32_t> letters;                             // every letter used by a key (for corrections)
    Spellings spellings;

    // Completion index: entries sorted by key. Rebuilt lazily after words are added (mutable: complete() is const).
    mutable std::vector<uint32_t> sorted;
    mutable bool sortedDirty = true;

    // Swipe side.
    SwipeEngine engine;
    NgramLM lm;
    bool lmLoaded = false, swipeDirty = true;
    std::unique_ptr<NgramWordPrior> prior;
    std::u32string layoutLetters;   // sorted letters of the layout the lexicon was built for
    std::vector<SwipeKey> keys;     // geometry currently set on the engine
    std::string bestShape;

    QString displayForm(const QString &w) const { return keepCase ? w : w.toLower(); }

    uint32_t entryFor(const QString &display)
    {
        const std::string identity = display.toLower().toStdString();
        const auto it = byIdentity.find(identity);
        if (it != byIdentity.end()) return it->second;
        Entry e;
        e.text = display.toStdString();
        e.key = keyOf(display);
        const auto id = uint32_t(entries.size());
        entries.push_back(std::move(e));
        byIdentity.emplace(identity, id);
        byKey[entries.back().key].push_back(id);
        for (char32_t c : entries.back().key)
            if (std::find(letters.begin(), letters.end(), c) == letters.end()) letters.push_back(c);
        sortedDirty = true;
        return id;
    }

    // One `.combined` line: " word=<w>,f=<n>,flags=...,originalFreq=..." (header and shortcut lines skipped).
    void readLine(const std::string &line)
    {
        if (line.compare(0, 6, " word=") != 0) return;
        const size_t comma = line.find(',', 6);
        if (comma == std::string::npos) return;
        const QString word = nfc(line.substr(6, comma - 6));
        if (!wellFormed(word)) return;
        int freq = -1;
        bool notAWord = false, offensive = false;
        std::istringstream fields(line.substr(comma + 1));
        for (std::string field; std::getline(fields, field, ',');) {
            const size_t eq = field.find('=');
            if (eq == std::string::npos) continue;
            const std::string name = field.substr(0, eq), value = field.substr(eq + 1);
            if (name == "f") freq = std::atoi(value.c_str());
            else if (name == "flags") { notAWord |= value.find("nonword") != std::string::npos; offensive |= value.find("offensive") != std::string::npos; }
            else if (name == "not_a_word") notAWord |= value == "true";
            else if (name == "possibly_offensive") offensive |= value == "true";
        }
        if (freq < 0) return;
        // WHY kept with frequency 0: a word the list marks "not a word" ("rn", "pls") may still be brought in by the
        // texting words; until then it is never offered.
        Entry &e = entries[entryFor(displayForm(word))];
        if (!notAWord) {
            const int f = std::clamp(freq, 0, 255);
            // WHY the most frequent capitalisation is shown ("The" vs "the" collapse to one word).
            if (f > e.base) { e.base = f; e.text = displayForm(word).toStdString(); }
        }
        e.offensive |= offensive;
    }

    void ensureSorted() const
    {
        if (!sortedDirty) return;
        sorted.resize(entries.size());
        for (uint32_t i = 0; i < sorted.size(); ++i) sorted[i] = i;
        std::sort(sorted.begin(), sorted.end(), [&](uint32_t a, uint32_t b) { return entries[a].key < entries[b].key; });
        sortedDirty = false;
    }

    // Every entry whose key starts with `prefix` and is at least `minLength` long, offered to `visit`.
    template <typename F> void withPrefix(const std::u32string &prefix, size_t minLength, F &&visit) const
    {
        auto it = std::lower_bound(sorted.begin(), sorted.end(), prefix,
                                   [&](uint32_t e, const std::u32string &p) { return entries[e].key < p; });
        for (; it != sorted.end(); ++it) {
            const auto &k = entries[*it].key;
            if (k.compare(0, prefix.size(), prefix) != 0) break;
            if (k.size() >= minLength) visit(*it);
        }
    }

    // One-edit variants of `s` with their cost (see kEditCost).
    std::vector<std::pair<std::u32string, float>> edits(const std::u32string &s) const
    {
        std::vector<std::pair<std::u32string, float>> out;
        for (size_t i = 0; i < s.size(); ++i) {
            out.push_back({s.substr(0, i) + s.substr(i + 1), 1.f});                                  // extra letter
            if (i + 1 < s.size() && s[i] != s[i + 1]) {
                std::u32string t = s; std::swap(t[i], t[i + 1]); out.push_back({t, kSwapped});         // swapped pair
            }
            for (char32_t c : letters)
                if (c != s[i]) { std::u32string t = s; t[i] = c; out.push_back({t, 1.f}); }            // wrong letter
        }
        for (size_t i = 0; i <= s.size(); ++i)
            for (char32_t c : letters) {                                                               // missing letter
                const bool doubled = (i > 0 && s[i - 1] == c) || (i < s.size() && s[i] == c);
                out.push_back({s.substr(0, i) + c + s.substr(i), doubled ? kDoubleLetter : 1.f});
            }
        return out;
    }

    std::vector<std::pair<std::string, int>> swipeWords() const
    {
        std::vector<std::pair<std::string, int>> out;
        out.reserve(entries.size());
        for (const auto &e : entries) if (e.freq() > 0) out.push_back({e.text, e.freq()});
        return out;
    }

    // Makes the engine match these key rectangles. False if the geometry is unusable.
    bool prepare(const std::vector<KeyRect> &rects, float &x0, float &y0, float &W, float &H)
    {
        if (rects.size() < 2) return false;
        x0 = y0 = 1e30f; float x1 = -1e30f, y1 = -1e30f;
        for (const auto &r : rects) {
            if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.w) || !std::isfinite(r.h) || r.w <= 0 || r.h <= 0) return false;
            x0 = std::min(x0, r.x); y0 = std::min(y0, r.y); x1 = std::max(x1, r.x + r.w); y1 = std::max(y1, r.y + r.h);
        }
        W = x1 - x0; H = y1 - y0;
        if (!(W > 0) || !(H > 0)) return false;
        // WHY sorted by letter: the lexicon stores key *numbers*, so the same letters must always get the same numbers;
        // then a window resize only moves keys and the (slow) word-list build is skipped.
        std::vector<KeyRect> byLetter(rects);
        std::stable_sort(byLetter.begin(), byLetter.end(), [](const KeyRect &a, const KeyRect &b) { return a.letter < b.letter; });
        std::vector<SwipeKey> ks;
        std::u32string seen;
        for (const auto &r : byLetter) {
            if (seen.find(r.letter) != std::u32string::npos) continue;   // a second key with the same letter adds nothing
            seen.push_back(r.letter);
            ks.push_back({r.letter, (r.x + r.w / 2 - x0) / W, (r.y + r.h / 2 - y0) / H, r.w / 2 / W, r.h / 2 / H});
        }
        if (swipeDirty || seen != layoutLetters) {
            engine.setLayout(SwipeLayout(ks));
            if (engine.setWords(swipeWords()) == 0 || !engine.ready()) return false;
            prior.reset();
            if (lmLoaded) prior = std::make_unique<NgramWordPrior>(lm, engine.lexicon());
            layoutLetters = seen; swipeDirty = false;
        } else {
            bool same = ks.size() == keys.size();
            for (size_t i = 0; same && i < ks.size(); ++i)
                same = std::fabs(ks[i].cx - keys[i].cx) < 1e-4f && std::fabs(ks[i].cy - keys[i].cy) < 1e-4f
                    && std::fabs(ks[i].hw - keys[i].hw) < 1e-4f && std::fabs(ks[i].hh - keys[i].hh) < 1e-4f;
            if (!same) engine.setLayout(SwipeLayout(ks));
        }
        keys = std::move(ks);
        return true;
    }

    const Entry *best(const std::u32string &key) const
    {
        const auto it = byKey.find(key);
        if (it == byKey.end()) return nullptr;
        const Entry *b = nullptr;
        for (uint32_t id : it->second)
            if (entries[id].freq() > 0 && (!b || entries[id].freq() > b->freq())) b = &entries[id];
        return b;
    }

    void addRuntime(const std::vector<std::pair<std::string, int>> &words, bool learned)
    {
        for (const auto &[w, f] : words) {
            const QString word = nfc(w);
            if (!lettersOnly(word) || size_t(word.size()) > kMaxLetters) continue;
            Entry &e = entries[entryFor(displayForm(word))];
            const int freq = std::clamp(f, 1, 255);
            if (learned) e.learned = freq; else e.lift = std::max(e.lift, freq);
        }
        swipeDirty = true;
    }
};

LanguageEngine::LanguageEngine() : d_(std::make_unique<Impl>()) {}
LanguageEngine::~LanguageEngine() = default;

bool LanguageEngine::init(const LanguageEngineSetup &setup)
{
    d_ = std::make_unique<Impl>();
    try {
        d_->setup = setup;
        d_->keepCase = setup.language.rfind("de", 0) == 0;
        std::ifstream f(setup.dictionary, std::ios::binary);
        if (setup.dictionary.empty() || !f) return false;
        std::ostringstream text;
        text << f.rdbuf();
        std::istringstream lines(text.str());
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            d_->readLine(line);
        }
        if (d_->swipeWords().empty()) return false;
        d_->spellings = Spellings::fromCombinedText(text.str(), setup.language);
        // WHY these values: the engine was tuned and scored with them (docs/active/specs/2026-10-23-own-swipe-engine.md).
        d_->engine.params.beam.beam = 48;
        d_->engine.params.geoWeight = 40.f;
        d_->engine.params.freqWeight = 0.06f;
        // WHY a missing or damaged model/pack is not an error: the engine then runs on geometry / word frequency.
        if (!setup.swipeModel.empty()) d_->engine.loadModel(setup.swipeModel);
        if (!setup.ngram.empty()) d_->lmLoaded = d_->lm.load(setup.ngram);
        d_->ok = true;
        return true;
    } catch (...) {
        d_ = std::make_unique<Impl>();
        return false;
    }
}

bool LanguageEngine::ready() const { return d_->ok; }
bool LanguageEngine::hasModel() const { return d_->ok && d_->engine.hasModel(); }
bool LanguageEngine::usingNgram() const { return d_->ok && d_->lmLoaded; }
const char *LanguageEngine::mode() const { return hasModel() ? "own" : "own-geometry"; }
std::string LanguageEngine::bestShapeWord() const { return d_->bestShape; }

std::vector<Candidate> LanguageEngine::recognize(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &rects,
                                                 const std::vector<std::string> &context, int topK)
{
    d_->bestShape.clear();
    if (!d_->ok || trace.size() < 2 || topK <= 0) return {};
    try {
        float x0, y0, W, H;
        if (!d_->prepare(rects, x0, y0, W, H)) return {};
        std::vector<SwipePoint> pts;
        pts.reserve(trace.size());
        for (const auto &p : trace) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) return {};
            pts.push_back({(p.x - x0) / W, (p.y - y0) / H, double(p.t_ms)});
        }
        if (d_->prior) {
            // WHY alphaForm: the n-gram file stores lower-case letters-only words.
            std::vector<std::string> previous;
            for (const auto &w : context) { const auto a = NgramLM::alphaForm(w); if (!a.empty()) previous.push_back(a); }
            d_->prior->setContext(previous);
            NgramWordPrior *pr = d_->prior.get();
            d_->engine.setPrior([pr](uint32_t w) { return (*pr)(w); }, d_->prior->hints());
        } else {
            d_->engine.setPrior({}, {});
        }
        std::vector<Candidate> out;
        float bestCtc = -1e30f;
        for (const auto &c : d_->engine.decode(pts, H / W, topK)) {
            const std::string &w = d_->engine.lexicon().text(c.word);
            if (w.empty()) continue;
            out.push_back({w, c.score});
            if (c.ctc > bestCtc) { bestCtc = c.ctc; d_->bestShape = w; }
        }
        return out;
    } catch (...) {
        d_->bestShape.clear();
        return {};
    }
}

std::vector<Candidate> LanguageEngine::complete(const std::string &typed, int topK) const
{
    if (!d_->ok || topK <= 0) return {};
    const auto q = keyOf(nfc(typed));
    if (q.empty() || q.size() > kMaxLetters) return {};
    d_->ensureSorted();
    std::unordered_map<uint32_t, float> score;   // entry -> best score
    auto offer = [&](uint32_t id, float cost) {
        const int f = d_->entries[id].freq();
        if (f <= 0) return;
        const float s = float(f) - kEditCost * cost;
        auto [it, fresh] = score.emplace(id, s);
        if (!fresh) it->second = std::max(it->second, s);
    };
    d_->withPrefix(q, q.size(), [&](uint32_t id) { offer(id, 0.f); });
    if (q.size() >= kCorrectFrom) {
        std::unordered_set<std::u32string> tried{q};
        for (const auto &[variant, cost] : d_->edits(q))
            if (tried.insert(variant).second) d_->withPrefix(variant, q.size(), [&, c = cost](uint32_t id) { offer(id, c); });
    }
    std::vector<std::pair<uint32_t, float>> ranked(score.begin(), score.end());
    std::sort(ranked.begin(), ranked.end(), [&](const auto &a, const auto &b) {
        if (a.second != b.second) return a.second > b.second;
        return d_->entries[a.first].text < d_->entries[b.first].text;
    });
    std::vector<Candidate> out;
    for (const auto &[id, s] : ranked) {
        if (int(out.size()) >= topK) break;
        out.push_back({d_->entries[id].text, s});
    }
    return out;
}

std::vector<Candidate> LanguageEngine::similar(const std::string &word, int topK) const
{
    if (!d_->ok || topK <= 0) return {};
    const auto w = keyOf(nfc(word));
    if (w.size() < 2 || w.size() > kMaxLetters) return {};
    std::vector<uint32_t> found;
    std::unordered_set<std::u32string> tried{w};
    for (const auto &[variant, cost] : d_->edits(w)) {
        if (!tried.insert(variant).second) continue;
        if (const auto *e = d_->best(variant)) found.push_back(uint32_t(e - d_->entries.data()));
    }
    std::sort(found.begin(), found.end(), [&](uint32_t a, uint32_t b) {
        const auto &ea = d_->entries[a], &eb = d_->entries[b];
        return ea.freq() != eb.freq() ? ea.freq() > eb.freq() : ea.text < eb.text;
    });
    std::vector<Candidate> out;
    for (uint32_t id : found) {
        if (int(out.size()) >= topK) break;
        out.push_back({d_->entries[id].text, float(d_->entries[id].freq())});
    }
    return out;
}

std::vector<std::string> LanguageEngine::otherSpellings(const std::string &word) const
{
    return d_->ok ? d_->spellings.others(word) : std::vector<std::string>{};
}

std::string LanguageEngine::displaySpelling(const std::string &word) const
{
    const auto *e = d_->ok ? d_->best(keyOf(nfc(word))) : nullptr;
    return e ? e->text : word;
}

bool LanguageEngine::hasWord(const std::string &word) const
{
    return d_->ok && d_->best(keyOf(nfc(word))) != nullptr;
}

bool LanguageEngine::isOffensive(const std::string &word) const
{
    if (!d_->ok) return false;
    const auto it = d_->byIdentity.find(nfc(word).toLower().toStdString());
    if (it != d_->byIdentity.end()) return d_->entries[it->second].offensive;
    const auto k = d_->byKey.find(keyOf(nfc(word)));
    if (k == d_->byKey.end()) return false;
    return std::any_of(k->second.begin(), k->second.end(), [&](uint32_t id) { return d_->entries[id].offensive; });
}

void LanguageEngine::setLearnedWords(const std::vector<std::pair<std::string, int>> &words)
{
    if (!d_->ok) return;
    for (auto &e : d_->entries) e.learned = -1;
    d_->addRuntime(words, true);
}

void LanguageEngine::liftWords(const std::vector<std::pair<std::string, int>> &words)
{
    if (d_->ok) d_->addRuntime(words, false);
}

} // namespace swipekb
