// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Interim implementation of the facade API (see openengine.h). Every piece below is project code without FUTO:
// OwnSwipe (our swipe engine), Spellings, the verified .combined dictionary and the lock-pinned n-gram files.

#include "openengine.h"

#include "languageprofile.h"
#include "languagetextrules.h"
#include "ownswipe.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <fstream>
#include <map>
#include <unordered_map>

static void initOpenLanguageAssets()
{
    // WHY: the lock and the word lists are embedded in the static language library; a static library's resources
    // must be registered by hand before the first read.
    Q_INIT_RESOURCE(language_assets);
}

namespace swipekb69
{
namespace
{
struct Entry {
    std::string key; // letters only, lower case: what a tap or a swipe can reach
    std::string display; // the dictionary's spelling ("don't")
    int freq = 0;
};

QString lettersKey(const QString &word)
{
    QString out;
    for (const QChar c : word.normalized(QString::NormalizationForm_C).toLower()) {
        if (c.isLetter()) {
            out += c;
        }
    }
    return out;
}

bool wellFormed(const QString &w)
{
    // WHY the same acceptance rule as the swipe word list (ownswipe.cpp): letters with inner ' or - only.
    static const QRegularExpression valid(QStringLiteral(R"(^\p{L}+(?:['’\-]\p{L}+)*$)"));
    return valid.match(w).hasMatch();
}
} // namespace

struct LanguageEngine::Impl {
    LanguageEngineSetup setup;
    swipekb::OwnSwipe own;
    swipekb::Spellings spellings;
    bool ready = false;
    std::string bestShape;
    std::vector<Entry> sorted; // by key, then by frequency (best first)
    std::unordered_map<std::string, size_t> best; // key -> index of its most frequent spelling in `sorted`
    std::map<std::string, Entry> learned, lifted; // by key
    QString alphabet;

    bool readDictionary()
    {
        std::ifstream f(setup.dictionary, std::ios::binary);
        if (!f) {
            return false;
        }
        std::string line;
        while (std::getline(f, line)) {
            if (line.compare(0, 6, " word=") != 0) {
                continue;
            }
            const size_t e = line.find(',', 6);
            const size_t fp = e == std::string::npos ? e : line.find(",f=", e);
            if (fp == std::string::npos) {
                continue;
            }
            const int freq = std::atoi(line.c_str() + fp + 3);
            if (freq <= 0 || line.find("not_a_word=true") != std::string::npos || line.find("nonword") != std::string::npos) {
                continue;
            }
            const QString w = QString::fromStdString(line.substr(6, e - 6)).normalized(QString::NormalizationForm_C);
            if (!wellFormed(w)) {
                continue;
            }
            sorted.push_back({lettersKey(w).toStdString(), w.toStdString(), freq});
        }
        std::stable_sort(sorted.begin(), sorted.end(), [](const Entry &a, const Entry &b) {
            return a.key != b.key ? a.key < b.key : a.freq > b.freq;
        });
        for (size_t i = 0; i < sorted.size(); ++i) {
            best.emplace(sorted[i].key, i); // first = most frequent spelling of that key
        }
        return !sorted.empty();
    }

    // The strongest entry for an exact key, from the dictionary or the extra sets.
    const Entry *exact(const std::string &key) const
    {
        const Entry *out = nullptr;
        if (auto it = best.find(key); it != best.end()) {
            out = &sorted[it->second];
        }
        for (const auto *set : {&learned, &lifted}) {
            if (auto it = set->find(key); it != set->end() && (!out || it->second.freq > out->freq)) {
                out = &it->second;
            }
        }
        return out;
    }

    void collectPrefix(const std::string &prefix, int penalty, std::map<std::string, Entry> &out) const
    {
        auto add = [&](const Entry &e) {
            Entry scored = e;
            scored.freq -= penalty;
            auto [it, inserted] = out.emplace(e.key, scored);
            if (!inserted && it->second.freq < scored.freq) {
                it->second = scored;
            }
        };
        auto it = std::lower_bound(sorted.begin(), sorted.end(), prefix, [](const Entry &e, const std::string &p) {
            return e.key < p;
        });
        // WHY bounded: a one-letter prefix reaches tens of thousands of words; the most frequent are found by keeping
        // the best per key while scanning at most this many entries (the list is frequency-sorted within a key only).
        int scanned = 0;
        for (; it != sorted.end() && it->key.compare(0, prefix.size(), prefix) == 0 && scanned < 20000; ++it, ++scanned) {
            add(*it);
        }
        for (const auto *set : {&learned, &lifted}) {
            for (const auto &[key, e] : *set) {
                if (key.compare(0, prefix.size(), prefix) == 0) {
                    add(e);
                }
            }
        }
    }

