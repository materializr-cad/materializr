// Patch (Tier 1 + 2): an N-sided surface fitted across a ring of edges, held
// at position (C0), tangent (G1) or curvature-continuous (G2) to the faces the
// edges came from, and sewn back into the body so the void is actually filled.
//
// The discriminating test is TangencyDomesTheCapOnALeaningWall: one rim, one
// solver, only the continuity changed. Position has no reason to leave the
// plane of the rim, so it lands as a flat disc. Tangent has to leave the rim
// along the wall, which no flat disc can do, so it domes. If the continuity
// were being dropped on the floor — as it silently is when the enumerator is
// spelled the way the kernel documents it — those two areas would match.
//
// PerpendicularWallsDegradeToPositionAndSaySo pins the other half: where the
// solver genuinely cannot deliver tangency, the void is still filled and the op
// reports that it fell back, rather than handing over a flat patch labelled
// tangent.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/History.h"
#include "modeling/PatchOp.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

#include <cmath>
#include <vector>

namespace {

double vol(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

double surfaceArea(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::SurfaceProperties(s, g);
    return g.Mass();
}

// Every face of `solid` except the one whose centre is highest / lowest along
// Z, sewn back into an open shell — i.e. a body with a hole in it, which is
// what this tool exists to repair.
TopoDS_Shape shellWithFaceRemoved(const TopoDS_Shape& solid, bool removeTop) {
    TopoDS_Face victim;
    double best = removeTop ? -1e30 : 1e30;
    for (TopExp_Explorer ex(solid, TopAbs_FACE); ex.More(); ex.Next()) {
        GProp_GProps g;
        BRepGProp::SurfaceProperties(ex.Current(), g);
        const double z = g.CentreOfMass().Z();
        if (removeTop ? (z > best) : (z < best)) {
            best = z;
            victim = TopoDS::Face(ex.Current());
        }
    }
    BRepBuilderAPI_Sewing sew(1e-6);
    for (TopExp_Explorer ex(solid, TopAbs_FACE); ex.More(); ex.Next())
        if (!ex.Current().IsSame(victim)) sew.Add(ex.Current());
    sew.Perform();
    return sew.SewedShape();
}

// The open boundary of `shape`: edges carried by exactly one face.
std::vector<TopoDS_Edge> freeEdges(const TopoDS_Shape& shape) {
    TopTools_IndexedDataMapOfShapeListOfShape anc;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, anc);
    std::vector<TopoDS_Edge> out;
    for (int i = 1; i <= anc.Extent(); ++i) {
        const TopoDS_Edge& e = TopoDS::Edge(anc.FindKey(i));
        if (BRep_Tool::Degenerated(e)) continue;
        if (anc.FindFromIndex(i).Extent() == 1) out.push_back(e);
    }
    return out;
}

} // namespace

// ── Tier 1: fill the void ───────────────────────────────────────────────────

TEST(Patch, FlatLidClosesTheBoxBackToItsOriginalVolume) {
    Document doc;
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    const TopoDS_Shape open = shellWithFaceRemoved(box, /*removeTop=*/true);
    const int id = doc.addBody(open, "Open");
    const std::vector<TopoDS_Edge> rim = freeEdges(open);
    ASSERT_EQ(rim.size(), 4u) << "a box missing one face has a four-edge rim";

    PatchOp op;
    op.setBody(id);
    op.setEdges(rim);
    op.setContinuity(PatchOp::Continuity::Position);
    ASSERT_TRUE(op.execute(doc));

    EXPECT_TRUE(op.healedIntoBody()) << "the patch should be sewn in, not "
                                        "added as a loose surface";
    EXPECT_NEAR(vol(doc.getBody(id)), 1000.0, 1.0);
    EXPECT_EQ(freeEdges(doc.getBody(id)).size(), 0u) << "no hole left";
    EXPECT_LT(op.g0Error(), 1e-3);
}

