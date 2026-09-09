#include "MergeFacesOp.h"
#include "FaceLineage.h"
#include "UnifyTolerance.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepTools_History.hxx>
#include <GProp_GProps.hxx>
#include <ShapeBuild_ReShape.hxx>
#include <ShapeFix_FixSmallFace.hxx>
#include <ShapeFix_Shape.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopTools_MapOfShape.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <imgui.h>
#include "../i18n.h"

namespace {

int faceCount(const TopoDS_Shape& s) {
    if (s.IsNull()) return 0;
    TopTools_IndexedMapOfShape m;
    TopExp::MapShapes(s, TopAbs_FACE, m);
    return m.Extent();
}

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

bool faceNormalPoint(const TopoDS_Face& f, gp_Dir& outN, gp_Pnt& outP) {
    try {
        BRepGProp_Face gf(f);
        Standard_Real u0, u1, v0, v1; gf.Bounds(u0, u1, v0, v1);
        gp_Pnt p; gp_Vec n;
        gf.Normal(0.5 * (u0 + u1), 0.5 * (v0 + v1), p, n);
        if (n.Magnitude() < 1e-9) return false;
        outN = gp_Dir(n);
        outP = p;
        return true;
    } catch (...) { return false; }
}

bool faceInShape(const TopoDS_Shape& face, const TopoDS_Shape& shape) {
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
        if (ex.Current().IsSame(face)) return true;
    return false;
}

// Every edge of the body EXCEPT the ones shared by two picked faces.
//
// KeepShapes is how the merge is bounded, and it is consulted for edges, not
// faces (ShapeUpgrade_UnifySameDomain only ever asks myKeepShapes.Contains on
// an edge). That is the right handle anyway: the seam the user is pointing at
// IS an edge, so protecting all the others means nothing outside the picked
// set can dissolve no matter how far the tolerance is loosened.
TopTools_MapOfShape edgesToKeep(const TopoDS_Shape& body,
                                const std::vector<TopoDS_Shape>& faces) {
    TopTools_MapOfShape picked;
    for (const auto& f : faces) picked.Add(f);

    TopTools_MapOfShape keep;
    TopTools_IndexedDataMapOfShapeListOfShape e2f;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, e2f);
    for (int i = 1; i <= e2f.Extent(); ++i) {
        const TopTools_ListOfShape& fl = e2f.FindFromIndex(i);
        bool between = false;
        if (fl.Extent() == 2) {
            TopTools_ListOfShape::Iterator it(fl);
            const TopoDS_Shape f1 = it.Value(); it.Next();
            const TopoDS_Shape f2 = it.Value();
            between = !f1.IsSame(f2) && picked.Contains(f1) && picked.Contains(f2);
        }
        if (!between) keep.Add(e2f.FindKey(i));
    }
    return keep;
}

struct Attempt {
    TopoDS_Shape shape;
    Handle(BRepTools_History) history;
    Handle(ShapeBuild_ReShape) fixContext;   // set only when ShapeFix ran
};

// One unify pass, plus a repair pass if the result came out invalid.
//
// Merging two planes that are a fraction of a degree apart leaves the surviving
// plane not quite carrying the other's boundary, which BRepCheck rejects.
// ShapeFix closes that gap; measured on the nacelle it rescues most of the
// seams that would otherwise be unmergeable (10 of 41 at 1e-2). The caller
// still has to accept the result - this only produces a candidate.
Attempt runUnify(const TopoDS_Shape& body, const TopTools_MapOfShape* keep,
                 double angularTol, double linearTol) {
    Attempt out;
    try {
        // concatBSplines deliberately FALSE. With it on, unify re-fits spline
        // surfaces rather than just dropping redundant boundaries, and on a
        // part with curved faces that MOVES geometry - measured on the nacelle
        // as a 5.5e-6 relative volume change, which the guard below rejected.
        // A repair must not reshape the part.
        ShapeUpgrade_UnifySameDomain u(body, /*edges=*/true, /*faces=*/true,
                                       /*concatBSplines=*/false);
        u.SetAngularTolerance(angularTol);
        // Left at OCCT's default (Precision::Confusion, 1e-7 mm) unless asked.
        // See kFaceScopedRungs for why a looser one is sometimes needed.
        if (linearTol > 0.0) u.SetLinearTolerance(linearTol);
        if (keep) u.KeepShapes(*keep);
        u.Build();
        TopoDS_Shape r = u.Shape();
        if (r.IsNull()) return out;
        out.history = u.History();

        if (!BRepCheck_Analyzer(r).IsValid()) {
            ShapeFix_Shape fix(r);
            fix.Perform();
            const TopoDS_Shape fixed = fix.Shape();
            if (fixed.IsNull() || !BRepCheck_Analyzer(fixed).IsValid()) return out;
            r = fixed;
            out.fixContext = fix.Context();
        }
        out.shape = r;
    } catch (...) {
        out = Attempt{};
    }
    return out;
}

