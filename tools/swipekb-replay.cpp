// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// swipekb-replay — re-runs recorded swipes through the recognition engine and scores the guesses.
//
// WHY: tuning recognition needs evidence from the owner's real swipes. The keyboard can record swipes
// (plugin/swiperecorder.h -> traces/swipes.jsonl); this tool replays them offline through the keyboard's own engine
// set-up (same profile check, same files, same texting words as the plugin and the Gaming Mode daemon), optionally
// with another dictionary, model or n-gram file, and prints how often the intended word was the first guess or in
// the top three. Nothing here touches the live keyboard.
//
// Usage:
//   swipekb-replay --data DIR --swipes swipes.jsonl --expect words.txt
//                  [--locale en_US] [--dict PATH] [--model PATH] [--no-model] [--ngram PATH] [--no-ngram]
// words.txt: one expected word per line, in the same order as the swipes; "-" (or a blank line) = skip.

#include <QByteArray>
#include <QHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "languageprofile.h"
#include "languageruntime.h"
#include "languagetextrules.h"
#include "swipekb/language_engine.hpp"

namespace {

struct Args {
    std::string data, swipes, expect, dict, model, ngram, candidatesJson, locale = "en_US";
    bool noModel = false, noNgram = false;
};

void usage()
{
    std::fprintf(stderr,
                 "usage: swipekb-replay --data DIR --swipes swipes.jsonl --expect words.txt\n"
                 "                      [--locale en_US] [--dict PATH] [--model PATH] [--no-model] [--ngram PATH] [--no-ngram]\n"
                 "  --data DIR      installed data folder (dict/, language-packs/), e.g. /usr/share/swipe-keyboard\n"
                 "  --swipes FILE   recorded swipes, one JSON line each (traces/swipes.jsonl)\n"
                 "  --expect FILE   intended word per swipe, one per line, same order; '-' skips a swipe\n"
                 "  --locale L      language profile (default en_US; other languages read DIR/language-packs)\n"
                 "  --dict PATH     use another dictionary file\n"
                 "  --model PATH    use another swipe model file;  --no-model: geometry only\n"
                 "  --ngram PATH    use another n-gram file;       --no-ngram: word frequency only\n"
                 "  --candidates-json FILE  also write every swipe's exact guesses and scores as JSON\n"
                 );
}

bool parseArgs(int argc, char **argv, Args &a)
{
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto value = [&](std::string &out) {
            if (i + 1 >= argc)
                return false;
            out = argv[++i];
            return true;
        };
        if (s == "--data") {
            if (!value(a.data)) return false;
        } else if (s == "--swipes") {
            if (!value(a.swipes)) return false;
        } else if (s == "--expect") {
            if (!value(a.expect)) return false;
        } else if (s == "--candidates-json") {
            // WHY: the table above cuts long words to fit its columns; the engine-daemon equivalence test
            // (tests/gaming/test_daemon.py) needs every guess and score exactly, so it reads this file instead.
            if (!value(a.candidatesJson)) return false;
        } else if (s == "--dict") {
            if (!value(a.dict)) return false;
        } else if (s == "--model") {
            if (!value(a.model)) return false;
        } else if (s == "--ngram") {
            if (!value(a.ngram)) return false;
        } else if (s == "--locale") {
            if (!value(a.locale)) return false;
        } else if (s == "--no-model") {
            a.noModel = true;
        } else if (s == "--no-ngram") {
            a.noNgram = true;
        } else {
            // WHY name the argument: an unknown option used to print only the usage text, on stderr. A run that
            // hides stderr (2>/dev/null) then showed nothing at all and looked like a crash. That happened with
            // o="--no-model --no-ngram"; $R ... $o in zsh, which (unlike bash) passes $o as ONE argument with
            // a space inside, so no option matched. Saying exactly which argument was refused, and why it
            // might have happened, turns that silent "crash" into a one-line explanation.
            std::fprintf(stderr, "swipekb-replay: unknown option '%s'\n", s.c_str());
            if (s.find(' ') != std::string::npos)
                std::fprintf(stderr, "  (it contains a space: several options were passed as one argument. In zsh an "
                                     "unquoted $var is not split; write ${=var} or list the options separately.)\n");
            return false;
        }
    }
    if (a.data.empty() || a.swipes.empty() || a.expect.empty()) {
        // WHY: same reason as above, say what is missing instead of only reprinting the usage text.
        std::fprintf(stderr, "swipekb-replay: --data, --swipes and --expect are all required\n");
        return false;
    }
    return true;
}

