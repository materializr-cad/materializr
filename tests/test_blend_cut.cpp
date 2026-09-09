// #55: a chamfer applied to an edge that a surface feature (drilled hole)
// already crosses. OCCT's native blend fails there; the swept-wedge cut
// fallback must produce EXACTLY the geometry of "chamfer first, feature
// after" - the reorder the user would otherwise rebuild by hand. Also checks
// the fallback refuses what it can't honestly build (concave edges) and that
// the native path still handles a plain box untouched.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/History.h"
#include "modeling/BlendCut.h"
#include "modeling/ChamferOp.h"
#include "modeling/FilletOp.h"
#include "modeling/GenerationLedger.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pnt.hxx>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace materializr;

namespace {

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

// Count coplanar-adjacent face pairs: two planar faces in the SAME plane that
// share an edge. The surface is geometrically flat there (a height map sees
// nothing), but the viewport draws the shared edge as a boundary line - this
// is exactly the "fan of seams across the ramp" artifact. The right metric
// for that class of bug; must be zero on a clean blend.
int coplanarSeamPairs(const TopoDS_Shape& body) {
    auto key = [](const TopoDS_Face& f, bool& ok) -> std::string {
        BRepAdaptor_Surface s(f);
        if (s.GetType() != GeomAbs_Plane) { ok = false; return ""; }
        ok = true;
        gp_Pln p = s.Plane();
        gp_Dir n = p.Axis().Direction();
        double d = n.X() * p.Location().X() + n.Y() * p.Location().Y() +
                   n.Z() * p.Location().Z();
        if (n.X() < -1e-6 || (std::abs(n.X()) < 1e-6 && n.Y() < -1e-6) ||
            (std::abs(n.X()) < 1e-6 && std::abs(n.Y()) < 1e-6 && n.Z() < 0)) {
            n.Reverse();
            d = -d;
        }
        char b[96];
        std::snprintf(b, sizeof b, "%.2f,%.2f,%.2f,%.2f", n.X(), n.Y(), n.Z(), d);
        return b;
    };
    TopTools_IndexedDataMapOfShapeListOfShape e2f;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, e2f);
    int seams = 0;
    for (int i = 1; i <= e2f.Extent(); ++i) {
        const auto& fl = e2f.FindFromIndex(i);
        if (fl.Extent() != 2) continue;
        TopoDS_Face fa = TopoDS::Face(fl.First()), fb;
        for (const auto& f : fl)
            if (!f.IsSame(fa)) { fb = TopoDS::Face(f); break; }
        if (fb.IsNull()) continue;
        bool oa, ob;
        std::string ka = key(fa, oa), kb = key(fb, ob);
        if (oa && ob && ka == kb) seams++;
    }
    return seams;
}

// Box 40x20x10; the "front top" edge runs along X at y=0, z=10.
TopoDS_Shape plainBox() { return BRepPrimAPI_MakeBox(40.0, 20.0, 10.0).Shape(); }

// Vertical 3mm-radius hole centred ON the front top edge at x=20 - bites a
// half-cylinder channel through it, fragmenting the edge in two.
TopoDS_Shape edgeCrossingHole() {
    gp_Ax2 ax(gp_Pnt(20.0, 0.0, -1.0), gp_Dir(0.0, 0.0, 1.0));
    return BRepPrimAPI_MakeCylinder(ax, 3.0, 12.0).Shape();
}

// Shallow rectangular pocket crossing the edge, floor at z=9 - INSIDE the
// 2mm bevel depth. This is the config where the native blend genuinely
// fails on OCCT 7.9.3 (probe_chamfer_fail), so it exercises the fallback
// end-to-end through ChamferOp.
TopoDS_Shape shallowPocketTool() {
    return BRepPrimAPI_MakeBox(gp_Pnt(17.0, -1.0, 9.0),
                               gp_Pnt(23.0, 4.0, 11.0)).Shape();
}

// All straight edges whose vertices sit at y≈0, z≈10 (the front-top edge or
// its fragments after the hole).
std::vector<TopoDS_Edge> frontTopEdges(const TopoDS_Shape& s) {
    std::vector<TopoDS_Edge> out;
    for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next()) {
        const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
        if (BRepAdaptor_Curve(e).GetType() != GeomAbs_Line) continue;
        bool ok = true;
        int nv = 0;
        for (TopExp_Explorer vx(e, TopAbs_VERTEX); vx.More(); vx.Next(), ++nv) {
            gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()));
            if (std::abs(p.Y()) > 1e-7 || std::abs(p.Z() - 10.0) > 1e-7)
                ok = false;
        }
        if (ok && nv == 2) {
            bool dup = false;
            for (const auto& u : out)
                if (u.IsSame(e)) dup = true;
            if (!dup) out.push_back(e);
        }
    }
    return out;
}

// The planar face lying on z = 10 (the top).
TopoDS_Face topFace(const TopoDS_Shape& s) {
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face& f = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface surf(f);
        if (surf.GetType() != GeomAbs_Plane) continue;
        gp_Pln p = surf.Plane();
        if (p.Axis().Direction().IsParallel(gp_Dir(0, 0, 1), 1e-6) &&
            std::abs(p.Distance(gp_Pnt(0, 0, 10))) < 1e-7)
            return f;
    }
    return TopoDS_Face();
}