// Is this candidate a merge we are willing to keep?
//
// Volume is checked at 1e-4 RELATIVE, not tighter. BRepGProp integrates over
// the faces, so its answer depends on how the boundary is subdivided - merging
// 189 faces into 183 moves the result by ~5e-6 relative on the nacelle with
// nothing geometrically changed. That is the integrator's own repeatability,
// not a reshape. A merge that actually moved material is orders of magnitude
// larger than this bound, and that is what stops the loosened face-scoped
// tolerance from quietly reshaping the part.
bool acceptable(const TopoDS_Shape& before, const Attempt& a, int facesBefore) {
    if (a.shape.IsNull()) return false;
    if (faceCount(a.shape) >= facesBefore) return false;
    try {
        const double v0 = volumeOf(before), v1 = volumeOf(a.shape);
        const double tol = 1e-4 * std::max(1.0, std::fabs(v0));
        if (std::fabs(v0 - v1) > tol) {
            std::fprintf(stderr, "[MergeFaces] volume changed %.6f -> %.6f; rejecting.\n",
                         v0, v1);
            return false;
        }
    } catch (...) { return false; }
    return true;
}

// Escalation ladder for a face-scoped merge. The scope is bounded to the picked
// faces' shared edges, so a loose tolerance can only affect what the user
// pointed at - and it has to be loose: on the nacelle NONE of the 41 surviving
// seams were within 1e-6, they sit between 1e-4 and 1e-2 rad. Tightest first, so
// a genuinely coplanar pair is still merged the conservative way.
//
// ANGULAR IS ONLY HALF OF IT. UnifySameDomain also has a LINEAR tolerance, and
// it was never set here, so it sat at OCCT's default Precision::Confusion() =
// 1e-7 mm. That decides whether two PARALLEL planes count as the same plane --
// a completely different axis from how far their normals may differ.
//
// Measured on robot dog cover.mzr, body "Extrude", the two faces of one flat
// surface a user could not merge:
//
//     face 55 plane Y = 85.700003054738048
//     face 56 plane Y = 85.700000000000003
//     separation      = 3.054738e-06 mm     -- 30x the default tolerance
//
// Their normals agree exactly; they share a 127.76 mm continuous boundary; and
// unify refused at EVERY angular rung, because the planes are 3 nanometres
// apart. That gap is the float32 error on 85.7 (see sliverBetween() above): the
// same single-precision sketch coordinate that leaves the slivers also leaves
// the two planes a hair apart, and removing the slivers does not move them.
//
// So the ladder escalates on both axes, tightest first. A linear rung of 0 means
// "leave OCCT's default"; the guard in accept() still vets every result, and a
// merge across a 3 nm step moves ~7.6e-9 of the volume -- four orders inside it.
struct FaceScopedRung { double angular; double linear; };
const FaceScopedRung kFaceScopedRungs[] = {
    {1e-9, 0.0}, {1e-6, 0.0}, {1e-4, 0.0}, {1e-3, 0.0}, {1e-2, 0.0},
    {1e-9, 1e-5}, {1e-6, 1e-5}, {1e-4, 1e-4}, {1e-2, 1e-3},
};

MergeFacesOp::Refusal g_lastRefusal = MergeFacesOp::Refusal::None;

// A planar face's outward normal.
//
// This MUST come from the surface parameterisation, not from the plane's stored
// Axis().Direction(): a Geom_Plane may be built with its axis ANTI-PARALLEL to
// dU x dV, which is legal and which no orientation flag corrects. On the part
// this was found on, 5 of 51 planar faces were like that, and reading the axis
// reported them backwards -- so two faces of one flat surface looked 180 deg
// apart and this very check refused to merge them, telling the user they
// "point in opposite directions" when they plainly did not.
//
// faceNormalPoint() above already does it correctly. Planar faces have a
// constant normal, so its UV-midpoint sample is exact even for a face whose
// midpoint falls in a hole.
bool outwardNormal(const TopoDS_Shape& s, gp_Dir& n) {
    if (s.IsNull() || s.ShapeType() != TopAbs_FACE) return false;
    const TopoDS_Face f = TopoDS::Face(s);
    if (BRepAdaptor_Surface(f).GetType() != GeomAbs_Plane) return false;
    gp_Pnt p;
    return faceNormalPoint(f, n, p);
}

