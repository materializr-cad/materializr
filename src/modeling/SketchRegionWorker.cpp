#include "SketchRegionWorker.h"

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepTools.hxx>
#include <Message_ProgressIndicator.hxx>
#include <OSD.hxx>
#include <Standard_ErrorHandler.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <chrono>
#include <utility>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#include <pmmintrin.h>
#define MZR_HAS_SSE 1
#endif

namespace materializr {

namespace {

// Breaks a build off once its job is superseded. OCCT polls UserBreak()
// between the boolean's interference pairs.
class StopFlag : public Message_ProgressIndicator {
public:
    DEFINE_STANDARD_RTTI_INLINE(StopFlag, Message_ProgressIndicator)
    explicit StopFlag(std::shared_ptr<std::atomic<bool>> stop) : m_stop(std::move(stop)) {}
    Standard_Boolean UserBreak() override { return m_stop->load(); }
protected:
    void Show(const Message_ProgressScope&, const Standard_Boolean) override {}
private:
    std::shared_ptr<std::atomic<bool>> m_stop;
};

// Finished builds nobody has adopted yet (their sketch was hidden, deleted or
// edited before the next pick). Old ones are dropped past this many.
constexpr size_t kMaxUnclaimed = 16;

} // namespace

SketchRegionWorker::SketchRegionWorker(size_t threads)
{
    for (size_t i = 0; i < std::max<size_t>(1, threads); ++i)
        m_threads.emplace_back([this] { run(); });
}

SketchRegionWorker::~SketchRegionWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
        for (auto& r : m_running) r.stop->store(true);
    }
    m_wake.notify_all();
    for (auto& t : m_threads)
        if (t.joinable()) t.join();
}

bool SketchRegionWorker::queuedOrRunningLocked(uint64_t key) const
{
    for (const auto& j : m_queue)
        if (j.key == key) return true;
    for (const auto& r : m_running)
        if (r.key == key && !r.stop->load()) return true;
    return false;
}

bool SketchRegionWorker::adoptLocked(const Sketch& sk, uint64_t key)
{
    for (size_t i = 0; i < m_done.size(); ++i) {
        if (m_done[i].key != key) continue;
        Done d = std::move(m_done[i]);
        m_done.erase(m_done.begin() + static_cast<std::ptrdiff_t>(i));
        // The fuse passes a face it did not split through as the SAME
        // TShape - here, the worker's copy of the host face. The main-thread
        // build hands back the live host face in that case, so do the same.
        if (!d.copyHost.IsNull()) {
            for (auto& r : d.regions) {
                if (!r.face.IsSame(d.copyHost)) continue;
                r.face = TopoDS::Face(d.liveHost.Oriented(r.face.Orientation()));
                r.outerWire = BRepTools::OuterWire(r.face);
                r.holeWires.clear();
                for (TopExp_Explorer w(r.face, TopAbs_WIRE); w.More(); w.Next())
                    if (!r.outerWire.IsNull() && !w.Current().IsSame(r.outerWire))
                        r.holeWires.push_back(TopoDS::Wire(w.Current()));
            }
        }
        return sk.adoptRegions(key, std::move(d.regions));
    }
    return false;
}

void SketchRegionWorker::enqueueLocked(const Sketch& sk, uint64_t key)
{
    const void* owner = &sk;
    // This sketch has moved on: its older build is no use to anyone.
    m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
                                 [&](const Job& j) { return j.owner == owner; }),
                  m_queue.end());
    for (auto& r : m_running)
        if (r.owner == owner && r.key != key) r.stop->store(true);

    Job job;
    job.owner = owner;
    job.key = key;
    job.stop = std::make_shared<std::atomic<bool>>(false);
    job.copy = std::make_shared<Sketch>(sk);
    job.liveHost = sk.getSourceFace();
    if (!job.liveHost.IsNull()) {
        try {
            // Geometry copied, mesh not: the worker must not share surfaces
            // or triangulations with anything the main thread can still write.
            BRepBuilderAPI_Copy copier(job.liveHost, Standard_True, Standard_False);
            job.copyHost = TopoDS::Face(copier.Shape());
        } catch (...) {
            return; // OCCT refused the copy; the caller's sketch stays cold
        }
        job.copy->setSourceFace(job.copyHost);
    }
    m_queue.push_back(std::move(job));
    m_wake.notify_one();
}

bool SketchRegionWorker::ensure(const Sketch& sk)
{
    if (sk.regionsCached()) return true;
    const uint64_t key = sk.regionKey();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (adoptLocked(sk, key)) return true;
    if (!queuedOrRunningLocked(key)) enqueueLocked(sk, key);
    return false;
}

bool SketchRegionWorker::ensureWithin(const Sketch& sk, int budgetMs)
{
    if (ensure(sk)) return true;
    const uint64_t key = sk.regionKey();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        if (adoptLocked(sk, key)) return true;
        if (!queuedOrRunningLocked(key)) return false; // copy refused, or broken off
        if (m_landed.wait_until(lock, until) == std::cv_status::timeout)
            return adoptLocked(sk, key);
    }
}

bool SketchRegionWorker::building(const Sketch& sk) const
{
    if (sk.regionsCached()) return false;
    const uint64_t key = sk.regionKey();
    std::lock_guard<std::mutex> lock(m_mutex);
    return queuedOrRunningLocked(key);
}

size_t SketchRegionWorker::pending() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size() + m_running.size();
}

void SketchRegionWorker::pause()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_paused = true;
}

void SketchRegionWorker::resume()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_paused = false;
    }
    m_wake.notify_all();
}

void SketchRegionWorker::run()
{
    OSD::SetThreadLocalSignal(OSD_SignalMode_Set, Standard_False);
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || (!m_paused && !m_queue.empty()); });
            if (m_stop) return;
            job = std::move(m_queue.front());
            m_queue.pop_front();
            m_running.push_back({job.owner, job.key, job.stop});
        }
#ifdef MZR_HAS_SSE
        // A thread starts with its creator's FPU mode, and the main thread's
        // GL work can leave flush-to-zero on - which OCCT's booleans do not
        // survive cleanly (see resetFpuForOcct in Application.cpp).
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_OFF);
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_OFF);
#endif
        Done done;
        done.key = job.key;
        done.liveHost = job.liveHost;
        done.copyHost = job.copyHost;
        try {
            OCC_CATCH_SIGNALS
            Handle(StopFlag) flag = new StopFlag(job.stop);
            done.regions = job.copy->buildRegionsCancellable(flag->Start());
        } catch (...) {
            // Land an empty result rather than nothing: no result would have
            // the next pick queue the same failing build again, every frame.
            done.regions.clear();
        }
        job.copy.reset();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (size_t i = 0; i < m_running.size(); ++i)
                if (m_running[i].stop == job.stop) {
                    m_running.erase(m_running.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            // Broken off mid-boolean: what came back is the unfused fallback.
            if (!job.stop->load()) {
                if (m_done.size() >= kMaxUnclaimed) m_done.erase(m_done.begin());
                m_done.push_back(std::move(done));
            }
        }
        m_landed.notify_all();
    }
}

} // namespace materializr
