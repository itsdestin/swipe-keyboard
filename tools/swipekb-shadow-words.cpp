// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT
// swipekb-shadow-words — finds the rare words of a long word list that would steal a common word's swipe.
//
// WHY this exists: going from 100k to 200k+ words per language brings in names and fragments ("Ammor", "Sivo",
// "pement") whose swipe draws nearly the same line as a common word ("amour", "suivi", "purement"). The n-gram model
// has never seen them, so they get a fixed score from their frequency while the common word's score depends on the
// sentence; on a clean swipe the rare word then sometimes wins. Measured on French: 21 of the 5,000 most common
// words lost their own perfect swipe at 150k words. This tool swipes each of the --common most frequent words through
// its own key centres, exactly as the keyboard decodes (swipekb::LanguageEngine, sentence start), plus --noisy swipes
// whose points are moved by up to --noise key widths (fixed seed), and lists every word past --keep (the part of the
// list being added) that wins the exact swipe or at least --min-wins of all of them. WHY the wobbly swipes: a thief's lead depends on the exact
// line (measured: "Sivo" lost "suivi"'s centred swipe on the real French board but won it on a slightly different
// one), and real fingers never pass through key centres. It repeats with those words left out until nothing more
// steals, because removing one thief can uncover another. The words of the first --keep lines are never listed, so a
// list built with this output left out can only differ from the old one in words that were not there before.
//
// Usage:
//   swipekb-shadow-words --dict LANG.combined --layout keys.tsv --ngram LANG.ngram --model swipe-model.bin
//       --language es [--keep 100000] [--common 20000] [--rounds 6] [--noisy 4 --noise 0.2 --min-wins 2] > shadows.txt
// Output: "word<TAB>common word it beat" per line, then a "# ..." summary line on stderr.
// Used by scripts/dict/build-corpus-dictionaries.sh through corpus_dictionary.py --drop (scripts/dict/swipe-shadows/).

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "swipekb/language_engine.hpp"

using namespace swipekb;

namespace {

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
        // WHY 1000 x 300 pixels: the same phone-like letter area as swipekb-sentence-check.
        keys.push_back({char32_t(u[0]), (cx - hw) * 1000.f, (cy - hh) * 300.f, 2 * hw * 1000.f, 2 * hh * 300.f});
    }
    return keys;
}

// The comparison form: lower case, curly apostrophe = straight one (French lists use both).
QString canon(const QString &w) { return w.toLower().replace(QChar(0x2019), QLatin1Char('\'')); }

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

// A swipe through the centres of the word's keys, every point moved by up to `noise` key sizes (0 = exact), or empty
// when a letter has no key.
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
    std::vector<TracePoint> out;
    float t = 0;
    std::uniform_real_distribution<float> jitter(-noise, noise);
    auto centre = [&](const KeyRect *k) { return std::pair{k->x + k->w / 2 + jitter(rng) * k->w, k->y + k->h / 2 + jitter(rng) * k->h}; };
    if (path.size() == 1) {
        const auto [x, y] = centre(path[0]);
        for (int s = 0; s < 10; ++s, t += 8) out.push_back({x + s, y, t});
        return out;
    }
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const auto [x0, y0] = centre(path[i]);
        const auto [x1, y1] = centre(path[i + 1]);
        for (int s = 0; s < 12; ++s, t += 8) out.push_back({x0 + (x1 - x0) * s / 12.f, y0 + (y1 - y0) * s / 12.f, t});
    }
    const auto [xe, ye] = centre(path.back());
    out.push_back({xe, ye, t});
    return out;
}

struct Line { std::string text, raw; };

} // namespace

