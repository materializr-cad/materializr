#include "MoveFaceOp.h"
#include "SubShapeIndex.h"
#include "Sketch.h"
#include <gp_Trsf.hxx>
#include <gp_Pln.hxx>

#include <imgui.h>
#include <cstdio>
#include <cstring>

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <gp_GTrsf.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_DataMapOfShapeInteger.hxx>
#include <TopTools_ListOfShape.hxx>
// TopTools_ListIteratorOfListOfShape typedef comes from TopTools_ListOfShape.hxx
// above. The standalone <...ListIteratorOfListOfShape.hxx> shim was removed in
// OCCT 8.0, so we no longer include it (vcpkg ships 8.0; system OCCT is 7.9).
#include <TopTools_HSequenceOfShape.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <vector>
#include <BRepGProp_Face.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <gp_Ax1.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <Geom_Surface.hxx>
#include <Geom_Plane.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#include <cmath>
#include <gp_Pnt.hxx>
#include <Standard_ErrorHandler.hxx>
#include <Standard_Failure.hxx>
#include <algorithm>
#include "../i18n.h"
#include "ParamParse.h"

namespace {
// The far cross-section of the feature attached to `face`: the edge LOOPS where
// the side-wall faces sharing `face`'s boundary end (meet a step / other
// geometry). On a plain prism that's the opposite cap; on a funnel→step→spout,
// from the spout cap it's the spout-top — which is an ANNULUS (outer + inner
// ring) when the spout is hollow, so this returns a SET of loops. Empty if it
// can't assemble clean closed loops (caller falls back to the opposite cap).
std::vector<TopoDS_Wire> featureFarLoops(const TopoDS_Shape& body,
                                         const TopoDS_Face& face) {
    std::vector<TopoDS_Wire> loops;
    try {
        TopTools_IndexedMapOfShape nearEdges;
        TopExp::MapShapes(face, TopAbs_EDGE, nearEdges);
        if (nearEdges.IsEmpty()) return loops;

        TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
        TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, edgeFaces);

        // tube faces = faces (other than `face`) sharing a near edge.
        TopTools_IndexedMapOfShape tubeFaces;
        for (int i = 1; i <= nearEdges.Extent(); ++i) {
            const TopoDS_Shape& e = nearEdges(i);
            if (!edgeFaces.Contains(e)) continue;
            for (TopTools_ListIteratorOfListOfShape it(edgeFaces.FindFromKey(e));
                 it.More(); it.Next())
                if (!it.Value().IsSame(face)) tubeFaces.Add(it.Value());
        }
        if (tubeFaces.IsEmpty()) return loops;

        // Count tube faces per edge (the seam of a periodic wall is shared by
        // its own face twice → count 2 → excluded, same as a box-corner edge).
        TopTools_DataMapOfShapeInteger cnt;
        for (int i = 1; i <= tubeFaces.Extent(); ++i)
            for (TopExp_Explorer ex(tubeFaces(i), TopAbs_EDGE); ex.More(); ex.Next()) {
                const TopoDS_Shape& e = ex.Current();
                if (cnt.IsBound(e)) cnt.ChangeFind(e)++;
                else cnt.Bind(e, 1);
            }

        // Far edges: on exactly ONE tube face and NOT a near edge.
        Handle(TopTools_HSequenceOfShape) farEdges = new TopTools_HSequenceOfShape();
        for (TopTools_DataMapIteratorOfDataMapOfShapeInteger it(cnt); it.More(); it.Next()) {
            if (it.Value() != 1) continue;
            if (nearEdges.Contains(it.Key())) continue;
            farEdges->Append(it.Key());
        }
        if (farEdges->IsEmpty()) return loops;

        // Group the far edges into connected closed loops (outer + any holes).
        Handle(TopTools_HSequenceOfShape) wires;
        ShapeAnalysis_FreeBounds::ConnectEdgesToWires(farEdges, 1e-5,
                                                      Standard_False, wires);
        if (wires.IsNull()) return loops;
        for (int i = 1; i <= wires->Length(); ++i) {
            TopoDS_Wire w = TopoDS::Wire(wires->Value(i));
            if (BRep_Tool::IsClosed(w)) loops.push_back(w);
        }
    } catch (...) {}
    return loops;
}
} // namespace

