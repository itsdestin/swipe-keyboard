#include "swipekb/learning.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace swipekb {

namespace {
constexpr char kHeader[] = "SWKBLEARN 1";
// WHY a size ceiling when reading: a damaged or hostile file must not make the keyboard allocate without bound.
constexpr std::size_t kMaxFileBytes = 4u << 20;
// Anything weaker than this has decayed to nothing and is dropped when the store is trimmed.
constexpr double kForgetBelow = 0.03;

bool parseDouble(const std::string &s, double &out)
{
    if (s.empty()) return false;
    char *end = nullptr;
    errno = 0;
    out = std::strtod(s.c_str(), &end);
    return errno == 0 && end == s.c_str() + s.size() && std::isfinite(out) && out >= 0 && out < 1e7;
}
bool parseInt(const std::string &s, std::int64_t &out)
{
    if (s.empty()) return false;
    char *end = nullptr;
    errno = 0;
    out = std::strtoll(s.c_str(), &end, 10);
    return errno == 0 && end == s.c_str() + s.size() && out >= 0;
}
std::vector<std::string> split(const std::string &line, char sep)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const auto at = line.find(sep, start);
        if (at == std::string::npos) { out.push_back(line.substr(start)); return out; }
        out.push_back(line.substr(start, at - start));
        start = at + 1;
    }
}
// Letters of the word only (apostrophes and hyphens dropped): "don't" starts with "dont", like the predictors do.
std::string lettersOnly(const std::string &s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\'' || c == '-') continue;
        if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 && static_cast<unsigned char>(s[i + 2]) == 0x99) { i += 2; continue; }   // U+2019
        out.push_back(char(c));
    }
    return out;
}
}

bool PersonalModel::plain(const std::string &s)
{
    // WHY: the file is tab/newline separated, so those can never be part of a word; the 64-byte bound matches
    // the length the prediction code already accepts.
    if (s.empty() || s.size() > 64) return false;
    for (const char c : s)
        if (c == '\t' || c == '\n' || c == '\r' || c == '\x1f' || static_cast<unsigned char>(c) < 0x20) return false;
    return true;
}

double PersonalModel::decayed(double score, std::int64_t last, std::int64_t now) const
{
    const double days = now > last ? double(now - last) / 86400.0 : 0.0;
    return score * std::exp2(-days / limits_.halfLifeDays);
}

bool PersonalModel::suggestible(const Word &w, std::int64_t now) const
{
    // WHY known words need no minimum: they are in the dictionary anyway, a single use only nudges their rank.
    return (w.known || w.uses >= limits_.newWordUses) && decayed(w.score, w.last, now) >= kForgetBelow;
}

PersonalModel::Undo PersonalModel::learn(const std::string &prev, const std::string &word, bool known, bool capitalized,
                                         std::int64_t now)
{
    Undo undo;
    if (!plain(word) || limits_.maxWords == 0) return undo;
    const bool usePrev = !prev.empty() && plain(prev);
    undo.valid = true; undo.prev = usePrev ? prev : std::string(); undo.word = word;
    const bool swipeBefore = learnedNewWord(word, now);

    auto w = words_.find(word);
    if (w != words_.end()) {
        undo.hadWord = true; undo.wordScore = w->second.score; undo.wordLast = w->second.last;
        undo.wordUses = w->second.uses; undo.wordCapUses = w->second.capUses; undo.wordKnown = w->second.known;
    } else {
        w = words_.emplace(word, Word{}).first;
        w->second.known = known;   // WHY only at the first sighting: later the word may sit in the swipe list because WE added it
    }
    w->second.score = decayed(w->second.score, w->second.last, now) + 1.0;
    w->second.last = now;
    ++w->second.uses;
    // WHY only mid-sentence: every sentence starts with a capital, which says nothing about the word.
    if (capitalized && usePrev) ++w->second.capUses;
    undo.wasForgotten = forgotten_.erase(word) != 0;   // typing it again is the person bringing it back

    if (usePrev) {
        const std::string key = pairKey(prev, word);
        auto p = pairs_.find(key);
        if (p != pairs_.end()) {
            undo.hadPair = true; undo.pairScore = p->second.score; undo.pairLast = p->second.last; undo.pairUses = p->second.uses;
        } else {
            p = pairs_.emplace(key, Pair{}).first;
            followers_[prev].push_back(word);
        }
        p->second.score = decayed(p->second.score, p->second.last, now) + 1.0;
        p->second.last = now;
        ++p->second.uses;
    }
    if (words_.size() > limits_.maxWords || pairs_.size() > limits_.maxPairs) prune(now);
    if (swipeBefore != learnedNewWord(word, now)) ++swipeVersion_;
    dirty_ = true;
    return undo;
}

