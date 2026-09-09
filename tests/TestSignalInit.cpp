// Tests run the SAME modelling code the app ships, so they must run it in the
// same signal environment - otherwise a test can "fail" in a way the app never
// would.
//
// OCCT's modelling algorithms can fault (SIGSEGV/SIGFPE) deep inside the
// kernel on geometry they can't handle. The app copes in two halves:
//   1. OSD::SetSignal() in main() installs OCCT's handler, which converts the
//      signal into a Standard_Failure;
//   2. OCC_CONVERT_SIGNALS (a compile definition) makes the OCC_CATCH_SIGNALS
//      macro inside ops like ShellOp expand to a real error handler, so the
//      op's own catch receives it and refuses the operation.
// Both halves are required; either alone does nothing.
//
// The test library had NEITHER. materializr_core now gets the definition (see
// tests/CMakeLists.txt) and this translation unit supplies the runtime half
// for every test binary, since almost all of them use gtest_main and have no
// main() of their own to call it from.
//
// Found via issue #80: Shell.FilletRingedFaceFailsNotSilentSeal SEGFAULTs on
// OCCT 7.7.0 while passing on 7.9.3. The test asserts ShellOp REFUSES a
// fillet-ringed face; on 7.7.0 the kernel faults reaching that answer, and
// without signal conversion the fault killed the test binary. The app, which
// has both halves, refuses cleanly on the same geometry.
//
// TRADE-OFF, deliberately accepted: this also converts a genuine SIGSEGV in
// OUR code into an exception rather than a core dump. A crash therefore
// surfaces as a failed assertion or an unexpected throw instead of a signal.
// That is the same deal the shipping app makes, and matching it is the point -
// a test that crashes where the app recovers is testing the wrong program.
#include <OSD.hxx>

// extern "C" for a stable, unmangled name: tests/CMakeLists.txt passes
// `-u mzrInstallOcctSignalHandler` to force this object file into each test
// binary, since nothing references it and the archive would otherwise drop it.
extern "C" void mzrInstallOcctSignalHandler() {
    OSD::SetSignal(Standard_False);
}

namespace {
struct OcctSignalInit {
    OcctSignalInit() { mzrInstallOcctSignalHandler(); }
};
// Static-init: runs before main(), so gtest_main binaries are covered too.
const OcctSignalInit g_occtSignalInit;
} // namespace