bool MoveFaceOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_face.IsNull()) return false;
    // Nothing-to-do guard, per kind.
    if (m_kind == Kind::Translate && m_move.Magnitude() < 1e-6) return false;
    if (m_kind == Kind::Rotate && !m_rotUseExplicit && std::abs(m_rotAngle) < 1e-6) return false;
    if (m_kind == Kind::Scale && !m_scaleNonUniform &&
        std::abs(m_scaleFactor - 1.0) < 1e-6) return false;
    if (m_kind == Kind::Scale && m_scaleNonUniform &&
        std::abs(m_scaleA - 1.0) < 1e-6 && std::abs(m_scaleB - 1.0) < 1e-6) return false;

    try {
        OCC_CATCH_SIGNALS // turn an OCCT kernel fault here into a catch below
        m_previousShape = doc.getBody(m_bodyId);
        if (m_previousShape.IsNull()) return false;

        // Name the target face on the first run (while m_face is still valid),
        // then re-resolve it whenever it's no longer a live sub-shape of the
        // (possibly rebuilt) body — an upstream sketch edit that MOVED the face
        // leaves m_face pointing at the old body. Sketch-anchored, so it
        // follows; falls back to the stale handle if unnameable.
        if (m_faceRef.empty()) {
            materializr::topo::Context mc;
            mc.doc = &doc; mc.shape = m_previousShape; mc.type = TopAbs_FACE;
            m_faceRef = materializr::topo::mint(m_face, mc);
        }
        if (!m_faceRef.empty()) {
            bool live = false;
            for (TopExp_Explorer ex(m_previousShape, TopAbs_FACE); ex.More(); ex.Next())
                if (ex.Current().IsSame(m_face)) { live = true; break; }
            if (!live) {
                materializr::topo::Context rc;
                rc.doc = &doc; rc.shape = m_previousShape; rc.type = TopAbs_FACE;
                rc.crossRebuild = true;   // body was rebuilt upstream
                TopoDS_Shape f;
                if (materializr::topo::resolve(m_faceRef, rc, f) &&
                    !f.IsNull() && f.ShapeType() == TopAbs_FACE) {
                    // SANITY GUARD: only adopt the resolved face if it points
                    // the same way as the stale one. The stale face's geometry
                    // is still readable (its TShape lives on), and it's what
                    // the pre-topo code would have used — so a resolution that
                    // flips orientation is a MIS-resolve (this is what made a
                    // body "slump": a taper re-applied about a wrong plane).
                    // Reject it and keep the old behaviour instead.
                    auto normalOf = [](const TopoDS_Face& fc, gp_Vec& n) -> bool {
                        try {
                            BRepGProp_Face p(fc);
                            double u1, u2, v1, v2; p.Bounds(u1, u2, v1, v2);
                            gp_Pnt c; p.Normal((u1+u2)*0.5, (v1+v2)*0.5, c, n);
                            return n.Magnitude() > 1e-9;
                        } catch (...) { return false; }
                    };
                    gp_Vec nOld, nNew;
                    if (normalOf(m_face, nOld) &&
                        normalOf(TopoDS::Face(f), nNew) &&
                        nOld.Normalized().Dot(nNew.Normalized()) > 0.7)
                        m_face = TopoDS::Face(f);
                }
            }
        }

        // Outward face normal N and a point P0 on the face (BRepGProp_Face's
        // Normal is orientation-corrected, so it points out of the body).
        BRepGProp_Face prop(m_face);
        double u1, u2, v1, v2;
        prop.Bounds(u1, u2, v1, v2);
        gp_Pnt c; gp_Vec nv;
        prop.Normal((u1 + u2) * 0.5, (v1 + v2) * 0.5, c, nv);
        if (nv.Magnitude() < 1e-9) return false;
        gp_Vec N = nv.Normalized();
        gp_Vec P0(c.X(), c.Y(), c.Z());

        // Pivot = the face's area centroid (the "invisible point in the middle"
        // that Rotate/Scale turn/scale about).
        gp_Pnt pivot;
        {
            GProp_GProps gp; BRepGProp::SurfaceProperties(m_face, gp);
            pivot = gp.CentreOfMass();
        }

        // The single transform applied to the moving (top) loops — translate,
        // rotate-about-pivot, or scale-about-pivot. Everything downstream (loft,
        // sketch follow, undo) just applies this one gp_Trsf.
        gp_Vec V = m_move - N * m_move.Dot(N); // in-plane slide (Translate)
        gp_Trsf topT;
        gp_GTrsf topGT;          // only used for non-uniform scale
        bool useGT = false;
        switch (m_kind) {
            case Kind::Translate:
                if (V.Magnitude() < 1e-6) return false;
                topT.SetTranslation(V);
                break;
            case Kind::Rotate:
                if (m_rotUseExplicit) topT = m_rotExplicit;
                else topT.SetRotation(gp_Ax1(pivot, m_rotAxis), m_rotAngle);
                break;
            case Kind::Twist:
                // Geometry is built by the layered loft below (loftTwist), not
                // by topT. topT is set AFTER the axis is known so on-face
                // sketches still follow the spin (see near m_appliedXform).
                if (std::fabs(m_twistAngle) < 1e-6) return false;
                break;
            case Kind::Scale:
                if (m_scaleNonUniform) {
                    // M = sA·(A⊗A) + sB·(B⊗B) + 1·(N⊗N); T pins the pivot.
                    double Av[3] = {m_scaleAxisA.X(), m_scaleAxisA.Y(), m_scaleAxisA.Z()};
                    double Bv[3] = {m_scaleAxisB.X(), m_scaleAxisB.Y(), m_scaleAxisB.Z()};
                    double Nv[3] = {N.X(), N.Y(), N.Z()};
                    double Pv[3] = {pivot.X(), pivot.Y(), pivot.Z()};
                    for (int i = 0; i < 3; ++i) {
                        double t = Pv[i];
                        for (int j = 0; j < 3; ++j) {
                            double m = m_scaleA * Av[i] * Av[j] +
                                       m_scaleB * Bv[i] * Bv[j] + Nv[i] * Nv[j];
                            topGT.SetValue(i + 1, j + 1, m);
                            t -= m * Pv[j];
                        }
                        topGT.SetValue(i + 1, 4, t);
                    }
                    useGT = true;
                } else {
                    topT.SetScale(pivot, m_scaleFactor);
                }
                break;
        }

        // LOFT REBUILD (replaces the old whole-body gp_GTrsf shear, which made
        // OCCT NURBS-convert the entire body and segfault on freeform/boolean
        // geometry). A prism is bounded by the selected face and an OPPOSITE
        // PARALLEL face. We loft a capped solid between the base OUTER wire and
        // the moved top outer wire, then subtract a loft of each HOLE loop
        // (base inner -> moved top inner). Every loop moves by V here (the whole
        // face slides, holes included); per-loop control (move a hole on its
        // own) layers on top later. All geometry is local wires — no whole-body
        // convert, so it survives bodies the shear crashed on. Non-prism bodies
        // refuse here instead of crashing.

        // Find the opposite planar face (the prism's far cap) = the planar face
        // FARTHEST along -N from the selected face. The normal need only be
        // roughly opposite (dot < -0.3), NOT near-perfectly antiparallel: after
        // a TILT the top is tilted while the base stays flat, so they're no
        // longer parallel — a strict test made a second op on a tilted face
        // refuse. The loose test still excludes the perpendicular side walls.
        TopoDS_Face baseFace;
        double bestDist = -1e300;
        for (TopExp_Explorer fx(m_previousShape, TopAbs_FACE); fx.More(); fx.Next()) {
            TopoDS_Face f = TopoDS::Face(fx.Current());
            if (f.IsSame(m_face)) continue;
            Handle(Geom_Surface) s = BRep_Tool::Surface(f);
            if (s.IsNull() || !s->IsKind(STANDARD_TYPE(Geom_Plane))) continue;
            BRepGProp_Face fp(f);
            double uu1, uu2, vv1, vv2; fp.Bounds(uu1, uu2, vv1, vv2);
            gp_Pnt fc; gp_Vec fn; fp.Normal((uu1 + uu2) * 0.5, (vv1 + vv2) * 0.5, fc, fn);
            if (fn.Magnitude() < 1e-9) continue;
            if (fn.Normalized().Dot(N) > -0.3) continue; // not roughly opposite
            double dist = -(gp_Vec(fc.X(), fc.Y(), fc.Z()) - P0).Dot(N);
            if (dist > bestDist) { bestDist = dist; baseFace = f; }
        }
        if (baseFace.IsNull()) {
            std::fprintf(stderr, "[MoveFace] refused: no opposite face to loft from\n");
            return false;
        }

        // Split each cap into its outer wire + inner (hole) loops.
        auto collect = [](const TopoDS_Face& f, TopoDS_Wire& outer,
                          std::vector<TopoDS_Wire>& inners) {
            outer = BRepTools::OuterWire(f);
            for (TopExp_Explorer wx(f, TopAbs_WIRE); wx.More(); wx.Next()) {
                TopoDS_Wire w = TopoDS::Wire(wx.Current());
                if (!w.IsSame(outer)) inners.push_back(w);
            }
        };
        auto centroid = [](const TopoDS_Wire& w) {
            gp_XYZ acc(0, 0, 0); int n = 0;
            for (TopExp_Explorer vx(w, TopAbs_VERTEX); vx.More(); vx.Next()) {
                acc += BRep_Tool::Pnt(TopoDS::Vertex(vx.Current())).XYZ(); ++n;
            }
            if (n) acc /= n;
            return gp_Pnt(acc);
        };

        // The feature's far cross-section: the spout-top loops on a funnel, or
        // the opposite cap on a plain prism. If it's a genuine SUB-feature (ends
        // before the opposite cap), loft only within it and boolean it back into
        // the body so the rest survives; otherwise fall back to the cap (old
        // "the loft IS the new body" path).
        std::vector<TopoDS_Wire> farLoops = featureFarLoops(m_previousShape, m_face);
        TopoDS_Wire topOuter, baseOuter;
        std::vector<TopoDS_Wire> topInners, baseInners;
        collect(m_face, topOuter, topInners);

        bool subFeature = false;
        if (!farLoops.empty()) {
            double maxPerim = -1; size_t oi = 0;
            for (size_t i = 0; i < farLoops.size(); ++i) {
                GProp_GProps lp; BRepGProp::LinearProperties(farLoops[i], lp);
                if (lp.Mass() > maxPerim) { maxPerim = lp.Mass(); oi = i; }
            }
            GProp_GProps bp; BRepGProp::LinearProperties(baseFace, bp);
            // Far loop NOT at the opposite cap => the feature is a sub-section.
            if (centroid(farLoops[oi]).Distance(bp.CentreOfMass()) > 1e-3) {
                subFeature = true;
                baseOuter = farLoops[oi];
                for (size_t i = 0; i < farLoops.size(); ++i)
                    if (i != oi) baseInners.push_back(farLoops[i]);
            }
        }
        if (!subFeature) collect(baseFace, baseOuter, baseInners);
        if (topOuter.IsNull() || baseOuter.IsNull()) return false;

        auto moved = [&](const TopoDS_Wire& w) {
            if (useGT)
                return TopoDS::Wire(BRepBuilderAPI_GTransform(w, topGT, Standard_True).Shape());
            return TopoDS::Wire(BRepBuilderAPI_Transform(w, topT, Standard_True).Shape());
        };
        auto loftSolid = [](TopoDS_Wire a, TopoDS_Wire b, TopoDS_Shape& out) -> bool {
            BRepOffsetAPI_ThruSections ts(Standard_True /*solid*/, Standard_True /*ruled*/);
            ts.AddWire(a); ts.AddWire(b); ts.Build();
            if (!ts.IsDone()) return false;
            out = ts.Shape();
            return !out.IsNull();
        };
        auto endFor = [&](const TopoDS_Wire& w, bool slides) {
            return slides ? moved(w) : w;
        };

        // ── Twist: layered loft about the prism's central axis ──────────────
        // A single ruled loft re-aligns wires and only twists honestly to
        // ~45deg; stepping the rotation up the height in small increments keeps
        // corner correspondence for any angle (proven in probe_twist_face). The
        // axis runs through the BASE centroid along the face normal; each layer
        // is the base wire rotated by angle·t and lifted by heightVec·t. Holes
        // ride the same axis (they orbit + spin), so a hole loft twists too.
        const gp_Pnt twBaseCtr = centroid(baseOuter);
        const gp_Ax1 twAxis(twBaseCtr, gp_Dir(N.X(), N.Y(), N.Z()));
        const gp_Vec twHeight(twBaseCtr, pivot);
        int twSteps = 2;
        {
            // ~10deg per layer: the ruled walls chord the true helicoid, so
            // finer steps hug it closer (less volume shaved, smoother look).
            // 10deg keeps a 90 twist to 9 layers / ~0.5% volume loss; capped so
            // an extreme twist can't explode the face count.
            double deg = std::fabs(m_twistAngle) * 180.0 / M_PI;
            twSteps = std::max(2, static_cast<int>(std::ceil(deg / 10.0)));
            if (twSteps > 64) twSteps = 64;
        }
        auto loftTwist = [&](const TopoDS_Wire& w, double angle,
                             TopoDS_Shape& out) -> bool {
            BRepOffsetAPI_ThruSections ts(Standard_True /*solid*/, Standard_True /*ruled*/);
            for (int i = 0; i <= twSteps; ++i) {
                double t = static_cast<double>(i) / twSteps;
                gp_Trsf rot; rot.SetRotation(twAxis, angle * t);
                gp_Trsf tr;  tr.SetTranslation(twHeight * t);
                TopoDS_Wire wi = TopoDS::Wire(
                    BRepBuilderAPI_Transform(w, tr * rot, Standard_True).Shape());
                if (i == 0) wi.Reverse(); // base orientation (matches buildFeature)
                ts.AddWire(wi);
            }
            try { ts.Build(); } catch (...) { return false; }
            if (!ts.IsDone() || ts.Shape().IsNull()) return false;
            out = ts.Shape();
            return true;
        };
        auto buildTwistFeature = [&](bool applyMove) -> TopoDS_Shape {
            double angle = applyMove ? m_twistAngle : 0.0;
            TopoDS_Shape feat;
            if (!loftTwist(baseOuter, angle, feat)) return TopoDS_Shape();
            for (const auto& bi : baseInners) {
                TopoDS_Shape holeSolid;
                if (!loftTwist(bi, angle, holeSolid)) continue;
                try {
                    BRepAlgoAPI_Cut cut(feat, holeSolid);
                    if (cut.IsDone() && !cut.Shape().IsNull()) feat = cut.Shape();
                } catch (...) {}
            }
            return feat;
        };

        // Build the feature solid base→top. applyMove OFF reconstructs the
        // ORIGINAL feature (the cut tool); ON builds the transformed one. The
        // hole rings ride/stay per the three-state flags (TILT must ride, else
        // the face can't close — the "half cover" bug).
        auto buildFeature = [&](bool applyMove) -> TopoDS_Shape {
            TopoDS_Wire bOuter = baseOuter; bOuter.Reverse();
            TopoDS_Shape feat;
            TopoDS_Wire topO = applyMove ? endFor(topOuter, m_moveOuter) : topOuter;
            if (!loftSolid(bOuter, topO, feat)) return TopoDS_Shape();
            for (size_t hi = 0; hi < topInners.size(); ++hi) {
                const TopoDS_Wire& tw = topInners[hi];
                bool slant    = (hi < m_holeSlant.size())    && m_holeSlant[hi];
                bool vertical = (hi < m_holeVertical.size()) && m_holeVertical[hi];
                bool topRidesFace = m_moveOuter && m_kind == Kind::Rotate;
                bool topSlides = applyMove && (topRidesFace || slant || vertical);
                bool botSlides = applyMove && vertical;
                gp_Pnt tc = centroid(tw);
                int best = -1; double bd = 1e300;
                for (size_t i = 0; i < baseInners.size(); ++i) {
                    gp_Pnt bc = centroid(baseInners[i]);
                    gp_Vec dv(bc.X() - tc.X(), bc.Y() - tc.Y(), bc.Z() - tc.Z());
                    double d = (dv - N * dv.Dot(N)).Magnitude(); // in-plane only
                    if (d < bd) { bd = d; best = static_cast<int>(i); }
                }
                if (best < 0) continue;
                TopoDS_Wire bi = baseInners[best]; bi.Reverse();
                TopoDS_Shape holeSolid;
                if (!loftSolid(endFor(bi, botSlides), endFor(tw, topSlides), holeSolid))
                    continue;
                BRepAlgoAPI_Cut cut(feat, holeSolid);
                if (cut.IsDone() && !cut.Shape().IsNull()) feat = cut.Shape();
            }
            return feat;
        };

        const bool isTwist = (m_kind == Kind::Twist);
        // On-face sketches follow the spin: give topT the full rotation now that
        // the axis is known (topT was left identity in the switch for Twist).
        if (isTwist) topT.SetRotation(twAxis, m_twistAngle);

        TopoDS_Shape newFeature = isTwist ? buildTwistFeature(true) : buildFeature(true);
        if (newFeature.IsNull()) {
            std::fprintf(stderr, "[MoveFace] feature loft failed — refusing\n");
            return false;
        }

        TopoDS_Shape result;
        if (!subFeature) {
            result = newFeature; // the loft IS the new body (plain prism)
        } else {
            // Cut the ORIGINAL feature out of the body, fuse the transformed one
            // back — keeps every other feature (the funnel above the spout).
            TopoDS_Shape oldFeature = isTwist ? buildTwistFeature(false) : buildFeature(false);
            if (oldFeature.IsNull()) {
                std::fprintf(stderr, "[MoveFace] original-feature loft failed\n");
                return false;
            }
            TopoDS_Shape rest;
            try {
                BRepAlgoAPI_Cut c(m_previousShape, oldFeature);
                c.Build(); if (c.IsDone()) rest = c.Shape();
            } catch (...) {}
            int restSolids = 0;
            if (!rest.IsNull())
                for (TopExp_Explorer sx(rest, TopAbs_SOLID); sx.More(); sx.Next()) ++restSolids;
            if (restSolids < 1) {
                result = newFeature; // the feature was the whole body after all
            } else {
                try {
                    BRepAlgoAPI_Fuse f(rest, newFeature);
                    f.Build(); if (f.IsDone()) result = f.Shape();
                } catch (...) {}
                if (result.IsNull()) {
                    std::fprintf(stderr, "[MoveFace] feature fuse failed — refusing\n");
                    return false;
                }
            }
        }

        if (result.IsNull() || !BRepCheck_Analyzer(result).IsValid()) {
            std::fprintf(stderr, "[MoveFace] result invalid — refusing\n");
            return false;
        }
        int nsolids = 0;
        for (TopExp_Explorer sx(result, TopAbs_SOLID); sx.More(); sx.Next()) ++nsolids;
        if (nsolids < 1) return false;

        // Sanity guards BRepCheck can't provide — a shelled (hollow) body run
        // through the loft/shear rebuild can come out topologically "valid" yet
        // WRONG: an inside-out solid (negative volume) or a re-solidified body
        // whose cavity was silently discarded. Refuse cleanly instead of
        // corrupting the model.
        {
            GProp_GProps gIn, gOut;
            BRepGProp::VolumeProperties(m_previousShape, gIn);
            BRepGProp::VolumeProperties(result, gOut);
            const double vIn = gIn.Mass(), vOut = gOut.Mass();
            if (!(vOut > 1e-9)) {
                std::fprintf(stderr, "[MoveFace] result volume %.3f — inside-out/"
                             "degenerate, refusing\n", vOut);
                return false;
            }
            // A closed internal cavity must survive (shell count preserved).
            int shIn = 0, shOut = 0;
            for (TopExp_Explorer e(m_previousShape, TopAbs_SHELL); e.More(); e.Next()) ++shIn;
            for (TopExp_Explorer e(result, TopAbs_SHELL); e.More(); e.Next()) ++shOut;
            if (shOut < shIn) {
                std::fprintf(stderr, "[MoveFace] would destroy the internal "
                             "cavity (%d -> %d shells) — refusing\n", shIn, shOut);
                return false;
            }
            // A slide is a volume-preserving shear and a tilt is bounded; a
            // large jump means the rebuild filled in a hollow interior (an
            // open-shell body has one shell, so the count can't catch it).
            const double ratio = vIn > 1e-9 ? vOut / vIn : 1.0;
            const bool volumeSane =
                (m_kind == Kind::Translate) ? (ratio > 0.75 && ratio < 1.33)
              : (m_kind == Kind::Rotate)    ? (ratio > 0.40 && ratio < 1.60)
              : true;   // Scale legitimately rescales volume; Twist self-checks
            if (!volumeSane) {
                std::fprintf(stderr, "[MoveFace] volume %.1f -> %.1f — the "
                             "rebuild lost the hollow interior, refusing\n",
                             vIn, vOut);
                return false;
            }
        }

        m_resultShape = result;
        doc.updateBody(m_bodyId, result);

        // Move on-face sketches by the SAME transform (slide / tilt / scale), so
        // they stay glued to the face — but only when the face OUTLINE moves
        // (sketches ride the face, not a hole). Stored for undo.
        m_appliedXform = m_moveOuter ? topT : gp_Trsf();
        if (m_moveOuter)
        for (int sid : m_sketchIds) {
            if (auto sk = doc.getSketch(sid)) {
                gp_Pln pln = sk->getPlane();
                pln.Transform(m_appliedXform);
                sk->setPlane(pln);
                // The cached host face is used to build the sketch's regions —
                // move it too (copy=true forces a fresh TShape so the region
                // cache, keyed on it, invalidates), or its stale geometry
                // highlights at the OLD position when the region is clicked.
                TopoDS_Face sf = sk->getSourceFace();
                if (!sf.IsNull()) {
                    TopoDS_Shape mv = BRepBuilderAPI_Transform(sf, m_appliedXform, Standard_True).Shape();
                    if (!mv.IsNull() && mv.ShapeType() == TopAbs_FACE)
                        sk->setSourceFace(TopoDS::Face(mv));
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool MoveFaceOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) return false;
    try {
        doc.updateBody(m_bodyId, m_previousShape);
        // Move the on-face sketches back by the inverse transform.
        gp_Trsf back = m_appliedXform.Inverted();
        for (int sid : m_sketchIds) {
            if (auto sk = doc.getSketch(sid)) {
                gp_Pln pln = sk->getPlane();
                pln.Transform(back);
                sk->setPlane(pln);
                TopoDS_Face sf = sk->getSourceFace();
                if (!sf.IsNull()) {
                    TopoDS_Shape mv = BRepBuilderAPI_Transform(sf, back, Standard_True).Shape();
                    if (!mv.IsNull() && mv.ShapeType() == TopAbs_FACE)
                        sk->setSourceFace(TopoDS::Face(mv));
                }
            }
        }
        return true;
    } catch (...) { return false; }
}

std::string MoveFaceOp::description() const {
    char buf[96];
    if (m_kind == Kind::Twist) {
        std::snprintf(buf, sizeof(buf), "Twist Face %.1f°",
                      m_twistAngle * 180.0 / M_PI);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "Move Face by (%.2f, %.2f, %.2f)",
                  m_move.X(), m_move.Y(), m_move.Z());
    return buf;
}

void MoveFaceOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Move Face"));
    ImGui::Separator();
    ImGui::Text(materializr::tr("Body ID: %d"), m_bodyId);
    ImGui::Text(materializr::tr("Move: (%.2f, %.2f, %.2f) mm"), m_move.X(), m_move.Y(), m_move.Z());
}

OperationDiff MoveFaceOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}