// The geometry the user actually wants: native chamfer on the PRISTINE box's
// edge (measured along the top face), then the feature cut through it.
double chamferThenCutVolume(const TopoDS_Shape& tool, double dTop,
                            double dOther) {
    TopoDS_Shape box = plainBox();
    std::vector<TopoDS_Edge> es = frontTopEdges(box);
    if (es.size() != 1) return -1.0;
    TopoDS_Face top = topFace(box);
    if (top.IsNull()) return -1.0;
    BRepFilletAPI_MakeChamfer mk(box);
    mk.Add(dTop, dOther, es.front(), top);
    mk.Build();
    if (!mk.IsDone()) return -1.0;
    return volumeOf(BRepAlgoAPI_Cut(mk.Shape(), tool).Shape());
}

// Fillet counterpart of chamferThenCutVolume: native fillet on the pristine
// box's edge, then the feature cut through it.
double filletThenCutVolume(const TopoDS_Shape& tool, double r) {
    TopoDS_Shape box = plainBox();
    std::vector<TopoDS_Edge> es = frontTopEdges(box);
    if (es.size() != 1) return -1.0;
    BRepFilletAPI_MakeFillet mk(box);
    mk.Add(r, es.front());
    mk.Build();
    if (!mk.IsDone()) return -1.0;
    return volumeOf(BRepAlgoAPI_Cut(mk.Shape(), tool).Shape());
}

} // namespace

// Native path untouched: a plain box chamfer must still build (and NOT via
// the fallback - the bevel of a native chamfer is reported by the builder,
// but the simplest regression-proof is that it succeeds and stays valid).
TEST(BlendCut, NativePathStillWorksOnPlainBox) {
    Document doc;
    int id = doc.addBody(plainBox(), "Box");
    std::vector<TopoDS_Edge> es = frontTopEdges(doc.getBody(id));
    ASSERT_EQ(es.size(), 1u);
    ChamferOp op;
    op.setBody(id);
    op.setEdges(es);
    op.setDistance(2.0);
    ASSERT_TRUE(op.execute(doc));
    const TopoDS_Shape& out = doc.getBody(id);
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());
    EXPECT_LT(volumeOf(out), volumeOf(plainBox()));
    EXPECT_FALSE(op.getGeneratedFaces().empty());
}

// The core contract: hole first, then cutChamfer over the two fragments =
// chamfer first, then hole. Same removal set, so the volumes are EQUAL.
TEST(BlendCut, SymmetricChamferAcrossHoleMatchesReorder) {
    TopoDS_Shape holed = BRepAlgoAPI_Cut(plainBox(), edgeCrossingHole()).Shape();
    std::vector<TopoDS_Edge> frags = frontTopEdges(holed);
    ASSERT_EQ(frags.size(), 2u) << "hole should fragment the edge in two";

    topo::GenerationLedger ledger;
    TopoDS_Shape out;
    std::vector<TopoDS_Shape> blends;
    ASSERT_TRUE(blendcut::cutChamfer(holed, frags, 2.0, 2.0, TopoDS_Face(),
                                     ledger, out, blends));
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());
    EXPECT_FALSE(blends.empty());

    double ref = chamferThenCutVolume(edgeCrossingHole(), 2.0, 2.0);
    ASSERT_GT(ref, 0.0);
    EXPECT_NEAR(volumeOf(out), ref, 1e-4);
}

// Same but asymmetric, with the setbacks aimed via the top face - including
// across the hole, where the reference face is the HOLED top (an inner wire,
// same plane).
TEST(BlendCut, AsymmetricChamferAcrossHoleMatchesReorder) {
    TopoDS_Shape holed = BRepAlgoAPI_Cut(plainBox(), edgeCrossingHole()).Shape();
    std::vector<TopoDS_Edge> frags = frontTopEdges(holed);
    ASSERT_EQ(frags.size(), 2u);
    TopoDS_Face top = topFace(holed);
    ASSERT_FALSE(top.IsNull());

    topo::GenerationLedger ledger;
    TopoDS_Shape out;
    std::vector<TopoDS_Shape> blends;
    ASSERT_TRUE(blendcut::cutChamfer(holed, frags, 3.0, 1.5, top,
                                     ledger, out, blends));
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());

    double ref = chamferThenCutVolume(edgeCrossingHole(), 3.0, 1.5);
    ASSERT_GT(ref, 0.0);
    EXPECT_NEAR(volumeOf(out), ref, 1e-4);
}

// Through the real op, on a config where the NATIVE build genuinely fails
// (probe_chamfer_fail: shallow pocket whose floor sits inside the bevel
// depth): ChamferOp must come back SUCCESSFUL with the reorder-equivalent
// geometry - the fallback wiring end-to-end: ledger, generated faces,
// lineage ids.
TEST(BlendCut, ChamferOpFallsBackAcrossShallowPocket) {
    Document doc;
    int id = doc.addBody(
        BRepAlgoAPI_Cut(plainBox(), shallowPocketTool()).Shape(), "Pocketed");
    std::vector<TopoDS_Edge> frags = frontTopEdges(doc.getBody(id));
    ASSERT_EQ(frags.size(), 2u);

    ChamferOp op;
    op.setBody(id);
    op.setEdges(frags);
    op.setDistance(2.0);
    ASSERT_TRUE(op.execute(doc));

    const TopoDS_Shape& out = doc.getBody(id);
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());
    double ref = chamferThenCutVolume(shallowPocketTool(), 2.0, 2.0);
    ASSERT_GT(ref, 0.0);
    EXPECT_NEAR(volumeOf(out), ref, 1e-4);

    // Click-to-edit machinery: bevel faces claimed, lineage ids minted.
    EXPECT_FALSE(op.getGeneratedFaces().empty());
    EXPECT_NE(op.serializeParams().find("genids="), std::string::npos);
    EXPECT_NE(doc.bodyFaceIds(id), nullptr);
}

