#include "server.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace gamingd {
namespace {

constexpr size_t kMaxConnections = 8;
volatile sig_atomic_t gStop = 0;
void onSignal(int) { gStop = 1; }

std::string randomHex(size_t bytes)
{
    std::string raw(bytes, '\0'), out;
    // WHY getrandom: ids and the token must be unguessable; never fall back to rand().
    if (getrandom(raw.data(), bytes, 0) != ssize_t(bytes)) return {};
    static const char *hex = "0123456789abcdef";
    for (unsigned char c : raw) { out += hex[c >> 4]; out += hex[c & 15]; }
    return out;
}

// Constant-time compare so the token cannot be learned byte by byte from response timing.
bool sameToken(const std::string &a, const std::string &b)
{
    unsigned char diff = a.size() != b.size();
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

long msSince(Clock::time_point t) { return long(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count()); }

} // namespace

void logEvent(const char *event, long a, long b)
{
    std::fprintf(stderr, "swipekb-gamingd: %s %ld %ld\n", event, a, b);
}

// ---- Engine ----------------------------------------------------------------------------------------------

swipekb::Recognizer *Engine::recognizer()
{
    lastUse_ = Clock::now();
    if (!recognizer_) {
        const auto t = Clock::now();
        auto r = std::make_unique<swipekb::Recognizer>();
        if (!r->init(swipekb::ModelPaths::fromDir(dataDir_), {})) { logEvent("recognizer_load_failed"); return nullptr; }
        recognizer_ = std::move(r);
        logEvent("recognizer_loaded_ms", msSince(t));
    }
    return recognizer_.get();
}

swipekb::Predictor *Engine::predictor()
{
    lastUse_ = Clock::now();
    if (!predictor_) {
        const auto t = Clock::now();
        const auto paths = swipekb::ModelPaths::fromDir(dataDir_);
        auto p = std::make_unique<swipekb::Predictor>();
        if (!p->init(paths.lm_model, paths.lm_vocab)) { logEvent("predictor_load_failed"); return nullptr; }
        predictor_ = std::move(p);
        logEvent("predictor_loaded_ms", msSince(t));
    }
    return predictor_.get();
}

int Engine::msUntilUnload(int idleMs) const
{
    if (!recognizer_ && !predictor_) return -1;
    return std::max(0, idleMs - int(msSince(lastUse_)));
}

void Engine::unloadIfIdle(int idleMs)
{
    if ((recognizer_ || predictor_) && msSince(lastUse_) >= idleMs) {
        recognizer_.reset();
        predictor_.reset();
        // WHY malloc_trim: freed model memory otherwise stays in the process and the idle budget is missed.
        malloc_trim(0);
        logEvent("models_unloaded");
    }
}

// ---- Session ---------------------------------------------------------------------------------------------

void Session::wipe()
{
    // WHY overwrite before clearing: best effort so words do not linger in freed memory after a close/reset.
    for (auto &kv : issued) std::fill(kv.second.begin(), kv.second.end(), '\0');
    issued.clear();
    layout.reset();
    issuedReq = 0;
}

// ---- Server ----------------------------------------------------------------------------------------------

Server::~Server()
{
    for (auto &s : sessions_) { s->wipe(); ::close(s->fd); }
    if (listenFd_ >= 0) { ::close(listenFd_); ::unlink(opt_.socketPath.c_str()); ::unlink(opt_.tokenPath.c_str()); }
}

bool Server::start(std::string &why)
{
    umask(0077);   // WHY: everything we create is owner-only from the first instant, not after a chmod
    const auto slash = opt_.socketPath.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : opt_.socketPath.substr(0, slash);
    for (size_t i = 1; i <= dir.size(); ++i)   // mkdir -p
        if (i == dir.size() || dir[i] == '/') { const std::string part = dir.substr(0, i); if (mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) { why = "cannot create socket directory"; return false; } }
    struct stat st;
    if (stat(dir.c_str(), &st) != 0 || st.st_uid != geteuid()) { why = "socket directory is not ours"; return false; }
    chmod(dir.c_str(), 0700);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (opt_.socketPath.size() >= sizeof addr.sun_path) { why = "socket path too long"; return false; }
    std::strcpy(addr.sun_path, opt_.socketPath.c_str());

    // Another daemon already answering on this path? Then do nothing, and above all do not touch its token.
    if (lstat(opt_.socketPath.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) { why = "socket path exists and is not a socket"; return false; }
        int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        const bool alive = connect(probe, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0;
        ::close(probe);
        if (alive) { why = "another swipekb-gamingd is already running"; return false; }
        ::unlink(opt_.socketPath.c_str());   // stale socket from a crashed run
    }

    token_ = randomHex(32);
    if (token_.empty()) { why = "no randomness available"; return false; }
    // Token first, then the socket: the socket appearing means a client can already read a valid token.
    const std::string tmp = opt_.tokenPath + ".tmp";
    ::unlink(tmp.c_str());
    int tf = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (tf < 0 || write(tf, (token_ + "\n").data(), token_.size() + 1) < 0) { why = "cannot write token file"; return false; }
    ::close(tf);
    if (rename(tmp.c_str(), opt_.tokenPath.c_str()) != 0) { why = "cannot install token file"; return false; }

    listenFd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0 || bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0 || chmod(opt_.socketPath.c_str(), 0600) != 0
        || listen(listenFd_, 16) != 0) {
        why = "cannot bind socket";
        if (listenFd_ >= 0) { ::close(listenFd_); listenFd_ = -1; }
        return false;
    }
    logEvent("listening");
    return true;
}

int Server::run()
{
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
    // WHY block + ppoll: a signal arriving between the stop check and the wait must still wake the wait.
    sigset_t block, orig;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    sigprocmask(SIG_BLOCK, &block, &orig);

    while (!gStop) {
        std::vector<pollfd> fds{{listenFd_, POLLIN, 0}};
        for (auto &s : sessions_) fds.push_back({s->fd, POLLIN, 0});
        // Wake only when something is due: no models loaded and no clients mid-frame means wait forever (0% CPU).
        long wait = engine_.msUntilUnload(opt_.idleUnloadMs);
        for (auto &s : sessions_)
            if (!s->in.empty() || !s->authed) {
                const long left = std::max(0L, opt_.frameTimeoutMs - msSince(s->since));
                wait = wait < 0 ? left : std::min(wait, left);
            }
        timespec ts{wait / 1000, (wait % 1000) * 1000000L};
        const int n = ppoll(fds.data(), fds.size(), wait < 0 ? nullptr : &ts, &orig);
        if (n < 0 && errno != EINTR) break;
        if (gStop) break;
        if (n > 0 && (fds[0].revents & POLLIN)) accept_();
        for (size_t i = 1; i < fds.size() && i - 1 < sessions_.size(); ++i)
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) readFrom(*sessions_[i - 1]);
        for (auto &s : sessions_)   // slow or silent clients
            if (!s->closing && (!s->in.empty() || !s->authed) && msSince(s->since) >= opt_.frameTimeoutMs) {
                logEvent("client_timeout");
                s->closing = true;
            }
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [](auto &s) {
            if (!s->closing) return false;
            s->wipe();
            ::close(s->fd);
            return true;
        }), sessions_.end());
        engine_.unloadIfIdle(opt_.idleUnloadMs);
    }
    logEvent("stopping");
    return 0;
}

