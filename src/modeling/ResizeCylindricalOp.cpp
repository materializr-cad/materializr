#include "ui/LengthField.h"
#include "ResizeCylindricalOp.h"
#include "../core/Verbose.h"

#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Bnd_Box.hxx>
#include <BRep_Tool.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Plane.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <TopoDS.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include "UnifyTolerance.h"

#include <imgui.h>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"

// Per-op diagnostic log. Off unless the user passed --verbose; under verbose,
// stderr is redirected to /tmp/materializr.log (or --log <path>), so these
// land in a file that survives the session.
#define MZLOG(...) \
    do { if (::materializr::isVerbose()) std::fprintf(stderr, __VA_ARGS__); } while (0)

namespace {

// (Removed in 0.3.0: detectCap / CapInfo / makeRevolvedFill. The Option-A
//  chamfer-aware revolved fill was abandoned in favour of the simpler
//  topology-agnostic cap-following half-space / prism trim implemented in
//  execute() below — which handles planar (incl. tilted) AND curved caps
//  without classifying the cap surface type.)


// Locate the cap face adjacent to the body's cylindrical hole face at the
// extremal-V wire edge (max V if topEdge, min V if !topEdge). Returns a null
// face if no match is found. Used by the cap-following fill to build a
// half-space that confines the fill to the body's natural axial extent — works
// for plane / cone / sphere / NURBS / anything, because we don't interpret
// the surface, we just use the face to bound a half-space.
TopoDS_Face findCapFace(const TopoDS_Shape& body, const gp_Ax2& cylAxis,
                        double cylRadius, bool topEdge) {
    if (body.IsNull()) return {};
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, edgeFaces);

    // Find the cylindrical face by AXIS LINE + radius. The caller's axis has
    // been shifted by v1 (the face's V_min) from the body's surface origin so
    // comparing point locations directly fails — instead, accept any cylinder
    // whose location lies anywhere on our axis line.
    TopoDS_Face cylFace;
    for (TopExp_Explorer fex(body, TopAbs_FACE); fex.More(); fex.Next()) {
        TopoDS_Face face = TopoDS::Face(fex.Current());
        Handle(Geom_CylindricalSurface) cs =
            Handle(Geom_CylindricalSurface)::DownCast(BRep_Tool::Surface(face));
        if (cs.IsNull()) continue;
        gp_Cylinder gc = cs->Cylinder();
        if (std::abs(gc.Radius() - cylRadius) > 1e-3) continue;
        if (std::abs(std::abs(gc.Position().Direction().Dot(cylAxis.Direction())) - 1.0) > 1e-3)
            continue;
        gp_Vec off(cylAxis.Location(), gc.Position().Location());
        gp_Vec axV(cylAxis.Direction());
        double along = off.Dot(axV);
        gp_Vec perp  = off - axV * along;
        if (perp.Magnitude() > 1e-3) continue;
        cylFace = face;
        break;
    }
    if (cylFace.IsNull()) return {};

    Handle(Geom_CylindricalSurface) bodyCs =
        Handle(Geom_CylindricalSurface)::DownCast(BRep_Tool::Surface(cylFace));
    gp_Pnt bodyOrigin = bodyCs->Cylinder().Position().Location();
    gp_Vec bodyDir(bodyCs->Cylinder().Position().Direction());

    // Pick the extremal-V WIRE EDGE (max V for top, min V for bottom). UVBounds
    // can over-report the parametric domain (see body 34 in the diagnostics
    // log: V_range=[42.83, 51] but the actual face wire's lowest edge is at
    // V=46.13). The wire edge is what we actually want.
    TopoDS_Edge extremalEdge;
    double extremalV = topEdge ? -1e30 : 1e30;
    for (TopExp_Explorer eex(cylFace, TopAbs_EDGE); eex.More(); eex.Next()) {
        TopoDS_Edge edge = TopoDS::Edge(eex.Current());
        try {
            BRepAdaptor_Curve curve(edge);
            double mid = 0.5 * (curve.FirstParameter() + curve.LastParameter());
            gp_Pnt center; curve.D0(mid, center);
            double v = gp_Vec(bodyOrigin, center).Dot(bodyDir);
            if ((topEdge && v > extremalV) || (!topEdge && v < extremalV)) {
                extremalV  = v;
                extremalEdge = edge;
            }
        } catch (...) { continue; }
    }
    if (extremalEdge.IsNull() || !edgeFaces.Contains(extremalEdge)) return {};

