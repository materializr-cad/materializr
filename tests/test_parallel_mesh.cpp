// Load tessellation: real OCCT shapes, bounded worker rendezvous, and the
// application's own tag bookkeeping. GL uploads go to sinks in this target.
#include "viewport/ParallelMesh.h"
#include "viewport/MeshTag.h"
#include "viewport/MeshDispatch.h"
#include "core/MeshParams.h"
#include "core/Document.h"
#include "io/ProjectIO.h"
#include "modeling/SplitBodyOp.h"
#include "app/DrawThrottle.h"

#include <gtest/gtest.h>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Trsf.hxx>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#if defined(MZR_PARALLEL_MESH_SUPPORTED)
#include "app/Application.h"
#include "viewport/ShapeRenderer.h"

namespace materializr {
struct ParallelMeshTestAccess {
    static Document& doc(Application& a) { return *a.m_document; }
    static ShapeRenderer& renderer(Application& a) { return *a.m_shapeRenderer; }
    static MeshDispatch& dispatch(Application& a) { return a.m_meshDispatch; }
    static auto& setup(Application& a) { return a.m_parallelMeshTestSetup; }
    static auto& before(Application& a) { return a.m_parallelMeshTestBeforeBookkeeping; }
    static auto& after(Application& a) { return a.m_parallelMeshTestAfterBookkeeping; }
    static bool load(Application& a, const std::string& path) { return a.loadProjectAt(path); }
    static void rebuild(Application& a) { a.m_meshesDirty = true; a.rebuildMeshes(); }
    static void loadWithProgress(Application& a, const std::string& path) { a.loadProjectWithProgress(path); }
    static bool pumping(Application& a) { return a.m_pumpMeshProgress; }
};
}
#endif

using namespace materializr;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

namespace {
constexpr float kDefl = 0.1f, kAng = 0.3f;

std::vector<ParallelMeshJob> boxes(size_t n = 4) {
    std::vector<ParallelMeshJob> jobs;
    for (size_t i = 0; i < n; ++i)
        jobs.push_back({int(i), BRepPrimAPI_MakeBox(10, 12, 14).Shape()});
    return jobs;
}

void mesh(const ParallelMeshJob& job, float defl, float ang) {
    BRepMesh_IncrementalMesh mesher(job.shape, meshParams(defl, ang, false));
}

std::vector<ParallelMeshJob> jobsFrom(Document& doc) {
    std::vector<ParallelMeshJob> jobs;
    for (int id : doc.getAllBodyIds()) jobs.push_back({id, doc.getBody(id)});
    return jobs;
}

struct ProjectFile {
    std::filesystem::path path;
    ProjectFile() {
        static std::atomic<unsigned> serial{0};
        path = std::filesystem::temp_directory_path() /
            ("materializr-parmesh-" + std::to_string(Clock::now().time_since_epoch().count()) +
             "-" + std::to_string(serial++) + ".materializr");
        Document doc;
        for (auto& j : boxes()) doc.addBody(j.shape, "Box");
        EXPECT_TRUE(ProjectIO::save(path.string(), doc).success);
    }
    ~ProjectFile() { std::filesystem::remove(path); }
};

void pointers(const TopoDS_Shape& shape, std::set<const void*>& keys) {
    if (!keys.insert(shape.TShape().get()).second) return;
    for (TopoDS_Iterator it(shape); it.More(); it.Next()) pointers(it.Value(), keys);
}

#if defined(MZR_PARALLEL_MESH_SUPPORTED)
using Access = ParallelMeshTestAccess;

// Count actual synchronous mesher calls, not bare faces or tag lookups.
int upload(Application& app) {
    auto& renderer = Access::renderer(app);
    const auto before = renderer.meshCallsForTest();
    Access::rebuild(app);
    return int(renderer.meshCallsForTest() - before);
}
void outerFaultLoad(const std::string& path) {
    // alarm(3) was POSIX-only. A detached watchdog thread that force-exits
    // after the same 3-second deadline is the portable equivalent: if the
    // real load call below hangs (the bug this test guards against), the
    // watchdog fires std::_Exit(2) before ASSERT_EXIT's own timeout, so the
    // failure is "wrong exit code" instead of an indefinite hang.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::_Exit(2);
    }).detach();
    Application app;
    Access::setup(app) = [](auto& options) {
        options.workerCount = 2;
        options.beforeDequeue = [] { throw std::runtime_error("dequeue"); };
    };
    Access::after(app) = [](const auto& batch) {
        EXPECT_LT(batch.completedJobs, batch.results.size());
        EXPECT_EQ(batch.threadsStarted, 2u);
        for (const auto& r : batch.results) EXPECT_FALSE(r.completed);
    };
    EXPECT_TRUE(Access::load(app, path));
    EXPECT_EQ(upload(app), 4);
    for (auto j : jobsFrom(Access::doc(app))) EXPECT_EQ(countUnmeshedFaces(j.shape), 0);
    std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
}
#endif
} // namespace

