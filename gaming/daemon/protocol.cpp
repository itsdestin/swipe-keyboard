// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "protocol.hpp"

#include <QChar>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QString>
#include <cmath>

namespace gamingd {
namespace {

bool finiteIn(const QJsonValue &v, double lo, double hi, double &out)
{
    if (!v.isDouble()) return false;
    out = v.toDouble();
    return std::isfinite(out) && out >= lo && out <= hi;
}

// A word as typed text: valid UTF-8, no control characters, bounded. WHY strict: these strings reach the models
// and later the typed output path, so anything odd is refused instead of cleaned.
std::optional<std::string> parseWord(const QJsonValue &v)
{
    if (!v.isString()) return std::nullopt;
    // WHY composed form (NFC) first: "n" + combining tilde and "ñ" are the same word. The engine, the dictionaries and
    // the plugin all use composed letters, so a decomposed spelling must not silently find nothing.
    const QString s = v.toString().normalized(QString::NormalizationForm_C);
    const QByteArray utf8 = s.toUtf8();
    if (utf8.isEmpty() || size_t(utf8.size()) > kMaxWordBytes) return std::nullopt;
    for (const QChar c : s)
        if (c.isNonCharacter() || c.category() == QChar::Other_Control || c.category() == QChar::Other_Surrogate
            || c.category() == QChar::Other_Format)
            return std::nullopt;
    return utf8.toStdString();
}

Parsed<int> parseTopK(const QJsonObject &req)
{
    if (!req.contains("top_k")) return {kDefaultTopK, nullptr};
    double d;
    if (!finiteIn(req.value("top_k"), 1, kMaxTopK, d) || d != std::floor(d)) return {std::nullopt, kErrBadRequest};
    return {int(d), nullptr};
}

// Shared by swipe + next_words. Returns false on malformed input. Sets `dropped` when unknown mode refuses text.
bool parseWordList(const QJsonObject &req, const char *field, Mode mode, std::vector<std::string> &out, bool &dropped)
{
    if (!req.contains(field)) return true;
    const QJsonValue v = req.value(field);
    if (!v.isArray()) return false;
    const QJsonArray a = v.toArray();
    const size_t cap = mode == Mode::Unknown ? kMaxUnknownContextWords : kMaxContextWords;
    if (size_t(a.size()) > cap) return false;
    for (const QJsonValue &w : a) {
        auto word = parseWord(w);
        if (!word) return false;
        if (mode == Mode::Unknown && looksLikeSecret(*word)) dropped = true;
        out.push_back(std::move(*word));
    }
    if (dropped) out.clear();
    return true;
}

} // namespace

Parsed<QJsonObject> parseBody(const QByteArray &body)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return {std::nullopt, kErrBadJson};
    const QJsonObject o = doc.object();
    if (!o.value("v").isDouble() || o.value("v").toInt(-1) != kProtocolVersion) return {std::nullopt, kErrVersion};
    if (!o.value("op").isString()) return {std::nullopt, kErrBadRequest};
    return {o, nullptr};
}

Parsed<Mode> parseMode(const QJsonObject &req, Mode fallback)
{
    if (!req.contains("mode")) return {fallback, nullptr};
    const QString m = req.value("mode").toString();
    if (m == QLatin1String("unknown")) return {Mode::Unknown, nullptr};
    if (m == QLatin1String("plain")) return {Mode::Plain, nullptr};
    return {std::nullopt, kErrBadRequest};
}

Parsed<std::vector<swipekb::KeyRect>> parseLayout(const QJsonObject &req)
{
    const QJsonValue v = req.value("keys");
    if (!v.isArray()) return {std::nullopt, kErrBadRequest};
    const QJsonArray a = v.toArray();
    if (a.isEmpty() || size_t(a.size()) > kMaxKeys) return {std::nullopt, kErrBadRequest};
    std::vector<swipekb::KeyRect> keys;
    for (const QJsonValue &kv : a) {
        if (!kv.isObject()) return {std::nullopt, kErrBadRequest};
        const QJsonObject k = kv.toObject();
        const QString letter = k.value("c").toString().normalized(QString::NormalizationForm_C);   // WHY NFC: see parseWord
        const auto cps = letter.toUcs4();
        if (cps.size() != 1 || !QChar::isLetter(cps[0])) return {std::nullopt, kErrBadRequest};
        double x, y, w, h;
        if (!finiteIn(k.value("x"), -kMaxCoord, kMaxCoord, x) || !finiteIn(k.value("y"), -kMaxCoord, kMaxCoord, y)
            || !finiteIn(k.value("w"), 0, kMaxCoord, w) || !finiteIn(k.value("h"), 0, kMaxCoord, h) || w <= 0 || h <= 0)
            return {std::nullopt, kErrBadRequest};
        const char32_t lower = char32_t(QChar::toLower(cps[0]));
        for (const auto &seen : keys)
            if (seen.letter == lower) return {std::nullopt, kErrBadRequest};   // WHY: two rects for one letter are ambiguous
        keys.push_back({lower, float(x), float(y), float(w), float(h)});
    }
    return {std::move(keys), nullptr};
}

