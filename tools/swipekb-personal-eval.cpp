// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// swipekb-personal-eval: does learning from a person's own writing make the suggestions better?
//
// A rough personal test (roadmap Phase 2, checkpoint 2). Tatoeba names the contributor of every sentence, so a
// contributor's sentences stand in for one person's writing: the keyboard learns from the first half of what they
// wrote and is then asked for next words and word completions on the second half. The same questions are asked
// with learning off. It measures repeated vocabulary and favourite word pairs, NOT real typing: a rough bar only.
//
// WHY only sentences whose id ends in 0: the language packs were built from the other nine tenths of Tatoeba, so
// these sentences were never seen by the n-gram files (the "test tenth"). Without that the baseline would already
// know the answers.
// WHY the real code: the learning is swipekb::PersonalModel, the predictions are swipekb::NgramPredictor and the
// re-ranking is PersonalModel::rerank(), exactly what the keyboard runs.
// WHY the learned model stays frozen while testing: it is the harder, more honest reading (a keyboard that kept
// learning would score higher).
//
//   swipekb-personal-eval --model es.ngram --vocab es.vocab.tsv --detailed spa_sentences_detailed.tsv.bz2
//       [--label es] [--min-sentences 60] [--max-sentences 600] [--users even|odd|all] [--per-user]
//       [--personal-words K [--personal-rate R]] [--config "name:key=value,key=value"]...
// --personal-words K is a SYNTHETIC variant, clearly not Tatoeba as written: each contributor gets K invented words
// (made-up names and slang, none in the language files) that replace a share R (default 4%) of their words, a few
// of them much more often than the rest. It shows what the mechanism can do for a person who really uses such
// words; the real-data figures above it do not.
// Config keys are the LearningWeights fields: rank-ratio pair-k pair-max pair-min word-k word-max next-word-word novel-k novel-max mix-cap.
// WHY several --config: the predictions are computed once and every config is scored on the same cases, so a
// weight sweep costs little. Prints one tab-separated result row per measurement and config (with a header).
#include <QCoreApplication>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "swipekb/learning.hpp"
#include "swipekb/ngram_predictor.hpp"
#include "swipekb/prediction.hpp"
#include "swipekb/text.hpp"