    for (TopTools_ListOfShape::Iterator it(edgeFaces.FindFromKey(extremalEdge));
         it.More(); it.Next()) {
        TopoDS_Face f = TopoDS::Face(it.Value());
        if (f.IsSame(cylFace)) continue;
        MZLOG(
            "[Resize] findCapFace: V=%.3f (topEdge=%d)\n",
            extremalV, topEdge ? 1 : 0);
        return f;
    }
    return {};
}


// MakeCone with equal R1/R2 has degenerate edge cases — use MakeCylinder
// where the radii match exactly, MakeCone where they differ.
// `BRepPrimAPI_Make*` primitives report IsDone() == false until Build() is
// explicitly called; the lazy Shape() accessor builds on demand but checking
// IsDone() pre-build looks like a failure. Build() first, then check.
TopoDS_Shape makeRevolveSolid(const gp_Ax2& axis, double r1, double r2, double h, bool* ok) {
    *ok = false;
    try {
        if (std::abs(r1 - r2) < 1e-7) {
            BRepPrimAPI_MakeCylinder mk(axis, r1, h);
            mk.Build();
            if (!mk.IsDone()) return TopoDS_Shape();
            *ok = true;
            return mk.Shape();
        }
        BRepPrimAPI_MakeCone mk(axis, r1, r2, h);
        mk.Build();
        if (!mk.IsDone()) return TopoDS_Shape();
        *ok = true;
        return mk.Shape();
    } catch (...) {
        return TopoDS_Shape();
    }
}
}

void ResizeCylindricalOp::setBody(int bodyId)         { m_bodyId = bodyId; }
void ResizeCylindricalOp::setAxis(const gp_Ax2& axis) { m_axis = axis; }
void ResizeCylindricalOp::setHeight(double h)         { m_height = h; }
void ResizeCylindricalOp::setOldRadii(double bottomR, double topR) {
    m_oldBottomR = bottomR; m_oldTopR = topR;
}
void ResizeCylindricalOp::setNewRadii(double bottomR, double topR) {
    m_newBottomR = bottomR; m_newTopR = topR;
}
void ResizeCylindricalOp::setIsHole(bool h)           { m_isHole = h; }

