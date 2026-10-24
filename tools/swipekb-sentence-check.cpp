// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// swipekb-sentence-check — swipes and predicts every word of held-out sentences, scoring the EXACT spelling
// (apostrophes count; capitals do not).
//
// WHY this exists: the recorded-swipe scoreboard compares letters only (its targets have no apostrophes), so "i's"
// for "is" or "were" for "we're" looked correct there. Here each word of a real sentence is swiped with the words
// before it as context, through swipekb::LanguageEngine (what the keyboard uses), and the next-word suggestions are
// checked at every position through swipekb::Predictor. "Pair words" are the words whose letters the dictionary also
// spells with a different apostrophe ("were"/"we're", "its"/"it's", "lune"/"l'une"), reported on their own.
//
// Usage:
//   swipekb-sentence-check --dict LANG.combined --layout keys.tsv [--model swipe-model.bin] [--ngram LANG.ngram]
//       [--vocab LANG.vocab.tsv] [--nwp LANG.nwp.bin] [--capitals LANG.tsv] [--language en] [--limit N per file] [--noise F]
//       [--misses N] [--by-word] SENTENCES.txt...
// --capitals: the keyboard's capitals list (packaging/capitals); adds "case" scores: of the words not starting a sentence
// that were guessed right, how many also have the sentence's own capitals ("I", "Paris", but "the hall").
// Sentences: one per line (scripts/lm/make-nextword-testsets.py output; never training text).
// Swipes: straight lines through the key centres, every point moved by a fixed-seed random offset of up to --noise
// key widths (default 0.25), so the shape alone is not always decisive. Output: one "key: value" line per score.
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "swipekb/language_engine.hpp"
#include "swipekb/prediction.hpp"

using namespace swipekb;

namespace {

struct Score {
    size_t n = 0, top1 = 0, top3 = 0;
    void add(bool first, bool three) { ++n; top1 += first; top3 += three; }
    std::string pct(size_t k) const
    {
        char b[32];
        std::snprintf(b, sizeof b, "%.2f%%", n ? 100.0 * double(k) / double(n) : 0.0);
        return b;
    }
};

std::vector<KeyRect> readLayout(const std::string &path)
{
    std::ifstream in(path);
    std::vector<KeyRect> keys;
    std::string line;
    if (!in || !std::getline(in, line)) return {};
    while (std::getline(in, line)) {
        std::istringstream f(line);
        std::string letter;
        float cx, cy, hw, hh;
        if (!(f >> letter >> cx >> cy >> hw >> hh)) continue;
        const auto u = QString::fromStdString(letter).toUcs4();
        if (u.size() != 1) continue;
        // WHY 1000 x 300 pixels: a phone-like letter area; the engine works in any pixel space.
        keys.push_back({char32_t(u[0]), (cx - hw) * 1000.f, (cy - hh) * 300.f, 2 * hw * 1000.f, 2 * hh * 300.f});
    }
    return keys;
}

// The comparison form: lower case, curly apostrophe = straight one (French lists use both).
QString canon(const QString &w) { return w.toLower().replace(QChar(0x2019), QLatin1Char('\'')); }
QString noApostrophes(const QString &w) { return canon(w).remove(QLatin1Char('\'')); }

// The key a letter is swiped on: its own key, else the key of its plain letter (é on e, ß on s, œ on o).
const KeyRect *keyFor(char32_t c, const std::vector<KeyRect> &keys)
{
    for (const auto &k : keys) if (k.letter == c) return &k;
    QString base = QString::fromUcs4(&c, 1).normalized(QString::NormalizationForm_D);
    char32_t b = base.isEmpty() ? c : base.at(0).unicode();
    if (c == U'ß') b = U's';
    if (c == U'œ') b = U'o';
    if (c == U'æ') b = U'a';
    for (const auto &k : keys) if (k.letter == b) return &k;
    return nullptr;
}

// A swipe through the word's keys, or empty when a letter has no key.
std::vector<TracePoint> swipeOf(const QString &word, const std::vector<KeyRect> &keys, float noise, std::mt19937 &rng)
{
    std::vector<const KeyRect *> path;
    for (char32_t c : canon(word).toUcs4()) {
        if (c == U'\'' || c == U'-') continue;
        const KeyRect *k = keyFor(c, keys);
        if (!k) return {};
        if (path.empty() || path.back() != k) path.push_back(k);
    }
    if (path.empty()) return {};
    std::uniform_real_distribution<float> jitter(-noise, noise);
    std::vector<TracePoint> out;
    float t = 0;
    auto point = [&](float x, float y, const KeyRect *k) {
        out.push_back({x + jitter(rng) * k->w, y + jitter(rng) * k->h, t});
        t += 8;
    };
    if (path.size() == 1) {
        for (int s = 0; s < 10; ++s) point(path[0]->x + path[0]->w / 2, path[0]->y + path[0]->h / 2, path[0]);
        return out;
    }
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const float x0 = path[i]->x + path[i]->w / 2, y0 = path[i]->y + path[i]->h / 2;
        const float x1 = path[i + 1]->x + path[i + 1]->w / 2, y1 = path[i + 1]->y + path[i + 1]->h / 2;
        for (int s = 0; s < 12; ++s) point(x0 + (x1 - x0) * s / 12.f, y0 + (y1 - y0) * s / 12.f, path[i]);
    }
    point(path.back()->x + path.back()->w / 2, path.back()->y + path.back()->h / 2, path.back());
    return out;
}

} // namespace

