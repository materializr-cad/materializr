// Verifies the planar-net unfold engine: a box's 6 faces flatten into a single
// connected net that preserves area (isometry) and reports 5 fold lines (a
// spanning tree over 6 faces), each a 90° fold.
#include <gtest/gtest.h>

#include "modeling/Unfold.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <gp_Circ.hxx>
#include <gp_Ax2.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "test_tmp_path.h"

using namespace materializr;

namespace {
double loopArea(const std::vector<glm::dvec2>& p) {
    double a = 0.0;
    for (size_t i = 0, n = p.size(); i < n; ++i)
        a += p[i].x * p[(i + 1) % n].y - p[(i + 1) % n].x * p[i].y;
    return std::fabs(0.5 * a);
}
std::vector<TopoDS_Face> facesOf(const TopoDS_Shape& s) {
    std::vector<TopoDS_Face> out;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next())
        out.push_back(TopoDS::Face(ex.Current()));
    return out;
}
} // namespace

TEST(Unfold, BoxUnfoldsToConnectedAreaPreservingNet) {
    const double X = 30, Y = 20, Z = 10;
    TopoDS_Shape box = BRepPrimAPI_MakeBox(X, Y, Z).Shape();
    auto faces = facesOf(box);
    ASSERT_EQ(faces.size(), 6u);

    FlatPattern fp = unfoldPlanarFaces(faces);
    ASSERT_TRUE(fp.ok) << fp.warning;

    // All six faces placed and connected; a spanning tree over 6 faces = 5 folds.
    EXPECT_EQ(fp.piecesPlaced, 6);
    EXPECT_EQ(fp.faces.size(), 6u);
    EXPECT_EQ(fp.folds.size(), 5u);

    // Box folds are right angles.
    for (const FoldLine& f : fp.folds)
        EXPECT_NEAR(f.foldAngleDeg, 90.0, 1e-6);

    // Isometry: total flattened area == box surface area.
    const double surfaceArea = 2.0 * (X * Y + Y * Z + X * Z);
    double flatArea = 0.0;
    for (const FlatFace& f : fp.faces)
        for (const FlatLoop& l : f.loops)
            flatArea += (l.isHole ? -1.0 : 1.0) * loopArea(l.pts);
    EXPECT_NEAR(flatArea, surfaceArea, 1e-6);
}

