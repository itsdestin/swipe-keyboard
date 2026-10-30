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
#include "swipekb/swap.hpp"
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
// WHY 80 frequency points: the same closeness that decides which other spellings are offered (spelling.cpp), so a
// spelling that can win a swipe is exactly one that is also offered as the alternative ("we're" for "were"), and
// rare junk sharing a common word's path ("I's" for "is", 121 points apart) never can.
constexpr int kSpellingWithin = 80;
// Sentence words for tap-back lists (SwapParams::tapBackSentenceWords), see swapCandidates(). WHY caps: the list is built on
// the input thread.
constexpr size_t kSentenceScan = 200000;      // continuations looked at (all of them in practice)
constexpr size_t kSentenceShortlist = 256;    // the best of them by the words before, scored with the words after

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

// One letter three or more times in a row ("youuuuu", "waaaay"): a stretched spelling, never a likely alternative.
bool stretched(const std::string &w)
{
    int run = 1;
    const auto u = QString::fromStdString(w).toLower().toUcs4();
    for (qsizetype i = 1; i < u.size(); ++i) {
        run = u[i] == u[i - 1] ? run + 1 : 1;
        if (run >= 3) return true;
    }
    return false;
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
        bool capitalised = false; // the dictionary's most frequent form starts with a capital (names, places)
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

    AlternativeParams alt;
    ContextRescore rescore;   // off unless a scorer is set (setContextRescore)
    std::shared_ptr<const Capitals> capitals;
    QString displayForm(const QString &w) const
    {
        if (keepCase) return w;
        return capitals ? QString::fromStdString(capitals->apply(w.toStdString())) : w.toLower();
    }

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
            if (f > e.base) { e.base = f; e.text = displayForm(word).toStdString(); e.capitalised = word.front().isUpper(); }
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
        // WHY one word per key unless the sentence model can tell spellings apart: "is" and "I's", "were" and "we're"
        // leave the same finger path. A letters-only model scores them as one word, so as separate swipe words they
        // tied and the apostrophe form won on alphabetical order ("i's", "wa's", "i'n"); then the most frequent
        // spelling is the swipe word and the others stay one tap away through otherSpellings(). A model with
        // apostrophes scores each spelling from the sentence ("we were" / "we're going"), so close spellings all
        // stay swipe words and the context picks. (A handicap for the rarer spelling, tried on 3,000 held-out Tatoeba
        // sentences per language, only lowered the right-first-time rate: the sentence model's choice is better.)
        const bool distinct = lmLoaded && lm.apostrophes();
        std::vector<std::pair<std::string, int>> out;
        out.reserve(byKey.size());
        for (const auto &[key, ids] : byKey) {
            const Entry *top = best(key);
            if (!top) continue;
            out.push_back({top->text, top->freq()});
            if (!distinct) continue;
            // WHY also a different model word: "e-mail"/"email" differ only by a hyphen, which the model drops too, so
            // they would tie again.
            const std::string topForm = lm.form(top->text);
            for (uint32_t id : ids) {
                const Entry &e = entries[id];
                if (&e != top && e.freq() > 0 && e.freq() >= top->freq() - kSpellingWithin && lm.form(e.text) != topForm)
                    out.push_back({e.text, e.freq()});
            }
        }
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

    // The finger path in the engine's 0..1 units, after making the engine match these keys; `aspect` = height/width.
    // False when the keys or the path are unusable.
    // WHY shared by recognize() and swapCandidates(): a swap list must see a swipe exactly as recognition did.
    bool toSwipePoints(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &rects,
                       std::vector<SwipePoint> &pts, float &aspect)
    {
        float x0, y0, W, H;
        if (!prepare(rects, x0, y0, W, H)) return false;
        pts.clear();
        pts.reserve(trace.size());
        for (const auto &p : trace) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
            pts.push_back({(p.x - x0) / W, (p.y - y0) / H, double(p.t_ms)});
        }
        aspect = H / W;
        return true;
    }

    // Gives the search the sentence prior for these previous words (word frequency when there is no n-gram pack).
    void setSentencePrior(const std::vector<std::string> &context)
    {
        if (prior) {
            // WHY the model's own form: the n-gram file stores lower-case words, letters only or with apostrophes.
            std::vector<std::string> previous;
            for (const auto &w : context) { const auto a = lm.form(w); if (!a.empty()) previous.push_back(a); }
            prior->setContext(previous);
            NgramWordPrior *pr = prior.get();
            engine.setPrior([pr](uint32_t w) { return (*pr)(w); }, prior->hints());
        } else {
            engine.setPrior({}, {});
        }
    }

    // The prior the search added to this word's score: the sentence (n-gram) prior, or plain frequency without a pack
    // (the search's own fallback, BeamParams::freqWeight = EngineParams::freqWeight).
    float priorOf(uint32_t w) const { return prior ? (*prior)(w) : engine.params.freqWeight * float(engine.lexicon().freq(w)); }

    // ContextRescore: re-scores the first `rescore.pool` found words with the neural next-word model, re-sorts them,
    // and cuts the list to `keep` (the size recognize() would have asked for without re-scoring).
    // WHY cut back to `keep`: the guesses after the first are chosen from that many words (AlternativeParams::pool),
    // so re-scoring must not quietly widen that choice too.
    // WHY a failed or short answer leaves the list as found: swiping must never break because of the extra model.
    void rescoreFound(std::vector<SwipeCandidate> &found, const std::vector<std::string> &context, size_t keep) const
    {
        constexpr float kLn10 = 2.302585093f;
        const size_t n = std::min(found.size(), size_t(std::max(0, rescore.pool)));
        std::vector<std::string> words;
        words.reserve(n);
        for (size_t i = 0; i < n; ++i) words.push_back(engine.lexicon().text(found[i].word));
        std::vector<float> neural;
        try { neural = rescore.scorer(context, words); } catch (...) { neural.clear(); }
        if (n > 0 && neural.size() == n) {
            for (size_t i = 0; i < n; ++i) {
                // WHY a word without a neural score (NaN: the next-word vocabulary lacks it, e.g. a word the person
                // taught) takes the n-gram's own estimate in its place: the floor would push a learned word below
                // every other guess. For English (n-gram weight 0, neural 1) its score stays exactly as found.
                // Without an n-gram pack the prior is plain frequency, not a probability: such a word keeps its score.
                if (!std::isfinite(neural[i])) {
                    if (prior) found[i].score += (rescore.ngramWeight - 1.f + rescore.neuralWeight) * priorOf(found[i].word);
                    continue;
                }
                found[i].score += (rescore.ngramWeight - 1.f) * priorOf(found[i].word) + rescore.neuralWeight * kLn10 * neural[i];
            }
            // WHY stable and only the re-scored part: equal scores keep the search's order, and words past the pool
            // have no neural score, so they stay behind the re-scored ones.
            std::stable_sort(found.begin(), found.begin() + std::ptrdiff_t(n),
                             [](const SwipeCandidate &a, const SwipeCandidate &b) { return a.score > b.score; });
        }
        if (found.size() > keep) found.resize(keep);
    }

    // May this found word be offered as an alternative (recognize) or a swap? `f` = its frequency (0 = unknown).
    // WHY shared: the swap list and the guess bar must agree on which words are junk.
    bool mayOffer(const std::string &w, int minFrequency, int &f) const
    {
        const auto it = byIdentity.find(QString::fromStdString(w).toLower().toStdString());
        const Entry *e = it == byIdentity.end() ? nullptr : &entries[it->second];
        f = e ? e->freq() : 0;
        // WHY learned and texting words are never filtered: the person (or the texting list) vouches for them.
        const bool vouched = e && (e->learned >= 0 || e->lift > 0);
        if (vouched) return true;
        if (f < minFrequency) return false;
        if (e && e->capitalised && !keepCase && f < alt.nameMinFrequency) return false;
        if (alt.dropStretched && stretched(w)) return false;
        return true;
    }

    const Entry *best(const std::u32string &key) const
    {
        const auto it = byKey.find(key);
        if (it == byKey.end()) return nullptr;
        const Entry *b = nullptr;
        // WHY on a tie the plain spelling: it is the form the swipe (or the typed letters) actually spelled.
        const auto plain = [&](const Entry &e) { return utf8(e.key) == QString::fromStdString(e.text).toLower().toStdString(); };
        for (uint32_t id : it->second) {
            const Entry &e = entries[id];
            if (e.freq() <= 0) continue;
            if (!b || e.freq() > b->freq() || (e.freq() == b->freq() && plain(e) && !plain(*b))) b = &e;
        }
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
        // WHY English only for the frequency rules: measured on real HeliBoard swipes (2026-10-24), the rare-word
        // cutoff and the extra sentence weight cost 2-5 points of "right word in the top 3" in Spanish, French and
        // German (their word lists spread frequencies differently), while dropping stretched spellings cost nothing.
        if (setup.language.rfind("en", 0) != 0) d_->alt = {16, 0.f, 0, 0, 1e9f, true};
        std::ifstream f(setup.dictionary, std::ios::binary);
        if (setup.dictionary.empty() || !f) return false;
        std::ostringstream text;
        text << f.rdbuf();
        std::istringstream lines(text.str());
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            d_->readLine(line);
        }
        // WHY a plain check and not swipeWords().empty(): that builds the whole swipe word list (~0.1 s for English) only
        // to throw it away; the first swipe builds it anyway. Same answer: a word list with no word of frequency > 0
        // can never be swiped.
        if (std::none_of(d_->entries.begin(), d_->entries.end(), [](const auto &e) { return e.freq() > 0; })) return false;
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
        std::vector<SwipePoint> pts;
        float aspect = 0;
        if (!d_->toSwipePoints(trace, rects, pts, aspect)) return {};
        d_->setSentencePrior(context);
        std::vector<Candidate> out;
        float bestCtc = -1e30f;
        // ContextRescore (off by default): re-score the search's best words with the neural next-word model. WHY only
        // with words before unless told otherwise: see ContextRescore::needWordsBefore.
        const int want = std::max(topK, d_->alt.pool);
        const bool rescoring = bool(d_->rescore.scorer) && (!d_->rescore.needWordsBefore || !context.empty());
        auto found = d_->engine.decode(pts, aspect, rescoring ? std::max(want, d_->rescore.pool) : want);
        if (rescoring) d_->rescoreFound(found, context, size_t(want));
        for (const auto &c : found) {
            const std::string &w = d_->engine.lexicon().text(c.word);
            if (!w.empty() && c.ctc > bestCtc) { bestCtc = c.ctc; d_->bestShape = w; }
        }
        // WHY the first guess as found: it is the best finger-path-plus-sentence match. The places after it are
        // re-chosen (see AlternativeParams): a word only belongs there if the person could plausibly have meant it.
        const AlternativeParams &p = d_->alt;
        std::vector<std::pair<float, Candidate>> others;
        float firstScore = 0;
        for (const auto &c : found) {
            const std::string &w = d_->engine.lexicon().text(c.word);
            if (w.empty()) continue;
            if (out.empty()) { out.push_back({w, c.score}); firstScore = c.score; continue; }
            if (c.score < firstScore - p.margin) continue;
            int f = 0;
            if (!d_->mayOffer(w, p.minFrequency, f)) continue;
            const float prior = d_->prior ? (*d_->prior)(c.word) : d_->engine.params.freqWeight * float(f);
            // WHY only with earlier words: measured (2026-10-24), the extra weight makes alternatives read like the
            // sentence ("you" -> "your", "to"), but with no sentence it only favours common words over the rarer
            // word that was swiped (HeliBoard English, no sentences: 87.96% -> 89.05% in the top 3 without it).
            const float weight = context.empty() ? 0.f : p.sentenceWeight;
            others.push_back({c.score + weight * prior, {w, c.score}});
        }
        std::stable_sort(others.begin(), others.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        for (const auto &[rank, c] : others) {
            if (int(out.size()) >= topK) break;
            out.push_back(c);
        }
        return out;
    } catch (...) {
        d_->bestShape.clear();
        return {};
    }
}