void Server::accept_()
{
    for (;;) {
        int fd = accept4(listenFd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        ucred cred{};
        socklen_t len = sizeof cred;
        // WHY: even with a 0600 socket, only our own user may ever talk to us.
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.uid != geteuid()) { ::close(fd); continue; }
        auto s = std::make_unique<Session>();
        s->fd = fd;
        s->since = Clock::now();
        if (sessions_.size() >= kMaxConnections) {
            reply(*s, {{"ok", false}, {"error", kErrTooMany}});
            ::close(fd);
            continue;
        }
        sessions_.push_back(std::move(s));
    }
}

void Server::readFrom(Session &s)
{
    char buf[65536];
    const ssize_t n = recv(s.fd, buf, sizeof buf, 0);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { s.closing = true; return; }
    if (n < 0) return;
    if (s.in.empty()) s.since = Clock::now();
    s.in.append(buf, size_t(n));
    while (!s.closing && s.in.size() >= 4) {
        const uint32_t len = (uint32_t((unsigned char)s.in[0]) << 24) | (uint32_t((unsigned char)s.in[1]) << 16)
            | (uint32_t((unsigned char)s.in[2]) << 8) | uint32_t((unsigned char)s.in[3]);
        if (len == 0 || len > kMaxFrame) { fail(s, kErrBadFrame, true); return; }   // cannot resynchronise
        if (s.in.size() < 4 + size_t(len)) break;
        const QByteArray body(s.in.data() + 4, qsizetype(len));
        s.in.erase(0, 4 + size_t(len));
        s.since = Clock::now();
        handle(s, body);
        if (s.authed == false && !s.closing) s.since = Clock::now();
    }
}

void Server::reply(Session &s, const QJsonObject &o)
{
    const QByteArray frame = encodeFrame(QJsonDocument(o).toJson(QJsonDocument::Compact));
    // Non-blocking send: a client that will not read its replies is dropped rather than allowed to stall us.
    if (frame.isEmpty() || send(s.fd, frame.constData(), size_t(frame.size()), MSG_NOSIGNAL | MSG_DONTWAIT) != frame.size())
        s.closing = true;
}

void Server::fail(Session &s, const char *code, bool fatal)
{
    reply(s, {{"ok", false}, {"error", code}});
    logEvent(code);   // WHY the code only: the offending bytes are never logged
    if (fatal || ++s.badRequests > 32) s.closing = true;
}

