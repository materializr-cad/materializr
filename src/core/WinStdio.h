#pragma once
// Windows only: standard-stream setup for a program with no console of its own.
//
// materializr.exe is linked as a GUI-subsystem program (see CMakeLists.txt), so
// double-clicking it opens only its window - no black console beside it. That
// console was the cause of #188: clicking it starts a text selection, and every
// write to it then blocks until a key is pressed, so a click before the app's
// own window appeared froze the app, and Ctrl+C / Alt+F4 on that window
// closed it with no chance to save. With no console there is nothing to click.
//
// A GUI-subsystem process starts with no stdout/stderr, so a user who runs it
// from a terminal would lose --help and the log. init() attaches to the
// terminal that started us, if there is one. Output that was redirected
// (`materializr.exe > log.txt`, a CI pipe) is already wired up by the C runtime
// and is left alone.

#if defined(_WIN32)

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace materializr {
namespace winstdio {

namespace detail {
// The CRT's reaction to a bad argument (a closed descriptor, say) is to kill
// the process outright - which is how a stdio call with a size MSVC rejected
// took the app down before its first window (#82). A GUI program has no
// stdin/stdout/stderr to begin with, so make those calls fail quietly instead.
inline void __cdecl ignoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*,
                                           unsigned int, uintptr_t) {}

inline bool handleUsable(DWORD which) {
    const HANDLE h = GetStdHandle(which);
    return h != nullptr && h != INVALID_HANDLE_VALUE;
}
} // namespace detail

inline void init() {
    _set_invalid_parameter_handler(detail::ignoreInvalidParameter);

    const bool needOut = !detail::handleUsable(STD_OUTPUT_HANDLE);
    const bool needErr = !detail::handleUsable(STD_ERROR_HANDLE);
    if (!needOut && !needErr) return;                 // redirected: already wired

    // Started from a terminal: write there. Started from Explorer there is no
    // terminal; send the streams to NUL so stdout/stderr are still real streams
    // (a GUI process's are not - fprintf on them is a no-op - and the stderr
    // tee in LogCapture needs a real descriptor to swap for its pipe).
    const bool terminal = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
    const char* target = terminal ? "CONOUT$" : "NUL";
    std::FILE* f = nullptr;
    if (needOut && freopen_s(&f, target, "w", stdout) == 0) std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (needErr && freopen_s(&f, target, "w", stderr) == 0) std::setvbuf(stderr, nullptr, _IONBF, 0);
}

} // namespace winstdio
} // namespace materializr

#endif // _WIN32