// The other native-failing class: chamfer distance LARGER than the corner
// clearance to a through-hole (d=3.5 vs r=3 hole on the edge).
TEST(BlendCut, ChamferOpFallsBackWhenBevelExceedsClearance) {
    Document doc;
    int id = doc.addBody(
        BRepAlgoAPI_Cut(plainBox(), edgeCrossingHole()).Shape(), "Holed");
    std::vector<TopoDS_Edge> frags = frontTopEdges(doc.getBody(id));
    ASSERT_EQ(frags.size(), 2u);

    ChamferOp op;
    op.setBody(id);
    op.setEdges(frags);
    op.setDistance(3.5);
    ASSERT_TRUE(op.execute(doc));

    const TopoDS_Shape& out = doc.getBody(id);
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());
    double ref = chamferThenCutVolume(edgeCrossingHole(), 3.5, 3.5);
    ASSERT_GT(ref, 0.0);
    EXPECT_NEAR(volumeOf(out), ref, 1e-4);
}

// #56 / #59 follow-up: editing a FEATURE-CROSSED chamfer from history must
// REBUILD it (through the same fallback the fresh apply took) to the same
// geometry - not strand the step into a planar face. The graceful-revert of
// #56 is the safety net; this pins that the net shouldn't need to fire for a
// blend that the fallback CAN rebuild. editStep re-executes the op, so a
// stranded rebuild would show as a changed final volume.
TEST(BlendCut, EditFeatureCrossedChamferRebuildsNotStrands) {
    Document doc;
    History hist;
    int id = doc.addBody(
        BRepAlgoAPI_Cut(plainBox(), edgeCrossingHole()).Shape(), "Holed");
    std::vector<TopoDS_Edge> frags = frontTopEdges(doc.getBody(id));
    ASSERT_EQ(frags.size(), 2u);

    auto op = std::make_unique<ChamferOp>();
    op->setBody(id);
    op->setEdges(frags);
    op->setDistance(3.5);   // exceeds hole clearance → forces the fallback
    ASSERT_TRUE(hist.pushOperation(std::move(op), doc));

    const double vApplied = volumeOf(doc.getBody(id));
    double ref = chamferThenCutVolume(edgeCrossingHole(), 3.5, 3.5);
    ASSERT_GT(ref, 0.0);
    ASSERT_NEAR(vApplied, ref, 1e-4) << "setup: fresh apply took the fallback";

    // THE EDIT: re-execute the step with UNCHANGED params - the case that
    // used to collapse to a planar face on the second pass.
    ASSERT_TRUE(hist.editStep(0, doc, /*transactional=*/true))
        << "editStep reverted instead of rebuilding the feature-crossed blend";
    const TopoDS_Shape& out = doc.getBody(id);
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());
    EXPECT_NEAR(volumeOf(out), vApplied, 1e-4)
        << "edited chamfer did not reproduce its geometry (stranded/planar)";
    // The op must still own a live blend face (didn't degenerate to a plane).
    const auto* c = dynamic_cast<const ChamferOp*>(hist.getStep(0));
    ASSERT_NE(c, nullptr);
    EXPECT_FALSE(c->getGeneratedFaces().empty())
        << "no blend face after edit - collapsed to a planar face";
}

// Fillet flavour of the core contract: hole first, then cutFillet over the
// fragments = fillet first, then hole. The arc tool must leave exactly the
// fillet cylinder, so the volumes are EQUAL.
TEST(BlendCut, FilletAcrossHoleMatchesReorder) {
    TopoDS_Shape holed = BRepAlgoAPI_Cut(plainBox(), edgeCrossingHole()).Shape();
    std::vector<TopoDS_Edge> frags = frontTopEdges(holed);
    ASSERT_EQ(frags.size(), 2u);

    topo::GenerationLedger ledger;
    TopoDS_Shape out;
    std::vector<TopoDS_Shape> blends;
    ASSERT_TRUE(blendcut::cutFillet(holed, frags, 2.0, ledger, out, blends));
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());
    EXPECT_FALSE(blends.empty());

    double ref = filletThenCutVolume(edgeCrossingHole(), 2.0);
    ASSERT_GT(ref, 0.0);
    EXPECT_NEAR(volumeOf(out), ref, 1e-4);
}

// Through the real op on the native-failing shallow-pocket config: FilletOp
// must succeed via the fallback with reorder-equivalent geometry.
TEST(BlendCut, FilletOpFallsBackAcrossShallowPocket) {
    Document doc;
    int id = doc.addBody(
        BRepAlgoAPI_Cut(plainBox(), shallowPocketTool()).Shape(), "Pocketed");
    std::vector<TopoDS_Edge> frags = frontTopEdges(doc.getBody(id));
    ASSERT_EQ(frags.size(), 2u);

    FilletOp op;
    op.setBody(id);
    op.setEdges(frags);
    op.setRadius(2.0);
    ASSERT_TRUE(op.execute(doc));

    const TopoDS_Shape& out = doc.getBody(id);
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());
    double ref = filletThenCutVolume(shallowPocketTool(), 2.0);
    ASSERT_GT(ref, 0.0);
    EXPECT_NEAR(volumeOf(out), ref, 1e-4);
    EXPECT_FALSE(op.getGeneratedFaces().empty());
    EXPECT_NE(doc.bodyFaceIds(id), nullptr);
}

