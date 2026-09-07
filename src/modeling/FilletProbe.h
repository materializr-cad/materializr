#pragma once
// Deadline guard for OCCT's fillet builder.
//
// BRepFilletAPI_MakeFillet::Build() takes a Message_ProgressRange, but it hands
// off to ChFi3d_Builder::Compute(), which takes NO progress argument (see
// ChFi3d_Builder.hxx). Nothing inside the blend algorithm ever polls for a user
// break, so a pathological radius cannot be cancelled: the call simply never
// returns. Observed on FOB.mzr — ChFi3d_Builder::StoreData spinning at 100% CPU
// with no progress for minutes, wedging the render loop that called it.
//
// Since the build cannot be interrupted, it must not be entered blind. probe()
// runs the SAME build on a DETACHED worker against a deep copy and waits only
// `budget`. A caller that gets true may then run the real build synchronously,
// knowing this shape + radius converges. A caller that gets false must refuse.
//
// The worker is detached, not a std::async future, on purpose: a std::future's
// destructor BLOCKS until the task finishes, so a wedged probe parked in a
// member vector would hang the app on quit — the same freeze, moved to exit.
//
// Detaching is a TRADE, not a clean win, and the trade is this: a detached
// worker holds its own TopoDS_Shape/TopoDS_Edge copies, so if the process exits
// while one is still inside Build(), OCCT's static destructors (its memory
// manager, the TopLoc registries) can run underneath it. Hang-on-quit becomes
// a possible crash-on-quit. A crash at exit costs the user nothing they had not
// already saved; a hang costs them the ability to quit at all. Neither is good.
// The cure for both is a helper process we can kill, which is a much larger
// change than this fix.
//
// The leak is real too: an abandoned worker keeps burning a core until OCCT
// returns, if it ever does.

#include <TopoDS_Shape.hxx>
#include <TopoDS_Edge.hxx>
#include <vector>

namespace materializr {
namespace fillet {

// Default wall-clock budget for one probe. Generous enough for a legitimately
// heavy blend on a large body, far below the point a user calls it frozen.
inline constexpr double kDefaultProbeSeconds = 2.5;

// Passed as `budget` to mean "whatever setProbeBudget() last configured".
// Distinct from 0.0, which is a real budget meaning "give up immediately" —
// the tests rely on that difference, so this sentinel is negative, not zero.
inline constexpr double kUseConfiguredBudget = -1.0;

// The budget probe() uses when asked for kUseConfiguredBudget. Set from the
// user's settings at startup. Clamped to a sane range: a budget below ~0.25s
// refuses fillets that were only ever slow, and one above 30s is indistinct
// from the freeze this exists to prevent.
void   setProbeBudget(double seconds);
double probeBudget();

// True when a fillet of `radius` on `edges` of `shape` completes inside
// `budget`. False on timeout, throw, or a build that fails outright.
//
// Results are memoised per (shape, edges, radius): an interactive preview
// re-runs the identical query every frame, and probing twice per frame would
// itself be the performance bug this exists to prevent.
bool probe(const TopoDS_Shape& shape, const std::vector<TopoDS_Edge>& edges,
           double radius, double budget = kUseConfiguredBudget);

// How many probes have actually run a build, i.e. missed the cache. The only
// honest way to assert memoisation: timing cannot distinguish a cache hit from
// a fast build, as a mutation that removed edge identity from the cache key
// demonstrated by passing a timing-based test unchanged.
unsigned long long probeRunCount();

// Drop memoised verdicts. Call when the body changes underneath a cached entry.
void clearProbeCache();

} // namespace fillet
} // namespace materializr
