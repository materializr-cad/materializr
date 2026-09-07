#include "ui/LengthField.h"
#include "core/Units.h"
#include "ScaleFaceOp.h"
#include "SubShapeIndex.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <GProp_GProps.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include "UnifyTolerance.h"
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>
#include <gp_Trsf.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Mat.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <gp_Vec.hxx>
#include <gp_Pln.hxx>
#include <imgui.h>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"

ScaleFaceOp::ScaleFaceOp() = default;

void ScaleFaceOp::setBody(int id) { m_bodyId = id; }
void ScaleFaceOp::setFace(const TopoDS_Face& f) { m_face = f; }
void ScaleFaceOp::setScalePercent(double s) { m_scaleU = m_scaleV = s; }
void ScaleFaceOp::setScaleUV(double su, double sv) {
    m_scaleU = su;
    m_scaleV = sv;
}
void ScaleFaceOp::setLength(double l) { m_length = l; }
void ScaleFaceOp::setMode(Mode m) { m_mode = m; }

bool ScaleFaceOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_face.IsNull() || m_length <= 1e-6 ||
        m_scaleU < 1.0 || m_scaleU > 500.0 ||
        m_scaleV < 1.0 || m_scaleV > 500.0 ||
        (std::abs(m_scaleU - 100.0) < 1e-6 &&
         std::abs(m_scaleV - 100.0) < 1e-6)) {
        return false;
    }
    try {
        m_previousShape = doc.getBody(m_bodyId);

        // Planar end faces only (a wing tip cap, a box end…).
        Handle(Geom_Plane) pl =
            Handle(Geom_Plane)::DownCast(BRep_Tool::Surface(m_face));
        if (pl.IsNull()) {
            std::fprintf(stderr, "[ScaleFace] face is not planar\n");
            return false;
        }

        // Outward normal (orientation-aware) and the face centroid — the
        // scale pivot.
        BRepGProp_Face gpf(m_face);
        double u1, u2, v1, v2;
        gpf.Bounds(u1, u2, v1, v2);
        gp_Pnt onFace;
        gp_Vec nv;
        gpf.Normal(0.5 * (u1 + u2), 0.5 * (v1 + v2), onFace, nv);
        if (nv.Magnitude() < 1e-9) return false;
        gp_Dir n(nv);
        GProp_GProps fp;
        BRepGProp::SurfaceProperties(m_face, fp);
        gp_Pnt centroid = fp.CentreOfMass();

        TopoDS_Wire capWire = BRepTools::OuterWire(m_face);
        if (capWire.IsNull()) return false;

        double su = m_scaleU / 100.0;
        double sv = m_scaleV / 100.0;

        // NON-UNIFORM scale about the centroid in the face plane's own
        // X / Y directions (deterministic per face, so reload-safe):
        // M = su*(u (x) u) + sv*(v (x) v) + (n (x) n), translation keeps
        // the centroid fixed. gp_Trsf can't do this; gp_GTrsf can.
        // COPY the plane — Pln() returns a temporary, and a reference
        // into it dangles after this statement (the axes came out as
        // uninitialized garbage: zero scale matrix, NaN directions).
        const gp_Pln fpln = pl->Pln();
        const gp_Ax3& fax = fpln.Position();
        gp_Dir ud = fax.XDirection();
        gp_Dir vd = fax.YDirection();
        gp_Dir nd = fax.Direction();
        auto outer = [](const gp_Dir& a, double k) {
            return gp_Mat(k * a.X() * a.X(), k * a.X() * a.Y(), k * a.X() * a.Z(),
                          k * a.Y() * a.X(), k * a.Y() * a.Y(), k * a.Y() * a.Z(),
                          k * a.Z() * a.X(), k * a.Z() * a.Y(), k * a.Z() * a.Z());
        };
        gp_Mat M = outer(ud, su);
        M += outer(vd, sv);
        M += outer(nd, 1.0);
        gp_XYZ c(centroid.X(), centroid.Y(), centroid.Z());
        gp_GTrsf scaleT;
        scaleT.SetVectorialPart(M);
        scaleT.SetTranslationPart(c - M * c);

        auto movedWire = [&](const TopoDS_Wire& w,
                             const gp_Trsf& t) -> TopoDS_Wire {
            BRepBuilderAPI_Transform xf(w, t, Standard_True);
            return TopoDS::Wire(xf.Shape());
        };
        auto gMovedWire = [&](const TopoDS_Wire& w,
                              const gp_GTrsf& t) -> TopoDS_Wire {
            BRepBuilderAPI_GTransform xf(w, t, Standard_True);
            return TopoDS::Wire(xf.Shape());
        };
        // UNIFORM scale must stay exact. GTransform converts every analytic
        // curve to a bspline (a general affine map can turn a circle into an
        // ellipse, so OCCT downgrades unconditionally) — a uniformly scaled
        // circle came back as a wobbly approximation: the loft wall rendered
        // lumpy and the scaled cap centroid drifted off-axis (Steve's
        // "strange geometry on the side wall", 2026-08-04). gp_Trsf's true
        // scaling keeps circles circles; the wire lies in the face plane
        // through the centroid, so the 3D scale about it IS the in-plane
        // scale. Only genuinely non-uniform scaling pays the bspline cost.
        const bool uniformScale = std::abs(su - sv) < 1e-9;
        gp_Trsf uScaleT;
        if (uniformScale) uScaleT.SetScale(centroid, su);
        auto scaledWire = [&](const TopoDS_Wire& w) -> TopoDS_Wire {
            return uniformScale ? movedWire(w, uScaleT)
                                : gMovedWire(w, scaleT);
        };

        TopoDS_Shape result;
        if (m_mode == Mode::Extend) {
            // Tip extension: cap outline → scaled outline at +L outward.
            gp_Trsf off;
            off.SetTranslation(gp_Vec(n) * m_length);
            TopoDS_Wire wTip = movedWire(scaledWire(capWire), off);

            // Ruled, not smoothed: the default approximation mode fits a
            // bspline through the sections even when a ruled surface is
            // exact (two circles → cone), and rounds the corners of scaled
            // polygonal caps.
            BRepOffsetAPI_ThruSections loft(Standard_True, Standard_True);
            loft.AddWire(capWire);
            loft.AddWire(wTip);
            loft.Build();
            if (!loft.IsDone()) {
                std::fprintf(stderr, "[ScaleFace] tip loft failed\n");
                return false;
            }
            BRepAlgoAPI_Fuse fuse(m_previousShape, loft.Shape());
            fuse.SetFuzzyValue(1.0e-4);
            fuse.Build();
            if (!fuse.IsDone()) {
                std::fprintf(stderr, "[ScaleFace] fuse failed\n");
                return false;
            }
            result = fuse.Shape();
        } else {
            // Pinch: reshape the last L of the body toward the scaled
            // outline. When L spans the WHOLE body ("scale the top face
            // and the sides follow from the base" — the default), the cut
            // degenerates and a single Common against the frustum does
            // everything.
            Bnd_Box bb;
            BRepBndLib::Add(m_previousShape, bb);
            double bx0, by0, bz0, bx1, by1, bz1;
            bb.Get(bx0, by0, bz0, bx1, by1, bz1);
            double diag = gp_Pnt(bx0, by0, bz0).Distance(
                gp_Pnt(bx1, by1, bz1)) + m_length;

            // Depth of the body behind the face along -n.
            double depth = 0.0;
            {
                gp_Pnt corners[8] = {
                    gp_Pnt(bx0, by0, bz0), gp_Pnt(bx1, by0, bz0),
                    gp_Pnt(bx0, by1, bz0), gp_Pnt(bx1, by1, bz0),
                    gp_Pnt(bx0, by0, bz1), gp_Pnt(bx1, by0, bz1),
                    gp_Pnt(bx0, by1, bz1), gp_Pnt(bx1, by1, bz1)};
                for (const auto& c : corners) {
                    double d = gp_Vec(c, centroid).Dot(gp_Vec(n));
                    depth = std::max(depth, d);
                }
            }
            bool fullDepth = m_length >= depth - 1e-4;

            gp_Pnt cutPt = centroid.Translated(gp_Vec(n) * (-m_length));

            // Pinching frustum: full-size outline AT the cut plane (or
            // just past the body's far side for full-depth) → scaled
            // outline at the original cap plane.
            gp_Trsf back;
            back.SetTranslation(gp_Vec(n) * (-m_length));
            TopoDS_Wire wBase = movedWire(capWire, back);
            TopoDS_Wire wTip = scaledWire(capWire);

            // Ruled for the same reason as the tip loft above.
            BRepOffsetAPI_ThruSections loft(Standard_True, Standard_True);
            loft.AddWire(wBase);
            loft.AddWire(wTip);
            loft.Build();
            if (!loft.IsDone()) {
                std::fprintf(stderr, "[ScaleFace] frustum loft failed\n");
                return false;
            }

            // GROWING flips the boolean. Common() can only ever remove
            // material, so a frustum wider than the body just clipped back to
            // the body and the op reported success having changed nothing
            // (Steve: "scale only makes a face smaller"). The frustum already
            // describes the wanted shape in both directions — full-size
            // outline at the base, scaled outline at the face — so growing is
            // the same solid UNIONED on instead of intersected, which flares
            // the side walls outward from the base and keeps the body's other
            // features. Union works for the partial-length case too: the
            // frustum only spans the last L, so only that band flares.
            const bool grow = (su > 1.0 || sv > 1.0);
            if (grow) {
                BRepAlgoAPI_Fuse fuse(m_previousShape, loft.Shape());
                fuse.SetFuzzyValue(1.0e-4);
                fuse.Build();
                if (!fuse.IsDone()) return false;
                result = fuse.Shape();
            } else if (fullDepth) {
                // The frustum spans the entire body: one Common does it.
                BRepAlgoAPI_Common common(m_previousShape, loft.Shape());
                common.SetFuzzyValue(1.0e-4);
                common.Build();
                if (!common.IsDone()) return false;
                result = common.Shape();
            } else {
                gp_Pln cutPln(cutPt, n);
                TopoDS_Face bigFace = BRepBuilderAPI_MakeFace(
                    cutPln, -diag, diag, -diag, diag).Face();
                TopoDS_Shape tipBox =
                    BRepPrimAPI_MakePrism(bigFace, gp_Vec(n) * (2.0 * diag))
                        .Shape();

                BRepAlgoAPI_Cut mainCut(m_previousShape, tipBox);
                mainCut.SetFuzzyValue(1.0e-4);
                mainCut.Build();
                if (!mainCut.IsDone()) return false;
                TopoDS_Shape mainPiece = mainCut.Shape();

                BRepAlgoAPI_Common tipCommon(m_previousShape, loft.Shape());
                tipCommon.SetFuzzyValue(1.0e-4);
                tipCommon.Build();
                if (!tipCommon.IsDone()) return false;
                // Keep only the part beyond the cut plane — the frustum
                // also overlaps inboard material.
                BRepAlgoAPI_Common tipPiece(tipCommon.Shape(), tipBox);
                tipPiece.SetFuzzyValue(1.0e-4);
                tipPiece.Build();
                if (!tipPiece.IsDone()) return false;

                BRepAlgoAPI_Fuse fuse(mainPiece, tipPiece.Shape());
                fuse.SetFuzzyValue(1.0e-4);
                fuse.Build();
                if (!fuse.IsDone()) return false;
                result = fuse.Shape();
            }
        }

        if (result.IsNull()) return false;

        // The booleans (the grow-Fuse especially) leave same-surface faces
        // split — the grown cap arrived as the ORIGINAL top disc plus a
        // coplanar annulus stacked at the same height. Merge them, the same
        // way Push/Pull does after its cut/fuse.
        result = materializr::unifySameDomain(result, "ScaleFace");

        // Sanity: the result must still have volume, and pinch must not
        // have annihilated the body.
        GProp_GProps vp;
        BRepGProp::VolumeProperties(result, vp);
        if (vp.Mass() < 1e-6) {
            std::fprintf(stderr, "[ScaleFace] degenerate result\n");
            return false;
        }

        if (!BRepCheck_Analyzer(result).IsValid()) {
            ShapeFix_Shape fixer(result);
            fixer.Perform();
            result = fixer.Shape();
        }
        doc.updateBody(m_bodyId, result);
        return true;
    } catch (...) {
        std::fprintf(stderr, "[ScaleFace] execute threw\n");
        return false;
    }
}