// Steve's "chamfer through the hole" case (#57 follow-up): a rectangular
// hole in the TOP FACE near - but not touching - the front edge. The edge
// itself is whole; the bevel legitimately sweeps ACROSS the hole. Native
// fails (hole inside the blend's reach); the fallback must build it and
// match chamfer-first-then-hole exactly. The old single-midpoint setback
// guard refused this whenever the hole sat across from the edge's middle.
TEST(BlendCut, ChamferSweepsAcrossHoleInAdjacentFace) {
    // Hole through the top face: x 15..25, y 3..7 - clearance 3mm from the
    // front edge, so a 5mm chamfer must pass over it.
    TopoDS_Shape holeTool = BRepPrimAPI_MakeBox(
        gp_Pnt(15.0, 3.0, 5.0), gp_Pnt(25.0, 7.0, 12.0)).Shape();

    Document doc;
    int id = doc.addBody(BRepAlgoAPI_Cut(plainBox(), holeTool).Shape(), "Holed");
    std::vector<TopoDS_Edge> es = frontTopEdges(doc.getBody(id));
    ASSERT_EQ(es.size(), 1u) << "hole must NOT touch the edge";

    ChamferOp op;
    op.setBody(id);
    op.setEdges(es);
    op.setDistance(5.0);
    ASSERT_TRUE(op.execute(doc));

    const TopoDS_Shape& out = doc.getBody(id);
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());
    double ref = chamferThenCutVolume(holeTool, 5.0, 5.0);
    ASSERT_GT(ref, 0.0);
    EXPECT_NEAR(volumeOf(out), ref, 1e-4);
    EXPECT_FALSE(op.getGeneratedFaces().empty());
}

// Interior-corner chamfer (a FILL ramp) whose footprint crosses a hole in
// the floor face - Steve's light-cover rim case (#57). Native handles the
// clean corner but refuses once the ramp must cross the hole; the fill
// fallback fuses the ramp over the full span and re-pierces the hole, which
// must equal chamfer-first-then-hole exactly.
TEST(BlendCut, FillRampSweepsAcrossHoleInFloor) {
    // Plate 40x20x2 with a 3mm wall along y=8..11; interior corner at y=8,
    // z=2. Square hole through the plate at x 15..25, y 3..6 - 2mm clear of
    // the corner, so a 2.5mm ramp must cross into it.
    auto plateWithWall = []() {
        TopoDS_Shape plate = BRepPrimAPI_MakeBox(40.0, 20.0, 2.0).Shape();
        TopoDS_Shape wall = BRepPrimAPI_MakeBox(
            gp_Pnt(0.0, 8.0, 2.0), gp_Pnt(40.0, 11.0, 5.0)).Shape();
        return BRepAlgoAPI_Fuse(plate, wall).Shape();
    };
    TopoDS_Shape holeTool = BRepPrimAPI_MakeBox(
        gp_Pnt(15.0, 3.0, -1.0), gp_Pnt(25.0, 6.0, 3.0)).Shape();
    // The interior corner edge: straight, both vertices at y=8, z=2.
    auto cornerEdge = [](const TopoDS_Shape& s) {
        for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next()) {
            const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
            if (BRepAdaptor_Curve(e).GetType() != GeomAbs_Line) continue;
            bool ok = true;
            int nv = 0;
            for (TopExp_Explorer vx(e, TopAbs_VERTEX); vx.More();
                 vx.Next(), ++nv) {
                gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()));
                if (std::abs(p.Y() - 8.0) > 1e-7 ||
                    std::abs(p.Z() - 2.0) > 1e-7)
                    ok = false;
            }
            if (ok && nv == 2) return e;
        }
        return TopoDS_Edge();
    };

    // Reference: chamfer FIRST on the clean corner (native fill), THEN hole.
    double ref = -1.0;
    {
        Document doc;
        int id = doc.addBody(plateWithWall(), "Ref");
        TopoDS_Edge e = cornerEdge(doc.getBody(id));
        ASSERT_FALSE(e.IsNull());
        ChamferOp op;
        op.setBody(id);
        op.setEdges({e});
        op.setDistance(2.5);
        ASSERT_TRUE(op.execute(doc)) << "native fill on clean corner failed";
        ref = volumeOf(
            BRepAlgoAPI_Cut(doc.getBody(id), holeTool).Shape());
    }
    ASSERT_GT(ref, 0.0);

    // Candidate: hole FIRST, then the same chamfer - the fill fallback.
    Document doc;
    int id = doc.addBody(
        BRepAlgoAPI_Cut(plateWithWall(), holeTool).Shape(), "Holed");
    TopoDS_Edge e = cornerEdge(doc.getBody(id));
    ASSERT_FALSE(e.IsNull());
    ChamferOp op;
    op.setBody(id);
    op.setEdges({e});
    op.setDistance(2.5);
    ASSERT_TRUE(op.execute(doc));
    const TopoDS_Shape& out = doc.getBody(id);
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());
    EXPECT_NEAR(volumeOf(out), ref, 1e-4);
    EXPECT_FALSE(op.getGeneratedFaces().empty());
}

