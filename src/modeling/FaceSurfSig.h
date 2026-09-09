#pragma once
// Geometric "same underlying surface?" test for two faces.
//
// The history-hover highlight fallback (facesCreatedVsPrev in Fillet/ChamferOp)
// needs to tell the faces an op CREATED from the faces it merely re-trimmed.
// Matching by face centroid is wrong: when a new chamfer/fillet trims a corner
// off an ADJACENT earlier bevel, that face keeps its exact surface but its
// centroid shifts - so a centroid test flags it as "new" and the hover lights
// up bevels from earlier steps. Comparing the unbounded surface instead, a
// trimmed face still matches its prev self (same plane/cylinder/…); only a
// genuinely new blend surface fails to match and is reported as created.
//
// Tolerances are generous (boolean rebuilds perturb surface params slightly);
// distinct model surfaces differ by far more than these, so loose is safe here
// and errs away from the over-highlight we're fixing.

#include <TopoDS_Face.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <gp_Pln.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Cone.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>
#include <cmath>

namespace materializr {

// True if a and b lie on the same unbounded analytic surface (same plane,
// coaxial+same-radius cylinder, etc.), independent of how each is trimmed.
// Non-analytic surfaces (B-spline/Bézier) return false - the caller falls
// back to its centroid test for those, preserving prior behaviour.
inline bool sameSurface(const TopoDS_Face& a, const TopoDS_Face& b,
                        double tolLin = 1e-4, double tolAng = 1e-4) {
    try {
        BRepAdaptor_Surface sa(a), sb(b);
        if (sa.GetType() != sb.GetType()) return false;
        // Same infinite line: parallel directions and zero perpendicular gap.
        auto axisSame = [&](const gp_Ax1& x, const gp_Ax1& y) {
            if (!x.Direction().IsParallel(y.Direction(), tolAng)) return false;
            gp_Vec off(x.Location(), y.Location());
            return off.CrossMagnitude(gp_Vec(x.Direction())) < tolLin;
        };
        switch (sa.GetType()) {
            case GeomAbs_Plane: {
                const gp_Pln pa = sa.Plane(), pb = sb.Plane();
                return pa.Axis().Direction().IsParallel(
                           pb.Axis().Direction(), tolAng)
                    && pa.Distance(pb.Location()) < tolLin;
            }
            case GeomAbs_Cylinder: {
                const gp_Cylinder ca = sa.Cylinder(), cb = sb.Cylinder();
                return std::abs(ca.Radius() - cb.Radius()) < tolLin
                    && axisSame(ca.Axis(), cb.Axis());
            }
            case GeomAbs_Cone: {
                const gp_Cone ca = sa.Cone(), cb = sb.Cone();
                return std::abs(ca.SemiAngle() - cb.SemiAngle()) < tolAng
                    && ca.Apex().Distance(cb.Apex()) < tolLin
                    && ca.Axis().Direction().IsParallel(
                           cb.Axis().Direction(), tolAng);
            }
            case GeomAbs_Sphere: {
                const gp_Sphere pa = sa.Sphere(), pb = sb.Sphere();
                return std::abs(pa.Radius() - pb.Radius()) < tolLin
                    && pa.Location().Distance(pb.Location()) < tolLin;
            }
            case GeomAbs_Torus: {
                const gp_Torus ta = sa.Torus(), tb = sb.Torus();
                return std::abs(ta.MajorRadius() - tb.MajorRadius()) < tolLin
                    && std::abs(ta.MinorRadius() - tb.MinorRadius()) < tolLin
                    && axisSame(ta.Axis(), tb.Axis());
            }
            default:
                return false;  // non-analytic - caller falls back to centroid
        }
    } catch (...) { return false; }
}

}  // namespace materializr
