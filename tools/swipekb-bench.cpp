// swipekb-bench — the scoreboard engine. Replays FUTO's real-swipe test file through OUR swipekb::Recognizer
// (not FUTO's infer_jsonl program), so every accuracy number is measured on exactly the code that ships:
// our key-rect normalisation, our context-word rules, our model/dictionary loading.
//
// Usage:
//   swipekb-bench --data DIR --jsonl FILE [--top-k 3] [--beam N] [--no-decoder] [--no-lm] [--dict PATH]
//                 [--limit N] [--recall-k 50] [--skip-oov] [--layout PATH]
//
// Output copies infer_jsonl's format (good/ok/bad lines, "Results:" block with "top-1:" and "top-4:" lines,
// "Timings" block) so bench/score.sh parses it unchanged, then adds an "Extras:" block.

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <climits>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "benchlayout.h"
#include "swipekb/recognizer.hpp"

namespace {

struct Args {
    std::string data, jsonl, dict, layout;
    int topK = 3, beam = 0, limit = 0, recallK = 50;
    bool noDecoder = false, noLm = false, skipOov = false;
};

void usage()
{
    std::fprintf(stderr,
                 "usage: swipekb-bench --data DIR --jsonl FILE [--top-k 3] [--beam N] [--no-decoder] [--no-lm]\n"
                 "                     [--dict PATH] [--limit N] [--recall-k 50] [--skip-oov] [--layout PATH]\n");
}

bool parseArgs(int argc, char **argv, Args &a)
{
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto str = [&](std::string &out) { if (i + 1 >= argc) return false; out = argv[++i]; return true; };
        auto num = [&](int &out) {
            std::string v;
            if (!str(v)) return false;
            out = std::atoi(v.c_str());
            return out > 0;
        };
        bool ok = true;
        if (s == "--data") ok = str(a.data);
        else if (s == "--jsonl") ok = str(a.jsonl);
        else if (s == "--dict") ok = str(a.dict);
        else if (s == "--layout") ok = str(a.layout);
        else if (s == "--top-k") ok = num(a.topK);
        else if (s == "--beam") ok = num(a.beam);
        else if (s == "--limit") ok = num(a.limit);
        else if (s == "--recall-k") ok = num(a.recallK);
        else if (s == "--no-decoder") a.noDecoder = true;
        else if (s == "--no-lm") a.noLm = true;
        else if (s == "--skip-oov") a.skipOov = true;
        else ok = false;
        if (!ok) return false;
    }
    return !a.data.empty() && !a.jsonl.empty();
}

// Same as infer_jsonl's normalize_word: lower-case, keep a-z and non-ASCII letters, drop everything else
// (apostrophes, hyphens, digits). Applied to both the target and the guesses so "don't" == "dont".
std::string normalizeWord(const QString &w)
{
    QString out;
    for (QChar c : w.toLower())
        if ((c >= u'a' && c <= u'z') || (c.unicode() > 0x7F && c.unicode() < 0xFFFD))
            out += c;
    return out.toStdString();
}

// Words of the dictionary file (" word=the,f=222,..." lines), only used by --skip-oov.
std::unordered_set<std::string> readDictWords(const std::string &path)
{
    std::unordered_set<std::string> words;
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly))
        return words;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        const int p = line.indexOf("word=");
        if (p < 0) continue;
        const int e = line.indexOf(',', p);
        words.insert(normalizeWord(QString::fromUtf8(line.mid(p + 5, e < 0 ? -1 : e - p - 5))));
    }
    return words;
}

} // namespace

