// The daemon proper: sockets, per-connection sessions, lazily loaded models. Protocol rules live in
// protocol.hpp; this file only decides what each request DOES.
#pragma once

#include <QString>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "languageprofile.h"
#include "protocol.hpp"
#include "swipekb/prediction.hpp"
#include "swipekb/recognizer.hpp"

namespace gamingd {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string socketPath, tokenPath, dataDir;
    // Where the desktop keyboard's settings page keeps the user's enabled languages (read-only here); empty = the
    // standard place. systemLocale is what "nothing enabled" falls back to; empty = this machine's own.
    std::string settingsFile, systemLocale;
    int idleUnloadMs = 10 * 60 * 1000;   // plan Section 5: unload models after 10 idle minutes
    int frameTimeoutMs = 5000;           // a client may not sit on a half-sent frame or skip "hello" longer than this
};

// Writes only counts and timings. WHY a const char* event and plain integers: there is no way to hand this
// function a std::string of typed text by accident (plan Section 8: logs contain no words, keys or traces).
void logEvent(const char *event, long a = -1, long b = -1);

// One language's models. WHY one per profile: each language has its own encoder pack, dictionary and context model,
// resolved and checked by the plugin's own profile code (plugin/languageprofile.cpp).
struct LanguageRuntime {
    swipekb::ProfileSnapshot profile;
    std::unique_ptr<swipekb::Recognizer> recognizer;
    std::unique_ptr<swipekb::Predictor> predictor;
    Clock::time_point lastUse;
};

// The models, loaded on first use and dropped after idle. Single-threaded: the daemon never runs two requests
// at once, so (unlike the Qt plugin) no worker thread is needed.
class Engine {
public:
    explicit Engine(std::string dataDir) : dataDir_(std::move(dataDir)) {}
    // Checks (and caches for a while) that this locale + physical layout has a complete, verified language pack.
    // False means "turn this language off": missing or damaged pack, never a silent English fallback.
    bool profileAvailable(const QString &locale, const QString &physicalLayout, int cacheMs);
    // The loaded models for a profile (loading them now if needed); nullptr if they cannot be loaded.
    swipekb::Recognizer *recognizer(const QString &locale, const QString &physicalLayout);
    swipekb::Predictor *predictor(const QString &locale, const QString &physicalLayout);
    // WHY called after each use too: loading takes seconds, and the idle clock must start when work FINISHES.
    void touch(const QString &locale, const QString &physicalLayout);
    bool recognizerLoaded() const;
    bool predictorLoaded() const;
    size_t profilesLoaded() const;   // how many languages currently hold models
    // Milliseconds until the loaded models should be dropped, or -1 when nothing is loaded.
    int msUntilUnload(int idleMs) const;
    void unloadIfIdle(int idleMs);
    static constexpr size_t kMaxLoadedProfiles = 2;   // WHY: ~200 MB each; two covers a typical English + one other
private:
    LanguageRuntime *runtime(const QString &locale, const QString &physicalLayout);   // verified profile, models not loaded yet
    void evictOldestExcept(const std::string &keep);
    std::string dataDir_;
    std::map<std::string, std::unique_ptr<LanguageRuntime>> runtimes_;
    quint64 epoch_ = 0;
};

// Everything the daemon remembers about one keyboard session. Destroyed (and wiped) when the connection ends,
// so nothing survives a close in either mode.
struct Session {
    int fd = -1;
    std::string in;                 // bytes received but not yet a whole frame
    Clock::time_point since;        // start of the frame being received, or of the connection before "hello"
    bool authed = false, closing = false;
    Mode mode = Mode::Unknown;
    std::optional<std::vector<swipekb::KeyRect>> layout;
    // The language this connection's layout was accepted for (set by "layout"), or why it was refused. Neither holds
    // typed text. A refused layout stays refused (swipe/next_words answer the same code) until the next "layout".
    QString locale, physicalLayout;
    const char *refusedWith = nullptr;
    uint64_t reqSeq = 0, issuedReq = 0;
    std::map<std::string, std::string> issued;   // candidate id -> word, for the latest request only
    int badRequests = 0;
    void wipe();                    // forget layout + issued candidates (overwrites the words first)
};

class Server {
public:
    explicit Server(Options o) : opt_(std::move(o)), engine_(opt_.dataDir) {}
    ~Server();
    bool start(std::string &why);   // token file, socket; false (with a reason) if it cannot
    int run();                      // until SIGTERM/SIGINT; returns the exit code
private:
    void accept_();
    void readFrom(Session &s);
    void handle(Session &s, const QByteArray &body);
    void reply(Session &s, const QJsonObject &o);
    void fail(Session &s, const char *code, bool fatal);
    QJsonObject issueCandidates(Session &s, const std::vector<swipekb::Candidate> &c, const std::string &altWord = {});
    void refuse(Session &s, const char *code);                 // a normal "layer stays off" answer: not a protocol error
    bool profileFor(Session &s, QString &locale, QString &physical, const char *&error);
    std::string settingsFile() const;

    Options opt_;
    Engine engine_;
    int listenFd_ = -1;
    std::string token_;
    std::vector<std::unique_ptr<Session>> sessions_;
};

} // namespace gamingd