bool ScaleFaceOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) return false;
    try {
        doc.updateBody(m_bodyId, m_previousShape);
        return true;
    } catch (...) { return false; }
}

std::string ScaleFaceOp::description() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Scale face to %.0f%%/%.0f%% over %s (%s)", m_scaleU, m_scaleV, materializr::fmtLength(m_length).c_str(), m_mode == Mode::Extend ? "extend" : "pinch");
    return buf;
}

void ScaleFaceOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Scale Face"));
    ImGui::Separator();
    // PERCENTAGES, not lengths — the header says "percent along the face
    // plane's XDirection" and the labels say (%). Routed through lengthField
    // they were converted display->mm on commit, so typing 100 under inches
    // stored 2540%.
    materializr::inputNumber(materializr::tr("Scale U (%)"), &m_scaleU, 1.0, 10.0, "%.1f");
    materializr::inputNumber(materializr::tr("Scale V (%)"), &m_scaleV, 1.0, 10.0, "%.1f");
    materializr::lengthField(materializr::trFormat("Length (%s)", materializr::unitSuffix()).c_str(), &m_length);
    ImGui::Text(materializr::tr("Mode: %s"), m_mode == Mode::Extend ? "Extend" : "Pinch");
    ImGui::Text(materializr::tr("Body ID: %d"), m_bodyId);
}