#if defined(MZR_PARALLEL_MESH_SUPPORTED)

// 1. Rendezvous is INSIDE the callable that performs the mesh. Serialising
// that callable, even in several threads, must time out here.
TEST(ParallelMesh, MeshCallsOverlap) {
    auto jobs = boxes();
    ParallelMeshOptions options;
    options.workerCount = 2;
    std::mutex mutex;
    std::condition_variable wake;
    int entered = 0;
    bool overlapped = false;
    options.mesh = [&](const auto& job, float defl, float ang) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (++entered == 1)
                overlapped = wake.wait_for(lock, Ms(500), [&] { return entered >= 2; });
            else wake.notify_all();
        }
        mesh(job, defl, ang);
    };
    const auto batch = parallelMesh(jobs, kDefl, kAng, options);
    EXPECT_EQ(batch.path, ParallelMeshPath::Pooled);
    EXPECT_TRUE(overlapped) << "mesh-call rendezvous timed out";
    EXPECT_EQ(batch.completedJobs, jobs.size());
}

// Cancel on a progress card has to land in a body's time, not the batch's (#181):
// once shouldStop says so no worker starts another job, and the jobs never
// reached come back not ok (the caller forgets their coverage tags).
TEST(ParallelMesh, ShouldStopLeavesTheRestOfTheBatchUnstarted) {
    std::vector<ParallelMeshJob> jobs;
    for (int i = 0; i < 40; ++i)
        jobs.push_back({i, BRepPrimAPI_MakeBox(1.0 + i, 2.0, 3.0).Shape()});
    ParallelMeshOptions options;
    options.workerCount = 1;
    options.mesh = [&](const auto& job, float defl, float ang) {
        std::this_thread::sleep_for(Ms(30));      // slow enough that stopping beats finishing
        mesh(job, defl, ang);
    };
    options.shouldStop = [] { return true; };
    const auto batch = parallelMesh(jobs, kDefl, kAng, options);
    EXPECT_LT(batch.completedJobs, jobs.size());
    size_t notOk = 0;
    for (const auto& r : batch.results) if (!r.ok) ++notOk;
    EXPECT_GE(notOk, jobs.size() - batch.completedJobs);
    EXPECT_GT(notOk, jobs.size() / 2) << "most of the batch was never started";
}

// 2. Reading twice matters: two handles to one loaded shape share its mesh.
TEST(ParallelMesh, IndependentlyLoadedCopiesAgreePerFace) {
    ProjectFile file;
    Document original;
    original.addBody(BRepPrimAPI_MakeCylinder(5, 12).Shape(), "Cylinder");
    for (auto& j : boxes()) original.addBody(j.shape, "Box");
    ASSERT_TRUE(ProjectIO::save(file.path.string(), original).success);
    Document pooled, serial;
    ASSERT_TRUE(ProjectIO::load(file.path.string(), pooled).success);
    ASSERT_TRUE(ProjectIO::load(file.path.string(), serial).success);
    auto p = jobsFrom(pooled), s = jobsFrom(serial);
    ASSERT_EQ(parallelMesh(p, kDefl, kAng).path, ParallelMeshPath::Pooled);
    for (size_t i = 0; i < p.size(); ++i) {
        ASSERT_NE(p[i].shape.TShape().get(), s[i].shape.TShape().get());
        BRepMesh_IncrementalMesh mesher(s[i].shape, meshParams(kDefl, kAng, true));
        TopExp_Explorer pe(p[i].shape, TopAbs_FACE), se(s[i].shape, TopAbs_FACE);
        for (; pe.More() && se.More(); pe.Next(), se.Next()) {
            TopLoc_Location pl, sl;
            auto pt = BRep_Tool::Triangulation(TopoDS::Face(pe.Current()), pl);
            auto st = BRep_Tool::Triangulation(TopoDS::Face(se.Current()), sl);
            ASSERT_FALSE(pt.IsNull());
            ASSERT_FALSE(st.IsNull());
            ASSERT_EQ(pt->NbTriangles(), st->NbTriangles());
            ASSERT_EQ(pt->NbNodes(), st->NbNodes());
            for (int n = 1; n <= pt->NbNodes(); ++n) {
                EXPECT_DOUBLE_EQ(pt->Node(n).X(), st->Node(n).X());
                EXPECT_DOUBLE_EQ(pt->Node(n).Y(), st->Node(n).Y());
                EXPECT_DOUBLE_EQ(pt->Node(n).Z(), st->Node(n).Z());
            }
            for (int t = 1; t <= pt->NbTriangles(); ++t)
                for (int corner = 1; corner <= 3; ++corner)
                    EXPECT_EQ(pt->Triangle(t).Value(corner), st->Triangle(t).Value(corner));
        }
        EXPECT_EQ(pe.More(), se.More());
    }
}