    void rebuildExtra()
    {
        std::vector<std::pair<std::string, int>> both;
        for (const auto *set : {&learned, &lifted}) {
            for (const auto &[key, e] : *set) {
                both.emplace_back(e.display, e.freq);
            }
        }
        own.setExtraWords(both);
    }
};

LanguageEngine::LanguageEngine()
    : d(std::make_unique<Impl>())
{
}

LanguageEngine::~LanguageEngine() = default;

bool LanguageEngine::init(const LanguageEngineSetup &setup)
{
    d = std::make_unique<Impl>();
    d->setup = setup;
    try {
        if (!d->readDictionary()) {
            return false;
        }
        swipekb::OwnSwipeSetup own;
        own.dictionary = setup.dictionary;
        own.ngram = setup.ngram;
        own.model = setup.swipeModel;
        own.keepCase = setup.language == "de"; // German keeps noun capitals (same rule as the legacy front end)
        if (!d->own.init(own)) {
            return false;
        }
        d->spellings = swipekb::Spellings::fromCombinedFile(setup.dictionary, setup.language);
        d->alphabet = swipekb::languageAlphabet(QString::fromStdString(setup.language));
        d->ready = true;
    } catch (...) {
        d->ready = false;
    }
    return d->ready;
}

bool LanguageEngine::ready() const
{
    return d->ready;
}

bool LanguageEngine::hasModel() const
{
    return d->ready && d->own.hasModel();
}

bool LanguageEngine::usingNgram() const
{
    return d->ready && d->own.usingNgram();
}

const char *LanguageEngine::mode() const
{
    return hasModel() ? "own" : "own-geometry";
}

std::vector<Candidate> LanguageEngine::recognize(const std::vector<TracePoint> &trace,
                                                 const std::vector<KeyRect> &keys,
                                                 const std::vector<std::string> &context,
                                                 int topK)
{
    d->bestShape.clear();
    if (!d->ready) {
        return {};
    }
    swipekb::OwnSwipe::Result result;
    if (!d->own.recognize(trace, keys, context, topK, result)) {
        return {};
    }
    d->bestShape = result.bestShape;
    std::vector<Candidate> out;
    float score = 0;
    for (const auto &w : result.words) {
        out.push_back({w, score});
        score -= 1; // rank only: callers use the order
    }
    return out;
}

std::string LanguageEngine::bestShapeWord() const
{
    return d->bestShape;
}

std::vector<std::string> LanguageEngine::otherSpellings(const std::string &word) const
{
    return d->ready ? d->spellings.others(word) : std::vector<std::string>{};
}

std::vector<Candidate> LanguageEngine::complete(const std::string &typedPrefix, int topK) const
{
    if (!d->ready || topK <= 0) {
        return {};
    }
    const std::string prefix = lettersKey(QString::fromStdString(typedPrefix)).toStdString();
    if (prefix.empty() || prefix.size() > 64) {
        return {};
    }
    std::map<std::string, Entry> found;
    d->collectPrefix(prefix, 0, found);
    // WHY corrections: one mistyped letter ("hrllo") should still offer "hello". Each variant costs a fixed amount
    // of frequency, so a real prefix match of similar strength always ranks first.
    const QString p = QString::fromStdString(prefix);
    if (p.size() >= 2 && p.size() <= 12) {
        for (int i = 0; i < p.size(); ++i) {
            for (const QChar c : std::as_const(d->alphabet)) {
                if (c == p[i]) {
                    continue;
                }
                QString variant = p;
                variant[i] = c;
                d->collectPrefix(variant.toStdString(), 60, found);
            }
        }
    }
    std::vector<Entry> ranked;
    for (auto &[key, e] : found) {
        ranked.push_back(e);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Entry &a, const Entry &b) {
        return a.freq > b.freq;
    });
    std::vector<Candidate> out;
    for (const auto &e : ranked) {
        out.push_back({e.display, float(e.freq)});
        if (int(out.size()) == topK) {
            break;
        }
    }
    return out;
}