namespace {
struct Sentence {
    long long id = 0;
    QString text;
};
struct Word {
    QString lower;                    // the word, lower case
    std::vector<std::string> context; // the words before it in its sentence (the keyboard's own context rules)
    bool capitalized = false;
};

QString lower(const std::string &word) { return QString::fromUtf8(word.data(), int(word.size())).toLower(); }

// Same word extraction as swipekb-nextword-eval, so both tools agree on what a "word" is.
std::vector<Word> wordsOf(const QString &line)
{
    std::vector<Word> out;
    const QStringList chunks = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (qsizetype k = 0; k < chunks.size(); ++k) {
        QString chunk = chunks[k];
        while (!chunk.isEmpty() && QStringLiteral(".!?").contains(chunk.back())) chunk.chop(1);
        const auto targetWords = swipekb::contextWords(chunk.toStdString(), 1);
        if (targetWords.empty() || swipekb::filterPredictedWords({{targetWords.front(), 0.f}}, 1).empty()) continue;
        const QString original = QString::fromStdString(targetWords.front());
        Word w;
        w.lower = original.toLower();
        w.capitalized = !original.isEmpty() && original.front().isUpper() && original != original.toUpper();
        const QString prefix = chunks.mid(0, k).join(QLatin1Char(' ')) + QLatin1Char(' ');
        w.context = swipekb::contextWords(prefix.toStdString(), 20);
        out.push_back(std::move(w));
    }
    return out;
}

// Invented words for the synthetic variant: consonant-vowel syllables, none of them in `known`.
std::vector<QString> inventWords(unsigned seed, int count, const std::unordered_set<std::string> &known)
{
    static const char *consonants[] = {"k", "m", "v", "z", "t", "r", "l", "n", "b", "d", "s", "g"};
    static const char *vowels[] = {"a", "e", "i", "o", "u"};
    std::vector<QString> out;
    unsigned state = seed * 2654435761u + 1;
    auto next = [&] { state = state * 1664525u + 1013904223u; return state >> 8; };
    while (int(out.size()) < count) {
        QString w;
        const int syllables = 2 + int(next() % 2);
        for (int i = 0; i < syllables; ++i) w += QString::fromLatin1(consonants[next() % 12]) + QString::fromLatin1(vowels[next() % 5]);
        if (!known.count(w.toStdString())) out.push_back(w);
    }
    return out;
}

std::string prevKey(const Word &w) { return w.context.empty() ? std::string() : lower(w.context.back()).toStdString(); }

// One question put to the keyboard: the predictor's list for it (computed once, shared by every config).
struct Case {
    int letters = 0;                    // 0 = next word, 1-2 = letters of the word already typed
    std::string prev, prefix;
    QString target;
    std::vector<std::string> base;      // the predictor's list as lower-case keys
    int offRank = 0;                    // place of the target in that list (0 = absent)
};
struct Person {
    QString name;
    std::vector<std::vector<Word>> learnSentences;
    std::vector<Case> cases;
};

struct Tally {
    long long n = 0, hit[2][3] = {};   // [0] without learning, [1] with; top1, top2, top3
    void add(int rankOff, int rankOn)
    {
        ++n;
        for (int i = 0; i < 3; ++i) {
            hit[0][i] += rankOff && rankOff <= i + 1;
            hit[1][i] += rankOn && rankOn <= i + 1;
        }
    }
    double pct(int which, int top) const { return n ? 100.0 * double(hit[which][top - 1]) / double(n) : 0.0; }
};

int rankOf(const std::vector<std::string> &list, const QString &target)
{
    for (size_t r = 0; r < list.size(); ++r)
        if (lower(list[r]) == target) return int(r) + 1;
    return 0;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString model, vocab, detailed, label = QStringLiteral("unnamed"), users = QStringLiteral("all");
    int minSentences = 60, maxSentences = 600, personalWords = 0;
    double personalRate = 0.04;
    bool perUser = false, diagnose = false;
    std::vector<std::pair<QString, swipekb::LearningWeights>> configs;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args[i];
        const bool hasValue = i + 1 < args.size();
        if (a == "--model" && hasValue) model = args[++i];
        else if (a == "--vocab" && hasValue) vocab = args[++i];
        else if (a == "--detailed" && hasValue) detailed = args[++i];
        else if (a == "--label" && hasValue) label = args[++i];
        else if (a == "--min-sentences" && hasValue) minSentences = args[++i].toInt();
        else if (a == "--max-sentences" && hasValue) maxSentences = args[++i].toInt();
        else if (a == "--users" && hasValue) users = args[++i];
        else if (a == "--personal-words" && hasValue) personalWords = args[++i].toInt();
        else if (a == "--personal-rate" && hasValue) personalRate = args[++i].toDouble();
        else if (a == "--per-user") perUser = true;
        else if (a == "--diagnose") diagnose = true;
        else if (a == "--config" && hasValue) {
            const QString spec = args[++i];
            swipekb::LearningWeights w;
            const int colon = spec.indexOf(QLatin1Char(':'));
            const QString name = colon < 0 ? spec : spec.left(colon);
            for (const QString &kv : (colon < 0 ? QString() : spec.mid(colon + 1)).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const QStringList p = kv.split(QLatin1Char('='));
                const double v = p.size() == 2 ? p[1].toDouble() : 0;
                if (p[0] == "rank-ratio") w.rankRatio = v;
                else if (p[0] == "pair-k") w.pairK = v;
                else if (p[0] == "pair-max") w.pairMax = v;
                else if (p[0] == "pair-min") w.pairMinUses = unsigned(v);
                else if (p[0] == "word-k") w.wordK = v;
                else if (p[0] == "word-max") w.wordMax = v;
                else if (p[0] == "next-word-word") w.nextWordWord = v;
                else if (p[0] == "novel-k") w.novelK = v;
                else if (p[0] == "novel-max") w.novelMax = v;
                else if (p[0] == "mix-cap") w.mixCap = v;
                else { std::fprintf(stderr, "unknown config key: %s\n", qPrintable(p[0])); return 2; }
            }
            configs.emplace_back(name, w);
        }
        else { std::fprintf(stderr, "unknown or incomplete option: %s\n", qPrintable(a)); return 2; }
    }
    if (model.isEmpty() || vocab.isEmpty() || detailed.isEmpty() || (users != "even" && users != "odd" && users != "all")) {
        std::fprintf(stderr, "usage: swipekb-personal-eval --model M --vocab V --detailed FILE.tsv.bz2 [--label L] "
                             "[--min-sentences N] [--max-sentences N] [--users even|odd|all] [--per-user] [--diagnose] [--personal-words K] [--personal-rate R] [--config name:k=v,...]\n");
        return 2;
    }
    if (configs.empty()) configs.emplace_back(QStringLiteral("default"), swipekb::LearningWeights());
    swipekb::NgramPredictor predictor;
    if (!predictor.load(model.toStdString(), vocab.toStdString())) {
        std::fprintf(stderr, "could not load model %s with vocabulary %s\n", qPrintable(model), qPrintable(vocab));
        return 1;
    }
    std::unordered_set<std::string> known;
    for (const auto &word : predictor.vocabulary()) known.insert(lower(word).toStdString());