bool ResizeCylindricalOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_height <= 0.0) {
        MZLOG("[Resize] bad params: bodyId=%d h=%.3f\n",
                     m_bodyId, m_height);
        return false;
    }
    if (m_newBottomR <= 0.0 || m_newTopR <= 0.0) {
        MZLOG("[Resize] non-positive new radii: bot=%.3f top=%.3f\n",
                     m_newBottomR, m_newTopR);
        return false;
    }

    try {
        m_previousShape = doc.getBody(m_bodyId);
    } catch (...) { return false; }

    // No-op: leave the body alone.
    if (std::abs(m_newBottomR - m_oldBottomR) < 1e-5 &&
        std::abs(m_newTopR    - m_oldTopR)    < 1e-5) return true;

    try {
        // We DON'T build the full new/old cylinder and swap them — that would
        // destroy other features sharing the cylinder's volume (e.g. a tube
        // becomes a solid cylinder because OLD is a R=0…outerR cylinder that
        // includes the tube's inner hole). Instead, build only the RING /
        // FRUSTUM SHELL of material that's actually changing (the volumetric
        // difference between OLD and NEW lateral surfaces), then fuse or cut
        // that into the body.

        // Direction of OLD's lateral pad: shift OLD AWAY from where the
        // change-volume will live, so its surface doesn't coincide with the
        // body's existing cylindrical face. growing → OLD shifts inward (–).
        // shrinking → OLD shifts outward (+).
        bool growing = (m_newBottomR > m_oldBottomR) || (m_newTopR > m_oldTopR);
        const double kRadialPad = (growing ? -1.0 : +1.0) * 0.01;
        double paddedOldBot = std::max(1e-4, m_oldBottomR + kRadialPad);
        double paddedOldTop = std::max(1e-4, m_oldTopR    + kRadialPad);

        // The "outer" cone contains the "inner" cone geometrically — pick by
        // grow direction. For face edits both ends move together; for edge
        // edits only one end moves and the other stays at oldR, but the pad
        // still makes outer ≥ inner everywhere.
        double outerBot, outerTop, innerBot, innerTop;
        if (growing) {
            outerBot = m_newBottomR;  outerTop = m_newTopR;
            innerBot = paddedOldBot;  innerTop = paddedOldTop;
        } else {
            outerBot = paddedOldBot;  outerTop = paddedOldTop;
            innerBot = m_newBottomR;  innerTop = m_newTopR;
        }

        // Axial extent of the change ring. m_height is the cylindrical FACE's
        // V range, which is fine when the hole exits through PLANAR caps —
        // the face stops right where the cylinder meets the cap. For curved
        // or angular caps the face's V range stops short of where the body's
        // cap surface actually ends axially, so the boolean leaves a sliver
        // of OLD-radius geometry along the cap.
        //
        // For CUT operations (hole growing, solid boundary shrinking) we can
        // safely extend the ring far past the body's bounding box on both
        // ends: the cut naturally clips to the body's true shape, so any
        // padding outside the body is a no-op. For FUSE operations the same
        // trick adds stubs of fresh material sticking out past the body's
        // caps, so we keep the axial extent equal to the face's V range
        // (current limitation: curved/angular cap on a SHRINK / solid-grow
        // looks the same as before — work for a future pass).
        bool addMaterial = (m_isHole != growing);
        double height = m_height;
        gp_Ax2 ringAxis = m_axis;
        if (!addMaterial) {
            try {
                Bnd_Box bb; BRepBndLib::Add(m_previousShape, bb);
                if (!bb.IsVoid()) {
                    double x1,y1,z1,x2,y2,z2; bb.Get(x1,y1,z1,x2,y2,z2);
                    double dx = x2 - x1, dy = y2 - y1, dz = z2 - z1;
                    double diag = std::sqrt(dx*dx + dy*dy + dz*dz);
                    double pad  = diag + 1.0;
                    // Pad the cut ring PAST the caps so a curved/angular cap's
                    // sliver of old-radius geometry gets removed — but ONLY
                    // past a FREE end. Padding past an end that butts a LARGER
                    // coaxial neighbour (a 13mm section next to a resized 11mm
                    // one) carves the [newR,oldR] shell out of that neighbour,
                    // turning it into a tube (Steve's bug). Sample the shell
                    // radius from just past each end out to the bbox extent;
                    // an end is free only when no body material sits there.
                    const gp_Dir ad = m_axis.Direction();
                    const gp_Dir xd = m_axis.XDirection();
                    const gp_Pnt base = m_axis.Location();
                    const double rMidBot = 0.5 * (m_newBottomR + paddedOldBot);
                    const double rMidTop = 0.5 * (m_newTopR    + paddedOldTop);
                    auto solidAt = [&](double v, double rr) {
                        gp_Pnt p(base.X() + ad.X()*v + xd.X()*rr,
                                 base.Y() + ad.Y()*v + xd.Y()*rr,
                                 base.Z() + ad.Z()*v + xd.Z()*rr);
                        try {
                            BRepClass3d_SolidClassifier c(m_previousShape, p, 1e-6);
                            return c.State() == TopAbs_IN;
                        } catch (...) { return true; }
                    };
                    auto endFree = [&](bool topEnd, double rr) {
                        for (int k = 1; k <= 24; ++k) {
                            const double d = pad * k / 24.0;
                            const double v = topEnd ? (m_height + d) : -d;
                            if (solidAt(v, rr)) return false;
                        }
                        return true;
                    };
                    const double padBot = endFree(false, rMidBot) ? pad : 0.0;
                    const double padTop = endFree(true,  rMidTop) ? pad : 0.0;
                    ringAxis.SetLocation(base.Translated(gp_Vec(ad) * -padBot));
                    height = m_height + padBot + padTop;
                }
            } catch (...) { /* fall through with original m_height */ }
        }

        bool ok = false;
        TopoDS_Shape outerSolid = makeRevolveSolid(ringAxis, outerBot, outerTop, height, &ok);
        if (!ok || outerSolid.IsNull()) {
            MZLOG("[Resize] outer revolve failed (R1=%.3f R2=%.3f H=%.3f)\n",
                         outerBot, outerTop, height);
            return false;
        }
        TopoDS_Shape innerSolid = makeRevolveSolid(ringAxis, innerBot, innerTop, height, &ok);
        if (!ok || innerSolid.IsNull()) {
            MZLOG("[Resize] inner revolve failed (R1=%.3f R2=%.3f H=%.3f)\n",
                         innerBot, innerTop, height);
            return false;
        }

        // Ring / frustum shell = outer − inner.
        BRepAlgoAPI_Cut ringMaker(outerSolid, innerSolid);
        ringMaker.Build();
        if (!ringMaker.IsDone()) {
            MZLOG("[Resize] ring cut failed\n");
            return false;
        }
        TopoDS_Shape ring = ringMaker.Shape();
        if (ring.IsNull()) {
            MZLOG("[Resize] ring null\n");
            return false;
        }

        // addMaterial already computed up top so the axial-padding decision
        // could be made. Add when (hole shrinking) OR (solid growing).
        MZLOG(
            "[Resize] isHole=%d growing=%d add=%d old(%.3f→%.3f) padded(%.3f→%.3f) "
            "new(%.3f→%.3f) H=%.3f\n",
            m_isHole ? 1 : 0, growing ? 1 : 0, addMaterial ? 1 : 0,
            m_oldBottomR, m_oldTopR, paddedOldBot, paddedOldTop,
            m_newBottomR, m_newTopR, height);

        TopoDS_Shape result;
        if (addMaterial) {
            // Topology-agnostic fill (works for plane, cone, NURBS, or any cap):
            // the volume to add into the body = (the body's hole void confined
            // to the old hole's axial column) MINUS (a cylinder of the new
            // radius). That carves the fill so it follows whatever cap surface
            // the body already has — no need to identify the cap face's
            // surface type or stitch a slope into the fill manually.
            //
            // Steps, for the HOLE-shrink case:
            //   1. bbox of the body, padded axially.
            //   2. bboxSolid − body = body's complement (hole void + outside).
            //   3. ∩ OUTER cylinder of (paddedOldR) → confines to the hole's
            //      axial column. Now we have exactly the original hole's
            //      missing volume, capped by whatever surfaces the body has.
            //   4. − INNER cylinder of (newR), axially over-extended → leaves
            //      the annulus between newR and oldR. This is the fill.
            //   5. body ∪ fill.
            // If any step fails, we fall back to the legacy ring (which has
            // the "washer at curved cap" limitation but is otherwise sound).
            TopoDS_Shape fillShape;
            bool fillBuilt = false;
            if (m_isHole) {
                try {
                    // Find the cap face at each end of the hole's cylindrical
                    // face. These are the faces we need to clip the fill ring
                    // by so it ends exactly at the body's natural cap surface
                    // instead of leaving a washer step.
                    TopoDS_Face topCap = findCapFace(m_previousShape, m_axis,
                                                    m_oldTopR,    true);
                    TopoDS_Face botCap = findCapFace(m_previousShape, m_axis,
                                                    m_oldBottomR, false);
                    if (topCap.IsNull() || botCap.IsNull())
                        throw std::runtime_error("cap face(s) not found");
                    Handle(Geom_Plane) topPlane = Handle(Geom_Plane)::DownCast(
                        BRep_Tool::Surface(topCap));
                    Handle(Geom_Plane) botPlane = Handle(Geom_Plane)::DownCast(
                        BRep_Tool::Surface(botCap));
                    MZLOG(
                        "[Resize] caps: top=%s bot=%s\n",
                        topPlane.IsNull() ? "non-planar" : "planar",
                        botPlane.IsNull() ? "non-planar" : "planar");

                    // Build an axially-padded ring (annular tube tall enough
                    // to comfortably bracket both caps). The half-space
                    // intersection trims it back to the body's natural extent.
                    Bnd_Box bb; BRepBndLib::Add(m_previousShape, bb);
                    if (bb.IsVoid()) throw std::runtime_error("body bbox void");
                    double x1,y1,z1,x2,y2,z2; bb.Get(x1,y1,z1,x2,y2,z2);
                    double diag = std::sqrt((x2-x1)*(x2-x1) +
                                            (y2-y1)*(y2-y1) +
                                            (z2-z1)*(z2-z1));
                    double pad = 1.0 + diag;
                    gp_Ax2 padAxis = m_axis;
                    padAxis.SetLocation(m_axis.Location().Translated(
                        gp_Vec(m_axis.Direction()) * -pad));
                    double padH = m_height + 2.0 * pad;
                    bool okp;
                    TopoDS_Shape outerPadded = makeRevolveSolid(
                        padAxis, outerBot, outerTop, padH, &okp);
                    if (!okp || outerPadded.IsNull())
                        throw std::runtime_error("padded outer revolve failed");
                    TopoDS_Shape innerPadded = makeRevolveSolid(
                        padAxis, innerBot, innerTop, padH, &okp);
                    if (!okp || innerPadded.IsNull())
                        throw std::runtime_error("padded inner revolve failed");
                    BRepAlgoAPI_Cut ringPadMake(outerPadded, innerPadded);
                    ringPadMake.Build();
                    if (!ringPadMake.IsDone() || ringPadMake.Shape().IsNull())
                        throw std::runtime_error("padded ring cut failed");
                    TopoDS_Shape ringPad = ringPadMake.Shape();

                    // MakeHalfSpace's semantics depend on face orientation: the
                    // half-space "side containing the reference point" can flip
                    // unexpectedly. Build with one reference point; if the
                    // resulting half-space ∩ ring is empty (we picked the
                    // OUTSIDE side), rebuild with a reference point on the
                    // other side. Self-correcting and surface-orientation-
                    // agnostic.
                    gp_Pnt midHole = m_axis.Location().Translated(
                        gp_Vec(m_axis.Direction()) * (m_height * 0.5));
                    gp_Pnt farAbove = m_axis.Location().Translated(
                        gp_Vec(m_axis.Direction()) * (m_height + 10.0 * pad));
                    gp_Pnt farBelow = m_axis.Location().Translated(
                        gp_Vec(m_axis.Direction()) * (-10.0 * pad));

                    auto volOf = [](const TopoDS_Shape& s) {
                        GProp_GProps gp;
                        BRepGProp::VolumeProperties(s, gp);
                        return gp.Mass();
                    };
                    // Clip a shape by one cap. Two strategies, ordered by
                    // reliability:
                    //   (1) PLANAR cap → build a fresh unbounded plane face
                    //       from the cap's gp_Pln and use MakeHalfSpace. The
                    //       unbounded plane face bounds the half-space cleanly
                    //       (whereas MakeHalfSpace on the body's trimmed cap
                    //       face often degenerates to "everything").
                    //   (2) NON-PLANAR cap → extrude the cap face by a huge
                    //       distance along the cylinder axis (toward midHole)
                    //       into a finite prism solid. The prism's top face
                    //       follows the cap's natural surface, so the ring
                    //       intersected with the prism is naturally cap-
                    //       conforming. Works for any surface type because we
                    //       just extrude the existing face.
                    // Whichever strategy is used, the resulting clipped shape
                    // must have strictly less volume than the input — otherwise
                    // we picked the wrong-orientation half-space (or the prism
                    // entirely contains the input). Both strategies retry with
                    // the opposite-side reference / opposite-direction extrude
                    // on the first attempt's failure.
                    auto clipBy = [&](const TopoDS_Shape& toClip,
                                       const TopoDS_Face& cap,
                                       const Handle(Geom_Plane)& asPlane,
                                       const gp_Pnt& refIn,   /* inside body */
                                       const gp_Pnt& refOut,  /* outside body */
                                       const char* label) -> TopoDS_Shape {
                        double inVol = volOf(toClip);
                        // Strategy 1: planar half-space.
                        if (!asPlane.IsNull()) {
                            TopoDS_Face planeFace =
                                BRepBuilderAPI_MakeFace(asPlane->Pln()).Face();
                            for (int attempt = 0; attempt < 2; ++attempt) {
                                const gp_Pnt& ref = (attempt == 0) ? refIn : refOut;
                                BRepPrimAPI_MakeHalfSpace hsMake(planeFace, ref);
                                if (!hsMake.IsDone()) continue;
                                BRepAlgoAPI_Common common(toClip, hsMake.Solid());
                                common.Build();
                                if (!common.IsDone() || common.Shape().IsNull()) continue;
                                double v = volOf(common.Shape());
                                MZLOG(
                                    "[Resize]   %s plane attempt %d → vol=%.3f\n",
                                    label, attempt, v);
                                if (v > 1e-6 && v < inVol - 1e-6) return common.Shape();
                            }
                            return {};
                        }
                        // Strategy 2: non-planar — extrude an UNTRIMMED face
                        // built from the cap's underlying surface. The body's
                        // cap face is trimmed by the hole circle (so its
                        // footprint is OUTSIDE the hole), but the ring lives
                        // INSIDE the hole — so extruding the trimmed face
                        // misses the ring entirely. Re-facing the surface
                        // with its natural domain gives us a face whose
                        // footprint covers the hole region too, so extruding
                        // it sweeps a prism that properly contains the ring's
                        // column.
                        Handle(Geom_Surface) capSurf = BRep_Tool::Surface(cap);
                        TopoDS_Face wideFace;
                        try {
                            wideFace = BRepBuilderAPI_MakeFace(capSurf, 1e-6).Face();
                        } catch (...) {}
                        if (wideFace.IsNull()) {
                            MZLOG(
                                "[Resize]   %s prism: could not re-face surface\n",
                                label);
                            return {};
                        }
                        gp_Vec axisDir(m_axis.Direction());
                        gp_Vec toIn(refOut, refIn); // from outside toward inside
                        double sign = axisDir.Dot(toIn) > 0 ? +1.0 : -1.0;
                        for (int attempt = 0; attempt < 2; ++attempt) {
                            double s = (attempt == 0) ? sign : -sign;
                            double extrudeLen = 4.0 * pad;
                            gp_Vec extrudeVec = axisDir * (s * extrudeLen);
                            try {
                                TopoDS_Shape prism =
                                    BRepPrimAPI_MakePrism(wideFace, extrudeVec).Shape();
                                if (prism.IsNull()) continue;
                                BRepAlgoAPI_Common common(toClip, prism);
                                common.Build();
                                if (!common.IsDone() || common.Shape().IsNull()) continue;
                                double v = volOf(common.Shape());
                                MZLOG(
                                    "[Resize]   %s prism attempt %d (s=%+.0f) → vol=%.3f\n",
                                    label, attempt, s, v);
                                if (v > 1e-6 && v < inVol - 1e-6) return common.Shape();
                            } catch (...) { continue; }
                        }
                        return {};
                    };

                    TopoDS_Shape afterTop = clipBy(ringPad, topCap, topPlane,
                                                   midHole, farAbove, "topClip");
                    if (afterTop.IsNull())
                        throw std::runtime_error("top clip never bounded the ring");
                    TopoDS_Shape afterBot = clipBy(afterTop, botCap, botPlane,
                                                   midHole, farBelow, "botClip");
                    if (afterBot.IsNull())
                        throw std::runtime_error("bot clip never bounded the ring");
                    fillShape = afterBot;
                    fillBuilt = true;
                    MZLOG(
                        "[Resize] cap-following fill built (final vol=%.3f mm³)\n",
                        volOf(fillShape));
                } catch (const std::exception& e) {
                    MZLOG("[Resize] cap-following fill failed (%s); "
                                         "falling back to ring\n", e.what());
                } catch (...) {
                    MZLOG("[Resize] cap-following fill threw; "
                                         "falling back to ring\n");
                }
            }
            if (!fillBuilt) fillShape = ring;

            BRepAlgoAPI_Fuse fuse(m_previousShape, fillShape);
            fuse.Build();
            if (!fuse.IsDone()) {
                MZLOG("[Resize] body fuse failed\n");
                return false;
            }
            result = fuse.Shape();
            {
                GProp_GProps gp1, gp2;
                BRepGProp::VolumeProperties(m_previousShape, gp1);
                BRepGProp::VolumeProperties(result, gp2);
                MZLOG(
                    "[Resize] fuse: bodyVol %.3f → %.3f mm³ (delta=%+.3f)\n",
                    gp1.Mass(), gp2.Mass(), gp2.Mass() - gp1.Mass());
            }
        } else {
            BRepAlgoAPI_Cut cut(m_previousShape, ring);
            cut.Build();
            if (!cut.IsDone()) {
                MZLOG("[Resize] body cut failed\n");
                return false;
            }
            result = cut.Shape();
        }
        if (result.IsNull()) {
            MZLOG("[Resize] result null\n");
            return false;
        }

        // Merge the ring's caps with the body's adjacent caps so the top and
        // bottom faces are single uniform faces again — without this OCCT
        // leaves them as separate adjacent planar faces that share an edge
        // (a visible hairline seam across the cap face). UnifySameDomain
        // walks the topology and merges any pair of adjacent same-surface
        // faces.
        result = materializr::unifySameDomain(result, "Resize",
                                             /*concatBSplines=*/false);

        // Validate before committing: a hole grown past the outer wall
        // (or any degenerate resize) removes all material — the cut
        // returns an empty/invalid solid. Storing it leaves nothing but
        // the stale face highlight on screen. Refuse so the caller
        // restores the original and flags an error, exactly as Scale
        // Face does.
        if (!BRepCheck_Analyzer(result).IsValid()) {
            MZLOG("[Resize] result invalid — refusing\n");
            return false;
        }
        int nsolids = 0;
        for (TopExp_Explorer sx(result, TopAbs_SOLID); sx.More(); sx.Next())
            ++nsolids;
        GProp_GProps gpR, gpO;
        BRepGProp::VolumeProperties(result, gpR);
        BRepGProp::VolumeProperties(m_previousShape, gpO);
        if (nsolids < 1 || gpR.Mass() < gpO.Mass() * 0.001) {
            MZLOG("[Resize] result collapsed (solids=%d vol=%.3f) — "
                  "refusing\n", nsolids, gpR.Mass());
            return false;
        }

        doc.updateBody(m_bodyId, result);
        return true;
    } catch (...) {
        MZLOG("[Resize] exception during boolean\n");
        return false;
    }
}