QJsonObject Server::issueCandidates(Session &s, const std::vector<swipekb::Candidate> &cands)
{
    for (auto &kv : s.issued) std::fill(kv.second.begin(), kv.second.end(), '\0');
    s.issued.clear();
    s.issuedReq = ++s.reqSeq;
    QJsonArray arr;
    for (const auto &c : cands) {
        if (c.word.empty()) continue;   // WHY: an empty guess would type nothing yet occupy a strip slot
        const std::string id = "c" + randomHex(8);
        s.issued[id] = c.word;
        arr.append(QJsonObject{{"id", QString::fromStdString(id)}, {"word", QString::fromStdString(c.word)}, {"score", double(c.score)}});
    }
    return {{"req", double(s.issuedReq)}, {"candidates", arr}};
}

void Server::handle(Session &s, const QByteArray &body)
{
    try {
        auto parsed = parseBody(body);
        if (!parsed.value) { fail(s, parsed.error, !s.authed); return; }
        const QJsonObject &req = *parsed.value;
        const QString op = req.value("op").toString();
        QJsonObject out{{"ok", true}, {"op", op}};

        if (!s.authed) {
            // WHY first: nothing but a correct hello is answered on a fresh connection.
            if (op != QLatin1String("hello") || !sameToken(req.value("token").toString().toStdString(), token_)) { fail(s, kErrUnauthorized, true); return; }
            const auto m = parseMode(req, Mode::Unknown);
            if (!m.value) { fail(s, m.error, true); return; }
            s.authed = true;
            s.mode = *m.value;
            out.insert("session", QString::fromStdString(randomHex(8)));
            reply(s, out);
            logEvent("hello", s.mode == Mode::Plain);
            return;
        }
        if (op == QLatin1String("ping")) { reply(s, out); return; }
        if (op == QLatin1String("status")) {
            out.insert("recognizer_loaded", engine_.recognizerLoaded());
            out.insert("predictor_loaded", engine_.predictorLoaded());
            out.insert("connections", double(sessions_.size()));
            reply(s, out);
            return;
        }
        if (op == QLatin1String("close")) { s.wipe(); reply(s, out); s.closing = true; return; }
        if (op == QLatin1String("reset")) {
            const auto m = parseMode(req, Mode::Unknown);   // WHY default unknown: privacy fails closed
            if (!m.value) { fail(s, m.error, false); return; }
            s.wipe();
            s.mode = *m.value;
            reply(s, out);
            return;
        }
        if (op == QLatin1String("layout")) {
            auto l = parseLayout(req);
            if (!l.value) { fail(s, l.error, false); return; }
            out.insert("keys", double(l.value->size()));
            s.layout = std::move(l.value);
            reply(s, out);
            return;
        }
        if (op == QLatin1String("swipe")) {
            if (!s.layout) { fail(s, kErrNoLayout, false); return; }
            auto a = parseSwipe(req, s.mode);
            if (!a.value) { fail(s, a.error, false); return; }
            auto *rec = engine_.recognizer();
            if (!rec) { fail(s, kErrModelUnavailable, false); return; }
            const auto t = Clock::now();
            const auto cands = rec->recognize(a.value->trace, *s.layout, a.value->context, a.value->topK);
            { const auto issued = issueCandidates(s, cands); for (auto it = issued.begin(); it != issued.end(); ++it) out.insert(it.key(), it.value()); }
            engine_.touch();
            logEvent("swipe_ms", msSince(t), long(cands.size()));
            reply(s, out);
            return;
        }
        if (op == QLatin1String("next_words")) {
            auto a = parseNextWords(req, s.mode);
            if (!a.value) { fail(s, a.error, false); return; }
            std::vector<swipekb::Candidate> cands;
            if (!a.value->suppressed) {
                if (!a.value->partial.empty()) {
                    auto *rec = engine_.recognizer();
                    if (!rec) { fail(s, kErrModelUnavailable, false); return; }
                    cands = rec->complete(a.value->partial, a.value->topK);
                } else {
                    auto *pred = engine_.predictor();
                    if (!pred) { fail(s, kErrModelUnavailable, false); return; }
                    std::vector<std::string> ctx;
                    for (const auto &w : a.value->words) ctx.push_back(QString::fromStdString(w).toLower().toStdString());
                    for (auto &w : pred->predict(ctx, a.value->topK)) cands.push_back({std::move(w), 0.f});
                }
            }
            { const auto issued = issueCandidates(s, cands); for (auto it = issued.begin(); it != issued.end(); ++it) out.insert(it.key(), it.value()); }
            engine_.touch();
            logEvent("next_words", long(cands.size()));
            reply(s, out);
            return;
        }
        if (op == QLatin1String("commit")) {
            // The ONLY way text leaves the daemon for typing: an id we issued, for the request we issued it in.
            if (!req.value("req").isDouble() || !req.value("cand").isString()) { fail(s, kErrBadRequest, false); return; }
            const auto it = s.issued.find(req.value("cand").toString().toStdString());
            if (double(s.issuedReq) != req.value("req").toDouble() || s.issuedReq == 0 || it == s.issued.end()) { fail(s, kErrUnknownCandidate, false); return; }
            out.insert("text", QString::fromStdString(it->second));
            reply(s, out);
            logEvent("commit");
            return;
        }
        fail(s, kErrUnknownOp, false);
    } catch (...) {
        fail(s, "internal_error", true);
    }
}

} // namespace gamingd
