// The daemon proper: sockets, per-connection sessions, lazily loaded models. Protocol rules live in
// protocol.hpp; this file only decides what each request DOES.
#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "protocol.hpp"
#include "swipekb/prediction.hpp"
#include "swipekb/recognizer.hpp"

namespace gamingd {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string socketPath, tokenPath, dataDir;
    int idleUnloadMs = 10 * 60 * 1000;   // plan Section 5: unload models after 10 idle minutes
    int frameTimeoutMs = 5000;           // a client may not sit on a half-sent frame or skip "hello" longer than this
};

// Writes only counts and timings. WHY a const char* event and plain integers: there is no way to hand this
// function a std::string of typed text by accident (plan Section 8: logs contain no words, keys or traces).
void logEvent(const char *event, long a = -1, long b = -1);

// The models, loaded on first use and dropped after idle. Single-threaded: the daemon never runs two requests
// at once, so (unlike the Qt plugin) no worker thread is needed.
class Engine {
public:
    explicit Engine(std::string dataDir) : dataDir_(std::move(dataDir)) {}
    swipekb::Recognizer *recognizer();   // nullptr if the models cannot be loaded
    swipekb::Predictor *predictor();
    // WHY called after each use too: loading takes seconds, and the idle clock must start when work FINISHES.
    void touch() { lastUse_ = Clock::now(); }
    bool recognizerLoaded() const { return bool(recognizer_); }
    bool predictorLoaded() const { return bool(predictor_); }
    // Milliseconds until the loaded models should be dropped, or -1 when nothing is loaded.
    int msUntilUnload(int idleMs) const;
    void unloadIfIdle(int idleMs);
private:
    std::string dataDir_;
    std::unique_ptr<swipekb::Recognizer> recognizer_;
    std::unique_ptr<swipekb::Predictor> predictor_;
    Clock::time_point lastUse_;
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
    QJsonObject issueCandidates(Session &s, const std::vector<swipekb::Candidate> &c);

    Options opt_;
    Engine engine_;
    int listenFd_ = -1;
    std::string token_;
    std::vector<std::unique_ptr<Session>> sessions_;
};

} // namespace gamingd
