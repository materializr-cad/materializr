#include "FilletProbe.h"

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <TopoDS.hxx>
#include <TopoDS_TShape.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <tuple>

namespace materializr {
namespace fillet {
namespace {

std::atomic<int> g_abandoned{0};
std::atomic<unsigned long long> g_runs{0};

// Configured budget. Atomic because probe() may be called from any thread and
// the setter runs on the UI thread when settings are applied.
std::atomic<double> g_budget{kDefaultProbeSeconds};

// One probe's result slot. Shared by value between the caller and the detached
// worker, so the worker can outlive the caller and still write somewhere valid
// — the whole reason this is a shared_ptr and not a stack local.
struct Slot {
    std::mutex              m;
    std::condition_variable cv;
    bool                    done = false;
    bool                    ok   = false;
};

// Memo key. The shape is identified by its TShape pointer: a fillet's inputs are
// the document's live body and its sub-edges, which keep identity between frames
// of one interactive drag — exactly the window where re-probing would hurt.
//
// A raw pointer alone would be unsound: free a TShape and the allocator may hand
// the same address to unrelated geometry, and a cached "this radius converges"
// would then wave through a build that hangs — the exact failure this guard
// exists to prevent. Each entry therefore keeps a Handle to the TShape it was
// keyed on, so that address cannot be recycled while the verdict is live.
using Key = std::tuple<const void*, unsigned long long, long long, long long>;

struct Entry {
    bool                       ok = false;
    Handle(TopoDS_TShape)      keepAlive;  // pins the address in the key
};

// Cap on remembered verdicts. A radius drag mints a key per frame, so an
// unbounded map would grow for as long as the session lasts — and every entry
// pins a TShape. On overflow drop everything: the same wholesale flush
// SketchRenderer uses for its static cache, and a re-probe costs one build.
constexpr size_t kMaxCacheEntries = 512;

Key makeKey(const TopoDS_Shape& shape, const std::vector<TopoDS_Edge>& edges,
            double radius, double budget) {
    // Quantise the radius to 1e-6 mm so float noise in a dragged value doesn't
    // miss the cache on every frame, while still separating genuinely distinct
    // radii (OCCT's failures are isolated points, sometimes microns apart).
    const long long q = static_cast<long long>(radius * 1e6 + (radius < 0 ? -0.5 : 0.5));
    // WHICH edges, not merely how many. Two different single-edge selections on
    // the same body at the same radius are different questions, and answering
    // one with the other's verdict would send an unprobed input straight into
    // the uninterruptible Build() this exists to stand in front of.
    // FNV-1a over each edge's TShape address, order-sensitive because the
    // caller's edge order is what reaches Add().
    unsigned long long h = 1469598103934665603ull;
    for (const auto& e : edges) {
        auto bits = reinterpret_cast<std::uintptr_t>(e.TShape().get());
        for (unsigned i = 0; i < sizeof(bits); ++i) {
            h ^= static_cast<unsigned long long>((bits >> (i * 8)) & 0xff);
            h *= 1099511628211ull;
        }
    }
    // The budget is part of the key. A refusal only means "did not converge in
    // THIS long", so a verdict reached under a truncated window must not veto a
    // later attempt that has the full budget to spend.
    const long long qb = static_cast<long long>(budget * 1e3 + 0.5);
    return Key{shape.TShape().get(), h, q, qb};
}

std::mutex             g_cacheMu;
std::map<Key, Entry>   g_cache;

} // namespace

void setProbeBudget(double seconds) {
    if (!(seconds > 0.0)) seconds = kDefaultProbeSeconds;   // also catches NaN
    g_budget.store(std::min(30.0, std::max(0.25, seconds)));
    // A changed budget can flip a cached verdict: a build previously abandoned
    // may fit the new window. Stale "false" would make the setting look inert.
    clearProbeCache();
}

double probeBudget() { return g_budget.load(); }

unsigned long long probeRunCount() { return g_runs.load(); }

void clearProbeCache() {
    std::lock_guard<std::mutex> lk(g_cacheMu);
    g_cache.clear();
}

bool probe(const TopoDS_Shape& shape, const std::vector<TopoDS_Edge>& edges,
           double radius, double budget) {
    if (shape.IsNull() || edges.empty() || radius <= 0.0) return false;
    if (budget < 0.0) budget = g_budget.load();

    const Key key = makeKey(shape, edges, radius, budget);
    {
        std::lock_guard<std::mutex> lk(g_cacheMu);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) return it->second.ok;
    }