// 3. The splitter's siblings must stay in this process; a save/read can
// sever their shared descendants. Repeated vertices within a box are fine.
TEST(ParallelMesh, SplitSiblingsFallBackButOrdinaryBodiesPool) {
    Document doc;
    int id = doc.addBody(BRepPrimAPI_MakeBox(10, 12, 14).Shape(), "Split");
    SplitBodyOp split;
    split.setBody(id);
    split.setSplitPlane(gp_Pln(gp_Pnt(5, 0, 0), gp_Dir(1, 0, 0)));
    ASSERT_TRUE(split.execute(doc));
    auto jobs = jobsFrom(doc);
    ASSERT_EQ(jobs.size(), 2u);
    ASSERT_NE(jobs[0].shape.TShape().get(), jobs[1].shape.TShape().get());
    std::set<const void*> a, b;
    pointers(jobs[0].shape, a);
    pointers(jobs[1].shape, b);
    bool shared = false;
    for (auto key : a) if (b.count(key)) shared = true;
    ASSERT_TRUE(shared);
    for (auto j : boxes(2)) jobs.push_back(j);
    const auto batch = parallelMesh(jobs, kDefl, kAng);
    EXPECT_EQ(batch.path, ParallelMeshPath::Sequential);
    EXPECT_STREQ(batch.reason, "shared-tshape");
    EXPECT_EQ(batch.threadsStarted, 0u);
    ProjectFile file;
    Application app;
    Access::setup(app) = [&](auto&) {
        Access::doc(app).clear();
        for (auto& job : jobs) Access::doc(app).addBody(job.shape, "Sibling");
    };
    ::testing::internal::CaptureStderr();
    const bool loaded = Access::load(app, file.path.string());
    const auto log = ::testing::internal::GetCapturedStderr();
    EXPECT_TRUE(loaded);
    EXPECT_NE(log.find("fallback=1 reason=shared-tshape"), std::string::npos);
    EXPECT_EQ(upload(app), 4);
    EXPECT_EQ(parallelMesh(boxes(), kDefl, kAng).path, ParallelMeshPath::Pooled)
        << "ordinary independent bodies must pool";
    auto located = boxes();
    gp_Trsf tr;
    tr.SetTranslation(gp_Vec(50, 0, 0));
    located[1].shape = located[0].shape.Moved(TopLoc_Location(tr));
    EXPECT_STREQ(parallelMesh(located, kDefl, kAng).reason, "shared-tshape");
}

// 4. Use the load's actual eligibility filter, then its real upload path.
TEST(ParallelMesh, CoveredBodiesAreNotResubmitted) {
    ProjectFile file;
    Application app;
    std::atomic<int> calls{0};
    Access::setup(app) = [&](auto& options) {
        for (auto& j : jobsFrom(Access::doc(app))) {
            mesh(j, kDefl, kAng);
            Access::renderer(app).notePreMeshed(j.shape, kDefl, kAng);
        }
        options.mesh = [&](const auto&, float, float) { ++calls; };
    };
    Access::after(app) = [](const auto& batch) { EXPECT_TRUE(batch.results.empty()); };
    ASSERT_TRUE(Access::load(app, file.path.string()));
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(upload(app), 0);
}