// One recorded swipe, converted to what the engine takes.
struct Swipe {
    std::vector<swipekb::TracePoint> trace;
    std::vector<swipekb::KeyRect> keys;
    std::vector<std::string> context;
    QString typed;   // what the live keyboard typed at the time (for comparison)
};

bool readSwipes(const std::string &path, std::vector<Swipe> &out)
{
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return false;
    }
    int lineNo = 0;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        ++lineNo;
        if (line.isEmpty())
            continue;
        QJsonParseError err;
        const QJsonObject o = QJsonDocument::fromJson(line, &err).object();
        if (err.error != QJsonParseError::NoError) {
            // WHY stop: a skipped bad line would shift every later swipe against the expected-words list.
            std::fprintf(stderr, "%s line %d: not valid JSON (%s)\n", path.c_str(), lineNo,
                         qPrintable(err.errorString()));
            return false;
        }
        // WHY skip: the file also holds guess-bar "pick" lines (corrections, see plugin/swiperecorder.h). They are
        // not swipes, so counting them would shift every later swipe against the expected-words list. Older
        // recordings have no "type" at all; those lines are swipes.
        const QString type = o.value("type").toString(QStringLiteral("swipe"));
        if (type != QStringLiteral("swipe"))
            continue;
        Swipe s;
        for (const QJsonValue &v : o.value("points").toArray()) {
            const QJsonObject p = v.toObject();
            s.trace.push_back({float(p.value("x").toDouble()), float(p.value("y").toDouble()),
                               float(p.value("t").toDouble())});
        }
        for (const QJsonValue &v : o.value("keys").toArray()) {
            const QJsonObject k = v.toObject();
            const QString letter = k.value("letter").toString();
            if (letter.size() != 1)
                continue;
            s.keys.push_back({char32_t(letter.at(0).unicode()), float(k.value("x").toDouble()), float(k.value("y").toDouble()),
                              float(k.value("w").toDouble()), float(k.value("h").toDouble())});
        }
        for (const QJsonValue &v : o.value("context").toArray())
            s.context.push_back(v.toString().toStdString());
        s.typed = o.value("typed").toString();
        out.push_back(std::move(s));
    }
    return true;
}

bool readExpected(const std::string &path, QStringList &out)
{
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return false;
    }
    out = QString::fromUtf8(f.readAll()).split('\n');
    // WHY: a file ending in a newline would otherwise get one phantom empty entry at the end.
    if (!out.isEmpty() && out.last().trimmed().isEmpty())
        out.removeLast();
    for (QString &w : out)
        w = w.trimmed();
    return true;
}

// Fixed-width cell; long words are cut so the table stays aligned.
std::string cell(const QString &s, int width)
{
    QString t = s.size() > width - 1 ? s.left(width - 2) + QStringLiteral("~") : s;
    return t.leftJustified(width).toStdString();
}

double pct(int n, int d) { return d ? 100.0 * n / d : 0.0; }

} // namespace

