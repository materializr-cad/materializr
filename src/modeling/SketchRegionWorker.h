#pragma once

#include "modeling/Sketch.h"

#include <TopoDS_Face.hxx>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace materializr {

// Builds sketch regions (Sketch::buildRegions) on worker threads (#130).
//
// A region build general-fuses the sketch's faces with its host face. With a
// B-spline host face whose edges the sketch lines run along, OCCT's edge-edge
// intersection takes 15-20 s on a desktop and minutes on a tablet - and the
// first click in the viewport used to run it on the main thread, which read
// as a hard freeze. Picking now asks this worker instead: a sketch whose
// regions are not built yet simply has none to offer until they land.
//
// Same rule as MeshWorker: the worker never touches a live TShape. request()
// copies the sketch on the main thread, host face deep-copied; the worker
// builds on its private copy; the main thread adopts the result into the
// live sketch's cache, mapping the copied host face back to the live one.
//
// Results are keyed by Sketch::regionKey(), so a build is only ever adopted
// by geometry it was made for. A newer request for the same sketch drops its
// queued build and breaks off its running one inside the boolean. The
// destructor breaks off everything and joins, so exit waits for at most one
// edge-edge intersection per thread.
class SketchRegionWorker {
public:
    explicit SketchRegionWorker(size_t threads = 2);
    ~SketchRegionWorker();
    SketchRegionWorker(const SketchRegionWorker&) = delete;
    SketchRegionWorker& operator=(const SketchRegionWorker&) = delete;

    // Main thread. True when `sk`'s regions are cached, adopting a finished
    // build for its current geometry first; buildRegions() is then a cache
    // hit. Otherwise queues a build (once per geometry) and returns false.
    bool ensure(const Sketch& sk);

    // Main thread. ensure(), waiting up to `budgetMs` for the build. A click
    // on a light sketch (a rectangle) still selects its region on the very
    // click; a heavy one costs the click this much and no more.
    bool ensureWithin(const Sketch& sk, int budgetMs);

    // Main thread. A build is queued or running for `sk`'s current geometry.
    bool building(const Sketch& sk) const;

    // Builds queued or running, for any sketch.
    size_t pending() const;

    // Hold the workers before they take their next build, and let them go.
    // For tests that need a build to stay pending.
    void pause();
    void resume();

private:
    struct Job {
        const void* owner = nullptr; // the live Sketch; a supersede tag only
        uint64_t key = 0;
        std::shared_ptr<Sketch> copy;
        TopoDS_Face liveHost, copyHost;
        std::shared_ptr<std::atomic<bool>> stop;
    };
    struct Done {
        uint64_t key = 0;
        std::vector<Sketch::Region> regions;
        TopoDS_Face liveHost, copyHost;
    };
    struct Running {
        const void* owner = nullptr;
        uint64_t key = 0;
        std::shared_ptr<std::atomic<bool>> stop;
    };

    void run();
    bool adoptLocked(const Sketch& sk, uint64_t key);
    bool queuedOrRunningLocked(uint64_t key) const;
    void enqueueLocked(const Sketch& sk, uint64_t key);

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;   // workers: a job, or stop
    std::condition_variable m_landed; // ensureWithin: a build finished
    std::deque<Job> m_queue;
    std::vector<Running> m_running;
    std::vector<Done> m_done;
    bool m_stop = false;
    bool m_paused = false;
    std::vector<std::thread> m_threads;
};

} // namespace materializr