// Is a degenerate sliver face wedged between two of the picked faces?
//
// Sketch point coordinates are stored as glm::vec2, i.e. 32-bit floats, while
// the kernel works in double. 85.7 has no float32 representation: it becomes
// 85.699996948, which is 3.05e-6 mm out. A sketch snapped to an edge at that
// coordinate therefore lands 3 NANOMETRES short, and the push/pull built from
// it leaves a strip face that thin between the two surfaces.
//
// Measured on robot dog cover.mzr, body "Extrude": three such faces, 97.00 mm
// and 15.38 mm long, each exactly 3.0546e-6 mm wide -- the same figure as the
// float32 error on 85.7. The two faces either side visibly touch and are
// coplanar, but share no edge, because those strips are between them. Merge
// then refuses and no tolerance can help: the slivers are real faces.
bool sliverBetween(const TopoDS_Shape& body,
                   const std::vector<TopoDS_Shape>& faces, double maxArea) {
    TopTools_MapOfShape picked;
    for (const auto& f : faces) picked.Add(f);
    TopTools_IndexedDataMapOfShapeListOfShape e2f;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, e2f);
    // faces adjacent to at least two DIFFERENT picked faces
    TopTools_IndexedMapOfShape all;
    TopExp::MapShapes(body, TopAbs_FACE, all);
    for (int i = 1; i <= all.Extent(); ++i) {
        const TopoDS_Shape& cand = all(i);
        if (picked.Contains(cand)) continue;
        GProp_GProps g;
        try { BRepGProp::SurfaceProperties(cand, g); } catch (...) { continue; }
        if (g.Mass() > maxArea) continue;
        int touches = 0;
        TopTools_MapOfShape seen;
        for (TopExp_Explorer ex(cand, TopAbs_EDGE); ex.More(); ex.Next()) {
            if (!e2f.Contains(ex.Current())) continue;
            const TopTools_ListOfShape& fl = e2f.FindFromKey(ex.Current());
            for (TopTools_ListIteratorOfListOfShape it(fl); it.More(); it.Next())
                if (picked.Contains(it.Value()) && seen.Add(it.Value())) ++touches;
        }
        if (touches >= 2) return true;
    }
    return false;
}

// Drop those slivers. Refuses anything that changes the part.
TopoDS_Shape withoutSlivers(const TopoDS_Shape& body) {
    try {
        ShapeFix_FixSmallFace sff;
        sff.Init(body);
        sff.SetPrecision(1e-3);
        sff.Perform();
        const TopoDS_Shape r = sff.FixShape();
        if (r.IsNull() || !BRepCheck_Analyzer(r).IsValid()) return TopoDS_Shape();
        const double v0 = volumeOf(body), v1 = volumeOf(r);
        if (std::fabs(v0 - v1) > 1e-4 * std::max(1.0, std::fabs(v0))) {
            std::fprintf(stderr, "[MergeFaces] sliver removal moved volume "
                                 "%.6f -> %.6f; keeping the original.\n", v0, v1);
            return TopoDS_Shape();
        }
        return r;
    } catch (...) { return TopoDS_Shape(); }
}

