#pragma once

// Included here, not left to callers, so MZ_MOBILE below is always defined
// (or not) by the time the guard runs regardless of what order a caller's
// own includes happen to fall in - ParallelMesh.cpp and the test file both
// used to include this after this header, which worked only because the
// raw condition was written far enough past that include to not notice.
#include "platform_defs.h"

// Checked for the one sharing pattern this codebase actually produces
// (SplitBodyOp's uncopied siblings; see claimShape in ParallelMesh.cpp) and
// for one concurrent-allocator hazard (BRepMeshData_Model's private,
// thread-safe allocator; OSD::SetThreadLocalSignal on each worker),
// against the installed macOS/Linux OCCT builds, and against Windows via a
// source/config-level audit (vcpkg's pinned 7.9.3 portfile, Microsoft's own
// CRT heap thread-safety guarantee, and a real hardware-fault test exercised
// on Windows CI - see windows-parallel-mesh-audit.md project memory). This
// is not a guarantee against every possible OCCT-internal concurrency
// hazard, only the ones checked. Mobile builds were never checked at all -
// they still fall back to the existing sequential loop. Named once here so
// every #if that gates the pool (this header, ParallelMesh.cpp,
// Application.cpp, the tests) shares one definition rather than repeating
// the raw condition.
#if !defined(MZ_MOBILE)
#define MZR_PARALLEL_MESH_SUPPORTED 1
#endif

#include <TopoDS_Shape.hxx>

#include <functional>
#include <thread>
#include <vector>

namespace materializr {

// A load's visible, uncovered bodies. The caller owns their lifetime and
// does not read their triangulations again until parallelMesh returns.
struct ParallelMeshJob {
    int bodyId = -1;
    TopoDS_Shape shape;
};

struct ParallelMeshResult {
    bool completed = false;
    bool ok = false;
    double millis = 0.0;
};

// Sequential does NOT mean parallelMesh meshed anything itself - a job left
// here (small batch, platform-unsupported, a shared TShape, a scan or pool
// failure) is simply not stamped or attempted. The one caller today always
// runs a full rebuildMeshes() afterward, which meshes anything not marked
// pre-meshed; a future caller that does not follow up the same way would
// silently never mesh these jobs.
enum class ParallelMeshPath { Sequential, Pooled };

struct ParallelMeshBatch {
    std::vector<ParallelMeshResult> results;
    ParallelMeshPath path = ParallelMeshPath::Sequential;
    const char* reason = "threshold";
    double poolMs = 0.0;
    double scanMs = 0.0;
    size_t completedJobs = 0;
    size_t threadsStarted = 0;
};

struct ParallelMeshOptions {
    unsigned workerCount = 0; // zero uses hardware_concurrency
    std::function<void(const ParallelMeshJob&, float, float)> mesh;
    std::function<void(size_t, size_t)> onTick;
    // Fault injection at the two boundaries the mesh catch cannot cover.
    std::function<void()> beforeDequeue;
    std::function<std::thread(std::function<void()>)> launch;
};

// Blocking: every started thread is joined before results become readable,
// including a partial launch failure. Failed/unreached jobs need their old
// coverage tags invalidated by the caller before its sequential fallback.
ParallelMeshBatch parallelMesh(const std::vector<ParallelMeshJob>& jobs,
                               float deflection, float angularDeflection,
                               const ParallelMeshOptions& options = {});

#ifdef MZR_PARALLEL_MESH_TESTING
size_t parallelMeshCallsForTest();
#endif

inline float parallelMeshFraction(size_t done, size_t total) {
    return done == 0 ? -1.0f : float(done) / float(total);
}

} // namespace materializr