std::string ResizeCylindricalOp::serializeParams() const {
    // The cylinder axis (gp_Ax2: location + main dir + X dir) is geometric, so
    // it serialises directly — execute() rebuilds the cut/fuse tools from it.
    const gp_Pnt& loc = m_axis.Location();
    const gp_Dir& dir = m_axis.Direction();
    const gp_Dir& xd  = m_axis.XDirection();
    char buf[420];
    std::snprintf(buf, sizeof(buf),
        "body=%d;oldBot=%.6f;oldTop=%.6f;newBot=%.6f;newTop=%.6f;height=%.6f;isHole=%d;"
        "axx=%.9g;axy=%.9g;axz=%.9g;adx=%.9g;ady=%.9g;adz=%.9g;"
        "aux=%.9g;auy=%.9g;auz=%.9g",
        m_bodyId, m_oldBottomR, m_oldTopR, m_newBottomR, m_newTopR, m_height,
        m_isHole ? 1 : 0,
        loc.X(), loc.Y(), loc.Z(), dir.X(), dir.Y(), dir.Z(),
        xd.X(), xd.Y(), xd.Z());
    return buf;
}

bool ResizeCylindricalOp::deserializeParams(const std::string& blob) {
    bool any = false;
    double lx = 0, ly = 0, lz = 0, dx = 0, dy = 0, dz = 1, ux = 1, uy = 0, uz = 0;
    bool haveAxis = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        std::string val = blob.substr(eq + 1, end - eq - 1);
        double d = std::atof(val.c_str());
        if      (key == "oldBot") { m_oldBottomR = d; any = true; }
        else if (key == "oldTop") { m_oldTopR    = d; any = true; }
        else if (key == "newBot") { m_newBottomR = d; any = true; }
        else if (key == "newTop") { m_newTopR    = d; any = true; }
        else if (key == "height") { m_height     = d; any = true; }
        else if (key == "isHole") { m_isHole = (std::atoi(val.c_str()) != 0); any = true; }
        else if (key == "body")   { m_bodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "axx") { lx = d; haveAxis = true; }
        else if (key == "axy") { ly = d; }
        else if (key == "axz") { lz = d; }
        else if (key == "adx") { dx = d; }
        else if (key == "ady") { dy = d; }
        else if (key == "adz") { dz = d; }
        else if (key == "aux") { ux = d; }
        else if (key == "auy") { uy = d; }
        else if (key == "auz") { uz = d; }
        pos = end + 1;
    }
    if (haveAxis) {
        try {
            m_axis = gp_Ax2(gp_Pnt(lx, ly, lz), gp_Dir(dx, dy, dz),
                            gp_Dir(ux, uy, uz));
            any = true;
        } catch (...) { /* degenerate dirs: keep default axis */ }
    }
    return any;
}

