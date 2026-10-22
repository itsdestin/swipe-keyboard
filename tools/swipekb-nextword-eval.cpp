// swipekb-nextword-eval: how often a language pack's next-word suggestions contain the word the writer
// actually typed next. Replays plain sentences (one per line) word by word through the keyboard's own
// n-gram prediction code (swipekb::NgramPredictor) and context rules (swipekb::contextWords).
//
// WHY top-1 and top-3: the guess bar shows three suggestions, best in the middle; top-12 is everything the
// keyboard asks the model for, i.e. the ceiling any re-ranking could reach.
// WHY case-insensitive: capitals are display (sentence start, German nouns); the model scores lower case.
// WHY --letters N: completion after the first N letters of the word, through the same predict() call the
// keyboard makes while a word is being typed. Words of N letters or fewer are skipped (nothing to complete),
// as in the Gboard comparison. top2% is the fair figure there: the keyboard's third slot shows the typed
// letters themselves, exactly like Gboard's.
//
// WHY --gboard: replays the exact queries Gboard answered (bench/gboard/*.tsv, recorded by gboard_probe.py)
// through our predictor, so "ours vs Gboard" is one command and uses the keyboard's own code.
//
//   swipekb-nextword-eval --model es.ngram --vocab es.vocab.tsv --sentences test.txt [--label text]
//                         [--limit N] [--letters N] [--dump]
//   swipekb-nextword-eval --model es.ngram --vocab es.vocab.tsv --gboard es-everyday.tsv [--label text]
// Prints one tab-separated result row (with a header); --dump adds one row per scored word. --gboard prints
// one row per number of typed letters (0-3).
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "swipekb/ngram_predictor.hpp"
#include "swipekb/prediction.hpp"
#include "swipekb/recognizer.hpp"