TEST(Patch, UndoPutsTheHoleBack) {
    Document doc;
    const TopoDS_Shape open =
        shellWithFaceRemoved(BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape(), true);
    const int id = doc.addBody(open, "Open");

    PatchOp op;
    op.setBody(id);
    op.setEdges(freeEdges(open));
    op.setContinuity(PatchOp::Continuity::Position);
    ASSERT_TRUE(op.execute(doc));
    ASSERT_EQ(freeEdges(doc.getBody(id)).size(), 0u);

    ASSERT_TRUE(op.undo(doc));
    EXPECT_EQ(freeEdges(doc.getBody(id)).size(), 4u);
}

// ── Tier 2: the continuity actually reaches the solver ──────────────────────

TEST(Patch, TangencyDomesTheCapOnALeaningWall) {
    // A cone frustum, lid removed. The wall leans ~11 degrees off vertical, so
    // a tangent cap has to leave the rim at that angle and dome — it cannot be
    // the flat disc that satisfies position alone.
    const double r1 = 5.0, r2 = 3.0, h = 10.0;
    const double flatArea = M_PI * r2 * r2;

    auto cap = [&](PatchOp::Continuity c, double& outArea, double& outG1,
                   bool& outAchieved) {
        Document doc;
        const TopoDS_Shape open = shellWithFaceRemoved(
            BRepPrimAPI_MakeCone(r1, r2, h).Shape(), /*removeTop=*/true);
        const int id = doc.addBody(open, "OpenCone");
        PatchOp op;
        op.setBody(id);
        op.setEdges(freeEdges(open));
        op.setContinuity(c);
        const bool ok = op.execute(doc);
        outArea = ok ? surfaceArea(op.patchFace()) : -1.0;
        outG1 = op.g1Error();
        outAchieved = op.continuityAchieved();
        EXPECT_EQ(op.unsupportedEdgeCount(), 0)
            << "the rim sits on the conical wall, so it has a support to blend into";
        return ok;
    };

    double flat = 0, domed = 0, g1flat = 0, g1domed = 0;
    bool achFlat = false, achDomed = false;
    ASSERT_TRUE(cap(PatchOp::Continuity::Position, flat, g1flat, achFlat));
    ASSERT_TRUE(cap(PatchOp::Continuity::Tangent, domed, g1domed, achDomed));

    EXPECT_NEAR(flat, flatArea, flatArea * 0.02) << "position-only stays a disc";
    EXPECT_TRUE(achDomed) << "an 11-degree wall is well inside the solver's reach";
    EXPECT_LT(g1domed, 1e-3) << "tangency error, radians";
    EXPECT_GT(domed, flatArea * 1.5) << "a tangent cap has to bulge";
}

TEST(Patch, CurvatureContinuityIsAskedForWithTheRightEnumerator) {
    // GeomAbs_G2 is the documented spelling and the one that throws; the op has
    // to send the enumerator whose ordinal is 2. If that regressed, every
    // curvature fit would fall down the ladder to position-only and the cap
    // would come back flat.
    Document doc;
    const TopoDS_Shape open = shellWithFaceRemoved(
        BRepPrimAPI_MakeCone(5.0, 3.0, 10.0).Shape(), true);
    const int id = doc.addBody(open, "OpenCone");

    PatchOp op;
    op.setBody(id);
    op.setEdges(freeEdges(open));
    op.setContinuity(PatchOp::Continuity::Curvature);
    ASSERT_TRUE(op.execute(doc));
    EXPECT_LT(op.g1Error(), 0.05) << "a curvature fit is tangent too";
    EXPECT_GT(surfaceArea(op.patchFace()), M_PI * 9.0 * 1.5);
}

TEST(Patch, PerpendicularWallsDegradeToPositionAndSaySo) {
    // A flat lid on vertical walls: the tangent solution would have to leave the
    // rim vertically and balloon without limit, and GeomPlate discards the
    // constraint rather than chasing it. What must NOT happen is a flat patch
    // reported as tangent, or a corrupted body.
    Document doc;
    const TopoDS_Shape open =
        shellWithFaceRemoved(BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape(), true);
    const int id = doc.addBody(open, "Open");

    PatchOp op;
    op.setBody(id);
    op.setEdges(freeEdges(open));
    op.setContinuity(PatchOp::Continuity::Tangent);
    ASSERT_TRUE(op.execute(doc)) << "the void still gets filled";
    EXPECT_FALSE(op.continuityAchieved()) << "and the op admits the tangency failed";
    EXPECT_TRUE(op.healedIntoBody());
    EXPECT_EQ(freeEdges(doc.getBody(id)).size(), 0u);
    EXPECT_NEAR(vol(doc.getBody(id)), 1000.0, 1.0);
}