std::vector<SwapCandidate> LanguageEngine::swapCandidates(const std::string &word, const std::vector<KeyRect> &rects,
                                                         const std::vector<std::string> &before,
                                                         const std::vector<std::string> &after,
                                                         const std::vector<TracePoint> &trace, const SwapParams &sp)
{
    if (!d_->ok || word.empty() || sp.pool <= 0) return {};
    try {
        // WHY a made-up path for a typed word (or a swipe no longer remembered): the swap list is about words whose
        // swipe looks like this one ("good" -> "food", "hood"), so the search needs a path to compare against.
        const auto t = trace.size() >= 2 ? trace : syntheticTrace(word, rects);
        std::vector<SwipePoint> pts;
        float aspect = 0;
        if (t.size() < 2 || !d_->toSwipePoints(t, rects, pts, aspect)) return {};
        // WHY the sentence prior during the search too: the search keeps only its likeliest words on the way, and
        // words that fit the sentence must survive that to be compared at all (as in recognize()).
        // SwapParams::searchPrior / beam change this for measurement (tools/swipekb-swap-bench --diagnose); the
        // recognizer's own settings are put back afterwards, so a swap list never changes the next swipe.
        // The search's own settings are changed only inside this block; `restore` puts them back when it ends,
        // right after the search (later steps, such as the neighbours' path fits, run with the recognizer's settings).
        decltype(d_->engine.decode(pts, aspect, sp.pool)) found;
        {
            // WHY a guard object, not three lines after decode(): if the search throws (out of memory on a huge pool),
            // the catch below returns an empty list, and the recognizer must still get its own width, frequency weight
            // and temperature back, or every later swipe would be searched with the swap list's settings.
            struct Restore {
                SwipeEngine &e;
                const BeamParams beam;
                const float freq, temp;
                ~Restore() { e.params.beam = beam; e.params.freqWeight = freq; e.params.temperature = temp; }
            } restore{d_->engine, d_->engine.params.beam, d_->engine.params.freqWeight, d_->engine.params.temperature};
            if (sp.beam > 0) d_->engine.params.beam.beam = sp.beam;
            if (trace.size() < 2) d_->engine.params.temperature = sp.syntheticTemperature;
            if (sp.searchPrior == 0) d_->setSentencePrior(before);
            else if (sp.searchPrior == 1) d_->engine.setPrior({}, {});
            else {
                d_->engine.setPrior([](uint32_t) { return 0.f; }, {});
                d_->engine.params.beam.lookahead = 0.f;
                d_->engine.params.freqWeight = 0.f;
            }
            found = d_->engine.decode(pts, aspect, sp.pool);
        }

        const QString tappedLow = QString::fromStdString(word).toLower();
        const QString shownLow = QString::fromStdString(displaySpelling(word)).toLower();
        // WHY only the word itself (any case) is left out, as in rankSwap(): its dictionary spelling is a real swap
        // ("dont" typed -> "don't" offered).
        const auto isTapped = [&](const std::string &w) { return QString::fromStdString(w).toLower() == tappedLow; };
        // The decoded word that stands for the tapped one: itself, or its dictionary spelling ("dont" -> "don't").
        const auto standsForTapped = [&](const std::string &w) {
            const QString low = QString::fromStdString(w).toLower();
            return low == tappedLow || low == shownLow;
        };
        // WHY the path alone (ctc) and not the search score: the score already holds the sentence prior, which
        // rankSwap() weighs separately (before/after). The reference is the tapped word's own fit to this path (its
        // other spellings share that path exactly); when the search did not find the tapped word, the best fit found.
        float ref = -1e30f, bestCtc = -1e30f;
        for (const auto &c : found) {
            const std::string &w = d_->engine.lexicon().text(c.word);
            if (w.empty()) continue;
            bestCtc = std::max(bestCtc, c.ctc);
            if (standsForTapped(w)) ref = std::max(ref, c.ctc);
        }
        if (ref <= -1e29f) ref = bestCtc > -1e29f ? bestCtc : 0.f;

        std::vector<SwapCandidate> out;
        std::unordered_set<std::string> seen;   // lower-case spellings already in `out`
        const auto add = [&](const std::string &w, float path) {
            if (int(out.size()) >= sp.pool || isTapped(w)) return;
            if (!seen.insert(QString::fromStdString(w).toLower().toStdString()).second) return;
            SwapCandidate c;
            c.word = w;
            c.path = path;
            out.push_back(std::move(c));
        };
        // WHY other spellings first: "it's" for "its" is the swap people most often want, so the pool limit must
        // never squeeze it out; it is not filtered (Spellings only lists close, real spellings).
        for (const auto &w : otherSpellings(word)) add(w, ref);
        // The junk rule (rare, stretched, rare-name words are never offered). WHY the language's own cutoff when
        // lower: outside English recognize() uses none (0), because the rare-word cutoff cost 2-5 points of "right
        // word in the top 3" there (see init()). A tap-back list (SwapParams::tapBack) may go down to
        // tapBackMinFrequency, for a word between the two cutoffs only when its path is within tapBackRareWithin of
        // the tapped word's (see swap.hpp).
        // A list for a word with no sentence around it (sp.noContext) never goes below sp.ncMinFrequency (see swap.hpp).
        const int floor = sp.noContext ? sp.ncMinFrequency : 0;
        const int minFreq = std::max(std::min(sp.minFrequency, d_->alt.minFrequency), floor);
        const int lowFreq = sp.tapBack ? std::max(std::min(minFreq, sp.tapBackMinFrequency), floor) : minFreq;
        const auto offered = [&](const std::string &w, float path) {
            if (!sp.junkFilter) return true;
            int f = 0;
            if (!d_->mayOffer(w, lowFreq, f)) return false;
            if (lowFreq >= minFreq) return true;
            // WHY mayOffer again: it passes a word above the usual cutoff, and learned and texting words whatever
            // their frequency; only the words that are rare by the usual rule need the strong path.
            return d_->mayOffer(w, minFreq, f) || path >= ref - sp.tapBackRareWithin;
        };
        for (const auto &c : found) {
            const std::string &w = d_->engine.lexicon().text(c.word);
            if (w.empty() || !offered(w, c.ctc)) continue;
            add(w, c.ctc);
        }
        // One-edit neighbours (SwapParams::neighbours): words the search may not reach because their path differs
        // in one letter from the tapped word's ("form" for "from"). WHY scored on the swipe's own network output:
        // a neighbour must compete on the same path scale as the searched words, not on a guess.
        if (sp.neighbours > 0) {
            std::vector<std::string> extra;
            for (const auto &c : similar(word, sp.neighbours)) {
                if (isTapped(c.word) || seen.count(QString::fromStdString(c.word).toLower().toStdString())) continue;
                if (!offered(c.word, 1e30f)) continue;   // first the frequency rule; the path rule once fitted
                extra.push_back(c.word);
            }
            const auto fits = d_->engine.pathScores(extra);
            for (size_t i = 0; i < extra.size(); ++i) {
                const float fit = fits.size() == extra.size() ? fits[i] : -1e30f;
                const float path = std::max(fit, sp.neighbourFloor >= 0 ? ref - sp.neighbourFloor : -1e30f);
                if (path <= -1e29f) continue;   // no usable fit and no floor
                if (!offered(extra[i], fit)) continue;   // WHY its own fit, not the floor: a rare word must earn its place
                seen.insert(QString::fromStdString(extra[i]).toLower().toStdString());
                SwapCandidate c;
                c.word = extra[i];
                c.path = path;
                out.push_back(std::move(c));
            }
        }

        constexpr float kLn10 = 2.302585093f;
        // WHY the model's form (as recognize() builds the previous words): the pack stores lower-case words,
        // letters only or with apostrophes. Following words stop at the first one the model cannot read
        // (a number, an emoji), so the two that are used are the two right after the word.
        std::vector<std::string> previous, next;
        std::vector<int> nextFreq;
        if (d_->lmLoaded) {
            for (const auto &w : before) { const auto a = d_->lm.form(w); if (!a.empty()) previous.push_back(a); }
            for (const auto &w : after) {
                if (next.size() >= 2) break;
                const auto a = d_->lm.form(w);
                if (a.empty()) break;
                next.push_back(a);
                nextFreq.push_back(frequency(displaySpelling(w)));
            }
        }
        const auto ctxBefore = d_->lm.context(previous);
        // WHY only the last two previous words for the words after: the model looks back two words at most, and this
        // runs for thousands of sentence-word candidates on the input thread.
        const std::vector<std::string> lastTwo(previous.end() - std::min<ptrdiff_t>(2, ptrdiff_t(previous.size())), previous.end());
        // log10 evidence of `form` from the words before and the words after (0 when none follow).
        const auto sentenceFit = [&](const std::string &form, float freq, float &b, float &a) {
            b = d_->lm.logProb(ctxBefore, form, freq);
            a = 0;
            if (next.empty()) return;
            std::vector<std::string> ctx = lastTwo;
            ctx.push_back(form);
            for (size_t k = 0; k < next.size(); ++k) {
                a += d_->lm.logProb(d_->lm.context(ctx), next[k], float(nextFreq[k]));
                ctx.push_back(next[k]);
            }
        };

        // Sentence words (SwapParams::tapBackSentenceWords, tap-back lists only): the words the n-gram expects in this place, given the words
        // before it and (fill in the blank) the words after it, each scored on its own fit to this path.
        // WHY: the search and the one-edit neighbours only find words whose path looks like the tapped word's; a word
        // meant for this place whose path is further off (a typed word's made-up path is only a guess at the swipe)
        // was never gathered, however well it fits the sentence. WHY only the n-gram here: it is what the input
        // thread has; the worker's neural score still re-ranks the whole list afterwards.
        if (sp.tapBack && sp.tapBackSentenceWords > 0 && d_->lmLoaded && d_->prior) {
            d_->prior->setContext(previous);
            const SwipeLexicon &lex = d_->engine.lexicon();
            // Step 1, every word the model has seen after the words before (trigram, then bigram continuations), by
            // that evidence alone. WHY all of them and not the first few: the model lists continuations in its word
            // order, which is alphabetical, so a short scan would favour words early in the alphabet. WHY the prior's
            // own score: it reads the model by word number, a couple of lookups per word (it is ln 10 times the
            // n-gram's log10 P with the weights LanguageEngine leaves at 1 and 0).
            std::vector<std::pair<float, uint32_t>> byBefore;   // (ln P(word | before), lexicon word); may repeat
            for (const uint32_t lw : d_->prior->hints(kSentenceScan)) byBefore.push_back({(*d_->prior)(lw), lw});
            // Step 2, the best of those that may be offered, scored again with the words after (fill in the blank).
            // WHY a shortlist: the words-after score costs a few model lookups per word. WHY only the best few
            // thousand are put in order: sorting every continuation of a common word would cost more than the search.
            const auto better = [](const auto &x, const auto &y) { return x.first > y.first || (x.first == y.first && x.second < y.second); };
            const size_t head = std::min(byBefore.size(), 8 * kSentenceShortlist);
            std::nth_element(byBefore.begin(), byBefore.begin() + head, byBefore.end(), better);
            std::sort(byBefore.begin(), byBefore.begin() + head, better);
            std::vector<std::pair<float, uint32_t>> fit;   // (log10 before + after, lexicon word)
            std::unordered_set<uint32_t> scanned;          // WHY: a word can be both a trigram and a bigram continuation
            for (size_t i = 0; i < head && fit.size() < kSentenceShortlist; ++i) {
                const uint32_t lw = byBefore[i].second;
                if (!scanned.insert(lw).second) continue;
                const std::string &w = lex.text(lw);
                if (w.empty() || isTapped(w) || seen.count(QString::fromStdString(w).toLower().toStdString())) continue;
                if (!offered(w, 1e30f)) continue;   // the path rule once fitted, below
                float b = 0, a = 0;
                sentenceFit(d_->lm.form(w), float(lex.freq(lw)), b, a);
                fit.push_back({b + a, lw});
            }
            const size_t n = std::min(fit.size(), size_t(sp.tapBackSentenceWords));
            std::partial_sort(fit.begin(), fit.begin() + n, fit.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
            std::vector<std::string> extra;
            for (size_t i = 0; i < n; ++i) extra.push_back(lex.text(fit[i].second));
            const auto fits = d_->engine.pathScores(extra);
            for (size_t i = 0; i < extra.size() && fits.size() == extra.size(); ++i) {
                // WHY no floor (unlike neighbours): these words are not look-alikes; a word with no path to this
                // swipe at all (a letter with no key) is left out, any other competes on its real fit.
                if (fits[i] <= -1e29f || !offered(extra[i], fits[i]) || !seen.insert(QString::fromStdString(extra[i]).toLower().toStdString()).second) continue;
                SwapCandidate c;
                c.word = extra[i];
                c.path = fits[i];
                out.push_back(std::move(c));
            }
        }

        if (d_->lmLoaded) {
            for (auto &c : out) {
                const std::string form = d_->lm.form(c.word);
                if (form.empty()) continue;   // WHY: the model cannot read it; no evidence either way (0, 0)
                sentenceFit(form, float(frequency(c.word)), c.before, c.after);
            }
        } else {
            // WHY divided by ln 10: without a pack the search's own frequency prior (nats) stands in, put on the
            // n-gram's log10 scale so the weights in SwapParams mean the same with or without one.
            for (auto &c : out) c.before = d_->engine.params.freqWeight * float(frequency(c.word)) / kLn10;
        }
        return out;
    } catch (...) {
        return {};
    }
}

std::vector<float> LanguageEngine::pathFits(const std::vector<std::string> &words, const std::vector<KeyRect> &rects,
                                           const std::vector<TracePoint> &trace)
{
    if (!d_->ok || words.empty()) return {};
    try {
        std::vector<SwipePoint> pts;
        float aspect = 0;
        if (trace.size() < 2 || !d_->toSwipePoints(trace, rects, pts, aspect)) return {};
        d_->setSentencePrior({});
        d_->engine.decode(pts, aspect, 1);   // WHY: runs the network on this path; the words are scored on its output
        return d_->engine.pathScores(words);
    } catch (...) {
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
    if (!d_->ok) return {};
    auto out = d_->spellings.others(word);
    // WHY: Spellings are lower case outside German; "ill" -> "I'll", not "i'll".
    for (auto &w : out) w = d_->displayForm(QString::fromStdString(w)).toStdString();
    return out;
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

void LanguageEngine::setAlternativeParams(const AlternativeParams &params) { d_->alt = params; }

void LanguageEngine::setContextRescore(ContextRescore rescore) { d_->rescore = std::move(rescore); }

// WHY these weights (2026-10-29, bench/rescore-tune.py, grid neural {0 .. 3} x n-gram {0 .. 1}, TUNING swipes only;
// right first guesses, the UX scoreboard's "no tap"):
//   English (FUTO trv8k, 7,198 swipes, 6,457 with words before): today 92.94 % -> 93.61 % (+48 swipes) at neural 1,
//     n-gram 0: the next-word model's score (which already blends the same n-gram, Predictor::scoreWords) takes the
//     n-gram prior's place; keeping both counts the n-gram twice (neural 1, n-gram 1: +4 swipes).
//   French + German (FUTO swipe-5 validation, 648 swipes, 575 with words before): 88.43 % -> 90.43 % (+13) at
//     neural 1, n-gram 0.25 (a flat top: 1.5 / 0 and 2 / 0.5 score within one swipe). Spanish has no tuning set and
//     takes these.
ContextRescore ContextRescore::forLanguage(const std::string &language)
{
    ContextRescore r;
    r.neuralWeight = 1.f;
    r.ngramWeight = language.rfind("en", 0) == 0 ? 0.f : 0.25f;
    return r;
}

std::vector<RawSwipeCandidate> LanguageEngine::rawCandidates(const std::vector<TracePoint> &trace, const std::vector<KeyRect> &rects,
                                                             const std::vector<std::string> &context, int topN, int beamWidth)
{
    if (!d_->ok || trace.size() < 2 || topN <= 0) return {};
    try {
        std::vector<SwipePoint> pts;
        float aspect = 0;
        if (!d_->toSwipePoints(trace, rects, pts, aspect)) return {};
        d_->setSentencePrior(context);
        // WHY a guard object: the recognizer must get its own search width back even if the search throws.
        struct Restore {
            SwipeEngine &e;
            const int beam;
            ~Restore() { e.params.beam.beam = beam; }
        } restore{d_->engine, d_->engine.params.beam.beam};
        if (beamWidth > 0) d_->engine.params.beam.beam = beamWidth;
        std::vector<RawSwipeCandidate> out;
        for (const auto &c : d_->engine.decode(pts, aspect, topN)) {
            const std::string &w = d_->engine.lexicon().text(c.word);
            if (!w.empty()) out.push_back({w, c.score, c.ctc, d_->priorOf(c.word)});
        }
        return out;
    } catch (...) {
        return {};
    }
}

int LanguageEngine::frequency(const std::string &word) const
{
    if (!d_->ok) return 0;
    const auto it = d_->byIdentity.find(nfc(word).toLower().toStdString());
    return it == d_->byIdentity.end() ? 0 : d_->entries[it->second].freq();
}

void LanguageEngine::setCapitals(std::shared_ptr<const Capitals> capitals)
{
    if (!d_->ok || d_->keepCase) return;
    d_->capitals = std::move(capitals);
    // WHY every word again: entries keep the form they are shown in, and the swipe word list is rebuilt from them.
    for (auto &e : d_->entries) e.text = d_->displayForm(QString::fromStdString(e.text)).toStdString();
    d_->swipeDirty = true;
}

} // namespace swipekb