TEST(Unfold, CylinderSkinUnrollsWithScoreLines) {
    const double R = 20, H = 50, ang = M_PI / 2; // open 90° strip (not a closed tube)
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(R, H, ang).Shape();
    // Pick the curved (lateral) face.
    TopoDS_Face lateral;
    for (TopExp_Explorer ex(cyl, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        if (BRepAdaptor_Surface(f).GetType() == GeomAbs_Cylinder) { lateral = f; break; }
    }
    ASSERT_FALSE(lateral.IsNull());

    FlatPattern fine   = unfoldFaces({lateral}, /*maxBevelDeg=*/5.0,  1.0);
    FlatPattern coarse = unfoldFaces({lateral}, /*maxBevelDeg=*/20.0, 1.0);
    ASSERT_TRUE(fine.ok) << fine.warning;
    ASSERT_TRUE(coarse.ok) << coarse.warning;

    // The bevel cap actually controls density: a coarser cap → fewer score lines.
    EXPECT_GT(fine.folds.size(), coarse.folds.size());
    EXPECT_GT(fine.folds.size(), 4u);

    // Unrolled area ≈ developed arc-strip R·θ·H (within chordal tessellation error).
    double flatArea = 0.0;
    for (const FlatFace& f : fine.faces)
        for (const FlatLoop& l : f.loops)
            flatArea += (l.isHole ? -1.0 : 1.0) * loopArea(l.pts);
    const double trueArea = R * ang * H;
    EXPECT_NEAR(flatArea, trueArea, trueArea * 0.05);  // within 5%
}

TEST(Unfold, ClosedCylinderOpensViaSeam) {
    // A FULL (closed) cylinder skin has no boundary edge; seam-cutting must open
    // it into a flat strip of area ≈ 2πRH. (This used to fail outright.)
    const double R = 20, H = 50;
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(R, H).Shape();
    TopoDS_Face lateral;
    for (TopExp_Explorer ex(cyl, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        if (BRepAdaptor_Surface(f).GetType() == GeomAbs_Cylinder) { lateral = f; break; }
    }
    ASSERT_FALSE(lateral.IsNull());

    FlatPattern fp = unfoldFaces({lateral}, /*maxBevelDeg=*/8.0, 1.0);
    ASSERT_TRUE(fp.ok) << fp.warning;
    EXPECT_LT(fp.curvatureDeg, 5.0);  // a cylinder is developable
    double flatArea = 0.0;
    for (const FlatFace& f : fp.faces)
        for (const FlatLoop& l : f.loops)
            flatArea += (l.isHole ? -1.0 : 1.0) * loopArea(l.pts);
    EXPECT_NEAR(flatArea, 2.0 * M_PI * R * H, 2.0 * M_PI * R * H * 0.06);
}

TEST(Unfold, DoublyCurvedFlagsHighCurvatureAndStillProduces) {
    // A sphere is strongly doubly-curved: it must report large curvature (so the
    // UI can warn) yet still produce a (relief-cut / multi-piece) pattern rather
    // than failing or shattering into nothing.
    TopoDS_Shape sph = BRepPrimAPI_MakeSphere(20.0).Shape();
    TopoDS_Face face;
    for (TopExp_Explorer ex(sph, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        if (BRepAdaptor_Surface(f).GetType() == GeomAbs_Sphere) { face = f; break; }
    }
    ASSERT_FALSE(face.IsNull());

    FlatPattern fp = unfoldFaces({face}, /*maxBevelDeg=*/12.0, 1.0);
    ASSERT_TRUE(fp.ok) << fp.warning;
    EXPECT_GT(fp.curvatureDeg, 200.0);    // strongly doubly-curved → UI warns
    EXPECT_GE(fp.piecesPlaced, 1);
    EXPECT_FALSE(fp.faces.empty());       // still emits a cut pattern
}

TEST(Unfold, ConformalDevelopableHasLowDistortion) {
    // LSCM of a developable surface (open cylinder strip) is isometric, so it
    // should come out as ONE piece with little area stretch.
    const double R = 20, H = 50, ang = M_PI / 2;
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(R, H, ang).Shape();
    TopoDS_Face lateral;
    for (TopExp_Explorer ex(cyl, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        if (BRepAdaptor_Surface(f).GetType() == GeomAbs_Cylinder) { lateral = f; break; }
    }
    ASSERT_FALSE(lateral.IsNull());

    FlatPattern fp = unfoldConformal({lateral}, /*maxBevelDeg=*/8.0, 1.0);
    ASSERT_TRUE(fp.ok) << fp.warning;
    EXPECT_EQ(fp.piecesPlaced, 1);
    EXPECT_LT(fp.distortionPct, 12.0);       // developable → near-isometric
    double flatArea = 0.0;
    for (const FlatFace& f : fp.faces)
        for (const FlatLoop& l : f.loops)
            flatArea += (l.isHole ? -1.0 : 1.0) * loopArea(l.pts);
    EXPECT_NEAR(flatArea, R * ang * H, R * ang * H * 0.08);  // area preserved by rescale
}

TEST(Unfold, ConeLateralUnrollsToOneSectorNotShards) {
    // A full cone's lateral surface is developable but CLOSED (a seam from apex to
    // base). The face-net engine keeps that seam open, so it must fan into ONE
    // clean sector - not shatter into shards (the old 3D-weld bug closed the seam
    // and the greedy unroll wrapped past 2π and self-overlapped).
    const double R = 20, H = 40;
    TopoDS_Shape cone = BRepPrimAPI_MakeCone(R, 0.0, H).Shape();
    TopoDS_Face lateral;
    for (TopExp_Explorer ex(cone, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        if (BRepAdaptor_Surface(f).GetType() == GeomAbs_Cone) { lateral = f; break; }
    }
    ASSERT_FALSE(lateral.IsNull());

    FlatPattern fp = unfoldDevelopableNet({lateral}, /*maxBevelDeg=*/10.0, 1.0);
    ASSERT_TRUE(fp.ok) << fp.warning;
    EXPECT_EQ(fp.piecesPlaced, 1) << "cone seam must stay open → one sector";
    EXPECT_LT(fp.curvatureDeg, 5.0);   // developable
    ASSERT_EQ(fp.faces.size(), 1u);
    EXPECT_EQ(fp.faces[0].loops.size(), 1u) << "apex must collapse - no spurious tip hole";

    // Unrolled area ≈ cone lateral area π·R·slant (within tessellation error).
    const double slant = std::sqrt(H * H + R * R);
    double flatArea = 0.0;
    for (const FlatFace& f : fp.faces)
        for (const FlatLoop& l : f.loops)
            flatArea += (l.isHole ? -1.0 : 1.0) * loopArea(l.pts);
    EXPECT_NEAR(flatArea, M_PI * R * slant, M_PI * R * slant * 0.06);
}

TEST(Unfold, MultiFaceDevelopableNetHingesAndPreservesArea) {
    // A closed cylinder (lateral + two circular caps): the net engine must unroll
    // each face and hinge them along shared edges, preserving total area - not
    // tangle the way the old single-soup BFS did once >2 faces were selected.
    const double R = 15, H = 30;
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(R, H).Shape();
    std::vector<TopoDS_Face> faces = facesOf(cyl);
    ASSERT_GE(faces.size(), 3u);  // lateral + 2 caps

    FlatPattern fp = unfoldDevelopableNet(faces, /*maxBevelDeg=*/10.0, 1.0);
    ASSERT_TRUE(fp.ok) << fp.warning;
    EXPECT_GE(fp.faces.size(), 3u);   // every face laid out

    // Each face unrolls isometrically, so summed flat area ≈ true surface area.
    const double trueArea = 2.0 * M_PI * R * H + 2.0 * M_PI * R * R;
    double flatArea = 0.0;
    for (const FlatFace& f : fp.faces)
        for (const FlatLoop& l : f.loops)
            flatArea += (l.isHole ? -1.0 : 1.0) * loopArea(l.pts);
    EXPECT_NEAR(flatArea, trueArea, trueArea * 0.08);
}

TEST(Unfold, NetEngineHandlesAllPlanarBoxAndPreservesArea) {
    // Route a box through the developable-net engine (not the planar engine) to
    // exercise the multi-root optimizer on several faces: every face must place
    // and total area must be preserved, whatever the piece split.
    const double X = 30, Y = 20, Z = 10;
    TopoDS_Shape box = BRepPrimAPI_MakeBox(X, Y, Z).Shape();
    auto faces = facesOf(box);
    ASSERT_EQ(faces.size(), 6u);

    FlatPattern fp = unfoldDevelopableNet(faces, /*maxBevelDeg=*/10.0, 1.0);
    ASSERT_TRUE(fp.ok) << fp.warning;
    EXPECT_EQ(fp.faces.size(), 6u);   // all six faces laid out

    const double surfaceArea = 2.0 * (X * Y + Y * Z + X * Z);
    double flatArea = 0.0;
    for (const FlatFace& f : fp.faces)
        for (const FlatLoop& l : f.loops)
            flatArea += (l.isHole ? -1.0 : 1.0) * loopArea(l.pts);
    EXPECT_NEAR(flatArea, surfaceArea, surfaceArea * 0.02);
}

TEST(Unfold, ConformalClosedCylinderOpensToLowDistortionStrip) {
    // A FULL (closed) cylinder skin has no boundary, so LSCM would collapse it.
    // The auto-seam-cut must slit it open into a developable strip → low area
    // stretch and preserved area. (Before the cut this came out degenerate.)
    const double R = 20, H = 50;
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(R, H).Shape();
    TopoDS_Face lateral;
    for (TopExp_Explorer ex(cyl, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        if (BRepAdaptor_Surface(f).GetType() == GeomAbs_Cylinder) { lateral = f; break; }
    }
    ASSERT_FALSE(lateral.IsNull());

    FlatPattern fp = unfoldConformal({lateral}, /*maxBevelDeg=*/8.0, 1.0);
    ASSERT_TRUE(fp.ok) << fp.warning;
    EXPECT_EQ(fp.piecesPlaced, 1);
    EXPECT_LT(fp.distortionPct, 15.0) << "seam-cut cylinder is developable → near-isometric";

    double flatArea = 0.0;
    for (const FlatFace& f : fp.faces)
        for (const FlatLoop& l : f.loops)
            flatArea += (l.isHole ? -1.0 : 1.0) * loopArea(l.pts);
    EXPECT_NEAR(flatArea, 2.0 * M_PI * R * H, 2.0 * M_PI * R * H * 0.10);
}

TEST(Unfold, ConformalClosedSphereProducesFiniteMap) {
    // A sphere is closed AND doubly-curved: the seam-cut must open it to a disk so
    // LSCM yields a real (finite) single-piece map instead of collapsing/NaN.
    TopoDS_Shape sph = BRepPrimAPI_MakeSphere(20.0).Shape();
    TopoDS_Face face;
    for (TopExp_Explorer ex(sph, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        if (BRepAdaptor_Surface(f).GetType() == GeomAbs_Sphere) { face = f; break; }
    }
    ASSERT_FALSE(face.IsNull());

    FlatPattern fp = unfoldConformal({face}, /*maxBevelDeg=*/12.0, 1.0);
    ASSERT_TRUE(fp.ok) << fp.warning;
    EXPECT_EQ(fp.piecesPlaced, 1);
    EXPECT_FALSE(fp.faces.empty());
    EXPECT_TRUE(std::isfinite(fp.distortionPct));

    // A genuine (non-collapsed) map encloses real area on the sheet.
    double flatArea = 0.0;
    for (const FlatFace& f : fp.faces)
        for (const FlatLoop& l : f.loops)
            flatArea += (l.isHole ? -1.0 : 1.0) * loopArea(l.pts);
    EXPECT_GT(flatArea, 100.0);
}

TEST(Unfold, LoftCircleCapDoesNotNestInsidePanels) {
    // A square→circle loft: the round cap shares a quarter-arc with each side
    // panel. The net must hinge it OUT (or split it off), never nest it inside the
    // fanned panels. Asserts no placed face's centroid lands inside another's.
    BRepBuilderAPI_MakePolygon sq;
    sq.Add(gp_Pnt(-20, -20, 0)); sq.Add(gp_Pnt(20, -20, 0));
    sq.Add(gp_Pnt(20, 20, 0));   sq.Add(gp_Pnt(-20, 20, 0));
    sq.Close();
    gp_Circ circ(gp_Ax2(gp_Pnt(0, 0, 40), gp_Dir(0, 0, 1)), 18.0);
    TopoDS_Wire cw = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(circ).Edge());
    BRepOffsetAPI_ThruSections loft(Standard_True);
    loft.AddWire(sq.Wire()); loft.AddWire(cw); loft.Build();
    ASSERT_TRUE(loft.IsDone());
    auto faces = facesOf(loft.Shape());
    ASSERT_GE(faces.size(), 5u);

    FlatPattern fp = unfoldDevelopableNet(faces, 10.0, 1.0);
    ASSERT_TRUE(fp.ok) << fp.warning;

    // Regression guard: the panels must stay essentially ONE connected net (the
    // round cap may at most split off), not fragment into singletons.
    EXPECT_LE(fp.piecesPlaced, 2) << "net fragmented instead of staying connected";

    // And no face may be BURIED under the others (the original "circle inside the
    // rest" bug): the fraction of any face's area covered by the union of the
    // other faces must stay well below 1. Slight overlap (these panels are
    // doubly-curved) is fine; full nesting is not.
    auto outerOf = [](const FlatFace& f) -> const std::vector<glm::dvec2>* {
        for (const auto& l : f.loops) if (!l.isHole) return &l.pts;
        return nullptr;
    };
    auto inPoly = [](const glm::dvec2& pt, const std::vector<glm::dvec2>& poly) {
        bool in = false; const size_t n = poly.size();
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const glm::dvec2& a = poly[i]; const glm::dvec2& b = poly[j];
            if (((a.y > pt.y) != (b.y > pt.y)) &&
                (pt.x < (b.x - a.x) * (pt.y - a.y) / (b.y - a.y) + a.x)) in = !in;
        }
        return in;
    };
    double worstCover = 0.0;
    for (size_t i = 0; i < fp.faces.size(); ++i) {
        const auto* oi = outerOf(fp.faces[i]); if (!oi || oi->size() < 3) continue;
        glm::dvec2 lo{1e300, 1e300}, hi{-1e300, -1e300};
        for (const auto& p : *oi) { lo.x=std::min(lo.x,p.x); lo.y=std::min(lo.y,p.y); hi.x=std::max(hi.x,p.x); hi.y=std::max(hi.y,p.y); }
        const int N = 20; const double cw = (hi.x-lo.x)/N, ch = (hi.y-lo.y)/N;
        if (cw <= 0 || ch <= 0) continue;
        int inside = 0, covered = 0;
        for (int a = 0; a < N; ++a)
            for (int b = 0; b < N; ++b) {
                const glm::dvec2 p{lo.x + (a+0.5)*cw, lo.y + (b+0.5)*ch};
                if (!inPoly(p, *oi)) continue;
                ++inside;
                for (size_t j = 0; j < fp.faces.size(); ++j) {
                    if (i == j) continue;
                    const auto* oj = outerOf(fp.faces[j]);
                    if (oj && oj->size() >= 3 && inPoly(p, *oj)) { ++covered; break; }
                }
            }
        if (inside) worstCover = std::max(worstCover, double(covered) / inside);
    }
    // The round cap meets the net only along arcs, which can't fold flat cleanly,
    // so it's slid just clear of the panel edge - no overlap at all.
    EXPECT_LT(worstCover, 0.02) << "a face overlaps another (cap not cleared)";
}

TEST(Unfold, PlanarFaceWithHoleKeepsHoleLoop) {
    // A plate with a through-hole - the flattened panel must keep the hole loop.
    TopoDS_Shape plate = BRepPrimAPI_MakeBox(40.0, 30.0, 2.0).Shape();
    gp_Ax2 ax(gp_Pnt(20, 15, -1), gp_Dir(0, 0, 1));
    TopoDS_Shape pin = BRepPrimAPI_MakeCylinder(ax, 5.0, 4.0).Shape();
    TopoDS_Shape holed = BRepAlgoAPI_Cut(plate, pin).Shape();

    TopoDS_Face holedFace;
    for (TopExp_Explorer ex(holed, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        if (BRepAdaptor_Surface(f).GetType() != GeomAbs_Plane) continue;
        int wires = 0;
        for (TopExp_Explorer wx(f, TopAbs_WIRE); wx.More(); wx.Next()) ++wires;
        if (wires >= 2) { holedFace = f; break; }
    }
    ASSERT_FALSE(holedFace.IsNull());

    FlatPattern fp = unfoldPlanarFaces({holedFace});
    ASSERT_TRUE(fp.ok) << fp.warning;
    ASSERT_EQ(fp.faces.size(), 1u);
    EXPECT_GE(fp.faces[0].loops.size(), 2u);   // outer + hole
    int holes = 0;
    for (const auto& l : fp.faces[0].loops) if (l.isHole) ++holes;
    EXPECT_EQ(holes, 1);
}

TEST(Unfold, ConformalConeUnwrapsToSectorNotDisk) {
    // A cone is developable. Conformal must slit its apex (a high angle-defect
    // interior vertex) and unwrap it to a near-isometric SECTOR - not wrap the
    // surface 360° around the apex into a full disk, which read ~12000% stretch
    // (the "conformal outputs a blob/circle" bug).
    TopoDS_Shape cone = BRepPrimAPI_MakeCone(20.0, 0.0, 40.0).Shape();
    TopoDS_Face lateral;
    for (TopExp_Explorer ex(cone, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        if (BRepAdaptor_Surface(f).GetType() == GeomAbs_Cone) { lateral = f; break; }
    }
    ASSERT_FALSE(lateral.IsNull());

    FlatPattern fp = unfoldConformal({lateral}, 10.0, 1.0);
    ASSERT_TRUE(fp.ok) << fp.warning;
    EXPECT_LT(fp.distortionPct, 5.0) << "cone must unwrap to a sector, not a wrapped disk";
}

// ── Diagnostic (DISABLED): dump conformal vs developable output for several
// shapes, with a self-intersection count, so the actual geometry can be eyeballed.
// Run: ./tests/test_unfold --gtest_also_run_disabled_tests --gtest_filter='*DiagConformal*'
namespace {
int countSelfIntersections(const FlatPattern& fp) {
    struct Seg { glm::dvec2 a, b; };
    std::vector<Seg> segs;
    for (const auto& f : fp.faces)
        for (const auto& l : f.loops) {
            const auto& p = l.pts;
            for (size_t i = 0; i < p.size(); ++i) segs.push_back({p[i], p[(i + 1) % p.size()]});
        }
    auto crs = [](glm::dvec2 a, glm::dvec2 b) { return a.x * b.y - a.y * b.x; };
    auto side = [&](glm::dvec2 a, glm::dvec2 b, glm::dvec2 x) { return crs(b - a, x - a); };
    auto X = [&](const Seg& s, const Seg& t) {
        if (glm::length(s.a - t.a) < 1e-6 || glm::length(s.a - t.b) < 1e-6 ||
            glm::length(s.b - t.a) < 1e-6 || glm::length(s.b - t.b) < 1e-6) return false;
        const double d1 = side(t.a, t.b, s.a), d2 = side(t.a, t.b, s.b);
        const double d3 = side(s.a, s.b, t.a), d4 = side(s.a, s.b, t.b);
        return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
    };
    int n = 0;
    for (size_t i = 0; i < segs.size(); ++i)
        for (size_t j = i + 1; j < segs.size(); ++j)
            if (X(segs[i], segs[j])) ++n;
    return n;
}
void dumpJson(const std::string& path, const FlatPattern& fp) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "{\"loops\":[");
    bool first = true;
    for (const auto& face : fp.faces)
        for (const auto& l : face.loops) {
            if (!first) std::fprintf(f, ","); first = false;
            std::fprintf(f, "[");
            for (size_t i = 0; i < l.pts.size(); ++i) {
                if (i) std::fprintf(f, ",");
                std::fprintf(f, "[%.4f,%.4f]", l.pts[i].x, l.pts[i].y);
            }
            std::fprintf(f, "]");
        }
    std::fprintf(f, "],\"folds\":[");
    for (size_t i = 0; i < fp.folds.size(); ++i) {
        if (i) std::fprintf(f, ",");
        std::fprintf(f, "[%.4f,%.4f,%.4f,%.4f]", fp.folds[i].a.x, fp.folds[i].a.y, fp.folds[i].b.x, fp.folds[i].b.y);
    }
    std::fprintf(f, "]}");
    std::fclose(f);
}
std::vector<TopoDS_Face> typedFaces(const TopoDS_Shape& s, GeomAbs_SurfaceType want) {
    std::vector<TopoDS_Face> out;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        if (BRepAdaptor_Surface(f).GetType() == want) out.push_back(f);
    }
    return out;
}
} // namespace

TEST(Unfold, DISABLED_DiagConformal) {
    struct Case { std::string name; std::vector<TopoDS_Face> faces; };
    std::vector<Case> cases;
    cases.push_back({"open_cyl",   typedFaces(BRepPrimAPI_MakeCylinder(20, 50, M_PI / 2).Shape(), GeomAbs_Cylinder)});
    cases.push_back({"closed_cyl", typedFaces(BRepPrimAPI_MakeCylinder(20, 50).Shape(), GeomAbs_Cylinder)});
    cases.push_back({"cone",       typedFaces(BRepPrimAPI_MakeCone(20, 0, 40).Shape(), GeomAbs_Cone)});
    cases.push_back({"sphere",     typedFaces(BRepPrimAPI_MakeSphere(20.0).Shape(), GeomAbs_Sphere)});
    {
        BRepBuilderAPI_MakePolygon sq;
        sq.Add(gp_Pnt(-20, -20, 0)); sq.Add(gp_Pnt(20, -20, 0));
        sq.Add(gp_Pnt(20, 20, 0));   sq.Add(gp_Pnt(-20, 20, 0)); sq.Close();
        gp_Circ circ(gp_Ax2(gp_Pnt(0, 0, 40), gp_Dir(0, 0, 1)), 18.0);
        TopoDS_Wire cw = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(circ).Edge());
        BRepOffsetAPI_ThruSections loft(Standard_True);
        loft.AddWire(sq.Wire()); loft.AddWire(cw); loft.Build();
        cases.push_back({"loft", facesOf(loft.Shape())});
    }

    const std::string diagDir = mzrtest::tmpSubdir("cdiag");
    std::fprintf(stderr, "\n%-11s %-12s ok pcs  dist%%  curv  loops  pts  selfX\n", "shape", "method");
    for (auto& c : cases) {
        if (c.faces.empty()) { std::fprintf(stderr, "%-11s (no faces)\n", c.name.c_str()); continue; }
        auto report = [&](const char* m, const FlatPattern& fp) {
            size_t pts = 0, loops = 0;
            for (auto& f : fp.faces) { loops += f.loops.size(); for (auto& l : f.loops) pts += l.pts.size(); }
            std::fprintf(stderr, "%-11s %-12s %d  %d  %6.1f %6.1f %5zu %5zu %6d\n",
                         c.name.c_str(), m, fp.ok ? 1 : 0, fp.piecesPlaced, fp.distortionPct,
                         fp.curvatureDeg, loops, pts, countSelfIntersections(fp));
            dumpJson(diagDir + "/" + c.name + "-" + m + ".json", fp);
        };
        report("conformal",   unfoldConformal(c.faces, 10.0, 1.0));
        report("developable", unfoldDevelopableNet(c.faces, 10.0, 1.0));
    }
}