std::string MoveFaceOp::serializeParams() const {
    std::string blob;
    char buf[200];
    // k = kind (0 Translate, 1 Rotate, 2 Scale, 3 Twist); tw = twist radians.
    // Older files omit both → deserialize keeps the Translate default (the only
    // kind that ever round-tripped before); a Twist round-trips as an editable
    // op via k+tw, other kinds still bake as a ReplayOp on reload.
    std::snprintf(buf, sizeof(buf),
                  "body=%d;k=%d;tw=%.6f;mx=%.6f;my=%.6f;mz=%.6f",
                  m_bodyId, static_cast<int>(m_kind), m_twistAngle,
                  m_move.X(), m_move.Y(), m_move.Z());
    blob += buf;
    if (!m_previousShape.IsNull() && !m_face.IsNull()) {
        std::vector<TopoDS_Shape> faces{m_face};
        std::string idx = SubShapeIndex::serialize(m_previousShape, faces, TopAbs_FACE);
        if (!idx.empty()) blob += ";face=" + idx;
    }
    // Topological face name (additive, robust to a moving edit); written last as
    // a single length-prefixed blob. Absent in old files.
    if (!m_faceRef.empty()) {
        std::string b = m_faceRef.serialize();
        blob += ";faceref=" + std::to_string(b.size()) + ":" + b;
    }
    return blob;
}