// 5. Enough visible bodies remain to pool: excluding the hidden one cannot
// accidentally pass just because the entire batch fell below threshold.
TEST(ParallelMesh, HiddenBodiesAreNotSubmitted) {
    ProjectFile file;
    Application app;
    int hidden = -1;
    std::atomic<int> calls{0};
    Access::setup(app) = [&](auto& options) {
        hidden = Access::doc(app).addBody(BRepPrimAPI_MakeBox(3, 4, 5).Shape(), "Hidden");
        Access::doc(app).setBodyVisible(hidden, false);
        options.mesh = [&](const auto& j, float d, float a) {
            EXPECT_NE(j.bodyId, hidden);
            ++calls;
            mesh(j, d, a);
        };
    };
    ASSERT_TRUE(Access::load(app, file.path.string()));
    EXPECT_EQ(calls, 4);
    EXPECT_EQ(countUnmeshedFaces(Access::doc(app).getBody(hidden)), 6);
    EXPECT_EQ(upload(app), 0);
}

// 6. No launch at all for a small batch, including a machine reporting zero
// hardware threads (the override's zero means use that machine's report).
TEST(ParallelMesh, SmallBatchesStaySequential) {
    for (size_t n = 0; n < 4; ++n) {
        ParallelMeshOptions options;
        options.launch = [](auto) -> std::thread { ADD_FAILURE(); throw std::runtime_error("launch"); };
        auto batch = parallelMesh(boxes(n), kDefl, kAng, options);
        EXPECT_EQ(batch.path, ParallelMeshPath::Sequential);
        EXPECT_STREQ(batch.reason, "threshold");
        EXPECT_EQ(batch.threadsStarted, 0u);
        EXPECT_EQ(batch.completedJobs, 0u);
    }
}

// 7. One worker fails its first job and still drains the rest. A mesh-only
// face keeps its only representation through the sequential Clean as well.
TEST(ParallelMesh, FailedJobContinuesAndFallbackMeshesExactlyOnce) {
    ProjectFile file;
    Application app;
    int meshOnlyId = -1;
    std::vector<int> calls;
    std::thread::id worker;
    Access::setup(app) = [&](auto& options) {
        TopoDS_Face face;
        BRep_Builder builder;
        Handle(Poly_Triangulation) tri = new Poly_Triangulation(3, 1, false);
        tri->SetNode(1, gp_Pnt(0, 0, 0)); tri->SetNode(2, gp_Pnt(1, 0, 0));
        tri->SetNode(3, gp_Pnt(0, 1, 0)); tri->SetTriangle(1, Poly_Triangle(1, 2, 3));
        builder.MakeFace(face, tri);
        meshOnlyId = Access::doc(app).addBody(face, "Mesh only");
        options.workerCount = 1;
        options.mesh = [&](const auto& j, float d, float a) {
            if (calls.empty()) worker = std::this_thread::get_id();
            EXPECT_EQ(worker, std::this_thread::get_id());
            calls.push_back(j.bodyId);
            if (calls.size() == 1) throw Standard_Failure("first job");
            if (j.bodyId == meshOnlyId) throw std::runtime_error("mesh only");
            mesh(j, d, a);
        };
    };
    Access::after(app) = [&](const auto& batch) {
        ASSERT_EQ(batch.results.size(), 5u);
        EXPECT_FALSE(batch.results[0].ok);
        EXPECT_TRUE(batch.results[1].ok);
        EXPECT_TRUE(batch.results[2].ok);
        EXPECT_TRUE(batch.results[3].ok);
        EXPECT_EQ(batch.completedJobs, 5u);
    };
    ASSERT_TRUE(Access::load(app, file.path.string()));
    EXPECT_EQ(calls.size(), 5u);
    EXPECT_EQ(upload(app), 2);
    for (auto j : jobsFrom(Access::doc(app))) EXPECT_EQ(countUnmeshedFaces(j.shape), 0);
    EXPECT_EQ(upload(app), 0);
}