int main(int argc, char **argv)
{
    std::string dict, layout, ngram, model, language;
    size_t keep = 100000, common = 20000, rounds = 6, noisy = 0, minWins = 1;
    float noise = 0.2f;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--dict") dict = val();
        else if (a == "--layout") layout = val();
        else if (a == "--ngram") ngram = val();
        else if (a == "--model") model = val();
        else if (a == "--language") language = val();
        else if (a == "--keep") keep = std::stoul(val());
        else if (a == "--common") common = std::stoul(val());
        else if (a == "--rounds") rounds = std::stoul(val());
        else if (a == "--noisy") noisy = std::stoul(val());
        else if (a == "--noise") noise = std::stof(val());
        else if (a == "--min-wins") minWins = std::stoul(val());
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (dict.empty() || layout.empty() || language.empty()) {
        std::fprintf(stderr, "usage: swipekb-shadow-words --dict F --layout F --language L [--ngram F] [--model F] [--keep N] [--common N]\n");
        return 2;
    }
    const auto keys = readLayout(layout);
    std::ifstream in(dict);
    if (keys.size() < 2 || !in) { std::fprintf(stderr, "cannot read %s or %s\n", dict.c_str(), layout.c_str()); return 2; }
    std::string header, line;
    std::getline(in, header);
    std::vector<Line> lines;
    while (std::getline(in, line)) {
        if (line.rfind(" word=", 0) != 0) continue;
        lines.push_back({line.substr(6, line.find(',', 6) - 6), line});
    }
    // Comparison form -> line numbers, so the engine's answer maps back to the list's own spellings.
    std::map<QString, std::vector<size_t>> byCanon;
    for (size_t i = 0; i < lines.size(); ++i) byCanon[canon(QString::fromStdString(lines[i].text))].push_back(i);

    QTemporaryDir tmp;
    if (!tmp.isValid()) return 2;
    const std::string trial = (tmp.path() + "/trial.combined").toStdString();
    std::set<size_t> dropped;
    std::vector<std::pair<std::string, std::string>> found;   // (thief, common word it beat), in discovery order
    for (size_t round = 0; round < rounds; ++round) {
        {
            std::ofstream out(trial);
            out << header << '\n';
            for (size_t i = 0; i < lines.size(); ++i) if (!dropped.count(i)) out << lines[i].raw << '\n';
        }
        LanguageEngine engine;
        LanguageEngineSetup setup;
        setup.language = language; setup.dictionary = trial; setup.ngram = ngram; setup.swipeModel = model;
        if (!engine.init(setup)) { std::fprintf(stderr, "engine init failed\n"); return 1; }
        size_t added = 0;
        for (size_t i = 0; i < std::min(common, lines.size()); ++i) {
            const QString word = QString::fromStdString(lines[i].text);
            // WHY a seed per word and round-independent: the same list always gives the same drop file.
            std::mt19937 rng(uint32_t(i) * 2654435761u + 1);
            std::map<QString, size_t> wins;   // new words that beat `word`, and on how many of its swipes
            QString cleanWinner;              // WHY kept apart: winning the exact swipe alone is always enough
            for (size_t attempt = 0; attempt <= noisy; ++attempt) {
                const auto trace = swipeOf(word, keys, attempt ? noise : 0.f, rng);
                if (trace.empty()) break;
                const auto got = engine.recognize(trace, keys, {}, 1);
                if (got.empty()) continue;
                const QString winner = canon(QString::fromStdString(got[0].word));
                if (winner == canon(word)) continue;
                const auto it = byCanon.find(winner);
                if (it == byCanon.end()) continue;
                // WHY only when every spelling of the winner is new: a word already in the first --keep lines is not
                // something this list adds, so leaving it out would change what the keyboard does today.
                bool allNew = true;
                for (size_t j : it->second) allNew = allNew && j >= keep;
                if (!allNew) continue;
                ++wins[winner];
                if (!attempt) cleanWinner = winner;
            }
            for (const auto &[winner, n] : wins) {
                if (n < minWins && winner != cleanWinner) continue;
                for (size_t j : byCanon[winner])
                    if (dropped.insert(j).second) { found.push_back({lines[j].text, lines[i].text}); ++added; }
            }
        }
        std::fprintf(stderr, "# round %zu: %zu more\n", round + 1, added);
        if (!added) break;
    }
    for (const auto &[thief, victim] : found) std::cout << thief << '\t' << victim << '\n';
    std::fprintf(stderr, "# %zu words left out of %zu (first %zu kept as they are; %zu most common swiped)\n",
                 found.size(), lines.size(), keep, std::min(common, lines.size()));
    return 0;
}