// Structural reasons a picked set can never merge, whatever the tolerance.
// Checked BEFORE the ladder so the message names the real cause instead of
// blaming proximity for something proximity has nothing to do with.
MergeFacesOp::Refusal diagnosePick(const TopoDS_Shape& body,
                                   const std::vector<TopoDS_Shape>& faces) {
    // Antiparallel: the two faces bound material on OPPOSITE sides, so there is
    // no single face that could replace them -- the solid would have to exist on
    // both sides of it. Measured on a real part (robot dog cover, body
    // "Extrude"): two coplanar faces, centres 0.47 mm apart, outward normals
    // exactly 180 deg apart. Reported as "not close enough", which is the one
    // thing it was not: they are the same plane to 3e-6 mm.
    for (std::size_t i = 0; i < faces.size(); ++i) {
        gp_Dir ni;
        if (!outwardNormal(faces[i], ni)) continue;
        for (std::size_t j = i + 1; j < faces.size(); ++j) {
            gp_Dir nj;
            if (!outwardNormal(faces[j], nj)) continue;
            if (ni.Angle(nj) > 170.0 * M_PI / 180.0)
                return MergeFacesOp::Refusal::OppositeNormals;
        }
    }
    // No shared edge: unify merges by DISSOLVING the edge between two faces, so
    // with nothing between them there is nothing to dissolve. Two faces a hair
    // apart with a step wall between them land here, and loosening the
    // tolerance will never help -- the fix is to move one of them first.
    TopTools_MapOfShape picked;
    for (const auto& f : faces) picked.Add(f);
    TopTools_IndexedDataMapOfShapeListOfShape e2f;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, e2f);
    for (int i = 1; i <= e2f.Extent(); ++i) {
        const TopTools_ListOfShape& fl = e2f.FindFromIndex(i);
        if (fl.Extent() != 2) continue;
        TopTools_ListOfShape::Iterator it(fl);
        const TopoDS_Shape f1 = it.Value(); it.Next();
        const TopoDS_Shape f2 = it.Value();
        if (!f1.IsSame(f2) && picked.Contains(f1) && picked.Contains(f2))
            return MergeFacesOp::Refusal::None;      // adjacent: tolerance is genuinely in play
    }
    return MergeFacesOp::Refusal::NotAdjacent;
}

} // namespace

MergeFacesOp::MergeFacesOp() = default;

void MergeFacesOp::setBody(int id) { m_bodyId = id; }

void MergeFacesOp::setFaces(const std::vector<TopoDS_Shape>& faces) {
    m_faces.clear();
    for (const auto& f : faces)
        if (!f.IsNull() && f.ShapeType() == TopAbs_FACE) m_faces.push_back(f);
}

void MergeFacesOp::captureAnchors(const TopoDS_Shape& body) {
    if (!m_anchors.empty() || m_faces.empty() || body.IsNull()) return;
    for (const auto& f : m_faces) {
        gp_Dir n; gp_Pnt p;
        if (faceNormalPoint(TopoDS::Face(f), n, p)) m_anchors.push_back({n, p});
    }
}

bool MergeFacesOp::rebindFaces(const TopoDS_Shape& body) {
    if (body.IsNull()) return false;

    // Fast path: every picked face is still a live face of this body.
    if (!m_faces.empty()) {
        bool allLive = true;
        for (const auto& f : m_faces)
            if (!faceInShape(f, body)) { allLive = false; break; }
        if (allLive) return true;
    }

    // Replay onto a rebuilt body: find each anchor's face again by orientation
    // first, then nearness. Same scheme as ShellOp - good enough because the
    // picked faces are, by definition, ones the user could see and click.
    if (m_anchors.empty()) return false;
    std::vector<TopoDS_Shape> rebound;
    for (const FaceAnchor& a : m_anchors) {
        TopoDS_Shape best; double bestScore = -1e18;
        for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
            gp_Dir n; gp_Pnt p;
            if (!faceNormalPoint(TopoDS::Face(ex.Current()), n, p)) continue;
            const double dot = n.Dot(a.normal);
            if (dot < 0.9) continue;
            const double dist = a.point.Distance(p);
            const double score = dot - 0.01 * dist;
            if (score > bestScore) { bestScore = score; best = ex.Current(); }
        }
        if (best.IsNull()) return false;
        // Two anchors landing on the same face means the merge already happened
        // upstream; nothing left to do, and merging a face with itself is not a
        // thing we can ask for.
        for (const auto& r : rebound) if (r.IsSame(best)) return false;
        rebound.push_back(best);
    }
    m_faces = std::move(rebound);
    return true;
}

MergeFacesOp::Refusal MergeFacesOp::lastRefusal() { return g_lastRefusal; }
void MergeFacesOp::resetLastRefusal() { g_lastRefusal = Refusal::None; }

