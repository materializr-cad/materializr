#include "LogCapture.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#elif !defined(__ANDROID__)
#include <unistd.h>
#endif

namespace materializr {
namespace logcapture {

namespace {
constexpr size_t kKeep = 256 * 1024;
std::mutex g_mu;
std::string g_ring;
std::atomic<bool> g_any{false};
std::atomic<int> g_origFd{-1};
} // namespace

void append(const char* data, size_t n) {
    if (!data || n == 0) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_ring.append(data, n);
    // Trim in big steps so a stream of small writes doesn't memmove 256 KB each.
    if (g_ring.size() > 2 * kKeep) g_ring.erase(0, g_ring.size() - kKeep);
    g_any = true;
}

std::string snapshot() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_ring.size() <= kKeep) return g_ring;
    size_t start = g_ring.size() - kKeep;
    const size_t nl = g_ring.find('\n', start);
    if (nl != std::string::npos) start = nl + 1;
    return g_ring.substr(start);
}

bool available() { return g_any.load(); }

int originalStderrFd() {
    const int fd = g_origFd.load();
    return fd >= 0 ? fd : 2;
}

void installStderrTee() {
#if defined(__ANDROID__)
    // Android's own stdio pump feeds append().
#else
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) return;

    std::fflush(stderr);
    int fds[2] = {-1, -1};
#if defined(_WIN32)
    // stderr's own descriptor: 2 normally, but a GUI-subsystem exe that was
    // re-pointed at a terminal or NUL (core/WinStdio.h) has it on a later one.
    const int errFd = _fileno(stderr);
    if (errFd < 0) return;
    if (_pipe(fds, 1 << 16, _O_BINARY) != 0) return;
    const int orig = _dup(errFd);      // -1 when there is nowhere to forward: fine
    if (_dup2(fds[1], errFd) != 0) { _close(fds[0]); _close(fds[1]); return; }
    _close(fds[1]);
#define MZ_LC_READ(fd, buf, n)  _read((fd), (buf), static_cast<unsigned>(n))
#define MZ_LC_WRITE(fd, buf, n) _write((fd), (buf), static_cast<unsigned>(n))
#else
    if (pipe(fds) != 0) return;
    const int orig = dup(2);
    if (dup2(fds[1], 2) < 0) { close(fds[0]); close(fds[1]); if (orig >= 0) close(orig); return; }
    close(fds[1]);
#define MZ_LC_READ(fd, buf, n)  read((fd), (buf), (n))
#define MZ_LC_WRITE(fd, buf, n) write((fd), (buf), (n))
#endif
    g_origFd = orig;
    // Unbuffered: every fprintf reaches the pipe at once, so the log tail is
    // current and nothing sits in a stdio buffer when the app dies.
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    g_any = true;

    const int rfd = fds[0];
    std::thread([rfd, orig]() {
        char buf[4096];
        for (;;) {
            const auto n = MZ_LC_READ(rfd, buf, sizeof(buf));
            if (n <= 0) break;
            append(buf, static_cast<size_t>(n));
            // Forward to the real stderr; a failed write (closed terminal) is
            // not our problem - the ring still has the text.
            if (orig >= 0) (void)MZ_LC_WRITE(orig, buf, static_cast<size_t>(n));
        }
    }).detach();
#undef MZ_LC_READ
#undef MZ_LC_WRITE
#endif
}

} // namespace logcapture
} // namespace materializr