// 8. Install a real partial tag, then make the shape bare before submission.
// The failed worker reconstructs exactly that partial count. No allocator
// address reuse, private map access, or direct-only invalidation shortcut.
TEST(ParallelMesh, FailedLoadInvalidatesBothTagGenerations) {
    for (int generation = 0; generation < 3; ++generation) {
        SCOPED_TRACE(generation);
        ProjectFile file;
        Application app;
        TopoDS_Shape shape;
        std::vector<TopoDS_Face> faces;
        Access::setup(app) = [&](auto& options) {
            shape = jobsFrom(Access::doc(app))[0].shape;
            for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
                faces.push_back(TopoDS::Face(ex.Current()));
            mesh({0, shape}, kDefl, kAng);
            BRepTools::Clean(faces.back());
            ASSERT_EQ(countUnmeshedFaces(shape), 1);
            auto& r = Access::renderer(app);
            r.notePreMeshed(shape, kDefl, kAng);
            if (generation != 0) r.retireAll();
            if (generation == 2) r.notePreMeshed(shape, kDefl, kAng);
            ASSERT_TRUE(r.isPreMeshed(shape, kDefl, kAng));
            BRepTools::Clean(shape);
            ASSERT_FALSE(r.isPreMeshed(shape, kDefl, kAng));
            options.mesh = [&](const auto& j, float d, float a) {
                if (j.shape.IsSame(shape)) {
                    for (size_t i = 0; i + 1 < faces.size(); ++i) mesh({0, faces[i]}, d, a);
                    throw std::runtime_error("partial mesh");
                }
                mesh(j, d, a);
            };
        };
        Access::before(app) = [&](const auto& batch) {
            EXPECT_FALSE(batch.results[0].ok);
            EXPECT_EQ(countUnmeshedFaces(shape), 1);
            EXPECT_TRUE(Access::renderer(app).isPreMeshed(shape, kDefl, kAng))
                << "stale tag must match before invalidation";
        };
        Access::after(app) = [&](const auto&) {
            EXPECT_FALSE(Access::renderer(app).isPreMeshed(shape, kDefl, kAng))
                << "failed load must invalidate both tag generations";
        };
        ASSERT_TRUE(Access::load(app, file.path.string()));
        EXPECT_EQ(upload(app), 1);
        EXPECT_EQ(countUnmeshedFaces(shape), 0);
    }
}

// 9. A mesh-call throw cannot exercise the outer catch. The alarm bounds the
// ACTUAL load call in a subprocess, including mutation of its wait predicate.
TEST(ParallelMesh, OuterFaultLoadReturnsBeforeDeadline) {
    ProjectFile file;
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ASSERT_EXIT(outerFaultLoad(file.path.string()), ::testing::ExitedWithCode(0), "") << "outer-fault load must return before the 3-second deadline";
}

// 9b. Every other fault-injection test above throws a plain C++ exception.
// This one injects a genuine hardware fault (SIGSEGV here; on Windows an
// access violation under /EHa) to prove it is CONTAINED - the process does
// not crash, the failing job is marked not-ok, and the worker keeps
// draining the remaining jobs. It does not isolate which specific
// mechanism performed the catch (OCCT's OSD::SetThreadLocalSignal
// translator vs. MSVC's own catch(...)-under-/EHa) - both already exist in
// the worker's catch chain (ParallelMesh.cpp:141-147) and either is
// sufficient for the property this test checks. Verified empirically on
// macOS before this test was written (see windows-parallel-mesh-audit.md);
// this is what makes that verification permanent and, once the guard below
// is flipped, gives it its first real Windows CI run.
TEST(ParallelMesh, RealHardwareFaultInWorkerIsContainedNotFatal) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ASSERT_EXIT({
        // Same watchdog pattern as outerFaultLoad: if fault containment
        // somehow deadlocks instead of completing or crashing, this forces
        // a diagnosable exit instead of an indefinite hang - ctest's own
        // 60s TIMEOUT (tests/CMakeLists.txt:354) does not apply when this
        // binary is run directly, as Step 3 below does.
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            std::_Exit(2);
        }).detach();
        auto jobs = boxes(); // kMinJobsForPool is 4; boxes() defaults to 4
        ParallelMeshOptions options;
        options.workerCount = 1; // deterministic: one worker drains the queue
        options.mesh = [](const auto& j, float d, float a) {
            if (j.bodyId == 0) {
                // volatile: keeps /O2 and -O2 from proving this is UB and
                // folding it away or erroring at compile time.
                volatile std::intptr_t zero = 0;
                *reinterpret_cast<volatile int*>(zero) = 1;
            }
            mesh(j, d, a);
        };
        const auto batch = parallelMesh(jobs, kDefl, kAng, options);
        const bool ok = batch.path == ParallelMeshPath::Pooled &&
            batch.results.size() == 4 && batch.completedJobs == 4 &&
            batch.results[0].completed && !batch.results[0].ok &&
            batch.results[1].completed && batch.results[1].ok &&
            batch.results[2].completed && batch.results[2].ok &&
            batch.results[3].completed && batch.results[3].ok;
        std::_Exit(ok ? 0 : 1);
    }, ::testing::ExitedWithCode(0), "")
        << "a hardware fault in one job must be contained, not crash the "
           "process, and must not stop the worker from draining the "
           "remaining jobs";
}