int main(int argc, char **argv)
{
    LanguageEngineSetup setup;
    std::string layout, vocab, nwp, capitalsPath;
    std::vector<std::string> inputs;
    size_t limit = 0, misses = 0;
    bool byWord = false;   // per pair word: how often seen, swiped right first, suggested in the top 3
    float noise = 0.25f;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--dict") setup.dictionary = val();
        else if (a == "--model") setup.swipeModel = val();
        else if (a == "--ngram") setup.ngram = val();
        else if (a == "--vocab") vocab = val();
        else if (a == "--nwp") nwp = val();
        else if (a == "--capitals") capitalsPath = val();
        else if (a == "--language") setup.language = val();
        else if (a == "--layout") layout = val();
        else if (a == "--limit") limit = size_t(std::atoll(val().c_str()));
        else if (a == "--noise") noise = float(std::atof(val().c_str()));
        else if (a == "--misses") misses = size_t(std::atoll(val().c_str()));
        else if (a == "--by-word") byWord = true;
        else if (a[0] != '-') inputs.push_back(a);
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (setup.dictionary.empty() || layout.empty() || inputs.empty()) {
        std::fprintf(stderr, "usage: swipekb-sentence-check --dict LANG.combined --layout keys.tsv [--model m.bin] [--ngram LANG.ngram]"
                             " [--vocab LANG.vocab.tsv] [--nwp LANG.nwp.bin] [--language en] [--limit N] [--noise F] [--misses N] SENTENCES.txt...\n");
        return 2;
    }
    const auto keys = readLayout(layout);
    if (keys.empty()) { std::fprintf(stderr, "cannot read the layout %s\n", layout.c_str()); return 1; }
    LanguageEngine engine;
    if (!engine.init(setup)) { std::fprintf(stderr, "cannot load the dictionary %s\n", setup.dictionary.c_str()); return 1; }
    std::shared_ptr<const Capitals> capitals;
    if (!capitalsPath.empty()) {
        std::ifstream c(capitalsPath);
        if (!c) { std::fprintf(stderr, "cannot read %s\n", capitalsPath.c_str()); return 1; }
        capitals = Capitals::fromTsv(std::string(std::istreambuf_iterator<char>(c), {}));
        engine.setCapitals(capitals);   // as the keyboard does
    }
    // WHY curly = straight only: the case scores compare capitals exactly.
    auto exact = [](const QString &w) { return QString(w).replace(QChar(0x2019), QLatin1Char('\'')); };
    Score swipeCase, nextCase;
    Predictor predictor;
    // WHY siblingFallback=false: only the neural file named on the command line is used, so runs are comparable.
    const bool predicting = !setup.ngram.empty() && !vocab.empty() && predictor.init(setup.ngram, vocab, nwp, false);

    // Same word rule as the n-gram builder (scripts/lm/build-fineweb2-model.py): letters with inner apostrophes.
    static const QRegularExpression word(QStringLiteral(R"([^\W\d_]+(?:['’][^\W\d_]+)*)"), QRegularExpression::UseUnicodePropertiesOption);
    std::mt19937 rng(20261023);
    Score swipeAll, swipePair, nextAll, nextPair;
    std::map<std::string, Score> pairSwipe, pairNext;
    size_t sentences = 0, skipped = 0, shown = 0;
    for (const auto &path : inputs) {
        std::ifstream in(path);
        if (!in) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); return 1; }
        // WHY per file: each test set (everyday, Tatoeba, web) gets the same share of sentences.
        size_t fromFile = 0;
        for (std::string line; std::getline(in, line) && (!limit || fromFile < limit);) {
            QStringList tokens;
            for (auto it = word.globalMatch(QString::fromStdString(line)); it.hasNext();) tokens << it.next().captured();
            if (tokens.size() < 2) continue;
            ++sentences; ++fromFile;
            std::vector<std::string> context;
            for (const QString &target : tokens) {
                bool pair = false;
                for (const auto &o : engine.otherSpellings(canon(target).toStdString())) {
                    const QString other = QString::fromStdString(o);
                    pair |= noApostrophes(other) == noApostrophes(target) && canon(other) != canon(target);
                }
                const auto trace = swipeOf(target, keys, noise, rng);
                if (trace.empty()) { ++skipped; }
                else {
                    const auto guesses = engine.recognize(trace, keys, context, 3);
                    bool first = false, three = false;
                    for (size_t k = 0; k < guesses.size(); ++k)
                        if (canon(QString::fromStdString(guesses[k].word)) == canon(target)) { three = true; first |= k == 0; }
                    swipeAll.add(first, three);
                    if (first && !context.empty()) {
                        const bool same = exact(QString::fromStdString(guesses[0].word)) == exact(target);
                        swipeCase.add(same, same);
                    }
                    if (pair) { swipePair.add(first, three); pairSwipe[canon(target).toStdString()].add(first, three); }
                    if (pair && !first && shown < misses) {
                        ++shown;
                        std::printf("miss: %s -> %s | %s\n", qPrintable(target),
                                    guesses.empty() ? "(none)" : guesses[0].word.c_str(), line.c_str());
                    }
                }
                if (predicting) {
                    const auto next = predictor.predict(context, 3);
                    bool first = false, three = false;
                    for (size_t k = 0; k < next.size(); ++k)
                        if (canon(QString::fromStdString(next[k])) == canon(target)) { three = true; first |= k == 0; }
                    nextAll.add(first, three);
                    if (first && !context.empty()) {
                        const QString shownWord = QString::fromStdString(capitals ? capitals->apply(next[0]) : next[0]);
                        const bool same = exact(shownWord) == exact(target);
                        nextCase.add(same, same);
                    }
                    if (pair) { nextPair.add(first, three); pairNext[canon(target).toStdString()].add(first, three); }
                }
                context.push_back(target.toStdString());
            }
        }
    }
    if (byWord)
        for (const auto &[w, sc] : pairSwipe)
            std::printf("pair %s: n=%zu swipe-top-1 %zu next-top-3 %zu\n", w.c_str(), sc.n, sc.top1, pairNext[w].top3);
    std::printf("sentences: %zu\nwords skipped (a letter with no key): %zu\n", sentences, skipped);
    std::printf("swipe all words: n=%zu top-1 %s top-3 %s\n", swipeAll.n, swipeAll.pct(swipeAll.top1).c_str(), swipeAll.pct(swipeAll.top3).c_str());
    std::printf("swipe pair words: n=%zu top-1 %s top-3 %s\n", swipePair.n, swipePair.pct(swipePair.top1).c_str(), swipePair.pct(swipePair.top3).c_str());
        std::printf("swipe case (mid-sentence, right word): n=%zu same capitals %s\n", swipeCase.n, swipeCase.pct(swipeCase.top1).c_str());
    if (predicting)
        std::printf("next word case (mid-sentence, right word): n=%zu same capitals %s\n", nextCase.n, nextCase.pct(nextCase.top1).c_str());
    if (predicting) {
        std::printf("next word all: n=%zu top-1 %s top-3 %s\n", nextAll.n, nextAll.pct(nextAll.top1).c_str(), nextAll.pct(nextAll.top3).c_str());
        std::printf("next word pair words: n=%zu top-1 %s top-3 %s\n", nextPair.n, nextPair.pct(nextPair.top1).c_str(), nextPair.pct(nextPair.top3).c_str());
    }
    return 0;
}
