// swipekb-nlm-score — scores given candidate words with the neural word model (engine/include/swipekb/nlm.hpp),
// in the context of FUTO's swipe-test sentences, and measures its speed and memory.
//
// WHY a separate tool: tuning how much the neural model should count (bench/tune-nlm.py) needs its score for
// each candidate once; re-ranking those stored numbers is then instant. Running the model inside every tuning
// trial would take hours.
//
// Usage:
//   swipekb-nlm-score --model FILE.gguf --jsonl SLICE.jsonl --requests REQ.jsonl --out OUT.jsonl
//                     [--threads 4] [--max-ctx 48] [--cold]
// REQ.jsonl: one line per swipe, {"id": <swipe id in SLICE>, "words": ["candidate", ...]}.
// OUT.jsonl: {"id": .., "s": [[lp, word_end, tokens], ...]} in the same word order (natural logs).
// The context is built exactly like swipekb-bench (and the keyboard) does: swipekb::contextWords of the words
// before the swiped one. --cold forgets the previous context every time (worst-case speed).

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "swipekb/nlm.hpp"
#include "swipekb/recognizer.hpp"

namespace {

long statusKb(const char *key)
{
    QFile st(QStringLiteral("/proc/self/status"));
    if (!st.open(QIODevice::ReadOnly))
        return -1;
    for (const QByteArray &l : st.readAll().split('\n'))
        if (l.startsWith(key))
            return l.mid(int(std::strlen(key))).simplified().split(' ').value(0).toLong();
    return -1;
}

} // namespace

int main(int argc, char **argv)
{
    std::string model, jsonl, requests, out;
    swipekb::NeuralLMOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (s == "--model") model = next();
        else if (s == "--jsonl") jsonl = next();
        else if (s == "--requests") requests = next();
        else if (s == "--out") out = next();
        else if (s == "--threads") opt.threads = std::atoi(next().c_str());
        else if (s == "--max-ctx") opt.max_context_tokens = std::atoi(next().c_str());
        else if (s == "--cold") opt.reuse_context = false;
        else {
            std::fprintf(stderr, "usage: swipekb-nlm-score --model F --jsonl F --requests F --out F [--threads N] "
                                 "[--max-ctx N] [--cold]\n");
            return 2;
        }
    }
    // Contexts by swipe id.
    QHash<int, std::vector<std::string>> contexts;
    {
        QFile f(QString::fromStdString(jsonl));
        if (!f.open(QIODevice::ReadOnly)) { std::fprintf(stderr, "cannot read %s\n", jsonl.c_str()); return 1; }
        while (!f.atEnd()) {
            const QJsonObject o = QJsonDocument::fromJson(f.readLine()).object();
            if (!o.contains("sentence") || !o.contains("word_idx")) continue;
            const QStringList words = o.value("sentence").toString().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            contexts.insert(o.value("id").toInt(),
                            swipekb::contextWords(words.mid(0, o.value("word_idx").toInt()).join(' ').toStdString()));
        }
    }
    const long rss0 = statusKb("VmRSS:");
    const auto t0 = std::chrono::steady_clock::now();
    auto scorer = swipekb::loadNeuralLM(model, opt);
    const double loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!scorer) return 1;
    const long rss1 = statusKb("VmRSS:");

    QFile in(QString::fromStdString(requests)), o(QString::fromStdString(out));
    if (!in.open(QIODevice::ReadOnly) || !o.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "cannot open requests/out\n");
        return 1;
    }
    std::vector<double> us;
    std::vector<int> nwords;
    while (!in.atEnd()) {
        const QByteArray line = in.readLine().trimmed();
        if (line.isEmpty()) continue;
        const QJsonObject r = QJsonDocument::fromJson(line).object();
        const int id = r.value("id").toInt();
        std::vector<std::string> words;
        for (const QJsonValue &w : r.value("words").toArray())
            words.push_back(w.toString().toStdString());
        const auto t1 = std::chrono::steady_clock::now();
        const auto sc = scorer->score(contexts.value(id), words);
        us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count());
        nwords.push_back(int(words.size()));
        QJsonArray arr;
        for (const auto &x : sc)
            arr.append(QJsonArray{x.lp, x.word_end, x.tokens});
        o.write(QJsonDocument(QJsonObject{{"id", id}, {"s", arr}}).toJson(QJsonDocument::Compact) + "\n");
    }
    if (us.empty()) return 1;
    std::vector<double> sorted = us;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (double x : us) sum += x;
    double nw = 0;
    for (int n : nwords) nw += n;
    std::printf("calls %zu  words/call %.2f  ms: mean %.2f p50 %.2f p95 %.2f max %.2f\n", us.size(), nw / us.size(),
                sum / us.size() / 1000, sorted[sorted.size() / 2] / 1000, sorted[sorted.size() * 95 / 100] / 1000,
                sorted.back() / 1000);
    // WHY file vs anonymous: the model file's pages are shared and the kernel can drop them; the rest is
    // memory the keyboard really holds.
    std::printf("load ms %.1f  RSS kB: before %ld after-load %ld end %ld peak %ld (end: file %ld, anonymous %ld)\n",
                loadMs, rss0, rss1, statusKb("VmRSS:"), statusKb("VmHWM:"), statusKb("RssFile:"), statusKb("RssAnon:"));
    return 0;
}