std::vector<Candidate> LanguageEngine::similar(const std::string &word, int topK) const
{
    if (!d->ready || topK <= 0) {
        return {};
    }
    const QString w = lettersKey(QString::fromStdString(word));
    if (w.isEmpty() || w.size() > 64) {
        return {};
    }
    std::map<std::string, const Entry *> found;
    auto consider = [&](const QString &variant) {
        const std::string key = variant.toStdString();
        if (variant == w) {
            return;
        }
        if (const Entry *e = d->exact(key)) {
            found.emplace(key, e);
        }
    };
    for (int i = 0; i <= w.size(); ++i) {
        if (i < w.size()) {
            consider(QString(w).remove(i, 1)); // dropped letter
        }
        if (i + 1 < w.size()) {
            QString swapped = w;
            std::swap(swapped[i], swapped[i + 1]);
            consider(swapped); // swapped letters
        }
        for (const QChar c : std::as_const(d->alphabet)) {
            QString added = w;
            added.insert(i, c);
            consider(added); // added letter
            if (i < w.size()) {
                QString replaced = w;
                replaced[i] = c;
                consider(replaced); // replaced letter
            }
        }
    }
    std::vector<const Entry *> ranked;
    for (const auto &[key, e] : found) {
        ranked.push_back(e);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Entry *a, const Entry *b) {
        return a->freq > b->freq;
    });
    std::vector<Candidate> out;
    for (const Entry *e : ranked) {
        out.push_back({e->display, float(e->freq)});
        if (int(out.size()) == topK) {
            break;
        }
    }
    return out;
}

bool LanguageEngine::hasWord(const std::string &word) const
{
    return d->ready && d->best.count(lettersKey(QString::fromStdString(word)).toStdString()) != 0;
}

bool LanguageEngine::isOffensive(const std::string &word) const
{
    return !swipekb::offeredUnprompted(QString::fromStdString(word), QString::fromStdString(d->setup.language));
}

void LanguageEngine::setLearnedWords(const std::vector<std::pair<std::string, int>> &words)
{
    d->learned.clear();
    for (const auto &[w, freq] : words) {
        const QString word = QString::fromStdString(w);
        if (!wellFormed(word)) {
            continue;
        }
        d->learned[lettersKey(word).toStdString()] = {lettersKey(word).toStdString(), w, std::clamp(freq, 1, 255)};
    }
    if (d->ready) {
        d->rebuildExtra();
    }
}