std::string ScaleFaceOp::serializeParams() const {
    std::string blob;
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "body=%d;scaleu=%.6f;scalev=%.6f;len=%.6f;mode=%d",
                  m_bodyId, m_scaleU, m_scaleV, m_length,
                  static_cast<int>(m_mode));
    blob += buf;
    if (!m_previousShape.IsNull() && !m_face.IsNull()) {
        std::vector<TopoDS_Shape> faces{m_face};
        std::string idx = SubShapeIndex::serialize(m_previousShape, faces,
                                                   TopAbs_FACE);
        if (!idx.empty()) blob += ";faces=" + idx;
    }
    return blob;
}

bool ScaleFaceOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "body")  { m_bodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "scale") { m_scaleU = m_scaleV = std::atof(val.c_str()); any = true; }
        else if (key == "scaleu") { m_scaleU = std::atof(val.c_str()); any = true; }
        else if (key == "scalev") { m_scaleV = std::atof(val.c_str()); any = true; }
        else if (key == "len")   { m_length = std::atof(val.c_str()); any = true; }
        else if (key == "mode")  {
            m_mode = std::atoi(val.c_str()) == 1 ? Mode::Pinch : Mode::Extend;
            any = true;
        }
        else if (key == "faces") {
            m_faceIndices = SubShapeIndex::parse(val);
            any = true;
        }
        pos = end + 1;
    }
    return any;
}

bool ScaleFaceOp::rehydrateFromReload(const ReloadState& state,
                                      Document& /*doc*/) {
    if (m_bodyId < 0 || m_faceIndices.empty()) return false;

    m_previousShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    if (m_previousShape.IsNull()) return false;

    std::vector<TopoDS_Shape> resolved;
    if (!SubShapeIndex::resolveAll(m_previousShape, m_faceIndices,
                                   TopAbs_FACE, resolved) ||
        resolved.empty()) {
        return false;
    }
    m_face = TopoDS::Face(resolved.front());
    return true;
}

OperationDiff ScaleFaceOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}