int main(int argc, char **argv)
{
    Args a;
    if (!parseArgs(argc, argv, a)) {
        usage();
        return 2;
    }

    // WHY the keyboard's own profile check and set-up: by default the tool measures what ships.
    const QString data = QString::fromStdString(a.data), locale = QString::fromStdString(a.locale);
    const auto profile = swipekb::resolveProfile(locale, QStringLiteral("qwerty"), QStringLiteral("Latin"),
                                                 swipekb::languageModelRoot(locale, data));
    swipekb::LanguageEngineSetup setup;
    if (!swipekb::configureLanguageEngine(profile, setup) && a.dict.empty()) {
        std::fprintf(stderr, "no usable language profile in %s (%s)\n", a.data.c_str(), qPrintable(profile.unavailableReason));
        return 1;
    }
    setup.language = swipekb::canonicalLocale(locale).section('-', 0, 0).toStdString();
    if (!a.dict.empty()) setup.dictionary = a.dict;
    if (!a.model.empty()) setup.swipeModel = a.model;
    if (!a.ngram.empty()) setup.ngram = a.ngram;
    if (a.noModel) setup.swipeModel.clear();
    if (a.noNgram) setup.ngram.clear();

    std::vector<Swipe> swipes;
    QStringList expected;
    if (!readSwipes(a.swipes, swipes) || !readExpected(a.expect, expected))
        return 1;
    if (static_cast<qsizetype>(swipes.size()) != expected.size())
        std::fprintf(stderr, "warning: %zu swipes but %lld expected words; only the first %lld are compared\n",
                     swipes.size(), static_cast<long long>(expected.size()),
                     static_cast<long long>(std::min<qsizetype>(swipes.size(), expected.size())));

    swipekb::LanguageEngine r;
    if (!r.init(setup)) {
        std::fprintf(stderr, "engine failed to load from %s\n", a.data.c_str());
        return 1;
    }
    // WHY the texting words, as the keyboard adds them (SwipeInputMethod::syncLearnedWords).
    r.liftWords(swipekb::textingWordPairs(QString::fromStdString(setup.language)));
    r.setCapitals(swipekb::capitalsFor(QString::fromStdString(setup.language)));   // as the keyboard does
    // Joint-model step 3: the learned re-ranker without a sentence scorer, as the Gaming Mode daemon sets it (these
    // replays have no words before, so the keyboards order them the same way).
    auto rescore = swipekb::ContextRescore::forLanguage(setup.language);
    rescore.scorer = nullptr;
    if (rescore.reranker) r.setContextRescore(rescore);

    // WHY "ranking"/"engine": a missing file silently falls back; the output must say which parts ran.
    std::printf("settings: engine=%s ranking=%s dict=%s\n", r.mode(), r.usingNgram() ? "ngram" : "frequency",
                setup.dictionary.c_str());
    std::printf("%-4s %s%s%s%s%s%s\n", "#", cell("expected", 14).c_str(), cell("guess1", 14).c_str(),
                cell("guess2", 14).c_str(), cell("guess3", 14).c_str(), cell("result", 8).c_str(), "live-typed");

    QJsonArray exact;   // per swipe: [[word, score], ...]; empty array for skipped swipes
    const size_t n = std::min<size_t>(swipes.size(), static_cast<size_t>(expected.size()));
    int scored = 0, skipped = 0, first = 0, top3 = 0;
    for (size_t i = 0; i < n; ++i) {
        const QString want = expected.at(static_cast<qsizetype>(i));
        if (want.isEmpty() || want == QStringLiteral("-")) {
            exact.append(QJsonArray());
            ++skipped;
            std::printf("%-4zu %s(skipped)\n", i + 1, cell("-", 14).c_str());
            continue;
        }
        const Swipe &s = swipes[i];
        // WHY 30 guesses, then the regional ranking cut to 3: the same steps as the keyboard (and the daemon), so
        // the replayed guesses are the ones the guess bar would show.
        QStringList guesses;
        QHash<QString, double> scoreOf;
        for (const auto &c : r.recognize(s.trace, s.keys, s.context, 30)) {
            const auto w = QString::fromStdString(c.word);
            if (!scoreOf.contains(w)) scoreOf.insert(w, double(c.score));
            guesses << w;
        }
        guesses = swipekb::rankRegionalCandidates(guesses, swipekb::canonicalLocale(locale), 3, QString::fromStdString(r.bestShapeWord()));
        QJsonArray exactRow;
        for (const auto &w : guesses) exactRow.append(QJsonArray{w, scoreOf.value(w)});
        exact.append(exactRow);
        // WHY case-insensitive: the engine may capitalize ("I", names); the shape of the swipe is what counts.
        int rank = -1;
        for (int k = 0; k < guesses.size(); ++k)
            if (guesses.at(k).compare(want, Qt::CaseInsensitive) == 0) {
                rank = k;
                break;
            }
        ++scored;
        if (rank == 0)
            ++first;
        if (rank >= 0)
            ++top3;
        const char *result = rank == 0 ? "first" : rank > 0 ? "top3" : "MISS";
        std::printf("%-4zu %s%s%s%s%s%s\n", i + 1, cell(want, 14).c_str(), cell(guesses.value(0), 14).c_str(),
                    cell(guesses.value(1), 14).c_str(), cell(guesses.value(2), 14).c_str(), cell(result, 8).c_str(),
                    qPrintable(s.typed));
    }

    std::printf("\nsummary: %d scored, %d skipped\n", scored, skipped);
    std::printf("first guess: %d/%d (%.1f%%)\n", first, scored, pct(first, scored));
    std::printf("top 3:       %d/%d (%.1f%%)\n", top3, scored, pct(top3, scored));
    if (!a.candidatesJson.empty()) {
        QFile out(QString::fromStdString(a.candidatesJson));
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || out.write(QJsonDocument(exact).toJson(QJsonDocument::Compact)) < 0) {
            std::fprintf(stderr, "cannot write %s\n", a.candidatesJson.c_str());
            return 1;
        }
    }
    return 0;
}