void PersonalModel::undo(const Undo &u)
{
    if (!u.valid) return;
    const std::int64_t anyTime = u.wordLast;
    const bool swipeBefore = learnedNewWord(u.word, anyTime);
    if (u.hadWord) {
        auto &w = words_[u.word];
        w.score = u.wordScore; w.last = u.wordLast; w.uses = u.wordUses; w.capUses = u.wordCapUses; w.known = u.wordKnown;
    } else {
        words_.erase(u.word);
    }
    if (!u.prev.empty()) {
        const std::string key = pairKey(u.prev, u.word);
        if (u.hadPair) {
            auto &p = pairs_[key];
            p.score = u.pairScore; p.last = u.pairLast; p.uses = u.pairUses;
        } else if (pairs_.erase(key)) {
            auto f = followers_.find(u.prev);
            if (f != followers_.end()) {
                f->second.erase(std::remove(f->second.begin(), f->second.end(), u.word), f->second.end());
                if (f->second.empty()) followers_.erase(f);
            }
        }
    }
    if (u.wasForgotten) forgotten_.insert(u.word);
    if (swipeBefore != learnedNewWord(u.word, anyTime)) ++swipeVersion_;
    dirty_ = true;
}

std::vector<PersonalModel::Ranked> PersonalModel::rerank(const std::vector<std::string> &base, const std::string &prev,
                                                         const std::string &prefix, std::int64_t now, std::size_t topK,
                                                         bool inject, double boostScale) const
{
    // Personal evidence, as shares: how the person's uses of each fitting word divide up.
    const std::string letters = lettersOnly(prefix);
    auto fits = [&](const std::string &key) {
        return letters.empty() || lettersOnly(key).compare(0, letters.size(), letters) == 0;
    };
    std::unordered_map<std::string, double> pairShare, wordShare, novelShare;
    double pairTotal = 0, wordTotal = 0, novelTotal = 0;
    if (!prev.empty()) {
        const auto f = followers_.find(prev);
        if (f != followers_.end())
            for (const auto &key : f->second) {
                const auto w = words_.find(key);
                const auto p = pairs_.find(pairKey(prev, key));
                if (w == words_.end() || p == pairs_.end() || p->second.uses < weights_.pairMinUses || !suggestible(w->second, now)
                    || isForgotten(key) || !fits(key)) continue;
                const double e = decayed(p->second.score, p->second.last, now);
                pairShare[key] = e; pairTotal += e;
            }
    }
    // WHY only when letters are typed (unless nextWordWord says otherwise): with nothing typed, "I write 'the' a lot"
    // says little about the next word, while among words starting "th" it says a great deal.
    const double wordWeightScale = letters.empty() ? weights_.nextWordWord : 1.0;
    if (wordWeightScale > 0)
        for (const auto &entry : words_) {
            if (!suggestible(entry.second, now) || isForgotten(entry.first) || !fits(entry.first)) continue;
            const double e = decayed(entry.second.score, entry.second.last, now);
            wordShare[entry.first] = e; wordTotal += e;
            // WHY not when nothing is typed: with no letters a new word says nothing about what comes next.
            if (!entry.second.known && !letters.empty()) { novelShare[entry.first] = e; novelTotal += e; }
        }
    double a = weights_.pairMax * pairTotal / (pairTotal + weights_.pairK) * boostScale;
    double b = weights_.wordMax * wordWeightScale * wordTotal / (wordTotal + weights_.wordK) * boostScale;
    // WHY divided by ALL the person's matching uses, not just the new words': what counts is how often, among the words
    // they write that start like this, the new one is the one they meant. After "s" that is rare; after "sus" it is not.
    double n = weights_.novelMax * novelTotal / (wordTotal + weights_.novelK) * boostScale;
    if (pairTotal <= 0) a = 0;
    if (wordTotal <= 0) b = 0;
    if (novelTotal <= 0) n = 0;
    if (a + b + n > weights_.mixCap) { const double f = weights_.mixCap / (a + b + n); a *= f; b *= f; n *= f; }

    struct Scored { Ranked ranked; double score; };
    std::vector<Scored> all;
    std::unordered_set<std::string> listed;
    double baseTotal = 0;
    auto place = [&](double r) { return std::pow(weights_.rankRatio, r); };
    for (std::size_t i = 0; i < base.size(); ++i)
        if (!isForgotten(base[i])) baseTotal += place(double(i));
    auto share = [](const std::unordered_map<std::string, double> &m, double total, const std::string &key) {
        const auto it = m.find(key);
        return it == m.end() || total <= 0 ? 0.0 : it->second / total;
    };
    for (std::size_t i = 0; i < base.size(); ++i) {
        // WHY no de-duplication here: German lists "Essen" and "essen" (same key, two words) and both must survive.
        if (isForgotten(base[i])) continue;
        listed.insert(base[i]);
        const double pBase = baseTotal > 0 ? place(double(i)) / baseTotal : 0.0;
        all.push_back({{base[i], int(i)}, (1.0 - a - b - n) * pBase + a * share(pairShare, pairTotal, base[i]) + b * share(wordShare, wordTotal, base[i])
                              + n * share(novelShare, novelTotal, base[i])});
    }
    if (inject) {
        // Learned words the predictor did not list. WHY only those the evidence above names: they enter on their
        // own personal share, so a word with a single stray use cannot crowd out the predictor's choices.
        auto add = [&](const std::string &key) {
            if (listed.count(key)) return;
            listed.insert(key);
            all.push_back({{key, -1}, a * share(pairShare, pairTotal, key) + b * share(wordShare, wordTotal, key)
                                          + n * share(novelShare, novelTotal, key)});
        };
        for (const auto &entry : pairShare) add(entry.first);
        for (const auto &entry : wordShare) add(entry.first);
    }
    // WHY stable and base-first on ties: with no personal data the order is exactly the predictor's.
    std::stable_sort(all.begin(), all.end(), [](const Scored &x, const Scored &y) { return x.score > y.score; });
    std::vector<Ranked> out;
    for (const auto &s : all) {
        if (out.size() >= topK) break;
        out.push_back(s.ranked);
    }
    return out;
}

