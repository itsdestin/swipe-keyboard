// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// swipekb-typing-bench — the typing half of the user-experience scoreboard (bench/ux-score.sh). Types each test
// sentence letter by letter (tools/typingsim.h) on the bar the keyboard builds:
//   before a word  the next-word bar: the prediction worker's answer (Predictor::predict, no letters, with the
//                  capitals list), then barflow::nextWordOffer and barrules::nextWordShown, as both front ends do.
//                  None at the start of a sentence (the keyboard asks only when there are words before the cursor).
//   after letters  the completion bar once the worker has answered: Predictor::predict with the letters, then the
//                  dictionary's LanguageEngine::complete, barflow::completionOffer and barrules::typedShown (the typed
//                  letters keep one of the three slots).
// It assumes the person waits for the worker (a few milliseconds) before looking, and has no learning switched on.
// Prints typingsim::Tally::format() and a "setup:" line.
//
//   swipekb-typing-bench --dict D --ngram N --vocab V [--nwp W] --language L [--locale en_US] --sentences FILE
//                        [--limit N]
#include <QString>
#include <QStringList>
#include <QFile>
#include <QTextStream>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "barflow.h"
#include "languagetextrules.h"
#include "nextwordsession.h"
#include "swipekb/language_engine.hpp"
#include "swipekb/prediction.hpp"
#include "typingsim.h"

int main(int argc, char **argv)
{
    swipekb::LanguageEngineSetup setup;
    std::string vocab, nwp, sentences, locale = "en_US";
    long long limit = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--dict") setup.dictionary = val();
        else if (a == "--ngram") setup.ngram = val();
        else if (a == "--vocab") vocab = val();
        else if (a == "--nwp") nwp = val();
        else if (a == "--language") setup.language = val();
        else if (a == "--locale") locale = val();
        else if (a == "--sentences") sentences = val();
        else if (a == "--limit") limit = std::atoll(val().c_str());
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (setup.dictionary.empty() || setup.ngram.empty() || vocab.empty() || sentences.empty()) {
        std::fprintf(stderr, "usage: swipekb-typing-bench --dict D --ngram N --vocab V [--nwp W] --language L"
                             " [--locale en_US] --sentences FILE [--limit N]\n");
        return 2;
    }
    swipekb::LanguageEngine engine;
    if (!engine.init(setup)) { std::fprintf(stderr, "cannot load the dictionary %s\n", setup.dictionary.c_str()); return 1; }
    const QString language = QString::fromStdString(setup.language);
    const QString qlocale = QString::fromStdString(locale);
    // WHY both, as SwipeInputMethod::syncLearnedWords does: texting words reach completions, and capitals are how
    // the keyboard spells the words it offers.
    engine.liftWords(swipekb::textingWordPairs(language));
    engine.setCapitals(swipekb::capitalsFor(language));
    // The prediction worker's model (plugin/nextwordworker.cpp). WHY siblingFallback=false: the keyboard only loads a
    // neural file it was handed.
    swipekb::Predictor predictor;
    if (!predictor.init(setup.ngram, vocab, nwp, false) || !predictor.ready()) {
        std::fprintf(stderr, "cannot load the sentence model %s\n", setup.ngram.c_str());
        return 1;
    }
    const auto capitals = swipekb::capitalsFor(language);
    // The worker's answer: at most 12 words, in the capitals list's spelling (NextWordWorker::run).
    auto worker = [&](const QStringList &context, const QString &prefix) {
        std::vector<std::string> ctx;
        // WHY the last 20: NextWordWorker::submit keeps no more.
        for (const auto &w : context.mid(qMax<qsizetype>(0, context.size() - 20))) ctx.push_back(w.left(256).toStdString());
        QStringList words;
        for (const auto &w : predictor.predict(ctx, 12, prefix.toStdString()))
            words << QString::fromStdString(capitals ? capitals->apply(w) : w);
        return words;
    };
    typingsim::Bar bar;
    bar.nextWord = [&](const QString &text) -> QStringList {
        // WHY nothing at a sentence start: the keyboard requests next words only when the text before the cursor has
        // words in the current sentence (SwipeInputMethod::observePredictionEcho).
        const QStringList context = swipekb::textContextWords(text);
        if (context.isEmpty()) return {};
        return barrules::nextWordShown(barflow::nextWordOffer(worker(context, {}), language, qlocale, {}, {}));
    };
    bar.completion = [&](const QString &text, const QString &typed) {
        // WHY the text before the letters: requestContextCompletion asks with the words before the typed letters.
        const QStringList ranked = worker(swipekb::textContextWords(text), typed);
        QStringList dictionary;
        for (const auto &c : engine.complete(typed.toStdString(), 12)) dictionary << QString::fromStdString(c.word);
        return barrules::typedShown(typed, barflow::completionOffer(ranked, dictionary, qlocale, {}));
    };

    QFile file(QString::fromStdString(sentences));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) { std::fprintf(stderr, "cannot read %s\n", sentences.c_str()); return 1; }
    QTextStream in(&file);
    typingsim::Tally tally;
    while (!in.atEnd() && (limit < 0 || (long long)tally.lines < limit)) {
        const QString line = in.readLine().trimmed();
        if (!line.isEmpty()) typingsim::typeLine(line, bar, tally);
    }
    std::printf("%s", tally.format().c_str());
    std::printf("setup: language=%s nwp=%d\n", setup.language.c_str(), nwp.empty() ? 0 : 1);
    return tally.words ? 0 : 1;
}