// Hip corner (#57 follow-up 3): a fill ramp ending at a corner whose
// neighbouring edge already carries a (smaller) chamfer must MITER into that
// bevel - hip-roof style - not punch a flat end wall up past it. Plate with
// two perpendicular walls: chamfer the short wall's base first (native),
// then run the big fill ramp along the long wall (a pocket forces the fill
// path). No material may stand above the neighbour's bevel plane.
TEST(BlendCut, FillRampMitersIntoNeighbourBevel) {
    auto lBody = []() {
        TopoDS_Shape plate = BRepPrimAPI_MakeBox(40.0, 20.0, 2.0).Shape();
        TopoDS_Shape wallA = BRepPrimAPI_MakeBox(
            gp_Pnt(0.0, 8.0, 2.0), gp_Pnt(40.0, 11.0, 5.0)).Shape();
        TopoDS_Shape wallB = BRepPrimAPI_MakeBox(
            gp_Pnt(0.0, 0.0, 2.0), gp_Pnt(3.0, 8.0, 5.0)).Shape();
        return BRepAlgoAPI_Fuse(BRepAlgoAPI_Fuse(plate, wallA).Shape(),
                                wallB).Shape();
    };
    TopoDS_Shape holeTool = BRepPrimAPI_MakeBox(
        gp_Pnt(15.0, 3.0, -1.0), gp_Pnt(25.0, 6.0, 3.0)).Shape();
    // Edge finder: straight edge whose two vertices satisfy the predicate.
    auto findEdge = [](const TopoDS_Shape& s, auto pred) {
        for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next()) {
            const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
            if (BRepAdaptor_Curve(e).GetType() != GeomAbs_Line) continue;
            bool ok = true;
            int nv = 0;
            for (TopExp_Explorer vx(e, TopAbs_VERTEX); vx.More();
                 vx.Next(), ++nv)
                if (!pred(BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()))))
                    ok = false;
            if (ok && nv == 2) return e;
        }
        return TopoDS_Edge();
    };

    Document doc;
    int id = doc.addBody(BRepAlgoAPI_Cut(lBody(), holeTool).Shape(), "L");
    // Step 1: small native chamfer on wallB's base edge (x=3, z=2).
    {
        TopoDS_Edge eB = findEdge(doc.getBody(id), [](const gp_Pnt& p) {
            return std::abs(p.X() - 3.0) < 1e-7 && std::abs(p.Z() - 2.0) < 1e-7;
        });
        ASSERT_FALSE(eB.IsNull());
        ChamferOp op;
        op.setBody(id);
        op.setEdges({eB});
        op.setDistance(1.5);
        ASSERT_TRUE(op.execute(doc));
    }
    // Step 2: big fill ramp on wallA's base edge (y=8, z=2) across the hole.
    {
        TopoDS_Edge eA = findEdge(doc.getBody(id), [](const gp_Pnt& p) {
            return std::abs(p.Y() - 8.0) < 1e-7 && std::abs(p.Z() - 2.0) < 1e-7;
        });
        ASSERT_FALSE(eA.IsNull());
        ChamferOp op;
        op.setBody(id);
        op.setEdges({eA});
        op.setDistance(2.5);   // up wallA
        op.setDistance2(5.0);  // across the floor, over the hole
        ASSERT_TRUE(op.execute(doc));
    }
    const TopoDS_Shape& out = doc.getBody(id);
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());
    // The neighbour's bevel plane: from (4.5, y, 2) to (3, y, 3.5) -
    // z = 2 + (4.5 - x). Sample just ABOVE it in the corner zone (where the
    // old flat prism cap poked through): all must be EMPTY.
    for (double x : {3.1, 3.5, 3.9, 4.3}) {
        for (double y : {4.0, 5.5, 7.0}) {
            const double zPlane = 2.0 + (4.5 - x);
            gp_Pnt probe(x, y, zPlane + 0.15);
            BRepClass3d_SolidClassifier sc(out, probe, 1e-7);
            EXPECT_NE(sc.State(), TopAbs_IN)
                << "ramp pokes above neighbour bevel at x=" << x
                << " y=" << y;
        }
    }
    // And the hip must EXIST: below BOTH slopes inside the corner strip there
    // is material (the ramp reaches the corner instead of stopping in a flat
    // cap at the neighbour's toe - the old "weird angle" left this empty).
    {
        gp_Pnt probe(4.0, 7.5, 2.3); // neighbour plane 2.5, own slope 4.25
        BRepClass3d_SolidClassifier sc(out, probe, 1e-7);
        EXPECT_EQ(sc.State(), TopAbs_IN)
            << "hip region is hollow - ramp didn't reach the corner";
    }
}

// Equal setbacks at the corner (#57 follow-up 4): when the fill ramp's
// vertical setback MATCHES the neighbour's, the hip-clip plane passes
// exactly through the prism's cap edge - the old half-space cut could hang
// OCCT there (app went unresponsive). The bounded-box clip plus tangent-skip
// must complete quickly and stay valid. (A hang fails via the test timeout.)
TEST(BlendCut, EqualHeightCornerDoesNotHang) {
    auto lBody = []() {
        TopoDS_Shape plate = BRepPrimAPI_MakeBox(40.0, 20.0, 2.0).Shape();
        TopoDS_Shape wallA = BRepPrimAPI_MakeBox(
            gp_Pnt(0.0, 8.0, 2.0), gp_Pnt(40.0, 11.0, 5.0)).Shape();
        TopoDS_Shape wallB = BRepPrimAPI_MakeBox(
            gp_Pnt(0.0, 0.0, 2.0), gp_Pnt(3.0, 8.0, 5.0)).Shape();
        return BRepAlgoAPI_Fuse(BRepAlgoAPI_Fuse(plate, wallA).Shape(),
                                wallB).Shape();
    };
    TopoDS_Shape holeTool = BRepPrimAPI_MakeBox(
        gp_Pnt(15.0, 3.0, -1.0), gp_Pnt(25.0, 6.0, 3.0)).Shape();
    auto findEdge = [](const TopoDS_Shape& s, auto pred) {
        for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next()) {
            const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
            if (BRepAdaptor_Curve(e).GetType() != GeomAbs_Line) continue;
            bool ok = true;
            int nv = 0;
            for (TopExp_Explorer vx(e, TopAbs_VERTEX); vx.More();
                 vx.Next(), ++nv)
                if (!pred(BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()))))
                    ok = false;
            if (ok && nv == 2) return e;
        }
        return TopoDS_Edge();
    };

    Document doc;
    int id = doc.addBody(BRepAlgoAPI_Cut(lBody(), holeTool).Shape(), "L");
    {
        TopoDS_Edge eB = findEdge(doc.getBody(id), [](const gp_Pnt& p) {
            return std::abs(p.X() - 3.0) < 1e-7 && std::abs(p.Z() - 2.0) < 1e-7;
        });
        ASSERT_FALSE(eB.IsNull());
        ChamferOp op;
        op.setBody(id);
        op.setEdges({eB});
        op.setDistance(2.5);   // SAME vertical setback as the fill below
        ASSERT_TRUE(op.execute(doc));
    }
    {
        TopoDS_Edge eA = findEdge(doc.getBody(id), [](const gp_Pnt& p) {
            return std::abs(p.Y() - 8.0) < 1e-7 && std::abs(p.Z() - 2.0) < 1e-7;
        });
        ASSERT_FALSE(eA.IsNull());
        ChamferOp op;
        op.setBody(id);
        op.setEdges({eA});
        op.setDistance(2.5);   // up wallA - equal heights at the corner
        op.setDistance2(5.0);  // across the floor, over the hole
        ASSERT_TRUE(op.execute(doc));
    }
    EXPECT_TRUE(BRepCheck_Analyzer(doc.getBody(id)).IsValid());
}

