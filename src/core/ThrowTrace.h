#pragma once
#include <string>

namespace materializr {

// Throw-site backtraces, split into a cheap capture and a costly render.
//
// WHY THE SPLIT: Document::getBody() throws on a missing body id, and that
// throw is ORDINARY CONTROL FLOW - around forty call sites are written as
// `try { getBody(id); } catch (...) {}` precisely because a body may be gone
// (deleted by a later step, retired by a replay). Printing a backtrace on
// every throw would bury the log in noise from code that is working fine.
//
// But when one of those throws is NOT guarded, it escapes to the frame
// firewall in Application::run(), and by then the stack is unwound - the one
// thing needed to find the culprit is gone. One such escape silently exited
// the app on Android (see the firewall's comment).
//
// So: capture program counters at the throw (no symbolization, no allocation)
// and render them only if the exception survives to the firewall.
void captureThrowTrace();

// Symbolized frames from the last capture, innermost first, one per line.
// Empty if nothing was captured or the platform has no unwinder.
std::string lastThrowTrace();

} // namespace materializr