bool ResizeCylindricalOp::rehydrateFromReload(const ReloadState& state,
                                              Document& /*doc*/) {
    if (m_bodyId < 0) return false;
    m_previousShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    // Without the before-shape undo can't restore; without an axis execute
    // can't rebuild the change volume. (Old files lack body=/ax* keys → the
    // step stays a baked ReplayOp.)
    return !m_previousShape.IsNull();
}

bool ResizeCylindricalOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) return false;
    try { doc.updateBody(m_bodyId, m_previousShape); return true; }
    catch (...) { return false; }
}

void ResizeCylindricalOp::renderProperties() {
    ImGui::Text(materializr::tr("Resize %s"), m_isHole ? "Hole" : "Cylinder");
    ImGui::Separator();
    // Directly editable — Apply (or Enter) re-executes the op with the new
    // diameters (m_old* stays the original cylinder, so the change volume is
    // rebuilt correctly). A uniform cylinder edits one Ø; a cone edits both.
    // FIXED item width: without it, the field grows with every typed digit in
    // an auto-resize panel and runs off the right edge (Steve's report). The
    // font scale already sizes it up on touch.
    const float diaW = 200.0f;
    if (std::abs(m_newTopR - m_newBottomR) < 1e-5) {
        double dia = m_newTopR * 2.0;
        ImGui::SetNextItemWidth(diaW);
        if (materializr::lengthField(materializr::trFormat("Diameter (%s)", materializr::unitSuffix()).c_str(), &dia)) {
            if (dia > 0.01) { m_newTopR = dia * 0.5; m_newBottomR = dia * 0.5; }
        }
    } else {
        double db = m_newBottomR * 2.0;
        double dt = m_newTopR * 2.0;
        ImGui::SetNextItemWidth(diaW);
        if (materializr::lengthField(materializr::trFormat("Bottom Ø (%s)", materializr::unitSuffix()).c_str(), &db)) {
            if (db > 0.01) m_newBottomR = db * 0.5;
        }
        ImGui::SetNextItemWidth(diaW);
        if (materializr::lengthField(materializr::trFormat("Top Ø (%s)", materializr::unitSuffix()).c_str(), &dt)) {
            if (dt > 0.01) m_newTopR = dt * 0.5;
        }
    }
    ImGui::TextUnformatted(materializr::trFormat("Length: %s", materializr::fmtLength(m_height)).c_str());
    // WRAPPED: inline history-panel editor — see ThreadOp for the same note.
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", materializr::tr("Clicking the circular edge / face in the viewport also re-edits."));
    ImGui::PopStyleColor();
}

std::string ResizeCylindricalOp::description() const {
    char buf[160];
    if (std::abs(m_newTopR - m_newBottomR) < 1e-5)
        std::snprintf(buf, sizeof(buf), "Resize %s D %s → %s", m_isHole ? "hole" : "outer", materializr::fmtLength(m_oldTopR * 2.0).c_str(), materializr::fmtLength(m_newTopR * 2.0).c_str());
    else
        std::snprintf(buf, sizeof(buf), "Shape %s: %s / %s (bottom / top)", m_isHole ? "hole" : "outer", materializr::fmtLength(m_newBottomR * 2.0).c_str(), materializr::fmtLength(m_newTopR * 2.0).c_str());
    return buf;
}

OperationDiff ResizeCylindricalOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}