// 10. The returned thread has a tail outside the pool's worker function.
// Seeing that tail here proves the join, not merely a worker counter of zero.
TEST(ParallelMesh, PartialLaunchJoinsBeforeFallback) {
    ProjectFile file;
    Application app;
    std::atomic<bool> exited{false};
    int launches = 0;
    Access::setup(app) = [&](auto& options) {
        options.workerCount = 2;
        options.beforeDequeue = [] { throw std::runtime_error("unreached"); };
        options.launch = [&](auto worker) -> std::thread {
            if (++launches == 2) throw std::runtime_error("launch");
            return std::thread([&, worker] {
                worker();
                std::this_thread::sleep_for(Ms(20));
                exited = true;
            });
        };
    };
    Access::before(app) = [&](const auto& batch) {
        EXPECT_TRUE(exited) << "started thread must be joined before bookkeeping/fallback";
        EXPECT_EQ(batch.path, ParallelMeshPath::Sequential);
        EXPECT_EQ(batch.threadsStarted, 1u);
    };
    ASSERT_TRUE(Access::load(app, file.path.string()));
    ASSERT_TRUE(exited);
    EXPECT_EQ(upload(app), 4);
    for (auto j : jobsFrom(Access::doc(app))) EXPECT_EQ(countUnmeshedFaces(j.shape), 0);
}

// 11. No completed jobs yet: the callback releases workers only after several
// polls, so Cancel/open-frame coverage cannot pass without a live pump.
TEST(ParallelMesh, OutstandingJobsKeepPollingWithoutZeroFraction) {
    for (bool frameOpen : {false, true}) {
        ParallelMeshOptions options;
        DrawThrottle throttle;
        auto now = Clock::time_point{};
        std::atomic<bool> release{false};
        int polls = 0, draws = 0, ticks = 0;
        options.workerCount = 2;
        options.mesh = [&](const auto& job, float d, float a) {
            const auto deadline = Clock::now() + Ms(500);
            while (!release && Clock::now() < deadline) std::this_thread::sleep_for(Ms(1));
            EXPECT_TRUE(release);
            mesh(job, d, a);
        };
        options.onTick = [&](size_t done, size_t total) {
            ++ticks;
            const float frac = parallelMeshFraction(done, total);
            if (done == 0) EXPECT_EQ(frac, -1.0f);
            EXPECT_NE(frac, 0.0f);
            pumpStep(throttle, progressFrameWouldDraw(frameOpen, true, true, frac),
                     [&] { return now; }, [&] { ++draws; return now; },
                     [&] { ++polls; now += Ms(10); });
            if (ticks >= 3) release = true;
        };
        auto batch = parallelMesh(boxes(), kDefl, kAng, options);
        EXPECT_EQ(batch.completedJobs, 4u);
        EXPECT_GE(polls, 3);
        EXPECT_EQ(polls, ticks);
        EXPECT_EQ(draws, 0);
    }
}

// 12. No deferred flag can leak from a load into a subsequent full rebuild.
TEST(ParallelMesh, NonLoadRebuildNeverInvokesPool) {
    ProjectFile file;
    Application app;
    auto before = parallelMeshCallsForTest();
    Access::doc(app).addBody(BRepPrimAPI_MakeBox(2, 3, 4).Shape(), "Existing");
    Access::rebuild(app);
    EXPECT_EQ(parallelMeshCallsForTest() - before, 0u);
    Access::loadWithProgress(app, file.path.string());
    EXPECT_EQ(parallelMeshCallsForTest() - before, 1u);
    EXPECT_FALSE(Access::pumping(app));
    before = parallelMeshCallsForTest();
    Access::rebuild(app);
    EXPECT_EQ(parallelMeshCallsForTest() - before, 0u);
    EXPECT_FALSE(Access::load(app, ""));
    Access::rebuild(app);
    EXPECT_EQ(parallelMeshCallsForTest() - before, 0u);
    Access::setup(app) = [](auto&) { throw std::runtime_error("load setup"); };
    EXPECT_THROW(Access::loadWithProgress(app, file.path.string()), std::runtime_error);
    EXPECT_FALSE(Access::pumping(app));
}

