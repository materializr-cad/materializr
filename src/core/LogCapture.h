#pragma once
// In-memory tail of the app's diagnostic output (everything written to stderr),
// kept so "Report a Bug" can attach a log without the user having launched the
// app from a terminal or with --log. Bounded: only the most recent ~256 KB.
//
// Desktop: installStderrTee() points fd 2 at a pipe and a reader thread copies
// each chunk to the ring AND on to the original stderr, so a terminal / --log
// file / journald still sees everything exactly as before. Android already
// pumps its stdio pipe into logcat; that pump feeds append() directly.

#include <cstddef>
#include <string>

namespace materializr {
namespace logcapture {

void append(const char* data, size_t n);
// The most recent output, starting on a line boundary. Empty if none.
std::string snapshot();
// True once something is feeding the ring (the tee is installed, or Android's
// pump has appended).
bool available();
// POSIX/Windows desktop only; no-op elsewhere. Safe to call once, early.
void installStderrTee();
// The fd that reaches the REAL stderr: crash handlers must write here, not to
// fd 2, because a dying process never lets the reader thread forward the pipe.
int originalStderrFd();

} // namespace logcapture
} // namespace materializr