bool MergeFacesOp::execute(Document& doc) {
    g_lastRefusal = Refusal::Internal;
    if (m_bodyId < 0) return false;
    try {
        m_previousShape = doc.getBody(m_bodyId);
        if (m_previousShape.IsNull()) return false;
        m_facesBefore = faceCount(m_previousShape);

        // UnifySameDomain edits its input in place when the merge goes wrong
        // (see UnifyTolerance.h), and runUnify below is handed the LIVE body -
        // up to five times on the face-scoped ladder. Without a spare, a merge
        // this op then refuses would leave the body silently reshaped: the user
        // is told nothing merged, and the part is already wrong. A merge that
        // succeeds does not touch its input, so this only ever pays off on the
        // path that was about to corrupt something.
        TopoDS_Shape spare;
        try { spare = BRepBuilderAPI_Copy(m_previousShape).Shape(); } catch (...) {}

        const bool faceScoped = isFaceScoped();
        if (faceScoped) {
            if (!rebindFaces(m_previousShape)) {
                // Say WHY, because "couldn't find those faces" with no detail is
                // impossible to act on: it covers both a stale selection and a
                // failed anchor re-bind, which have nothing to do with each other.
                int live = 0;
                for (const auto& f : m_faces)
                    if (faceInShape(f, m_previousShape)) ++live;
                std::fprintf(stderr,
                    "[MergeFaces] re-bind FAILED: body %d has %d faces; %zu picked, "
                    "%d of them still live; %zu anchor(s) stored.\n",
                    m_bodyId, faceCount(m_previousShape), m_faces.size(), live,
                    m_anchors.size());
                g_lastRefusal = Refusal::FacesNotFound; return false;
            }
            if (m_faces.size() < 2) {
                g_lastRefusal = Refusal::NeedTwoFaces; return false;
            }
            captureAnchors(m_previousShape);
        }

        // Why this pick cannot work, if it cannot. Worked out before the ladder
        // runs, because these are properties of the selection rather than of any
        // tolerance, and the ladder's failure looks identical in every case.
        Refusal pickIssue =
            faceScoped ? diagnosePick(m_previousShape, m_faces) : Refusal::None;
        if (faceScoped) {
            const char* what = pickIssue == Refusal::None            ? "adjacent, tolerance decides"
                             : pickIssue == Refusal::OppositeNormals ? "outward normals oppose"
                             : pickIssue == Refusal::NotAdjacent     ? "no shared edge"
                                                                     : "other";
            std::fprintf(stderr, "[MergeFaces] body %d: %zu picked face(s) of %d; pick reads as: %s\n",
                         m_bodyId, m_faces.size(), m_facesBefore, what);
        }

        // "They don't touch" is sometimes a 3-nanometre lie: see sliverBetween()
        // above. If a degenerate strip is wedged between the picked faces, drop
        // it and look again. The picked faces are re-found on the cleaned body
        // through the same anchors a replay uses, and if that does not leave
        // them genuinely adjacent the original body is kept untouched.
        TopoDS_Shape workShape = m_previousShape;
        if (faceScoped && pickIssue == Refusal::NotAdjacent &&
            sliverBetween(m_previousShape, m_faces, 1e-3)) {
            const TopoDS_Shape cleaned = withoutSlivers(m_previousShape);
            if (!cleaned.IsNull()) {
                const std::vector<TopoDS_Shape> saved = m_faces;
                if (rebindFaces(cleaned) && m_faces.size() >= 2 &&
                    diagnosePick(cleaned, m_faces) == Refusal::None) {
                    std::fprintf(stderr, "[MergeFaces] removed degenerate sliver "
                                         "face(s) between the picked faces.\n");
                    workShape = cleaned;
                    pickIssue = Refusal::None;
                } else {
                    m_faces = saved;
                }
            }
        }
        const int facesInWork = faceCount(workShape);

        // A merge the guard threw out is a different story from one that never
        // happened: the faces ARE one surface, but joining them would have moved
        // material. Worth saying so rather than implying they were too far apart.
        bool guardRejected = false;

        Attempt best;
        if (faceScoped) {
            const TopTools_MapOfShape keep = edgesToKeep(workShape, m_faces);
            for (const FaceScopedRung& rung : kFaceScopedRungs) {
                Attempt a = runUnify(workShape, &keep, rung.angular, rung.linear);
                if (acceptable(workShape, a, facesInWork)) { best = a; break; }
                if (!a.shape.IsNull() && faceCount(a.shape) < facesInWork)
                    guardRejected = true;
            }
            // The sliver removal alone is a real simplification, so keep it even
            // when nothing further merged -- the faces now touch, which is what
            // lets a fillet run across them.
            if (best.shape.IsNull() && facesInWork < m_facesBefore)
                best.shape = workShape;
        } else {
            // Whole-body stays conservative on BOTH axes: it is not the user
            // asserting anything about a particular pair.
            Attempt a = runUnify(workShape, nullptr, materializr::kUnifyAngularTol, 0.0);
            if (acceptable(workShape, a, facesInWork)) best = a;
        }
        // Nothing merged. Refusing means History::pushOperation declines and no
        // step is added - a merge that did nothing should not litter the
        // timeline, and the caller says so instead.
        if (best.shape.IsNull()) {
            g_lastRefusal = pickIssue != Refusal::None ? pickIssue
                          : (guardRejected ? Refusal::Unsafe : Refusal::NotSameSurface);
            std::fprintf(stderr, "[MergeFaces] nothing merged (%d faces in, guard %s).\n",
                         facesInWork, guardRejected ? "rejected a candidate" : "never fired");
            if (!spare.IsNull()) {
                doc.updateBody(m_bodyId, spare);
                m_previousShape = spare;
            }
            return false;
        }
        g_lastRefusal = Refusal::None;

        m_facesAfter = faceCount(best.shape);

        // Carry the body's face lineage across the merge BEFORE updateBody
        // clears it: a merged face inherits both parents' ids, so a downstream
        // fillet/chamfer that named one of them still resolves. Without this
        // the repair would silently drift every reference on the body.
        materializr::topo::FaceIdMap carried;
        bool haveMap = false;
        if (const auto* im = doc.bodyFaceIds(m_bodyId)) {
            carried = materializr::topo::carryThrough(*im, best.history, best.shape);
            // Unify's history describes the shape it built. If ShapeFix then
            // rebuilt faces to close the gap, those ids point at faces that are
            // no longer in the body - follow the repair's own substitutions
            // rather than leaving the map half-stale.
            if (!best.fixContext.IsNull()) {
                materializr::topo::FaceIdMap remapped;
                for (const auto& e : carried) {
                    TopoDS_Shape f = e.face;
                    if (best.fixContext->IsRecorded(f)) f = best.fixContext->Value(f);
                    if (f.IsNull() || !faceInShape(f, best.shape)) continue;
                    for (int id : e.ids) materializr::topo::addId(remapped, f, id);
                }
                carried = std::move(remapped);
            }
            haveMap = true;
        }

        doc.updateBody(m_bodyId, best.shape);
        if (haveMap) doc.setBodyFaceIds(m_bodyId, std::move(carried));
        return true;
    } catch (...) {
        return false;
    }
}