bool MoveFaceOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        // faceref is a length-prefixed opaque blob written last — read to end.
        if (key == "faceref") {
            std::string rest = blob.substr(eq + 1);
            size_t c = rest.find(':');
            if (c != std::string::npos) {
                // Checked length, bounded by subtraction (ParamParse.h).
                size_t n = 0, payload = 0;
                if (materializr::readLenPrefix(rest, 0, c, n, payload))
                    m_faceRef = materializr::topo::Ref::parse(rest.substr(payload, n));
            }
            any = true;
            break;
        }
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "body") { m_bodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "k")    { int k = std::atoi(val.c_str());
                                  if (k >= 0 && k <= 3) m_kind = static_cast<Kind>(k); any = true; }
        else if (key == "tw")   { m_twistAngle = std::atof(val.c_str()); any = true; }
        else if (key == "mx")   { m_move.SetX(std::atof(val.c_str())); any = true; }
        else if (key == "my")   { m_move.SetY(std::atof(val.c_str())); any = true; }
        else if (key == "mz")   { m_move.SetZ(std::atof(val.c_str())); any = true; }
        else if (key == "face") { m_faceIndices = SubShapeIndex::parse(val); any = true; }
        pos = end + 1;
    }
    return any;
}

bool MoveFaceOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_bodyId < 0 || m_faceIndices.empty()) return false;
    m_previousShape.Nullify();
    m_resultShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    for (const auto& [id, shp] : state.modifiedAfter)
        if (id == m_bodyId) { m_resultShape = shp; break; }
    if (m_previousShape.IsNull()) return false;

    std::vector<TopoDS_Shape> resolved;
    if (!SubShapeIndex::resolveAll(m_previousShape, m_faceIndices, TopAbs_FACE, resolved) ||
        resolved.empty()) {
        return false;
    }
    m_face = TopoDS::Face(resolved.front());
    return true;
}
