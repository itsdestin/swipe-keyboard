// swipekb-nextword-eval: how often a language pack's next-word suggestions contain the word the writer
// actually typed next. Replays plain sentences (one per line) word by word through the keyboard's own
// n-gram prediction code (swipekb::NgramPredictor) and context rules (swipekb::contextWords).
//
// WHY top-1 and top-3: the guess bar shows three suggestions, best in the middle; top-12 is everything the
// keyboard asks the model for, i.e. the ceiling any re-ranking could reach.
// WHY case-insensitive: capitals are display (sentence start, German nouns); the model scores lower case.
//
//   swipekb-nextword-eval --model es.ngram --vocab es.vocab.tsv --sentences test.txt [--label text]
//                         [--limit N] [--dump]
// Prints one tab-separated result row (with a header); --dump adds one row per scored word.
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
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString model, vocab, sentences, label = QStringLiteral("unnamed");
    long long limit = -1;
    bool dump = false;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args[i];
        const bool hasValue = i + 1 < args.size();
        if (a == "--model" && hasValue) model = args[++i];
        else if (a == "--vocab" && hasValue) vocab = args[++i];
        else if (a == "--sentences" && hasValue) sentences = args[++i];
        else if (a == "--label" && hasValue) label = args[++i];
        else if (a == "--limit" && hasValue) limit = args[++i].toLongLong();
        else if (a == "--dump") dump = true;
        else { std::fprintf(stderr, "unknown or incomplete option: %s\n", qPrintable(a)); return 2; }
    }
    if (model.isEmpty() || vocab.isEmpty() || sentences.isEmpty()) {
        std::fprintf(stderr, "usage: swipekb-nextword-eval --model M --vocab V --sentences FILE [--label L] [--limit N] [--dump]\n");
        return 2;
    }
    swipekb::NgramPredictor predictor;
    if (!predictor.load(model.toStdString(), vocab.toStdString())) {
        std::fprintf(stderr, "could not load model %s with vocabulary %s\n", qPrintable(model), qPrintable(vocab));
        return 1;
    }
    QFile file(sentences);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr, "could not read %s\n", qPrintable(sentences));
        return 1;
    }
    std::unordered_set<std::string> known;
    for (const auto &word : predictor.vocabulary()) known.insert(lower(word).toStdString());

    long long lines = 0, scored = 0, inVocab = 0, top1 = 0, top3 = 0, top12 = 0;
    long long midScored = 0, midTop1 = 0, midTop3 = 0;
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
            const QString prefix = chunks.mid(0, k).join(QLatin1Char(' ')) + QLatin1Char(' ');
            const auto context = swipekb::contextWords(prefix.toStdString(), 20);
            QElapsedTimer timer;
            timer.start();
            const auto suggestions = predictor.predict(context, 12);
            nanos += timer.nsecsElapsed();
            int rank = 0;
            for (size_t r = 0; r < suggestions.size() && !rank; ++r)
                if (lower(suggestions[r]) == target) rank = int(r) + 1;
            ++scored;
            if (known.count(target.toStdString())) ++inVocab;
            if (rank == 1) ++top1;
            if (rank && rank <= 3) ++top3;
            if (rank) ++top12;
            // WHY a separate mid-sentence figure: sentence starts are guessed with no context at all.
            if (!context.empty()) {
                ++midScored;
                if (rank == 1) ++midTop1;
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
    out << "label\tsentences\twords\ttop1%\ttop3%\ttop12%\tmid_top1%\tmid_top3%\tin_vocab%\tms_per_prediction\n"
        << label << '\t' << lines << '\t' << scored << '\t' << percent(top1, scored) << '\t' << percent(top3, scored)
        << '\t' << percent(top12, scored) << '\t' << percent(midTop1, midScored) << '\t' << percent(midTop3, midScored)
        << '\t' << percent(inVocab, scored) << '\t'
        << (scored ? QString::number(double(nanos) / 1e6 / double(scored), 'f', 3) : QStringLiteral("0")) << '\n';
    return scored ? 0 : 1;
}