// ── Solver knobs are wired, not decorative ──────────────────────────────────

TEST(Patch, SamplesPerCurveChangeTheFit) {
    auto fitError = [](int nbPts) {
        Document doc;
        const TopoDS_Shape open = shellWithFaceRemoved(
            BRepPrimAPI_MakeCone(5.0, 3.0, 10.0).Shape(), true);
        const int id = doc.addBody(open, "OpenCone");
        PatchOp op;
        op.setBody(id);
        op.setEdges(freeEdges(open));
        op.setContinuity(PatchOp::Continuity::Tangent);
        PatchOp::Solver s;
        s.nbPtsOnCur = nbPts;
        op.setSolver(s);
        return op.execute(doc) ? op.g0Error() : -1.0;
    };
    const double coarse = fitError(5);
    const double fine = fitError(30);
    ASSERT_GE(coarse, 0.0);
    ASSERT_GE(fine, 0.0);
    EXPECT_NE(coarse, fine) << "nbPtsOnCur reached the solver";
}

// ── Standalone patch (no body to heal) ──────────────────────────────────────

TEST(Patch, LooseRingBecomesItsOwnSurfaceBody) {
    Document doc;
    const gp_Pnt a(0, 0, 0), b(10, 0, 0), c(10, 10, 3), d(0, 10, 3);
    std::vector<TopoDS_Edge> ring = {
        BRepBuilderAPI_MakeEdge(a, b), BRepBuilderAPI_MakeEdge(b, c),
        BRepBuilderAPI_MakeEdge(c, d), BRepBuilderAPI_MakeEdge(d, a)};

    PatchOp op;
    op.setEdges(ring);          // no body
    op.setContinuity(PatchOp::Continuity::Position);
    ASSERT_TRUE(op.execute(doc));
    EXPECT_FALSE(op.healedIntoBody());
    EXPECT_EQ(doc.getAllBodyIds().size(), 1u);

    const OperationDiff diff = op.captureDiff();
    EXPECT_EQ(diff.created.size(), 1u);
    EXPECT_TRUE(diff.modifiedBefore.empty());

    ASSERT_TRUE(op.undo(doc));
    EXPECT_TRUE(doc.getAllBodyIds().empty());
}

// ── Round trip ──────────────────────────────────────────────────────────────

TEST(Patch, ParamsRoundTrip) {
    Document doc;
    const TopoDS_Shape open = shellWithFaceRemoved(
        BRepPrimAPI_MakeCone(5.0, 3.0, 10.0).Shape(), true);
    const int id = doc.addBody(open, "OpenCone");

    PatchOp op;
    op.setBody(id);
    op.setEdges(freeEdges(open));
    op.setContinuity(PatchOp::Continuity::Curvature);
    PatchOp::Solver s;
    s.nbPtsOnCur = 22;
    s.maxDeg = 11;
    s.tol3d = 5e-4;
    op.setSolver(s);
    ASSERT_TRUE(op.execute(doc));

    const std::string blob = op.serializeParams();
    PatchOp back;
    ASSERT_TRUE(back.deserializeParams(blob));
    EXPECT_EQ(back.getBodyId(), id);
    EXPECT_EQ(back.edgeCount(), op.edgeCount());
    EXPECT_EQ(static_cast<int>(back.continuity()),
              static_cast<int>(PatchOp::Continuity::Curvature));
    EXPECT_EQ(back.solver().nbPtsOnCur, 22);
    EXPECT_EQ(back.solver().maxDeg, 11);
    EXPECT_NEAR(back.solver().tol3d, 5e-4, 1e-12);

    // A reloaded patch re-fits against the live body rather than replaying a
    // baked shape.
    Document doc2;
    const int id2 = doc2.addBody(open, "Open");
    ASSERT_EQ(id2, id) << "fixture assumption: same first body id";
    EXPECT_TRUE(back.execute(doc2));
    EXPECT_EQ(freeEdges(doc2.getBody(id2)).size(), 0u);
}