bool PersonalModel::usedByPerson(const std::string &word, unsigned minUses) const
{
    const auto w = words_.find(word);
    return w != words_.end() && w->second.uses >= minUses && !isForgotten(word);
}

bool PersonalModel::learnedNewWord(const std::string &word, std::int64_t now) const
{
    const auto w = words_.find(word);
    return w != words_.end() && !w->second.known && suggestible(w->second, now) && !isForgotten(word);
}

bool PersonalModel::removable(const std::string &word) const
{
    // WHY only words the language did not know: those are what the person taught the keyboard. A long press on an
    // ordinary word ("the") must never offer to remove it; erasing habits is what "forget everything" is for.
    const auto w = words_.find(word);
    return w != words_.end() && !w->second.known && !isForgotten(word);
}

bool PersonalModel::wantsCapital(const std::string &word) const
{
    const auto w = words_.find(word);
    return w != words_.end() && w->second.capUses * 2 > w->second.uses;
}

std::vector<std::pair<std::string, int>> PersonalModel::swipeWords(std::int64_t now) const
{
    std::vector<std::pair<std::string, int>> out;
    for (const auto &entry : words_)
        if (learnedNewWord(entry.first, now)) {
            // WHY 110 + 15*log2(uses), at most 190: well inside the dictionaries' 0..255 range, so a name the
            // person uses often competes with ordinary words but a rare slip never beats the very common ones.
            const int freq = std::min(190, 110 + int(15.0 * std::log2(double(std::max<std::uint32_t>(1, entry.second.uses)))));
            out.emplace_back(entry.first, freq);
        }
    std::sort(out.begin(), out.end());
    return out;
}

void PersonalModel::forget(const std::string &word)
{
    const auto entry = words_.find(word);
    const bool wasNew = entry != words_.end() && !entry->second.known;
    const bool swipeBefore = wasNew;
    words_.erase(word);
    for (auto it = pairs_.begin(); it != pairs_.end();) {
        const auto sep = it->first.find('\x1f');
        if (it->first.compare(0, sep, word) == 0 || it->first.compare(sep + 1, std::string::npos, word) == 0) it = pairs_.erase(it);
        else ++it;
    }
    rebuildFollowers();
    // WHY remember it: the swipe dictionary keeps a taught word until the keyboard restarts, so it must be kept out
    // of suggestions by name; only the person typing it again lifts this (see learn()).
    // (An ordinary dictionary word is never put on the list: it only loses what was learned about it.)
    if (wasNew && plain(word) && forgotten_.size() < limits_.maxForgotten) forgotten_.insert(word);
    if (swipeBefore) ++swipeVersion_;
    dirty_ = true;
}

void PersonalModel::clear()
{
    words_.clear(); pairs_.clear(); followers_.clear(); forgotten_.clear();
    ++swipeVersion_;
    dirty_ = false;
}

void PersonalModel::rebuildFollowers()
{
    followers_.clear();
    for (const auto &entry : pairs_) {
        const auto sep = entry.first.find('\x1f');
        followers_[entry.first.substr(0, sep)].push_back(entry.first.substr(sep + 1));
    }
}