// OUTSIDE plan corner (#57 follow-up 5): two interior-corner ramps wrapping
// a raised block's corner sit in DISJOINT floor quadrants - no overlap, so
// no clip can join them. The corner FAN tetra must fill the wrap so the
// blends meet like native chamfers do, instead of two abrupt end walls.
TEST(BlendCut, OutsideCornerGetsFan) {
    // Plate 40x40x2 with a raised block 0..20 x 0..20, walls 3 tall.
    auto blockBody = []() {
        TopoDS_Shape plate = BRepPrimAPI_MakeBox(40.0, 40.0, 2.0).Shape();
        TopoDS_Shape block = BRepPrimAPI_MakeBox(
            gp_Pnt(0.0, 0.0, 2.0), gp_Pnt(20.0, 20.0, 5.0)).Shape();
        return BRepAlgoAPI_Fuse(plate, block).Shape();
    };
    // Shallow pocket crossing the second edge's floor approach - forces the
    // FILL path for it (native refuses a blend into a pocket floor).
    TopoDS_Shape pocket = BRepPrimAPI_MakeBox(
        gp_Pnt(8.0, 20.5, 1.0), gp_Pnt(14.0, 23.0, 3.0)).Shape();
    auto findEdge = [](const TopoDS_Shape& s, auto pred) {
        for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next()) {
            const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
            if (BRepAdaptor_Curve(e).GetType() != GeomAbs_Line) continue;
            bool ok = true;
            int nv = 0;
            for (TopExp_Explorer vx(e, TopAbs_VERTEX); vx.More();
                 vx.Next(), ++nv)
                if (!pred(BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()))))
                    ok = false;
            if (ok && nv == 2) return e;
        }
        return TopoDS_Edge();
    };

    Document doc;
    int id = doc.addBody(BRepAlgoAPI_Cut(blockBody(), pocket).Shape(), "Blk");
    // First ramp: block side x=20 (floor side x>20), symmetric 2.5.
    {
        TopoDS_Edge e = findEdge(doc.getBody(id), [](const gp_Pnt& p) {
            return std::abs(p.X() - 20.0) < 1e-7 &&
                   std::abs(p.Z() - 2.0) < 1e-7 && p.Y() < 20.0 + 1e-7;
        });
        ASSERT_FALSE(e.IsNull());
        ChamferOp op;
        op.setBody(id);
        op.setEdges({e});
        op.setDistance(2.5);
        ASSERT_TRUE(op.execute(doc));
    }
    // Second ramp: block side y=20 (floor side y>20), crosses the pocket.
    {
        TopoDS_Edge e = findEdge(doc.getBody(id), [](const gp_Pnt& p) {
            return std::abs(p.Y() - 20.0) < 1e-7 &&
                   std::abs(p.Z() - 2.0) < 1e-7 && p.X() < 20.0 + 1e-7;
        });
        ASSERT_FALSE(e.IsNull());
        ChamferOp op;
        op.setBody(id);
        op.setEdges({e});
        op.setDistance(2.5);
        ASSERT_TRUE(op.execute(doc));
    }
    const TopoDS_Shape& out = doc.getBody(id);
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());
    // The fan: material inside the wrap tetra {V(20,20,2) W(20,20,4.5)
    // T1(20,22.5,2) T2(22.5,20,2)} - the old flat caps left this quadrant
    // empty at floor level right off the corner.
    {
        gp_Pnt probe(20.5, 20.5, 2.3);
        BRepClass3d_SolidClassifier sc(out, probe, 1e-7);
        EXPECT_EQ(sc.State(), TopAbs_IN)
            << "outside corner has no fan - abrupt end walls";
    }
    EXPECT_EQ(coplanarSeamPairs(out), 0)
        << "ramp fragmented into coplanar facets (visible fan of seams)";
}