// 13. The provisional sample biases the first edit toward the worker. Only
// an adopted result corrects it; stale results keep the old estimate.
TEST(ParallelMesh, DurationsFeedDispatchAndSelfCorrect) {
    ProjectFile file;
    Application app;
    Access::setup(app) = [](auto& options) {
        options.mesh = [](const auto& j, float d, float a) {
            std::this_thread::sleep_for(Ms(25));
            mesh(j, d, a);
        };
    };
    int samples = 0;
    Access::after(app) = [&](const auto& batch) {
        auto jobs = jobsFrom(Access::doc(app));
        for (size_t i = 0; i < jobs.size(); ++i) {
            ASSERT_TRUE(batch.results[i].ok);
            EXPECT_GT(batch.results[i].millis, MeshDispatch::kAsyncMeshMs);
            ++samples;
            MeshRequest request{jobs[i].shape.TShape().get(), kDefl, kAng};
            auto& dispatch = Access::dispatch(app);
            EXPECT_EQ(dispatch.samplesForTest(jobs[i].bodyId), 1);
            EXPECT_EQ(dispatch.decide(jobs[i].bodyId, request), MeshPath::Worker);
            dispatch.requested(jobs[i].bodyId, request);
            dispatch.finished(jobs[i].bodyId, request, 1.0, false);
            EXPECT_EQ(dispatch.decide(jobs[i].bodyId, request), MeshPath::Worker);
            dispatch.requested(jobs[i].bodyId, request);
            dispatch.finished(jobs[i].bodyId, request, 1.0, true);
            request.deflection *= 0.5f; // next edit, not the already-landed request
            EXPECT_EQ(dispatch.decide(jobs[i].bodyId, request), MeshPath::InFrame);
        }
    };
    ASSERT_TRUE(Access::load(app, file.path.string()));
    EXPECT_EQ(samples, 4);
    EXPECT_EQ(upload(app), 0);
    for (auto& j : jobsFrom(Access::doc(app))) {
        const MeshRequest edit{j.shape.TShape().get(), kDefl * 0.5f, kAng};
        EXPECT_EQ(Access::dispatch(app).decide(j.bodyId, edit), MeshPath::InFrame);
        EXPECT_EQ(Access::dispatch(app).samplesForTest(j.bodyId), 1);
    }
    // The decide() checks above are what prove the dispatch behaviour; this
    // is only a sanity check on the fixture's own premise (that an isolated
    // small box is genuinely fast, the reason it takes the InFrame path at
    // all). A generous ceiling, not the real threshold, so it does not flake
    // under CI/system load - if a small box ever regressed to this cost,
    // something is badly wrong regardless of the exact number.
    auto isolated = boxes(1);
    const auto start = Clock::now();
    BRepMesh_IncrementalMesh mesher(isolated[0].shape, meshParams(kDefl, kAng, true));
    EXPECT_LT((std::chrono::duration<double, std::milli>(Clock::now() - start).count()),
              MeshDispatch::kAsyncMeshMs * 20.0);
}

// 14. Removing the ACTUAL load stamp makes the upload call the mesher N
// times. The mutation runner also exercises tests 1, 3, 8, and 9 above.
TEST(ParallelMesh, SuccessfulLoadSkipsEveryUploadMeshCall) {
    ProjectFile file;
    Application app;
    Access::after(app) = [](const auto& batch) {
        EXPECT_EQ(batch.path, ParallelMeshPath::Pooled);
        EXPECT_EQ(batch.completedJobs, 4u);
        for (const auto& result : batch.results) EXPECT_TRUE(result.ok);
    };
    ASSERT_TRUE(Access::load(app, file.path.string()));
    EXPECT_EQ(upload(app), 0) << "successful pooled jobs must skip all upload mesh calls";
}
#else
TEST(ParallelMesh, PlatformFallsBackWithoutLaunching) {
    const auto batch = parallelMesh(boxes(), kDefl, kAng);
    EXPECT_EQ(batch.path, ParallelMeshPath::Sequential);
    EXPECT_EQ(batch.threadsStarted, 0u);
    EXPECT_STREQ(batch.reason, "platform");
}
#endif
