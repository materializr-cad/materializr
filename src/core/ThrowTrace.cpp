#include "ThrowTrace.h"

#include <cstdint>
#include <cstdio>

// _Unwind_Backtrace is the portable-ish unwinder that bionic actually ships
// (Android has no <execinfo.h>/backtrace()); it is also what Android's own
// libbase uses. glibc has it too, so Linux desktop gets the same treatment.
// MSVC has neither header, and this is a diagnostic - no-op there rather than
// drag in DbgHelp for a build that has never seen the bug.
#if defined(__ANDROID__) || defined(__linux__)
#  define MZR_HAVE_UNWIND 1
#  include <unwind.h>
#  include <dlfcn.h>
#  include <cxxabi.h>
#  include <cstdlib>
#  include <cstring>
#endif

namespace materializr {

#if defined(MZR_HAVE_UNWIND)
namespace {

constexpr int kMaxFrames = 40;

struct Frames {
    void* pc[kMaxFrames];
    int   n = 0;
};
// thread_local: the render loop is one thread, but ops run on worker threads
// (async thread re-cuts), and a capture there must not clobber the main one's.
thread_local Frames g_frames;

_Unwind_Reason_Code collect(_Unwind_Context* ctx, void* arg) {
    auto* f = static_cast<Frames*>(arg);
    if (f->n >= kMaxFrames) return _URC_END_OF_STACK;
    const uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc != 0) f->pc[f->n++] = reinterpret_cast<void*>(pc);
    return _URC_NO_REASON;
}

} // namespace

void captureThrowTrace() {
    g_frames.n = 0;
    _Unwind_Backtrace(&collect, &g_frames);
}

std::string lastThrowTrace() {
    const Frames& f = g_frames;
    if (f.n == 0) return {};
    std::string out;
    // Skip the innermost two frames: this function's caller chain into
    // captureThrowTrace itself, which is never interesting.
    // MODULE+OFFSET, not the raw pc. Two reasons, both learned the hard way:
    // ASLR makes an absolute address meaningless in a later addr2line run, and
    // dladdr resolves only DYNAMIC symbols - every static / anonymous-namespace
    // function (i.e. most of this codebase) comes back unnamed, so a
    // symbol-only trace names almost nothing. The offset is exact and always
    // resolvable offline:
    //     addr2line -Cfe <module> 0x<offset>
    for (int i = 2; i < f.n; ++i) {
        Dl_info info{};
        const bool have = dladdr(f.pc[i], &info) != 0 && info.dli_fbase;
        const char* mod = (have && info.dli_fname) ? info.dli_fname : "?";
        // Basename keeps the line readable; the path is the same for every
        // frame in our own binary anyway.
        if (const char* slash = std::strrchr(mod, '/')) mod = slash + 1;
        const uintptr_t off =
            have ? reinterpret_cast<uintptr_t>(f.pc[i]) -
                   reinterpret_cast<uintptr_t>(info.dli_fbase)
                 : 0;

        char* demangled = nullptr;
        const char* sym = nullptr;
        if (have && info.dli_sname) {
            int status = 0;
            demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr,
                                            &status);
            sym = (status == 0 && demangled) ? demangled : info.dli_sname;
        }
        char line[768];
        std::snprintf(line, sizeof(line), "    #%-2d %s+0x%zx  %s\n", i - 2, mod,
                      static_cast<size_t>(off), sym ? sym : "-");
        out += line;
        if (demangled) std::free(demangled);
    }
    out += "    (resolve with: addr2line -Cfe <module> <offset>)\n";
    return out;
}

#else   // no unwinder (MSVC)

void captureThrowTrace() {}
std::string lastThrowTrace() { return {}; }

#endif

} // namespace materializr