    // Contributors and their test-tenth sentences. Columns: id, language, text, user, ...
    std::map<QString, std::vector<Sentence>> byUser;
    {
        const std::string command = "bzcat '" + detailed.toStdString() + "'";
        FILE *pipe = popen(command.c_str(), "r");
        if (!pipe) { std::fprintf(stderr, "could not read %s\n", qPrintable(detailed)); return 1; }
        std::string raw;
        char buffer[8192];
        while (std::fgets(buffer, sizeof buffer, pipe)) {
            raw += buffer;
            if (raw.empty() || raw.back() != '\n') continue;
            const QStringList c = QString::fromUtf8(raw.data(), int(raw.size())).trimmed().split(QLatin1Char('\t'));
            raw.clear();
            if (c.size() < 4) continue;
            bool ok = false;
            const long long id = c[0].toLongLong(&ok);
            if (!ok || id % 10 != 0 || c[3].isEmpty() || c[3] == QLatin1String("\\N")) continue;
            byUser[c[3]].push_back({id, c[2]});
        }
        pclose(pipe);
    }

    // Ask the predictor every question once.
    std::vector<Person> people;
    for (auto &entry : byUser) {
        auto &list = entry.second;
        if (int(list.size()) < minSentences) continue;
        // WHY FNV-1a (not Qt's qHash): the split must come out the same on every machine and every run.
        unsigned h = 2166136261u;
        for (const char c : entry.first.toUtf8()) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
        if ((users == "even" && (h & 1)) || (users == "odd" && !(h & 1))) continue;
        std::sort(list.begin(), list.end(), [](const Sentence &a, const Sentence &b) { return a.id < b.id; });
        // WHY a cap: a few contributors wrote thousands of sentences; the test models months of typing, not years,
        // and the run stays short. The cap keeps their earliest sentences (half to learn from, half to test on).
        if (maxSentences > 0 && int(list.size()) > maxSentences) list.resize(size_t(maxSentences));
        const size_t half = list.size() / 2;
        if (personalWords > 0) {
            // WHY a skewed pick (squared random number): a person has a few favourites and a long tail, not K equals.
            unsigned seed = h;
            const auto names = inventWords(seed, personalWords, known);
            unsigned state = seed ^ 0x9e3779b9u;
            auto next01 = [&] { state = state * 1664525u + 1013904223u; return double(state >> 8) / double(1u << 24); };
            for (auto &sentence : list) {
                QStringList chunks = sentence.text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
                for (auto &chunk : chunks) {
                    if (next01() >= personalRate) continue;
                    QString tail;
                    while (!chunk.isEmpty() && !chunk.back().isLetter()) { tail.prepend(chunk.back()); chunk.chop(1); }
                    const double r = next01();
                    chunk = names[size_t(r * r * names.size())] + tail;
                }
                sentence.text = chunks.join(QLatin1Char(' '));
            }
        }
        Person person;
        person.name = entry.first;
        for (size_t s = 0; s < half; ++s) person.learnSentences.push_back(wordsOf(list[s].text));
        for (size_t s = half; s < list.size(); ++s)
            for (const auto &w : wordsOf(list[s].text))
                for (int letters = 0; letters <= 3; ++letters) {
                    if (letters && w.lower.size() <= letters) continue;
                    Case c;
                    c.letters = letters; c.prev = prevKey(w); c.target = w.lower;
                    c.prefix = w.lower.left(letters).toStdString();
                    const auto list12 = predictor.predict(w.context, 12, c.prefix);
                    for (const auto &b : list12) c.base.push_back(lower(b).toStdString());
                    c.offRank = rankOf(list12, w.lower);
                    person.cases.push_back(std::move(c));
                }
        people.push_back(std::move(person));
    }