TEST(Patch, RefusesAGarbageBlob) {
    PatchOp op;
    EXPECT_FALSE(op.deserializeParams(""));
    EXPECT_FALSE(op.deserializeParams("body=0;ne=-1;brep=4:junk"));
    EXPECT_FALSE(op.deserializeParams("body=0;ne=2;ns=0;brep=99999999999999:x"));
    EXPECT_FALSE(op.deserializeParams("body=0;ne=2;ns=0;t3d=nan;brep=2:xx"));
}

// ── Why a patch didn't join the body ───────────────────────────────────────
//
// Three different situations hand the user the same loose surface, and they
// want three different things done next. The one that reads as the tool
// ignoring you is a hole that goes right THROUGH: capping one end cannot close
// the body, so the patch correctly refuses to sew — and used to say nothing at
// all about why.

TEST(Patch, AClosedBodyReportsThatThereIsNothingToSewInto) {
    Document doc;
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    const int id = doc.addBody(box, "Box");

    // The four top edges of an intact solid: a perfectly good ring, on a body
    // with no opening anywhere.
    const TopoDS_Face top = [&] {
        TopoDS_Face best;
        double bestZ = -1e30;
        for (TopExp_Explorer ex(box, TopAbs_FACE); ex.More(); ex.Next()) {
            GProp_GProps g;
            BRepGProp::SurfaceProperties(ex.Current(), g);
            if (g.CentreOfMass().Z() > bestZ) { bestZ = g.CentreOfMass().Z(); best = TopoDS::Face(ex.Current()); }
        }
        return best;
    }();
    std::vector<TopoDS_Edge> ring;
    for (TopExp_Explorer ex(top, TopAbs_EDGE); ex.More(); ex.Next())
        ring.push_back(TopoDS::Edge(ex.Current()));
    ASSERT_EQ(ring.size(), 4u);

    PatchOp op;
    op.setBody(id);
    op.setEdges(ring);
    op.setContinuity(PatchOp::Continuity::Position);
    ASSERT_TRUE(op.execute(doc)) << "the surface still fits";

    EXPECT_FALSE(op.healedIntoBody());
    EXPECT_EQ(op.healOutcome(), PatchOp::Heal::BodyIsClosed)
        << "and the panel can now say WHY, instead of dropping a silent extra body";
    EXPECT_EQ(doc.getAllBodyIds().size(), 2u) << "the patch is its own surface";
    EXPECT_NEAR(vol(doc.getBody(id)), 1000.0, 1e-6) << "the solid is untouched";
}

TEST(Patch, HealingReportsItself) {
    Document doc;
    const TopoDS_Shape open =
        shellWithFaceRemoved(BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape(), true);
    const int id = doc.addBody(open, "Open");

    PatchOp op;
    op.setBody(id);
    op.setEdges(freeEdges(open));
    op.setContinuity(PatchOp::Continuity::Position);
    ASSERT_TRUE(op.execute(doc));
    EXPECT_EQ(op.healOutcome(), PatchOp::Heal::Sewn);
}

TEST(Patch, EdgesFromNoBodyReportNoSingleBody) {
    Document doc;
    const gp_Pnt a(0, 0, 0), b(10, 0, 0), c(10, 10, 3), d(0, 10, 3);
    PatchOp op;
    op.setEdges({BRepBuilderAPI_MakeEdge(a, b), BRepBuilderAPI_MakeEdge(b, c),
                 BRepBuilderAPI_MakeEdge(c, d), BRepBuilderAPI_MakeEdge(d, a)});
    op.setContinuity(PatchOp::Continuity::Position);
    ASSERT_TRUE(op.execute(doc));
    EXPECT_EQ(op.healOutcome(), PatchOp::Heal::NoSingleBody);
}