// Forward contract for the multi-edge fill: chamfer several adjacent base
// edges of a raised boss in ONE op, each crossing a pocket so the FILL path
// fires, forming mitered corners all round. The fused ramps + miters must
// leave NO coplanar-adjacent seam pairs (the "fan across the ramp"). NOTE:
// this synthetic case does NOT reproduce the specific FixSmallEdges-corruption
// that caused #59 on the real light cover (it passes on the pre-#59 code
// too) - the genuine regression guard for that is the real-file rebuild probe
// (wip-tests/probe_rebuild_render, 27 seams pre-fix → 0 post-fix). This test
// still holds the invariant against a DIFFERENT future merge regression.
TEST(BlendCut, PictureFrameFillHasNoCoplanarSeams) {
    // Boss 0..40 x 0..30, 3 tall, on a 60x50 plate (margins all round so
    // every base edge's ramp toe lands on open floor, not the plate edge).
    auto bossBody = []() {
        TopoDS_Shape plate = BRepPrimAPI_MakeBox(
            gp_Pnt(-10.0, -10.0, 0.0), gp_Pnt(50.0, 40.0, 2.0)).Shape();
        TopoDS_Shape boss = BRepPrimAPI_MakeBox(
            gp_Pnt(0.0, 0.0, 2.0), gp_Pnt(40.0, 30.0, 5.0)).Shape();
        return BRepAlgoAPI_Fuse(plate, boss).Shape();
    };
    // A shallow pocket straddling each of the four base edges → fill path.
    auto pocket = [](double x0, double y0, double x1, double y1) {
        return BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, 1.0), gp_Pnt(x1, y1, 3.0))
            .Shape();
    };
    TopoDS_Shape b = bossBody();
    b = BRepAlgoAPI_Cut(b, pocket(15.0, -2.0, 21.0, 4.0)).Shape();   // y=0 edge
    b = BRepAlgoAPI_Cut(b, pocket(15.0, 26.0, 21.0, 32.0)).Shape();  // y=30 edge
    b = BRepAlgoAPI_Cut(b, pocket(-2.0, 12.0, 4.0, 18.0)).Shape();   // x=0 edge
    b = BRepAlgoAPI_Cut(b, pocket(36.0, 12.0, 42.0, 18.0)).Shape();  // x=40 edge

    auto findEdges = [](const TopoDS_Shape& s, auto pred) {
        std::vector<TopoDS_Edge> out;
        for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next()) {
            const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
            if (BRepAdaptor_Curve(e).GetType() != GeomAbs_Line) continue;
            bool ok = true;
            int nv = 0;
            for (TopExp_Explorer vx(e, TopAbs_VERTEX); vx.More();
                 vx.Next(), ++nv)
                if (!pred(BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()))))
                    ok = false;
            if (ok && nv == 2) out.push_back(e);
        }
        return out;
    };

    Document doc;
    int id = doc.addBody(b, "Frame");
    // All four base edges (z=2, on the boss footprint boundary) in one op.
    std::vector<TopoDS_Edge> edges = findEdges(
        doc.getBody(id), [](const gp_Pnt& p) {
            if (std::abs(p.Z() - 2.0) > 1e-7) return false;
            const bool onX = std::abs(p.X()) < 1e-7 || std::abs(p.X() - 40.0) < 1e-7;
            const bool onY = std::abs(p.Y()) < 1e-7 || std::abs(p.Y() - 30.0) < 1e-7;
            return onX || onY;
        });
    ASSERT_GE(edges.size(), 4u);
    ChamferOp op;
    op.setBody(id);
    op.setEdges(edges);
    op.setDistance(2.5);   // up the boss
    op.setDistance2(5.0);  // across the floor, over each pocket
    ASSERT_TRUE(op.execute(doc));

    const TopoDS_Shape& out = doc.getBody(id);
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());
    EXPECT_EQ(coplanarSeamPairs(out), 0)
        << "picture-frame ramp fanned into coplanar facets (the light-cover bug)";
}

