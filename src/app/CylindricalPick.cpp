#include "CylindricalPick.h"
#include "../core/Document.h"
#include "../core/SelectionManager.h"
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepGProp_Face.hxx>
#include <Geom_Surface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <GeomAbs_CurveType.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Cylinder.hxx>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {

CylindricalPick detectCylindricalPick(const Document& doc,
                                      const SelectionManager& selection) {
    CylindricalPick p;

    // Accept either exactly one face (edits both ends → stays cylindrical) or
    // exactly one edge (edits just that end → makes a cone). Anything else is
    // unambiguous to interpret, so we bail.
    TopoDS_Shape pickedFace, pickedEdge;
    int bodyId = -1;
    int faceCount = 0, edgeCount = 0;
    for (const auto& e : selection.getSelection()) {
        if (e.shape.IsNull()) continue;
        if (e.type == SelectionType::Face) {
            ++faceCount; pickedFace = e.shape; bodyId = e.bodyId;
        } else if (e.type == SelectionType::Edge) {
            ++edgeCount; pickedEdge = e.shape; bodyId = e.bodyId;
        }
    }
    if (bodyId < 0) return p;
    if (faceCount + edgeCount != 1) return p;

    // The selection can name a body that no longer exists. This runs from the
    // frame loop whenever the selection OR HISTORY revision changes, and an
    // Apply Changes bumps the history revision: a replay retires the body it
    // rebuilds, while the selection still holds the OLD id for one more frame.
    // Unguarded, getBody's throw escaped the frame, reached main()'s handler
    // and - on Android - made SDL_main return, which finishes the activity, so
    // the app silently VANISHED with the user's unsaved work. (Steve's tablet,
    // "Body not found: 1"; found via the throw-site backtrace in ThrowTrace.h.)
    // Nothing to detect on a body that's gone; the next frame re-runs this
    // with a settled selection.
    TopoDS_Shape body;
    try { body = doc.getBody(bodyId); } catch (...) { return p; }

    // Find the cylindrical face we'll operate on. For a face pick, it's the
    // pick itself (must be cylindrical). For an edge pick, walk the body's
    // faces and pick the first cylindrical one that contains the edge.
    TopoDS_Face cylFace;
    if (!pickedFace.IsNull()) {
        TopoDS_Face face = TopoDS::Face(pickedFace);
        Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
        if (Handle(Geom_CylindricalSurface)::DownCast(surf).IsNull()) return p;
        cylFace = face;
    } else {
        TopoDS_Edge edge = TopoDS::Edge(pickedEdge);
        // Edge must be a circle for the diameter concept to make sense.
        try {
            BRepAdaptor_Curve curve(edge);
            if (curve.GetType() != GeomAbs_Circle) return p;
        } catch (...) { return p; }

        TopExp_Explorer fex(body, TopAbs_FACE);
        for (; fex.More(); fex.Next()) {
            TopoDS_Face face = TopoDS::Face(fex.Current());
            Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
            if (Handle(Geom_CylindricalSurface)::DownCast(surf).IsNull()) continue;
            TopExp_Explorer eex(face, TopAbs_EDGE);
            for (; eex.More(); eex.Next()) {
                if (eex.Current().IsSame(edge)) { cylFace = face; break; }
            }
            if (!cylFace.IsNull()) break;
        }
        if (cylFace.IsNull()) return p;
    }

    Handle(Geom_Surface) surf = BRep_Tool::Surface(cylFace);
    Handle(Geom_CylindricalSurface) cylSurf =
        Handle(Geom_CylindricalSurface)::DownCast(surf);
    if (cylSurf.IsNull()) return p;

    // Bounded parametric range. U = angular wrap, V = along axis. Must be a
    // CLOSED cylinder (full 2π) - partial sleeves (fillet faces) don't have
    // a meaningful single diameter.
    double u1, u2, v1, v2;
    BRepTools::UVBounds(cylFace, u1, u2, v1, v2);
    const double kCircle = 2.0 * M_PI;
    if (std::abs((u2 - u1) - kCircle) > 1e-3) return p;
    double height = std::abs(v2 - v1);
    if (height < 1e-6) return p;

    gp_Cylinder cyl = cylSurf->Cylinder();
    gp_Pnt shifted = cyl.Position().Location()
                        .Translated(gp_Vec(cyl.Position().Direction()) * v1);
    gp_Ax2 axis(shifted, cyl.Position().Direction(), cyl.Position().XDirection());
    double radius = cyl.Radius();

    // Hole vs solid boundary, from the face's outward normal at its centre.
    // Normal toward axis → material is OUTSIDE → hole. Away → solid boundary.
    // BRepGProp_Face::Normal() already applies the face's orientation
    // internally - don't reverse again (doing so double-negates and makes
    // every hole look like a solid boundary).
    BRepGProp_Face prop(cylFace);
    gp_Pnt centerPt; gp_Vec normVec;
    prop.Normal(0.5 * (u1 + u2), 0.5 * (v1 + v2), centerPt, normVec);
    gp_Vec axisVec(axis.Direction());
    gp_Vec toCenter(shifted, centerPt);
    toCenter -= axisVec * toCenter.Dot(axisVec);
    if (toCenter.Magnitude() < 1e-6) return p;
    bool isHole = (normVec.Dot(toCenter) < 0.0);

    // Which end(s) are we editing? Face pick → both. Edge pick → just the
    // end whose centroid is closer to that edge's circle centre.
    bool editBottom = true, editTop = true;
    if (!pickedEdge.IsNull()) {
        BRepAdaptor_Curve curve(TopoDS::Edge(pickedEdge));
        gp_Pnt edgeC = curve.Circle().Location();
        gp_Pnt botPnt = shifted;
        gp_Pnt topPnt = shifted.Translated(gp_Vec(axis.Direction()) * height);
        if (edgeC.Distance(botPnt) < edgeC.Distance(topPnt)) {
            editBottom = true; editTop = false;
        } else {
            editBottom = false; editTop = true;
        }
    }

    p.ok         = true;
    p.bodyId     = bodyId;
    p.isHole     = isHole;
    p.axis       = axis;
    p.height     = height;
    p.bottomR    = radius;
    p.topR       = radius;
    p.editBottom = editBottom;
    p.editTop    = editTop;
    return p;
}

} // namespace materializr