int main(int argc, char **argv)
{
    Args a;
    if (!parseArgs(argc, argv, a)) {
        usage();
        return 2;
    }
    swipekb::ModelPaths paths = swipekb::ModelPaths::fromDir(a.data);
    if (a.noDecoder) paths.decoder.clear();
    if (a.noLm) { paths.lm_model.clear(); paths.lm_vocab.clear(); }
    if (!a.dict.empty()) paths.dictionary = a.dict;

    // Layout file: same FUTO file infer_jsonl is given. Default is the copy inside the repo's submodule.
    const std::string layoutPath = !a.layout.empty() ? a.layout
        : std::string(SWIPEKB_DEFAULT_LAYOUT);
    std::vector<benchlayout::LayoutKey> layout;
    if (!benchlayout::loadLayout(QString::fromStdString(layoutPath), layout)) {
        std::fprintf(stderr, "cannot read a 26-key layout from %s\n", layoutPath.c_str());
        return 1;
    }
    constexpr float kScale = 1000.f;
    const std::vector<swipekb::KeyRect> keys = benchlayout::toRects(layout, kScale);

    swipekb::Recognizer r;
    if (!r.init(paths, {.beam_width = a.beam})) {
        std::fprintf(stderr, "engine failed to load from %s\n", a.data.c_str());
        return 1;
    }
    std::unordered_set<std::string> dictWords;
    if (a.skipOov) dictWords = readDictWords(paths.dictionary);

    QFile f(QString::fromStdString(a.jsonl));
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot read %s\n", a.jsonl.c_str());
        return 1;
    }
    // WHY the engine is asked for max(top-k, recall-k) once: with the context model on, FUTO's search always
    // keeps 200 candidates and only truncates at the end, so the first top-k entries of a longer list equal what a
    // top-k call returns. One call gives both the scored top-k and the beam-recall number.
    const int askK = std::max(a.topK, a.recallK);

    int good = 0, ok = 0, bad = 0, recallHit = 0, firstN = 0, firstGood = 0, lineNo = 0, skipped = 0;
    double totalUs = 0;
    while (!f.atEnd()) {
        if (a.limit && good + ok + bad >= a.limit) break;
        const QByteArray line = f.readLine().trimmed();
        ++lineNo;
        if (line.isEmpty()) continue;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        const QJsonValue data = o.value("data");
        // WHY these skips match infer_jsonl: target shorter than 2 letters, and traces of 2 points or fewer.
        // DIFFERENCE: infer_jsonl also accepts the newer {"L":[..],"R":[..]} two-hand format (y scaled by 1.4017);
        // our Recognizer takes one trace, so those lines are skipped (counted). The shipped test slice has none.
        const std::string tgt = normalizeWord(o.value("word").toString());
        if (tgt.size() <= 1) continue;
        if (!data.isArray()) { ++skipped; continue; }
        if (a.skipOov && !dictWords.count(tgt)) { std::cout << "oov " << tgt << "\n"; continue; }
        const QJsonArray pts = data.toArray();
        if (pts.size() <= 2) continue;

        std::vector<swipekb::TracePoint> trace;
        const double t0 = pts.at(0).toObject().value("t").toDouble();
        for (const QJsonValue &v : pts) {
            const QJsonObject p = v.toObject();
            trace.push_back({float(p.value("x").toDouble() * kScale), float(p.value("y").toDouble() * kScale),
                             float(p.value("t").toDouble() - t0)});
        }

        // Context exactly as the live keyboard delivers it: the raw text before the cursor goes through
        // swipekb::contextWords (sentence reset, punctuation trimmed). DIFFERENCE from infer_jsonl: it hands the
        // model the raw whitespace-split words (punctuation attached, no sentence reset).
        std::vector<std::string> context;
        if (o.contains("sentence") && o.contains("word_idx")) {
            const QStringList words = o.value("sentence").toString().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            const QStringList before = words.mid(0, o.value("word_idx").toInt());
            context = swipekb::contextWords(before.join(' ').toStdString());
        }

        const auto t1 = std::chrono::steady_clock::now();
        const auto results = r.recognize(trace, keys, context, askK);
        totalUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count();

        int recallIdx = INT_MAX;
        for (size_t i = 0; i < results.size(); ++i)
            if (normalizeWord(QString::fromStdString(results[i].word)) == tgt) {
                recallIdx = int(i);
                break;
            }
        // The scored list is the first top-k entries only (what the keyboard would show).
        const int idx = recallIdx < a.topK ? recallIdx : INT_MAX;
        if (recallIdx < askK) ++recallHit;
        if (idx == 0) { std::cout << "good"; ++good; }
        else if (idx <= 3) { std::cout << "ok"; ++ok; }
        else { std::cout << "bad"; ++bad; }
        if (context.empty()) { ++firstN; if (idx == 0) ++firstGood; }
        std::cout << " " << o.value("id").toInt() << " " << tgt << ":";
        if (!results.empty()) std::cout << normalizeWord(QString::fromStdString(results[0].word));
        std::cout << ":";
        if (idx == 0) {
            std::cout << "[]";
        } else {
            std::cout << "[";
            for (size_t i = 1; i < results.size() && int(i) < a.topK; ++i) {
                std::cout << "'" << normalizeWord(QString::fromStdString(results[i].word)) << "'";
                if (int(i) + 1 < std::min<int>(a.topK, results.size())) std::cout << ", ";
            }
            std::cout << "]";
        }
        std::cout << "\n";
    }

    const int total = good + ok + bad;
    if (total == 0) {
        std::fprintf(stderr, "no scorable swipes in %s\n", a.jsonl.c_str());
        return 1;
    }
    // Same arithmetic and number formatting as infer_jsonl (float, default precision) so rows stay comparable.
    std::cout << "\nResults: " << total << " samples\n";
    std::cout << "  top-1:  " << good << " / " << total << " = " << (good * 100.0f) / total << "%\n";
    std::cout << "  top-4:  " << (good + ok) << " / " << total << " = " << ((good + ok) * 100.0f) / total << "%\n";
    std::cout << "  miss:   " << bad << " / " << total << " = " << (bad * 100.0f) / total << "%\n";
    std::cout << "\nExtras:\n";
    std::cout << "  recall@" << askK << ":  " << recallHit << " / " << total << " = " << (recallHit * 100.0f) / total << "%\n";
    std::cout << "  first-word top-1:  " << firstGood << " / " << firstN << " = " << (firstN ? (firstGood * 100.0f) / firstN : 0.0f) << "%\n";
    if (skipped) std::cout << "  skipped (two-hand format):  " << skipped << "\n";
    std::cout << "\nTimings (avg µs per sample):\n" << std::fixed << std::setprecision(1);
    std::cout << "  total:    " << totalUs / total << "\n";
    return 0;
}