// The miter's clip face lies exactly IN the neighbour bevel's plane, so after
// the fuse it must MERGE into the neighbour bevel - one face, no seam. The
// fuse can leave a zero-length edge at the toe junction (piece extension
// collapsing against the body boundary when the neighbour's toe lands exactly
// on the plate edge), and one degenerate edge made UnifySameDomain keep the
// wedge as a separate coplanar facet: invisible in geometry checks, but the
// viewport draws its boundary as a "weird blend at the bottom of the corner".
TEST(BlendCut, MiterWedgeMergesIntoNeighbourBevel) {
    // Raised block on a plate; the SIDE bevel's setback reaches the plate
    // edge exactly (toe on the boundary), like the light cover's rim bevels.
    auto body = []() {
        TopoDS_Shape plate = BRepPrimAPI_MakeBox(40.0, 30.0, 2.0).Shape();
        TopoDS_Shape block = BRepPrimAPI_MakeBox(
            gp_Pnt(0.0, 0.0, 2.0), gp_Pnt(30.0, 20.0, 5.0)).Shape();
        return BRepAlgoAPI_Fuse(plate, block).Shape();
    };
    // Pocket crossing the long ramp's floor so it takes the FILL path.
    TopoDS_Shape pocket = BRepPrimAPI_MakeBox(
        gp_Pnt(8.0, 20.5, 1.0), gp_Pnt(14.0, 23.0, 3.0)).Shape();
    auto findEdge = [](const TopoDS_Shape& s, auto pred) {
        for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next()) {
            const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
            if (BRepAdaptor_Curve(e).GetType() != GeomAbs_Line) continue;
            bool ok = true;
            int nv = 0;
            for (TopExp_Explorer vx(e, TopAbs_VERTEX); vx.More();
                 vx.Next(), ++nv)
                if (!pred(BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()))))
                    ok = false;
            if (ok && nv == 2) return e;
        }
        return TopoDS_Edge();
    };

    Document doc;
    int id = doc.addBody(BRepAlgoAPI_Cut(body(), pocket).Shape(), "Blk");
    // Side bevel: block edge x=30, 3 up / 10 out - toe exactly at x=40.
    {
        TopoDS_Edge e = findEdge(doc.getBody(id), [](const gp_Pnt& p) {
            return std::abs(p.X() - 30.0) < 1e-7 &&
                   std::abs(p.Z() - 2.0) < 1e-7 && p.Y() < 20.0 + 1e-7;
        });
        ASSERT_FALSE(e.IsNull());
        ChamferOp op;
        op.setBody(id);
        op.setEdges({e});
        op.setDistance(3.0);
        op.setDistance2(10.0);
        ASSERT_TRUE(op.execute(doc));
    }
    // Long ramp: block edge y=20, 3 up / 8 out, crosses the pocket → fill
    // path with a miter into the side bevel at the (30,20) outside corner.
    {
        // After the side fill, its cap base merges coplanar with this wall
        // base into one longer edge (may run past x=30) - match on y/z only.
        TopoDS_Edge e = findEdge(doc.getBody(id), [](const gp_Pnt& p) {
            return std::abs(p.Y() - 20.0) < 1e-7 &&
                   std::abs(p.Z() - 2.0) < 1e-7;
        });
        ASSERT_FALSE(e.IsNull());
        ChamferOp op;
        op.setBody(id);
        op.setEdges({e});
        op.setDistance(3.0);
        op.setDistance2(8.0);
        ASSERT_TRUE(op.execute(doc));
    }
    const TopoDS_Shape& out = doc.getBody(id);
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());
    // Exactly ONE face on the side bevel's plane (rises 3 over a 10 run in
    // +x → normal (3,0,10)/√109), covering the full span INCLUDING the
    // miter corner.
    int sideFaces = 0;
    double zMaxTop = -1e9, yMax = -1e9;
    const gp_Dir want(3.0 / std::sqrt(109.0), 0.0, 10.0 / std::sqrt(109.0));
    for (TopExp_Explorer fx(out, TopAbs_FACE); fx.More(); fx.Next()) {
        const TopoDS_Face f = TopoDS::Face(fx.Current());
        BRepAdaptor_Surface surf(f);
        if (surf.GetType() != GeomAbs_Plane) continue;
        gp_Dir n = surf.Plane().Axis().Direction();
        if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
        if (n.Angle(want) > 0.01) continue;
        sideFaces++;
        Bnd_Box bb;
        BRepBndLib::Add(f, bb);
        double x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        zMaxTop = std::max(zMaxTop, z1);
        yMax = std::max(yMax, y1);
    }
    EXPECT_EQ(sideFaces, 1)
        << "miter clip face left as a separate coplanar facet (visible seam)";
    // The single face must include the mitered corner wrap (reaches past the
    // block corner y=20 toward the long ramp's territory).
    EXPECT_GT(yMax, 20.5);
    EXPECT_GT(zMaxTop, 4.5);
    // The plate's outer wall at x=40 (where the side bevel's toe lands) must
    // stay ONE face: an apex bulge dipping below the floor sweeps a coplanar
    // strip + razor faces onto it - flush geometry, but the viewport draws
    // the fragment boundaries as a sliver under the toe.
    int wallFaces = 0;
    for (TopExp_Explorer fx(out, TopAbs_FACE); fx.More(); fx.Next()) {
        const TopoDS_Face f = TopoDS::Face(fx.Current());
        BRepAdaptor_Surface surf(f);
        if (surf.GetType() != GeomAbs_Plane) continue;
        gp_Dir n = surf.Plane().Axis().Direction();
        if (std::abs(std::abs(n.X()) - 1.0) > 1e-6) continue;
        Bnd_Box bb;
        BRepBndLib::Add(f, bb);
        double x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        if (std::abs(x0 - 40.0) > 0.2 || std::abs(x1 - 40.0) > 0.2) continue;
        wallFaces++;
    }
    EXPECT_EQ(wallFaces, 1)
        << "outer wall fragmented at the bevel toe (bottom-sliver imprint)";
}

// A cut can only REMOVE material, so a concave (inside-corner) edge must be
// refused - silently "chamfering" it with a cut would dig a groove instead
// of adding the bevel sliver.
TEST(BlendCut, ConcaveEdgeRefused) {
    TopoDS_Shape lower = BRepPrimAPI_MakeBox(40.0, 20.0, 10.0).Shape();
    TopoDS_Shape upper = BRepPrimAPI_MakeBox(
        gp_Pnt(0.0, 0.0, 10.0), gp_Pnt(40.0, 10.0, 20.0)).Shape();
    TopoDS_Shape lShape = BRepAlgoAPI_Fuse(lower, upper).Shape();

    // The concave junction runs along X at y=10, z=10.
    std::vector<TopoDS_Edge> concave;
    for (TopExp_Explorer ex(lShape, TopAbs_EDGE); ex.More(); ex.Next()) {
        const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
        if (BRepAdaptor_Curve(e).GetType() != GeomAbs_Line) continue;
        bool ok = true;
        for (TopExp_Explorer vx(e, TopAbs_VERTEX); vx.More(); vx.Next()) {
            gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()));
            if (std::abs(p.Y() - 10.0) > 1e-7 || std::abs(p.Z() - 10.0) > 1e-7)
                ok = false;
        }
        if (ok) { concave.push_back(e); break; }
    }
    ASSERT_FALSE(concave.empty());

    topo::GenerationLedger ledger;
    TopoDS_Shape out;
    std::vector<TopoDS_Shape> blends;
    EXPECT_FALSE(blendcut::cutChamfer(lShape, concave, 2.0, 2.0,
                                      TopoDS_Face(), ledger, out, blends));
    EXPECT_FALSE(blendcut::cutFillet(lShape, concave, 2.0,
                                     ledger, out, blends));
}