Parsed<std::string> parseSteamLayout(const QJsonObject &req)
{
    if (!req.contains("steam_layout")) return {std::string("qwerty"), nullptr};
    const QJsonValue v = req.value("steam_layout");
    if (!v.isString()) return {std::nullopt, kErrBadRequest};
    const std::string id = v.toString().toStdString();
    // WHY lower-case letters, digits and "_" only, at most 48: Steam's ids look like "tchinese_bopomofo". The id is
    // only ever compared with a fixed list and never reaches a path, a log line or a shell.
    if (id.empty() || id.size() > 48) return {std::nullopt, kErrBadRequest};
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return {std::nullopt, kErrBadRequest};
    return {id, nullptr};
}

Parsed<SwipeArgs> parseSwipe(const QJsonObject &req, Mode mode)
{
    SwipeArgs out;
    const QJsonValue pv = req.value("points");
    if (!pv.isArray()) return {std::nullopt, kErrBadRequest};
    const QJsonArray pts = pv.toArray();
    if (size_t(pts.size()) < kMinPoints || size_t(pts.size()) > kMaxPoints) return {std::nullopt, kErrBadRequest};
    out.trace.reserve(size_t(pts.size()));
    for (const QJsonValue &p : pts) {
        const QJsonArray t = p.toArray();
        double x, y, ms;
        if (t.size() != 3 || !finiteIn(t[0], -kMaxCoord, kMaxCoord, x) || !finiteIn(t[1], -kMaxCoord, kMaxCoord, y)
            || !finiteIn(t[2], 0, kMaxTimeMs, ms))
            return {std::nullopt, kErrBadRequest};
        out.trace.push_back({float(x), float(y), float(ms)});
    }
    if (!parseWordList(req, "context", mode, out.context, out.contextDropped)) return {std::nullopt, kErrBadRequest};
    const auto k = parseTopK(req);
    if (!k.value) return {std::nullopt, k.error};
    out.topK = *k.value;
    return {std::move(out), nullptr};
}

Parsed<NextWordsArgs> parseNextWords(const QJsonObject &req, Mode mode)
{
    NextWordsArgs out;
    if (!parseWordList(req, "words", mode, out.words, out.suppressed)) return {std::nullopt, kErrBadRequest};
    if (req.contains("partial")) {
        // The letters of the word being typed right now (built by the caller from tapped keys).
        auto partial = parseWord(req.value("partial"));
        if (!partial) return {std::nullopt, kErrBadRequest};
        if (mode == Mode::Unknown && looksLikeSecret(*partial)) { out.suppressed = true; out.words.clear(); }
        else out.partial = std::move(*partial);
    }
    const auto k = parseTopK(req);
    if (!k.value) return {std::nullopt, k.error};
    out.topK = *k.value;
    return {std::move(out), nullptr};
}

bool looksLikeSecret(const std::string &word)
{
    const QString s = QString::fromUtf8(word.data(), qsizetype(word.size()));
    bool lower = false, upper = false, digit = false, symbol = false, vowel = false;
    for (const QChar c : s) {
        if (c.isLower()) lower = true;
        else if (c.isUpper()) upper = true;
        else if (c.isDigit()) digit = true;
        else if (c != QLatin1Char('\'') && c != QLatin1Char('-') && c != QChar(0x2019)) symbol = true;   // apostrophes/hyphens occur in real words
        // WHY the base letter, not the character: "é", "ü", "ï" are vowels too. Without this a long French or German word
        // of accented vowels could be taken for a password.
        if (QStringLiteral("aeiouy").contains(QString(c).normalized(QString::NormalizationForm_D).at(0).toLower())
            || QStringLiteral("æœø").contains(c.toLower()))
            vowel = true;
    }
    const int classes = int(lower) + int(upper) + int(digit) + int(symbol);
    if (s.size() >= 8 && classes >= 3) return true;
    return s.size() >= 16 && !vowel;
}

QByteArray encodeFrame(const QByteArray &body)
{
    if (uint64_t(body.size()) > kMaxFrame) return {};
    QByteArray out;
    out.reserve(4 + body.size());
    const uint32_t n = uint32_t(body.size());
    out.append(char(n >> 24)).append(char(n >> 16)).append(char(n >> 8)).append(char(n));
    out.append(body);
    return out;
}

} // namespace gamingd
