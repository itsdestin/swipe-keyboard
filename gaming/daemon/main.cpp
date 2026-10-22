// swipekb-gamingd — the Gaming Mode engine daemon. See protocol.hpp for the wire protocol and the plan
// (docs/active/plans/2026-10-22-gaming-mode.md, Sections 3, 5, 8, 9) for why it exists.
// No network code lives here: the main Flatpak has no network permission, and a test enforces both.
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <linux/limits.h>

#include "server.hpp"

int main(int argc, char **argv)
{
    gamingd::Options o;
    const char *xdg = std::getenv("XDG_RUNTIME_DIR");
    const std::string runDir = std::string(xdg && *xdg ? xdg : "/tmp") + "/swipekb";
    o.socketPath = runDir + "/engine.sock";
    o.tokenPath = runDir + "/engine.token";
    if (const char *d = std::getenv("SWIPEKB_DATA")) o.dataDir = d;
    else {
        char exe[PATH_MAX];
        const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        std::string p = n > 0 ? std::string(exe, size_t(n)) : "";
        p = p.substr(0, p.rfind('/'));
        o.dataDir = p + "/../share/swipe-keyboard";   // Flatpak /app/bin -> /app/share/swipe-keyboard
    }
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&](std::string &out) { if (i + 1 >= argc) return false; out = argv[++i]; return true; };
        std::string v;
        if (a == "--socket") { if (!val(o.socketPath)) return 2; }
        else if (a == "--token-file") { if (!val(o.tokenPath)) return 2; }
        else if (a == "--data") { if (!val(o.dataDir)) return 2; }
        else if (a == "--idle-unload-ms") { if (!val(v) || (o.idleUnloadMs = std::atoi(v.c_str())) <= 0) return 2; }
        else if (a == "--frame-timeout-ms") { if (!val(v) || (o.frameTimeoutMs = std::atoi(v.c_str())) <= 0) return 2; }
        else {
            std::fprintf(stderr, "usage: swipekb-gamingd [--socket P] [--token-file P] [--data DIR] [--idle-unload-ms N] [--frame-timeout-ms N]\n");
            return 2;
        }
    }
    gamingd::Server server(std::move(o));
    std::string why;
    if (!server.start(why)) {
        std::fprintf(stderr, "swipekb-gamingd: cannot start: %s\n", why.c_str());
        return 1;
    }
    return server.run();
}