    QTextStream out(stdout);
    if (diagnose) {
        // How much room is there at all? For the one-letter questions the predictor missed (target not in its top 2):
        // how often had this person already used that exact word, or that exact word after the previous word?
        long long cases = 0, baseHit = 0, miss = 0, missWordSeen = 0, missPairSeen = 0, missInList = 0, hitPairSeen = 0, hitWordSeen = 0;
        for (const auto &person : people) {
            std::unordered_set<std::string> words, pairs;
            for (const auto &sentence : person.learnSentences)
                for (const auto &w : sentence) { words.insert(w.lower.toStdString()); pairs.insert(prevKey(w) + '\x1f' + w.lower.toStdString()); }
            for (const auto &c : person.cases) {
                if (c.letters != 1) continue;
                ++cases;
                const std::string t = c.target.toStdString();
                const bool wordSeen = words.count(t) != 0, pairSeen = !c.prev.empty() && pairs.count(c.prev + '\x1f' + t) != 0;
                if (c.offRank && c.offRank <= 2) { ++baseHit; hitWordSeen += wordSeen; hitPairSeen += pairSeen; continue; }
                ++miss; missWordSeen += wordSeen; missPairSeen += pairSeen; missInList += c.offRank != 0;
            }
        }
        out << "diagnose\tcases(1 letter)\t" << cases << "\tbase top2 hits\t" << baseHit << "\tmisses\t" << miss
            << "\tmisses where the word was seen before\t" << missWordSeen << "\tmisses where the pair was seen before\t" << missPairSeen
            << "\tmisses with the word in the predictor's list (rank 3-12)\t" << missInList
            << "\thits where the word was seen\t" << hitWordSeen << "\thits where the pair was seen\t" << hitPairSeen << "\n";
    }
    if (perUser) out << "config\tuser\tlearn_sentences\ttest_cases\tnext_top3_off\tnext_top3_on\tletter1_top2_off\tletter1_top2_on\n";
    out << "label\tconfig\tusers\tlearn_words\tnovel%\tmeasure\tcases\toff%\ton%\tgain\n";
    for (const auto &[configName, weights] : configs) {
        Tally nextWord, afterOne, afterTwo, afterThree, lackNext, lackOne, lackTwo;   // lack* = only words the language files do not know
        long long learnedWords = 0, learnedNovel = 0;
        for (const auto &person : people) {
            swipekb::PersonalModel personal({}, weights);
            // Learning phase: a sentence every ten minutes, in the order it was written.
            long long now = 1700000000;
            for (const auto &sentence : person.learnSentences) {
                for (const auto &w : sentence) {
                    const bool isKnown = known.count(w.lower.toStdString()) != 0;
                    personal.learn(prevKey(w), w.lower.toStdString(), isKnown, w.capitalized, now);
                    ++learnedWords;
                    learnedNovel += !isKnown;
                }
                now += 600;
            }
            const long long testNow = now + 86400;
            Tally userNext, userOne;
            for (const auto &c : person.cases) {
                std::vector<std::string> personalList;
                for (const auto &r : personal.rerank(c.base, c.prev, c.prefix, testNow, 12, true)) personalList.push_back(r.key);
                int on = 0;
                for (size_t r = 0; r < personalList.size() && !on; ++r)
                    if (QString::fromStdString(personalList[r]) == c.target) on = int(r) + 1;
                (c.letters == 0 ? nextWord : c.letters == 1 ? afterOne : c.letters == 2 ? afterTwo : afterThree).add(c.offRank, on);
                // WHY a separate figure: new words (names, slang) are what learning is really for, and they are only
                // ~1% of Tatoeba words, so they barely move the overall numbers above.
                if (!known.count(c.target.toStdString()) && c.letters <= 2) (c.letters == 0 ? lackNext : c.letters == 1 ? lackOne : lackTwo).add(c.offRank, on);
                if (c.letters == 0) userNext.add(c.offRank, on);
                if (c.letters == 1) userOne.add(c.offRank, on);
            }
            if (perUser)
                out << configName << '\t' << person.name << '\t' << person.learnSentences.size() << '\t' << person.cases.size() << '\t'
                    << userNext.pct(0, 3) << '\t' << userNext.pct(1, 3) << '\t' << userOne.pct(0, 2) << '\t' << userOne.pct(1, 2) << '\n';
        }
        const auto row = [&](const char *name, const Tally &t, int top) {
            out << label << '\t' << configName << '\t' << people.size() << '\t' << learnedWords << '\t'
                << (learnedWords ? QString::number(100.0 * double(learnedNovel) / double(learnedWords), 'f', 1) : QStringLiteral("0")) << '\t'
                << name << '\t' << t.n << '\t' << QString::number(t.pct(0, top), 'f', 2) << '\t' << QString::number(t.pct(1, top), 'f', 2)
                << '\t' << QString::number(t.pct(1, top) - t.pct(0, top), 'f', 2) << '\n';
        };
        row("next-word top3", nextWord, 3);
        row("next-word top1", nextWord, 1);
        row("after-1-letter top2", afterOne, 2);
        row("after-1-letter top3", afterOne, 3);
        row("after-1-letter top1", afterOne, 1);
        row("after-2-letters top2", afterTwo, 2);
        row("after-3-letters top2", afterThree, 2);
        row("only words the pack lacks: next-word top3", lackNext, 3);
        row("only words the pack lacks: after-1-letter top2", lackOne, 2);
        row("only words the pack lacks: after-2-letters top2", lackTwo, 2);
        out.flush();
    }
    return people.empty() ? 1 : 0;
}