bool MergeFacesOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) return false;
    try {
        doc.updateBody(m_bodyId, m_previousShape);
        return true;
    } catch (...) {
        return false;
    }
}

std::string MergeFacesOp::description() const {
    return "Merge faces (" + std::to_string(m_facesBefore) + " \xE2\x86\x92 " +
           std::to_string(m_facesAfter) + ")";
}

void MergeFacesOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Merge Faces"));
    ImGui::Separator();
    ImGui::Text(materializr::tr("Scope: %s"), isFaceScoped() ? "selected faces" : "whole body");
    ImGui::Text(materializr::tr("Faces: %d -> %d"), m_facesBefore, m_facesAfter);
    ImGui::Text(materializr::tr("Body ID: %d"), m_bodyId);
}

std::string MergeFacesOp::serializeParams() const {
    std::string s = "body=" + std::to_string(m_bodyId) +
                    ";before=" + std::to_string(m_facesBefore) +
                    ";after=" + std::to_string(m_facesAfter);
    // The picked faces have no stable name on an imported body, so persist the
    // anchors instead and re-find them on replay.
    char buf[192];
    for (const auto& a : m_anchors) {
        std::snprintf(buf, sizeof(buf), ";anchor=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
                      a.normal.X(), a.normal.Y(), a.normal.Z(),
                      a.point.X(), a.point.Y(), a.point.Z());
        s += buf;
    }
    return s;
}

bool MergeFacesOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        const std::string key = blob.substr(pos, eq - pos);
        const std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "body")   { m_bodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "before") { m_facesBefore = std::atoi(val.c_str()); }
        else if (key == "after")  { m_facesAfter = std::atoi(val.c_str()); }
        else if (key == "anchor") {
            double v[6] = {0, 0, 0, 0, 0, 0};
            if (std::sscanf(val.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf",
                            &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
                try {
                    m_anchors.push_back({gp_Dir(v[0], v[1], v[2]),
                                         gp_Pnt(v[3], v[4], v[5])});
                } catch (...) {}   // a zero-length normal: drop that anchor
            }
        }
        pos = end + 1;
    }
    return any;
}

bool MergeFacesOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_bodyId < 0) return false;
    m_previousShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    // The picked faces are re-found from the anchors at execute() time, against
    // whatever the body has become by this point in the replay - the stored
    // TopoDS_Shapes belong to a session that no longer exists.
    m_faces.clear();
    return !m_previousShape.IsNull();
}

OperationDiff MergeFacesOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}