namespace {
QString lower(const std::string &word) { return QString::fromUtf8(word.data(), int(word.size())).toLower(); }

QString percent(long long part, long long whole)
{
    return whole ? QString::number(100.0 * double(part) / double(whole), 'f', 2) : QStringLiteral("0.00");
}

// Rows: sentence# word# letters context target gboard_left gboard_middle gboard_right.
int compareWithGboard(const swipekb::NgramPredictor &predictor, const QString &path, const QString &label)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr, "could not read %s\n", qPrintable(path));
        return 1;
    }
    long long n[4] = {}, g1[4] = {}, g3[4] = {}, o1[4] = {}, o2[4] = {}, o3[4] = {};
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QStringList c = in.readLine().split(QLatin1Char('\t'));
        if (c.size() < 8) continue;
        const int k = c[2].toInt();
        // WHY skip sentence starts: Gboard shows shortcut chips there, not words.
        if (c[1] == QLatin1String("0") || k < 0 || k > 3) continue;
        // WHY one apostrophe: "c’est" typed and "c'est" suggested are the same word to the person.
        const auto same = [](QString word) { return word.toLower().replace(QChar(0x2019), QLatin1Char('\'')); };
        const QString target = same(c[4]);
        const bool middle = same(c[6]) == target;
        const bool any = middle || same(c[5]) == target || same(c[7]) == target;
        const auto suggestions = predictor.predict(swipekb::contextWords(c[3].toStdString(), 20), 3,
                                                   target.left(k).toStdString());
        int rank = 0;
        for (size_t r = 0; r < suggestions.size() && !rank; ++r)
            if (same(QString::fromStdString(suggestions[r])) == target) rank = int(r) + 1;
        ++n[k]; g1[k] += middle; g3[k] += any;
        o1[k] += rank == 1; o2[k] += rank && rank <= 2; o3[k] += rank && rank <= 3;
    }
    // WHY ours_top2 is the fair figure once letters are typed: Gboard (and our keyboard) give one of the 3
    // slots to the typed letters themselves.
    QTextStream out(stdout);
    out << "label\tletters\tqueries\tgboard_top1%\tgboard_top3%\tours_top1%\tours_top2%\tours_top3%\n";
    for (int k = 0; k < 4; ++k)
        if (n[k])
            out << label << '\t' << k << '\t' << n[k] << '\t' << percent(g1[k], n[k]) << '\t' << percent(g3[k], n[k])
                << '\t' << percent(o1[k], n[k]) << '\t' << percent(o2[k], n[k]) << '\t' << percent(o3[k], n[k]) << '\n';
    return n[0] + n[1] + n[2] + n[3] ? 0 : 1;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString model, vocab, sentences, gboard, label = QStringLiteral("unnamed");
    long long limit = -1;
    int letters = 0;
    bool dump = false;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args[i];
        const bool hasValue = i + 1 < args.size();
        if (a == "--model" && hasValue) model = args[++i];
        else if (a == "--vocab" && hasValue) vocab = args[++i];
        else if (a == "--sentences" && hasValue) sentences = args[++i];
        else if (a == "--gboard" && hasValue) gboard = args[++i];
        else if (a == "--label" && hasValue) label = args[++i];
        else if (a == "--limit" && hasValue) limit = args[++i].toLongLong();
        else if (a == "--letters" && hasValue) letters = qBound(0, args[++i].toInt(), 64);
        else if (a == "--dump") dump = true;
        else { std::fprintf(stderr, "unknown or incomplete option: %s\n", qPrintable(a)); return 2; }
    }
    if (model.isEmpty() || vocab.isEmpty() || (sentences.isEmpty() == gboard.isEmpty())) {
        std::fprintf(stderr, "usage: swipekb-nextword-eval --model M --vocab V (--sentences FILE | --gboard TSV) "
                             "[--label L] [--limit N] [--letters N] [--dump]\n");
        return 2;
    }
    swipekb::NgramPredictor predictor;
    if (!predictor.load(model.toStdString(), vocab.toStdString())) {
        std::fprintf(stderr, "could not load model %s with vocabulary %s\n", qPrintable(model), qPrintable(vocab));
        return 1;
    }
    if (!gboard.isEmpty()) return compareWithGboard(predictor, gboard, label);
    QFile file(sentences);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr, "could not read %s\n", qPrintable(sentences));
        return 1;
    }
    std::unordered_set<std::string> known;
    for (const auto &word : predictor.vocabulary()) known.insert(lower(word).toStdString());

    long long lines = 0, scored = 0, inVocab = 0, top1 = 0, top2 = 0, top3 = 0, top12 = 0;
    long long midScored = 0, midTop1 = 0, midTop2 = 0, midTop3 = 0;
    qint64 nanos = 0;
    QTextStream in(&file);
    QTextStream out(stdout);
    if (dump) out << "context\ttarget\trank\tsuggestions\n";
    while (!in.atEnd() && (limit < 0 || lines < limit)) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;
        ++lines;
        const QStringList chunks = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        for (qsizetype k = 0; k < chunks.size(); ++k) {
            // WHY whole-chunk targets through contextWords: punctuation is stripped exactly as the keyboard
            // strips it from the text before the cursor, so targets and context agree.
            // WHY trailing . ! ? removed first: contextWords starts a new sentence after them, which would
            // otherwise drop every sentence's last word ("aquí." -> nothing).
            QString chunk = chunks[k];
            while (!chunk.isEmpty() && QStringLiteral(".!?").contains(chunk.back())) chunk.chop(1);
            const auto targetWords = swipekb::contextWords(chunk.toStdString(), 1);
            if (targetWords.empty() || swipekb::filterPredictedWords({{targetWords.front(), 0.f}}, 1).empty()) continue;
            const QString target = lower(targetWords.front());
            if (letters && target.size() <= letters) continue;
            const QString prefix = chunks.mid(0, k).join(QLatin1Char(' ')) + QLatin1Char(' ');
            const auto context = swipekb::contextWords(prefix.toStdString(), 20);
            QElapsedTimer timer;
            timer.start();
            const auto suggestions = predictor.predict(context, 12, target.left(letters).toStdString());
            nanos += timer.nsecsElapsed();
            int rank = 0;
            for (size_t r = 0; r < suggestions.size() && !rank; ++r)
                if (lower(suggestions[r]) == target) rank = int(r) + 1;
            ++scored;
            if (known.count(target.toStdString())) ++inVocab;
            if (rank == 1) ++top1;
            if (rank && rank <= 2) ++top2;
            if (rank && rank <= 3) ++top3;
            if (rank) ++top12;
            // WHY a separate mid-sentence figure: sentence starts are guessed with no context at all.
            if (!context.empty()) {
                ++midScored;
                if (rank == 1) ++midTop1;
                if (rank && rank <= 2) ++midTop2;
                if (rank && rank <= 3) ++midTop3;
            }
            if (dump) {
                QStringList shown;
                for (const auto &s : suggestions) shown << QString::fromStdString(s);
                QStringList contextShown;
                for (const auto &c : context) contextShown << QString::fromStdString(c);
                out << contextShown.join(' ') << '\t' << target << '\t' << rank << '\t' << shown.join(' ') << '\n';
            }
        }
    }
    out << "label\tsentences\twords\ttop1%\ttop3%\ttop12%\tmid_top1%\tmid_top3%\tin_vocab%\tms_per_prediction"
           "\ttop2%\tmid_top2%\tletters\n"
        << label << '\t' << lines << '\t' << scored << '\t' << percent(top1, scored) << '\t' << percent(top3, scored)
        << '\t' << percent(top12, scored) << '\t' << percent(midTop1, midScored) << '\t' << percent(midTop3, midScored)
        << '\t' << percent(inVocab, scored) << '\t'
        << (scored ? QString::number(double(nanos) / 1e6 / double(scored), 'f', 3) : QStringLiteral("0"))
        << '\t' << percent(top2, scored) << '\t' << percent(midTop2, midScored) << '\t' << letters << '\n';
    return scored ? 0 : 1;
}