    // Deep copy for the worker. Mandatory, not defensive: OCCT lazily fills
    // BSplSLib caches inside Geom_BSplineSurface on first evaluation, so the
    // worker calling D1() on the SAME surface the render thread is meshing is a
    // straight data race. The copy also remaps the edges — sub-shapes of the
    // original are not sub-shapes of the copy, and Add() on a foreign edge does
    // not build what the caller asked for.
    TopoDS_Shape             copy;
    std::vector<TopoDS_Edge> copyEdges;
    try {
        BRepBuilderAPI_Copy copier(shape);
        if (!copier.IsDone()) return false;
        copy = copier.Shape();
        if (copy.IsNull()) return false;
        copyEdges.reserve(edges.size());
        for (const auto& e : edges) {
            const TopoDS_Shape m = copier.ModifiedShape(e);
            if (m.IsNull() || m.ShapeType() != TopAbs_EDGE) return false;
            copyEdges.push_back(TopoDS::Edge(m));
        }
    } catch (...) {
        return false;
    }

    auto slot = std::make_shared<Slot>();
    try {
    std::thread([slot, copy, copyEdges, radius]() {
        bool ok = false;
        try {
            BRepFilletAPI_MakeFillet mk(copy);
            for (const auto& e : copyEdges) mk.Add(radius, e);
            mk.Build();
            ok = mk.IsDone() && !mk.Shape().IsNull();
        } catch (...) {
            ok = false;
        }
        bool late = false;
        {
            std::lock_guard<std::mutex> lk(slot->m);
            late      = slot->done;   // the caller already gave up on us
            slot->ok  = ok;
            slot->done = true;
        }
        if (late) g_abandoned.fetch_sub(1);
        slot->cv.notify_all();
    }).detach();
    // Counted only once the worker is actually running. It used to be
    // incremented before the thread was constructed, so a thread-exhaustion
    // failure below still counted as a run — and probeRunCount() is documented
    // as "probes that have actually run a build" and is the sole assertion in
    // the memoisation test.
    g_runs.fetch_add(1);
    } catch (const std::system_error&) {
        // Out of threads — every previously abandoned probe still holds one.
        // Refuse rather than propagate: the caller's only guard wraps Build().
        std::fprintf(stderr, "[Fillet] could not start probe worker (%d already "
                             "abandoned) — refusing the build.\n",
                     g_abandoned.load());
        return false;
    }

    bool ok = false;
    {
        std::unique_lock<std::mutex> lk(slot->m);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::duration<double>(budget);
        if (slot->cv.wait_until(lk, deadline, [&] { return slot->done; })) {
            ok = slot->ok;
        } else {
            // Timed out. Mark the slot spent so the worker knows nobody is
            // listening, and leave it running — there is no way to stop it.
            slot->done = true;
            g_abandoned.fetch_add(1);
            std::fprintf(stderr,
                "[Fillet] probe exceeded %.1fs at R=%.4f — refusing the build "
                "(OCCT's blend cannot be interrupted; worker abandoned).\n",
                budget, radius);
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_cacheMu);
        if (g_cache.size() >= kMaxCacheEntries) g_cache.clear();
        g_cache[key] = Entry{ok, shape.TShape()};
    }
    return ok;
}

} // namespace fillet
} // namespace materializr
