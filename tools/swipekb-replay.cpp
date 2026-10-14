// swipekb-replay — re-runs recorded swipes through the recognition engine and scores the guesses.
//
// WHY: tuning recognition needs evidence from the owner's real swipes. The keyboard can record swipes
// (plugin/swiperecorder.h -> traces/swipes.jsonl); this tool replays them offline with different settings
// (no decoder, no context model, other beam width, other dictionary) and prints how often the intended word
// was the first guess or in the top three. Nothing here touches the live keyboard.
//
// Usage:
//   swipekb-replay --data DIR --swipes swipes.jsonl --expect words.txt
//                  [--no-decoder] [--no-lm] [--beam N] [--dict PATH]
// words.txt: one expected word per line, in the same order as the swipes; "-" (or a blank line) = skip.

#include <QByteArray>
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

#include "swipekb/recognizer.hpp"

namespace {

struct Args {
    std::string data, swipes, expect, dict;
    bool noDecoder = false, noLm = false;
    int beam = 0;
};

void usage()
{
    std::fprintf(stderr,
                 "usage: swipekb-replay --data DIR --swipes swipes.jsonl --expect words.txt\n"
                 "                      [--no-decoder] [--no-lm] [--beam N] [--dict PATH]\n"
                 "  --data DIR      folder with models/ and dict/ (e.g. ~/.local/share/swipe-keyboard)\n"
                 "  --swipes FILE   recorded swipes, one JSON line each (traces/swipes.jsonl)\n"
                 "  --expect FILE   intended word per swipe, one per line, same order; '-' skips a swipe\n"
                 "  --no-decoder    encoder only (drop the second model)\n"
                 "  --no-lm         drop the context language model\n"
                 "  --beam N        beam width (default: engine default, 100)\n"
                 "  --dict PATH     use another dictionary file\n");
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
        } else if (s == "--dict") {
            if (!value(a.dict)) return false;
        } else if (s == "--beam") {
            std::string n;
            if (!value(n)) return false;
            a.beam = std::atoi(n.c_str());
            if (a.beam <= 0) return false;
        } else if (s == "--no-decoder") {
            a.noDecoder = true;
        } else if (s == "--no-lm") {
            a.noLm = true;
        } else {
            return false;
        }
    }
    return !a.data.empty() && !a.swipes.empty() && !a.expect.empty();
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
            s.keys.push_back({letter.at(0).toLatin1(), float(k.value("x").toDouble()), float(k.value("y").toDouble()),
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

    swipekb::ModelPaths paths = swipekb::ModelPaths::fromDir(a.data);
    // WHY clearing a path drops that model: the engine only loads the optional models whose files exist.
    if (a.noDecoder)
        paths.decoder.clear();
    if (a.noLm) {
        paths.lm_model.clear();
        paths.lm_vocab.clear();
    }
    if (!a.dict.empty())
        paths.dictionary = a.dict;

    std::vector<Swipe> swipes;
    QStringList expected;
    if (!readSwipes(a.swipes, swipes) || !readExpected(a.expect, expected))
        return 1;
    if (static_cast<qsizetype>(swipes.size()) != expected.size())
        std::fprintf(stderr, "warning: %zu swipes but %lld expected words; only the first %lld are compared\n",
                     swipes.size(), static_cast<long long>(expected.size()),
                     static_cast<long long>(std::min<qsizetype>(swipes.size(), expected.size())));

    swipekb::Recognizer r;
    if (!r.init(paths, {.beam_width = a.beam})) {
        std::fprintf(stderr, "engine failed to load from %s\n", a.data.c_str());
        return 1;
    }

    std::printf("settings: decoder=%s lm=%s beam=%s dict=%s\n", a.noDecoder ? "off" : "on", a.noLm ? "off" : "on",
                a.beam ? std::to_string(a.beam).c_str() : "default", paths.dictionary.c_str());
    std::printf("%-4s %s%s%s%s%s%s\n", "#", cell("expected", 14).c_str(), cell("guess1", 14).c_str(),
                cell("guess2", 14).c_str(), cell("guess3", 14).c_str(), cell("result", 8).c_str(), "live-typed");

    const size_t n = std::min<size_t>(swipes.size(), static_cast<size_t>(expected.size()));
    int scored = 0, skipped = 0, first = 0, top3 = 0;
    for (size_t i = 0; i < n; ++i) {
        const QString want = expected.at(static_cast<qsizetype>(i));
        if (want.isEmpty() || want == QStringLiteral("-")) {
            ++skipped;
            std::printf("%-4zu %s(skipped)\n", i + 1, cell("-", 14).c_str());
            continue;
        }
        const Swipe &s = swipes[i];
        const auto cands = r.recognize(s.trace, s.keys, s.context, 3);
        QStringList guesses;
        for (const auto &c : cands)
            guesses << QString::fromStdString(c.word);
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
    return 0;
}
