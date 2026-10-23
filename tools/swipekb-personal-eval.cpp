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
// re-ranking is PersonalModel::rerank(), exactly what the keyboard runs (no FUTO library involved).
// WHY the learned model stays frozen while testing: it is the harder, more honest reading (a keyboard that kept
// learning would score higher).
//
//   swipekb-personal-eval --model es.ngram --vocab es.vocab.tsv --detailed spa_sentences_detailed.tsv.bz2
//       [--label es] [--min-sentences 60] [--users even|odd|all] [--per-user]
//       [--pair W] [--word W] [--pair-speed S] [--word-speed S] [--rank-decay D] [--outside P]
// Prints one tab-separated result row per measurement (with a header).
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
#include "swipekb/recognizer.hpp"

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

std::string prevKey(const Word &w) { return w.context.empty() ? std::string() : lower(w.context.back()).toStdString(); }

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
    int minSentences = 60;
    bool perUser = false;
    swipekb::LearningWeights weights;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args[i];
        const bool hasValue = i + 1 < args.size();
        if (a == "--model" && hasValue) model = args[++i];
        else if (a == "--vocab" && hasValue) vocab = args[++i];
        else if (a == "--detailed" && hasValue) detailed = args[++i];
        else if (a == "--label" && hasValue) label = args[++i];
        else if (a == "--min-sentences" && hasValue) minSentences = args[++i].toInt();
        else if (a == "--users" && hasValue) users = args[++i];
        else if (a == "--per-user") perUser = true;
        else if (a == "--pair" && hasValue) weights.pair = args[++i].toDouble();
        else if (a == "--word" && hasValue) weights.word = args[++i].toDouble();
        else if (a == "--pair-speed" && hasValue) weights.pairSpeed = args[++i].toDouble();
        else if (a == "--word-speed" && hasValue) weights.wordSpeed = args[++i].toDouble();
        else if (a == "--rank-decay" && hasValue) weights.rankDecay = args[++i].toDouble();
        else if (a == "--outside" && hasValue) weights.outsidePlace = args[++i].toDouble();
        else { std::fprintf(stderr, "unknown or incomplete option: %s\n", qPrintable(a)); return 2; }
    }
    if (model.isEmpty() || vocab.isEmpty() || detailed.isEmpty() || (users != "even" && users != "odd" && users != "all")) {
        std::fprintf(stderr, "usage: swipekb-personal-eval --model M --vocab V --detailed FILE.tsv.bz2 [--label L] "
                             "[--min-sentences N] [--users even|odd|all] [--per-user] [weight options]\n");
        return 2;
    }
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

    Tally nextWord, afterOne, afterTwo;   // next word (3 slots), after 1 letter, after 2 letters
    int people = 0;
    long long learnedWords = 0, learnedNovel = 0;
    QTextStream out(stdout);
    if (perUser) out << "user\tsentences\ttest_words\tnext_top3_off\tnext_top3_on\tletter1_top2_off\tletter1_top2_on\n";
    for (auto &entry : byUser) {
        auto &list = entry.second;
        if (int(list.size()) < minSentences) continue;
        // WHY a hash of the name, not its place in the list: a stable even/odd split of people, so weights can be
        // tuned on one half and reported on the other.
        // WHY FNV-1a (not Qt's qHash): the split must come out the same on every machine and every run.
        unsigned h = 2166136261u;
        for (const char c : entry.first.toUtf8()) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
        if ((users == "even" && (h & 1)) || (users == "odd" && !(h & 1))) continue;
        std::sort(list.begin(), list.end(), [](const Sentence &a, const Sentence &b) { return a.id < b.id; });
        const size_t half = list.size() / 2;
        swipekb::PersonalModel personal({}, weights);
        // Learning phase: a sentence every ten minutes, in the order it was written.
        const long long start = 1700000000;
        long long now = start;
        for (size_t s = 0; s < half; ++s, now += 600)
            for (const auto &w : wordsOf(list[s].text)) {
                const bool isKnown = known.count(w.lower.toStdString()) != 0;
                personal.learn(prevKey(w), w.lower.toStdString(), isKnown, w.capitalized, now);
                ++learnedWords;
                learnedNovel += !isKnown;
            }
        ++people;
        Tally userNext, userOne;
        const long long testNow = now + 86400;
        for (size_t s = half; s < list.size(); ++s)
            for (const auto &w : wordsOf(list[s].text)) {
                const std::string prev = prevKey(w);
                for (int letters = 0; letters <= 2; ++letters) {
                    if (letters && w.lower.size() <= letters) continue;
                    const std::string prefix = w.lower.left(letters).toStdString();
                    const auto base = predictor.predict(w.context, 12, prefix);
                    std::vector<std::string> keys;
                    for (const auto &b : base) keys.push_back(lower(b).toStdString());
                    std::vector<std::string> personalList;
                    for (const auto &r : personal.rerank(keys, prev, prefix, testNow, 12, true)) personalList.push_back(r.key);
                    const int off = rankOf(base, w.lower), on = rankOf(personalList, w.lower);
                    (letters == 0 ? nextWord : letters == 1 ? afterOne : afterTwo).add(off, on);
                    if (letters == 0) userNext.add(off, on);
                    if (letters == 1) userOne.add(off, on);
                }
            }
        if (perUser)
            out << entry.first << '\t' << list.size() << '\t' << userNext.n << '\t' << userNext.pct(0, 3) << '\t' << userNext.pct(1, 3)
                << '\t' << userOne.pct(0, 2) << '\t' << userOne.pct(1, 2) << '\n';
    }
    out << "label\tusers\tlearn_words\tnovel%\tmeasure\twords\toff%\ton%\tgain\n";
    const auto row = [&](const char *name, const Tally &t, int top) {
        out << label << '\t' << people << '\t' << learnedWords << '\t'
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
    return people ? 0 : 1;
}