void LanguageEngine::liftWords(const std::vector<std::pair<std::string, int>> &words)
{
    for (const auto &[w, freq] : words) {
        const QString word = QString::fromStdString(w);
        if (!wellFormed(word)) {
            continue;
        }
        const std::string key = lettersKey(word).toStdString();
        // WHY only raises: an ordinary word that is already stronger keeps its own score.
        if (const Entry *e = d->exact(key); e && e->freq >= freq) {
            continue;
        }
        d->lifted[key] = {key, w, std::clamp(freq, 1, 255)};
    }
    if (d->ready) {
        d->rebuildExtra();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Interim language resolution: the same pins the legacy profile checks, minus every FUTO file.
// ---------------------------------------------------------------------------------------------------------------
namespace
{
QString localPath(const QString &root, const QString &relative)
{
    if (relative.isEmpty() || QDir::isAbsolutePath(relative)) {
        return {};
    }
    const QString base = QFileInfo(root).canonicalFilePath();
    const QString path = QFileInfo(QDir(root).filePath(relative)).canonicalFilePath();
    // WHY canonical: reject symlink/traversal escapes even when the relative spelling looks safe.
    if (base.isEmpty() || !path.startsWith(base + QLatin1Char('/')) || !QFileInfo(path).isFile()) {
        return {};
    }
    return path;
}

QString digest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool pinned(const QString &path, const QString &sha, qint64 bytes)
{
    return !path.isEmpty() && sha.size() == 64 && (bytes < 0 || QFileInfo(path).size() == bytes) && digest(path) == sha;
}

QJsonObject lock()
{
    initOpenLanguageAssets();
    QFile file(QStringLiteral(":/swipekb/assets.lock.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}
} // namespace

OpenLanguage resolveOpenLanguage(const QString &locale, const QString &modelRoot)
{
    OpenLanguage out;
    out.requestedLocale = swipekb::canonicalLocale(locale);
    out.language = out.requestedLocale.section(QLatin1Char('-'), 0, 0);
    out.setup.language = out.language.toStdString();
    if (!QStringList{QStringLiteral("en"), QStringLiteral("es"), QStringLiteral("fr"), QStringLiteral("de")}.contains(out.language)) {
        out.unavailableReason = QStringLiteral("Unsupported language");
        return out;
    }
    const QJsonObject pins = lock();
    const QString packs = qEnvironmentVariable("SWIPEKB_LANGUAGE_PACKS", QDir(modelRoot).filePath(QStringLiteral("language-packs")));
    // WHY the same folders the legacy front end and the facade use: English's dictionary is in dict/, the other
    // languages' in the language-pack folder; an English n-gram may sit in either.
    const auto dictPin = pins[QStringLiteral("language_dictionaries")].toObject()[out.language].toObject();
    const QString dict = out.language == QLatin1String("en") ? localPath(modelRoot, QStringLiteral("dict/en_swipe.combined"))
                                                              : localPath(packs, dictPin[QStringLiteral("path")].toString());
    if (!pinned(dict, dictPin[QStringLiteral("sha256")].toString(), dictPin[QStringLiteral("bytes")].toInteger(-1))) {
        out.unavailableReason = QStringLiteral("Dictionary missing or checksum mismatch");
        return out;
    }
    out.setup.dictionary = dict.toStdString();
    out.swipe = true;
    // WHY optional: a missing or swapped swipe model leaves the engine geometry-only, exactly like the legacy path.
    const QString overrideModel = qEnvironmentVariable("SWIPEKB_SWIPE_MODEL");
    QString model = overrideModel;
    if (model.isEmpty()) {
        model = swipekb::verifiedSwipeModel(QDir(modelRoot).filePath(QStringLiteral("models")));
    }
    if (model.isEmpty()) {
        model = swipekb::verifiedSwipeModel(modelRoot);
    }
    out.setup.swipeModel = model.toStdString();
    // Next-word files: judged against the lock only (never a manifest's own claim).
    const auto ctx = pins[QStringLiteral("multilingual_context_models")].toObject()[out.language].toObject();
    const auto shared = pins[QStringLiteral("language_files")].toObject();
    for (const QString &dir : {packs, modelRoot}) {
        const QString ngram = localPath(dir, out.language + QStringLiteral(".ngram"));
        if (ngram.isEmpty()) {
            continue;
        }
        const QString vocab = localPath(dir, out.language + QStringLiteral(".vocab.tsv"));
        // WHY the licence/credit files are required too: the files may only be used where their terms travel with them.
        const bool terms = pinned(localPath(dir, out.language + QStringLiteral("-CONTRIBUTORS.txt")), ctx[QStringLiteral("contributorsSha256")].toString(), -1)
            && pinned(localPath(dir, QStringLiteral("TATOEBA-CC-BY-2.0-FR.txt")), pins[QStringLiteral("tatoeba_license_notice_sha256")].toString(), -1)
            && pinned(localPath(dir, shared[QStringLiteral("odcTerms")].toObject()[QStringLiteral("path")].toString()),
                      shared[QStringLiteral("odcTerms")].toObject()[QStringLiteral("sha256")].toString(),
                      -1)
            && pinned(localPath(dir, shared[QStringLiteral("notice")].toObject()[QStringLiteral("path")].toString()),
                      shared[QStringLiteral("notice")].toObject()[QStringLiteral("sha256")].toString(),
                      -1)
            && (out.language != QLatin1String("en")
                || pinned(localPath(dir, shared[QStringLiteral("apacheTerms")].toObject()[QStringLiteral("path")].toString()),
                          shared[QStringLiteral("apacheTerms")].toObject()[QStringLiteral("sha256")].toString(),
                          -1));
        if (terms && pinned(ngram, ctx[QStringLiteral("modelSha256")].toString(), ctx[QStringLiteral("modelBytes")].toInteger(-1))
            && pinned(vocab, ctx[QStringLiteral("vocabSha256")].toString(), ctx[QStringLiteral("vocabBytes")].toInteger(-1))) {
            out.setup.ngram = ngram.toStdString();
            out.nextWord = true;
            out.nextModel = ngram;
            out.nextVocab = vocab;
            out.modelIdentity = ctx[QStringLiteral("modelSha256")].toString() + ctx[QStringLiteral("vocabSha256")].toString();
            const auto nwp = pins[QStringLiteral("trained_models")].toObject()[QStringLiteral("nextWord")].toObject()[out.language].toObject();
            const QString nwpPath = localPath(dir, out.language + QStringLiteral(".nwp.bin"));
            if (pinned(nwpPath, nwp[QStringLiteral("sha256")].toString(), nwp[QStringLiteral("bytes")].toInteger(-1))) {
                out.nextNwp = nwpPath;
                out.modelIdentity += nwp[QStringLiteral("sha256")].toString();
            }
        }
        break; // WHY first folder that has the file decides, like the legacy profile: never mix two packs
    }
    return out;
}

} // namespace swipekb69
