// Deadline guard for OCCT's uninterruptible fillet builder.
//
// BRepFilletAPI_MakeFillet::Build() cannot be cancelled — it delegates to
// ChFi3d_Builder::Compute(), which takes no Message_ProgressRange — so a radius
// OCCT cannot resolve wedges the calling thread forever. Observed live: the
// render loop spun at 100% CPU inside ChFi3d_Builder::StoreData with no
// progress, and the app had to be killed. probe() exists so the synchronous
// build is never entered without evidence it terminates.

#include "modeling/FilletProbe.h"

#include <gtest/gtest.h>

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>

#include <chrono>
#include <vector>

namespace {

TopoDS_Shape box(double dx, double dy, double dz) {
    return BRepPrimAPI_MakeBox(dx, dy, dz).Shape();
}

std::vector<TopoDS_Edge> firstEdge(const TopoDS_Shape& s) {
    for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next())
        return { TopoDS::Edge(ex.Current()) };
    return {};
}

double secondsOf(const std::function<void()>& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
        .count();
}

} // namespace

// A fillet OCCT can build must be reported buildable. If this ever fails, the
// guard has become a blanket refusal and every fillet is silently broken.
TEST(FilletProbe, AcceptsABuildableFillet) {
    materializr::fillet::clearProbeCache();
    const TopoDS_Shape s = box(20, 20, 20);
    const auto edges = firstEdge(s);
    ASSERT_FALSE(edges.empty());
    EXPECT_TRUE(materializr::fillet::probe(s, edges, 2.0));
}

// Degenerate inputs must be refused without launching a worker at all.
TEST(FilletProbe, RefusesDegenerateInput) {
    materializr::fillet::clearProbeCache();
    const TopoDS_Shape s = box(20, 20, 20);
    const auto edges = firstEdge(s);
    ASSERT_FALSE(edges.empty());
    EXPECT_FALSE(materializr::fillet::probe(s, edges, 0.0));
    EXPECT_FALSE(materializr::fillet::probe(s, edges, -1.0));
    EXPECT_FALSE(materializr::fillet::probe(s, {}, 2.0));
    EXPECT_FALSE(materializr::fillet::probe(TopoDS_Shape(), edges, 2.0));
}

// A radius the geometry cannot take (larger than the box) must come back false
// rather than hanging. This is the guard's whole purpose: bounded time, always.
TEST(FilletProbe, ImpossibleRadiusReturnsWithinBudget) {
    materializr::fillet::clearProbeCache();
    const TopoDS_Shape s = box(20, 20, 20);
    const auto edges = firstEdge(s);
    ASSERT_FALSE(edges.empty());
    bool ok = true;
    const double secs = secondsOf([&] {
        ok = materializr::fillet::probe(s, edges, 500.0, 1.0);
    });
    EXPECT_FALSE(ok);
    // Whether OCCT refuses quickly or has to be abandoned at the deadline, the
    // CALLER must be released on schedule. Generous slack for CI scheduling.
    EXPECT_LT(secs, 5.0);
}

// The budget must be honoured even when the build genuinely cannot finish. A
// zero budget is the sharpest form of the question: give up immediately.
TEST(FilletProbe, ZeroBudgetGivesUpImmediately) {
    materializr::fillet::clearProbeCache();
    const TopoDS_Shape s = box(20, 20, 20);
    const auto edges = firstEdge(s);
    ASSERT_FALSE(edges.empty());
    bool ok = true;
    const double secs = secondsOf([&] {
        ok = materializr::fillet::probe(s, edges, 2.0, 0.0);
    });
    EXPECT_FALSE(ok);
    EXPECT_LT(secs, 2.0);
}

// An interactive preview re-asks the identical question every frame. Without
// memoisation the guard would double every fillet's cost forever — the probe
// must answer a repeat query from cache, not by rebuilding.
//
// Asserted by RUN COUNT. Timing cannot tell a cache hit from a fast build: a
// mutation removing edge identity from the key sailed through a timing-based
// version of this test.
TEST(FilletProbe, RepeatQueryIsMemoised) {
    materializr::fillet::clearProbeCache();
    const TopoDS_Shape s = box(20, 20, 20);
    const auto edges = firstEdge(s);
    ASSERT_FALSE(edges.empty());

    const auto before = materializr::fillet::probeRunCount();
    EXPECT_TRUE(materializr::fillet::probe(s, edges, 2.0));
    EXPECT_EQ(before + 1, materializr::fillet::probeRunCount()) << "cold miss";

    EXPECT_TRUE(materializr::fillet::probe(s, edges, 2.0));
    EXPECT_EQ(before + 1, materializr::fillet::probeRunCount())
        << "repeat query ran a second build instead of using the cache";
}

// The budget is a user setting, so it must actually take effect — and be
// clamped, since a zero or negative value would turn every fillet into an
// instant refusal and a huge one would restore the freeze it exists to stop.
TEST(FilletProbe, BudgetIsConfigurableAndClamped) {
    const double original = materializr::fillet::probeBudget();

    materializr::fillet::setProbeBudget(10.0);
    EXPECT_DOUBLE_EQ(10.0, materializr::fillet::probeBudget());

    // Below the floor clamps up, not down to zero.
    materializr::fillet::setProbeBudget(0.01);
    EXPECT_DOUBLE_EQ(0.25, materializr::fillet::probeBudget());

    // Above the ceiling clamps down — 10 minutes is indistinguishable from the
    // hang this guards against.
    materializr::fillet::setProbeBudget(600.0);
    EXPECT_DOUBLE_EQ(30.0, materializr::fillet::probeBudget());

    // Nonsense falls back to the default rather than to zero.
    materializr::fillet::setProbeBudget(0.0);
    EXPECT_DOUBLE_EQ(materializr::fillet::kDefaultProbeSeconds,
                     materializr::fillet::probeBudget());
    materializr::fillet::setProbeBudget(-5.0);
    EXPECT_DOUBLE_EQ(materializr::fillet::kDefaultProbeSeconds,
                     materializr::fillet::probeBudget());

    materializr::fillet::setProbeBudget(original);
}

