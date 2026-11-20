#include "ParallelMesh.h"

#include "core/MeshParams.h"
#include "io/MeshDiskCache.h"

#include <BRepMesh_IncrementalMesh.hxx>
#include <OSD.hxx>
#include <Standard_ErrorHandler.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Iterator.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <unordered_map>

namespace materializr {
namespace {
using Clock = std::chrono::steady_clock;
#ifdef MZR_PARALLEL_MESH_TESTING
std::atomic<size_t> callsForTest{0};
#endif

// A batch below this many jobs is not worth pooling: thread launch and the
// ownership scan cost more than the sequential loop saves at that size.
constexpr size_t kMinJobsForPool = 4;

#if defined(MZR_PARALLEL_MESH_SUPPORTED)
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

bool claimShape(const TopoDS_Shape& shape, size_t job,
                std::unordered_map<const void*, size_t>& owners) {
    if (shape.IsNull()) return true;
    auto entry = owners.emplace(shape.TShape().get(), job);
    if (!entry.second) return entry.first->second == job;
    // Ignore Location: two placed instances still write the same TShape.
    for (TopoDS_Iterator it(shape); it.More(); it.Next())
        if (!claimShape(it.Value(), job, owners)) return false;
    return true;
}

struct LiveWorker {
    std::atomic<int>& live;
    std::condition_variable& wake;
    // Nothing after the worker's own outer catch (below) may throw
    // uncaught - std::thread terminates the process if it does. notify_all()
    // is not declared noexcept, so this guards it explicitly rather than
    // relying on real implementations not actually throwing in practice.
    ~LiveWorker() {
        --live;
        try { wake.notify_all(); } catch (...) {}
    }
};

struct JoinWorkers {
    std::vector<std::thread>& threads;
    std::atomic<bool>& abort;
    ~JoinWorkers() {
        abort = true;
        for (auto& thread : threads)
            if (thread.joinable()) thread.join();
    }
};
#endif
} // namespace

#ifdef MZR_PARALLEL_MESH_TESTING
size_t parallelMeshCallsForTest() { return callsForTest.load(); }
#endif

ParallelMeshBatch parallelMesh(const std::vector<ParallelMeshJob>& jobs,
                               float deflection, float angularDeflection,
                               const ParallelMeshOptions& options) {
#ifdef MZR_PARALLEL_MESH_TESTING
    ++callsForTest;
#endif
    ParallelMeshBatch batch;
    batch.results.resize(jobs.size());
#if defined(MZR_PARALLEL_MESH_SUPPORTED)
    if (jobs.size() < kMinJobsForPool) return batch;
    const auto scanStart = Clock::now();
    try {
        std::unordered_map<const void*, size_t> owners;
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (!claimShape(jobs[i].shape, i, owners)) {
                batch.reason = "shared-tshape";
                batch.scanMs = elapsed(scanStart);
                return batch;
            }
        }
    } catch (...) {
        batch.reason = "scan-failure";
        batch.scanMs = elapsed(scanStart);
        return batch;
    }
    batch.scanMs = elapsed(scanStart);
    const size_t workers = std::min(jobs.size(), size_t(std::max(1u,
        options.workerCount ? options.workerCount : std::thread::hardware_concurrency())));
    std::atomic<size_t> nextJob{0}, completedJobs{0};
    std::atomic<int> liveWorkers{0};
    std::atomic<bool> abort{false};
    std::condition_variable wake;
    std::mutex mutex;
    std::vector<std::thread> threads;
    threads.reserve(workers);
    const auto poolStart = Clock::now();
    {
        JoinWorkers join{threads, abort};
        try {
            auto worker = [&] {
                LiveWorker live{liveWorkers, wake};
                try {
                    OSD::SetThreadLocalSignal(OSD_SignalMode_Set, Standard_False);
                    while (!abort.load()) {
                        if (options.beforeDequeue) options.beforeDequeue();
                        const size_t i = nextJob.fetch_add(1);
                        if (i >= jobs.size()) break;
                        ParallelMeshResult result;
                        try {
                            OCC_CATCH_SIGNALS
                            const auto start = Clock::now();
                            // No BRepTools::Clean() first, unlike ShapeRenderer::
                            // tessellate(). Safe only because the one caller is
                            // a fresh load: ProjectIO never persists a
                            // triangulation (BinTools::Write with
                            // Standard_False for triangulation), so every job
                            // here is guaranteed bare. A caller meshing
                            // already-triangulated shapes would need to Clean
                            // first, the same reason tessellate() does.
                            if (options.mesh) options.mesh(jobs[i], deflection, angularDeflection);
                            else {
                                // Last open's mesh for these exact bytes, if
                                // it was slow enough to keep (MeshDiskCache).
                                if (!meshcache::load(jobs[i].shape, deflection,
                                                     angularDeflection)) {
                                    premeshRuledWalls(jobs[i].shape, deflection,
                                                      angularDeflection);
                                    BRepMesh_IncrementalMesh mesher(jobs[i].shape,
                                        meshParams(deflection, angularDeflection, false));
                                    repairInaccurateFaces(jobs[i].shape, deflection,
                                                          angularDeflection);
                                    if (elapsed(start) >= meshcache::kStoreMinMs)
                                        meshcache::store(jobs[i].shape, deflection,
                                                         angularDeflection);
                                }
                            }
                            result.ok = true;
                            result.millis = elapsed(start);
                        } catch (const Standard_Failure&) {
                            result.ok = false;
                        } catch (const std::exception&) {
                            result.ok = false;
                        } catch (...) {
                            result.ok = false;
                        }
                        result.completed = true;
                        batch.results[i] = result;
                        ++completedJobs;
                        wake.notify_all();
                    }
                } catch (...) {
                    // A bookkeeping fault leaves unclaimed jobs for the main
                    // thread. A mesh fault above only fails its own job.
                }
            };
            for (size_t i = 0; i < workers; ++i) {
                ++liveWorkers;
                try {
                    threads.push_back(options.launch ? options.launch(worker) : std::thread(worker));
                } catch (...) {
                    --liveWorkers; // no thread ran to own this increment
                    throw;
                }
                ++batch.threadsStarted;
            }
            batch.path = ParallelMeshPath::Pooled;
            batch.reason = "none";
            std::unique_lock<std::mutex> lock(mutex);
            while (completedJobs.load() != jobs.size() && liveWorkers.load() != 0) {
                wake.wait_for(lock, std::chrono::milliseconds(10));
                if (options.onTick) options.onTick(completedJobs.load(), jobs.size());
                if (options.shouldStop && options.shouldStop()) abort.store(true);
            }
        } catch (...) {
            batch.path = ParallelMeshPath::Sequential;
            batch.reason = "pool-failure";
        }
    }
    batch.poolMs = elapsed(poolStart);
    batch.completedJobs = completedJobs.load();
    if (batch.path == ParallelMeshPath::Pooled) {
        for (const auto& result : batch.results)
            if (!result.ok) { batch.reason = "job-failure"; break; }
    }
#else
    batch.reason = "platform";
#endif
    return batch;
}

} // namespace materializr