void PersonalModel::prune(std::int64_t now)
{
    auto trim = [&](auto &map, std::size_t cap) {
        std::vector<std::pair<double, std::string>> order;
        for (const auto &entry : map) order.emplace_back(decayed(entry.second.score, entry.second.last, now), entry.first);
        std::sort(order.begin(), order.end());
        // WHY down to 90% of the cap: trimming one entry per new word would sort the whole table every time.
        const std::size_t keep = cap - cap / 10;
        std::size_t drop = order.size() > cap ? order.size() - keep : 0;
        for (std::size_t i = 0; i < order.size(); ++i)
            if (i < drop || order[i].first < kForgetBelow) map.erase(order[i].second);
    };
    trim(words_, limits_.maxWords);
    trim(pairs_, limits_.maxPairs);
    // A pair whose word is gone is useless, and would grow without bound.
    for (auto it = pairs_.begin(); it != pairs_.end();) {
        const auto sep = it->first.find('\x1f');
        if (!words_.count(it->first.substr(sep + 1))) it = pairs_.erase(it);
        else ++it;
    }
    rebuildFollowers();
    ++swipeVersion_;
}

std::string PersonalModel::serialize() const
{
    std::string out = std::string(kHeader) + "\n";
    char buf[160];
    std::vector<std::string> keys;
    for (const auto &entry : words_) keys.push_back(entry.first);
    std::sort(keys.begin(), keys.end());
    for (const auto &key : keys) {
        const Word &w = words_.at(key);
        std::snprintf(buf, sizeof buf, "\t%.6g\t%lld\t%u\t%u\t%d\n", w.score, static_cast<long long>(w.last), w.uses, w.capUses, w.known ? 1 : 0);
        out += "W\t" + key + buf;
    }
    keys.clear();
    for (const auto &entry : pairs_) keys.push_back(entry.first);
    std::sort(keys.begin(), keys.end());
    for (const auto &key : keys) {
        const Pair &p = pairs_.at(key);
        const auto sep = key.find('\x1f');
        std::snprintf(buf, sizeof buf, "\t%.6g\t%lld\t%u\n", p.score, static_cast<long long>(p.last), p.uses);
        out += "P\t" + key.substr(0, sep) + "\t" + key.substr(sep + 1) + buf;
    }
    std::vector<std::string> gone(forgotten_.begin(), forgotten_.end());
    std::sort(gone.begin(), gone.end());
    for (const auto &key : gone) out += "F\t" + key + "\n";
    // WHY a closing line with the counts: a file cut short by a crash or a full disk is then detectable.
    out += "E\t" + std::to_string(words_.size()) + "\t" + std::to_string(pairs_.size()) + "\t" + std::to_string(forgotten_.size()) + "\n";
    return out;
}

bool PersonalModel::parse(const std::string &text)
{
    clear();
    auto fail = [&] { clear(); dirty_ = false; return false; };
    if (text.size() > kMaxFileBytes) return fail();
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != kHeader) return fail();
    bool ended = false;
    std::size_t nW = 0, nP = 0, nF = 0;
    while (std::getline(in, line)) {
        if (ended) return fail();   // nothing may follow the end line
        const auto f = split(line, '\t');
        if (f.empty()) return fail();
        if (f[0] == "W" && f.size() == 7) {
            double score; std::int64_t last, uses, caps, known;
            if (!plain(f[1]) || !parseDouble(f[2], score) || !parseInt(f[3], last) || !parseInt(f[4], uses) || !parseInt(f[5], caps)
                || !parseInt(f[6], known) || known > 1 || caps > uses || uses > 100000000) return fail();
            Word w; w.score = score; w.last = last; w.uses = std::uint32_t(uses); w.capUses = std::uint32_t(caps); w.known = known == 1;
            if (!words_.emplace(f[1], w).second) return fail();
            ++nW;
        } else if (f[0] == "P" && f.size() == 6) {
            double score; std::int64_t last, uses;
            if (!plain(f[1]) || !plain(f[2]) || !parseDouble(f[3], score) || !parseInt(f[4], last) || !parseInt(f[5], uses)
                || uses > 100000000) return fail();
            Pair p; p.score = score; p.last = last; p.uses = std::uint32_t(uses);
            if (!pairs_.emplace(pairKey(f[1], f[2]), p).second) return fail();
            ++nP;
        } else if (f[0] == "F" && f.size() == 2) {
            if (!plain(f[1])) return fail();
            forgotten_.insert(f[1]);
            ++nF;
        } else if (f[0] == "E" && f.size() == 4) {
            std::int64_t a, b, c;
            if (!parseInt(f[1], a) || !parseInt(f[2], b) || !parseInt(f[3], c) || std::size_t(a) != nW || std::size_t(b) != nP
                || std::size_t(c) != nF) return fail();
            ended = true;
        } else {
            return fail();
        }
    }
    if (!ended) return fail();
    rebuildFollowers();
    // A file edited by hand (or from a build with bigger caps) is trimmed to this build's caps.
    if (words_.size() > limits_.maxWords || pairs_.size() > limits_.maxPairs) prune(0);
    ++swipeVersion_;
    dirty_ = false;
    return true;
}

}