// Changing the budget must drop cached verdicts. A refusal reached under a
// tight budget means only "not in that long" — if it survived a budget increase
// the setting would look inert on exactly the fillets it was raised for.
TEST(FilletProbe, ChangingBudgetInvalidatesCache) {
    const double original = materializr::fillet::probeBudget();
    materializr::fillet::clearProbeCache();

    const TopoDS_Shape s = box(20, 20, 20);
    const auto edges = firstEdge(s);
    ASSERT_FALSE(edges.empty());

    // Refuse it with a budget nothing can meet, so a "false" is now cached.
    EXPECT_FALSE(materializr::fillet::probe(s, edges, 2.0, 0.0));

    // With room to work, the SAME fillet must come back buildable. A stale
    // cache — or a key that ignored the budget — would return false here.
    materializr::fillet::setProbeBudget(5.0);
    EXPECT_TRUE(materializr::fillet::probe(s, edges, 2.0));

    materializr::fillet::setProbeBudget(original);
}

// Two DIFFERENT edges of the same body, same count, same radius, are different
// questions. Answering one with the other's cached verdict would hand an
// unprobed input straight to the uninterruptible Build() this guards.
//
// Asserted by OUTCOME, not timing: a cache hit still takes a nonzero number of
// seconds, so "was it fast?" proves nothing. Instead this finds two edges of one
// body that genuinely DISAGREE at a single radius, and checks the second is not
// served the first's answer.
TEST(FilletProbe, DistinctEdgesAreDistinctCacheEntries) {
    // A thin plate: a radius larger than the thickness cannot be blended onto a
    // face edge, but the upright corner edges have material to spare.
    const TopoDS_Shape s = box(20, 20, 1);
    const double r = 1.5;

    std::vector<TopoDS_Edge> all;
    for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next())
        all.push_back(TopoDS::Edge(ex.Current()));
    ASSERT_GE(all.size(), 2u);

    // Classify each edge on its own, with a clean cache each time.
    TopoDS_Edge yes, no;
    bool haveYes = false, haveNo = false;
    for (const auto& e : all) {
        materializr::fillet::clearProbeCache();
        const std::vector<TopoDS_Edge> one{ e };
        if (materializr::fillet::probe(s, one, r)) {
            if (!haveYes) { yes = e; haveYes = true; }
        } else {
            if (!haveNo) { no = e; haveNo = true; }
        }
        if (haveYes && haveNo) break;
    }
    ASSERT_TRUE(haveYes && haveNo)
        << "need one buildable and one unbuildable edge at R=" << r
        << " to tell the cache entries apart";

    // Cache the positive verdict, then ask the negative question. A key that
    // only counted edges would hand back the cached true.
    materializr::fillet::clearProbeCache();
    EXPECT_TRUE(materializr::fillet::probe(s, { yes }, r));
    EXPECT_FALSE(materializr::fillet::probe(s, { no }, r))
        << "an unbuildable edge was served a different edge's cached verdict";
}

// The cache pins the TShape each verdict was keyed on. Without that pin a freed
// shape's address could be reused by unrelated geometry and serve a stale
// "converges" for it — waving through exactly the build this guard exists to
// refuse. Probing many short-lived shapes must stay correct, and must not grow
// without bound.
TEST(FilletProbe, ManyShortLivedShapesStayCorrectAndBounded) {
    materializr::fillet::clearProbeCache();
    // Far more distinct shapes than the internal cap, each destroyed before the
    // next is built, which is what makes address reuse likely.
    for (int i = 0; i < 600; ++i) {
        const TopoDS_Shape s = box(10 + i * 0.01, 10, 10);
        const auto edges = firstEdge(s);
        ASSERT_FALSE(edges.empty());
        // Every one of these is genuinely filletable; a stale verdict from a
        // recycled address would show up as a wrong answer here.
        EXPECT_TRUE(materializr::fillet::probe(s, edges, 1.0)) << "i=" << i;
    }
}

// Clearing must actually clear, or a stale verdict outlives the body it
// described and refuses (or permits) the wrong build after an edit.
TEST(FilletProbe, ClearDropsCachedVerdicts) {
    materializr::fillet::clearProbeCache();
    const TopoDS_Shape s = box(20, 20, 20);
    const auto edges = firstEdge(s);
    ASSERT_FALSE(edges.empty());

    EXPECT_TRUE(materializr::fillet::probe(s, edges, 2.0));
    const auto afterFirst = materializr::fillet::probeRunCount();

    materializr::fillet::clearProbeCache();
    EXPECT_TRUE(materializr::fillet::probe(s, edges, 2.0));
    EXPECT_EQ(afterFirst + 1, materializr::fillet::probeRunCount())
        << "verdict survived clearProbeCache()";
}
