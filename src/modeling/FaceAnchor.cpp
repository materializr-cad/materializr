#include "FaceAnchor.h"
#include "Sketch.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <cstdlib>
#include <cstdio>
#include <BRepTools.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Curve.hxx>
#include <Geom_SurfaceOfLinearExtrusion.hxx>
#include <GeomAdaptor_Curve.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <glm/glm.hpp>
#include <cmath>
#include <cstdio>

namespace FaceAnchor {
namespace {

constexpr double kUVTol   = 1e-3;  // sketch-plane position match tolerance
constexpr double kAxisDot = 0.999; // |dir·axis| above this = parallel
// Loose second pass: a project saved after a failed cascade holds sketches
// slightly ahead of the body (uncommitted drift), so retrofit capture may pair
// stale geometry with a drifted sketch. Applied only to faces the strict pass
// leaves unattributed. Mirrors EdgeAnchor::kDriftTol.
constexpr double kDriftTol = 1.0;

// Sketch-plane frame - identical convention to EdgeAnchor (o + axis/xd/yd,
// with u/v in-plane and h along the normal).
struct Frame {
    gp_Pnt o; gp_Dir axis, xd, yd;
    double u(const gp_Pnt& p) const { return gp_Vec(o, p).Dot(gp_Vec(xd)); }
    double v(const gp_Pnt& p) const { return gp_Vec(o, p).Dot(gp_Vec(yd)); }
    double h(const gp_Pnt& p) const { return gp_Vec(o, p).Dot(gp_Vec(axis)); }
};

Frame frameOf(const materializr::Sketch& sk) {
    const gp_Pln& pln = sk.getPlane();
    return { pln.Location(), pln.Axis().Direction(),
             pln.XAxis().Direction(), pln.YAxis().Direction() };
}

bool near2(double ax, double ay, double bx, double by, double tol) {
    return std::hypot(ax - bx, ay - by) < tol;
}

bool faceCentroid(const TopoDS_Face& f, gp_Pnt& c) {
    try {
        GProp_GProps props;
        BRepGProp::SurfaceProperties(f, props);
        c = props.CentreOfMass();
        return true;
    } catch (...) { return false; }
}

// (u,v)-fraction of point P along segment A-B; and is P on the segment within
// tol? Straight copy of EdgeAnchor::onSegment's geometry so wall footprints and
// rim edges classify identically. Sets t = fraction of the projection.
bool onSegment(double px, double py, double ax, double ay, double bx, double by,
               double tol, double& t) {
    const double dx = bx - ax, dy = by - ay;
    const double len2 = dx * dx + dy * dy;
    if (len2 < tol * tol) return false;      // degenerate line
    const double len = std::sqrt(len2);
    const double s = ((px - ax) * dx + (py - ay) * dy) / len2;
    if (s < -tol / len || s > 1.0 + tol / len) return false;   // beyond A-B
    const double cx = ax + s * dx, cy = ay + s * dy;
    if (!near2(px, py, cx, cy, tol)) return false;             // off the line
    t = s;
    return true;
}

// Distance from (px,py) to a sampled polyline (min over segments). Large if
// the polyline is degenerate.
double distToPolyline(double px, double py, const std::vector<glm::vec2>& poly) {
    double best = 1e18;
    for (size_t i = 1; i < poly.size(); ++i) {
        const double ax = poly[i - 1].x, ay = poly[i - 1].y;
        const double bx = poly[i].x,     by = poly[i].y;
        const double dx = bx - ax, dy = by - ay;
        const double len2 = dx * dx + dy * dy;
        double s = len2 > 1e-18 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0;
        s = std::max(0.0, std::min(1.0, s));
        best = std::min(best, std::hypot(px - (ax + s * dx), py - (ay + s * dy)));
    }
    return best;
}

// Sample the (profile) basis curve of a surface-of-extrusion face. For a
// trimmed face the underlying surface still carries the FULL profile curve, so
// these points trace the whole sketch curve regardless of clipping - good for
// attribution; the centroid disambiguates fragments at resolve.
std::vector<gp_Pnt> extrusionProfilePts(const TopoDS_Face& face) {
    std::vector<gp_Pnt> pts;
    Handle(Geom_Surface) gs = BRep_Tool::Surface(face);
    Handle(Geom_SurfaceOfLinearExtrusion) ext =
        Handle(Geom_SurfaceOfLinearExtrusion)::DownCast(gs);
    if (ext.IsNull()) return pts;
    Handle(Geom_Curve) bc = ext->BasisCurve();
    if (bc.IsNull()) return pts;
    GeomAdaptor_Curve ac(bc);
    double a = ac.FirstParameter(), b = ac.LastParameter();
    // Sample over the FACE's trimmed U-range, not the basis curve's full
    // parameter span. After the region walker splits a spline wall at an
    // intersection, the basis curve extends past the face - full-range
    // samples run off the sketch spline and the whole face was rejected
    // (corvus body 532: 15 extr walls unclassified).
    double u0, u1, v0, v1;
    try {
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        if (std::isfinite(u0) && std::isfinite(u1) && u1 > u0) {
            a = std::max(a, u0);
            b = std::min(b, u1);
        }
    } catch (...) {}
    if (!std::isfinite(a) || !std::isfinite(b) || b <= a) return pts;
    const int N = 24;
    for (int i = 0; i <= N; ++i) {
        try { pts.push_back(ac.Value(a + (b - a) * i / N)); } catch (...) {}
    }
    return pts;
}

// Classify one face against one sketch frame. Returns a None anchor on no
// match. `tol` widens for the drift pass.
Anchor classify(const TopoDS_Face& face, int sketchId,
                const materializr::Sketch& sk, const Frame& f, double tol) {
    Anchor a;
    gp_Pnt c;
    if (!faceCentroid(face, c)) return a;
    const double cu = f.u(c), cv = f.v(c), ch = f.h(c);

    BRepAdaptor_Surface s(face);
    const GeomAbs_SurfaceType type = s.GetType();

    if (type == GeomAbs_Plane) {
        const gp_Dir n = s.Plane().Axis().Direction();
        const double align = std::abs(n.Dot(f.axis));
        if (align > kAxisDot) {
            // Cap - normal parallel to the extrude axis. Not element-tied.
            return { Anchor::Cap, sketchId, -1, ch, cu, cv };
        }
        // Wall - normal perpendicular to the axis: swept from a sketch line the
        // centroid sits over. Intersecting features clip walls into fragments,
        // so on-segment containment (not endpoint equality), like rim edges.
        for (const auto& L : sk.getLines()) {
            const auto* pa = sk.getPoint(L.startPointId);
            const auto* pb = sk.getPoint(L.endPointId);
            if (!pa || !pb) continue;
            double t;
            if (onSegment(cu, cv, pa->pos.x, pa->pos.y, pb->pos.x, pb->pos.y,
                          tol, t))
                return { Anchor::Wall, sketchId, L.id, ch, cu, cv };
        }
        return a; // wall over no line of THIS sketch
    }

    if (type == GeomAbs_Cylinder) {
        const gp_Ax1 ax = s.Cylinder().Axis();
        if (std::abs(ax.Direction().Dot(f.axis)) < kAxisDot) return a; // oblique
        const double r  = s.Cylinder().Radius();
        const double au = f.u(ax.Location()), av = f.v(ax.Location());
        for (const auto& A : sk.getArcs()) {
            const auto* pc = sk.getPoint(A.centerPointId);
            if (!pc) continue;
            if (near2(au, av, pc->pos.x, pc->pos.y, tol) &&
                std::abs(r - A.radius) < tol)
                return { Anchor::Cyl, sketchId, A.id, ch, cu, cv };
        }
        for (const auto& C : sk.getCircles()) {
            const auto* pc = sk.getPoint(C.centerPointId);
            if (!pc) continue;
            if (near2(au, av, pc->pos.x, pc->pos.y, tol) &&
                std::abs(r - C.radius) < tol)
                return { Anchor::Cyl, sketchId, C.id, ch, cu, cv };
        }
        return a;
    }

    if (type == GeomAbs_SurfaceOfExtrusion) {
        // Curved wall swept from a sketch spline: extruded along the sketch
        // normal, its basis (profile) curve lying on a spline's sampled curve.
        // Curve matching is inherently approximate, so allow a little slack
        // over the position tol.
        if (std::abs(s.Direction().Dot(f.axis)) < kAxisDot) return a;
        const std::vector<gp_Pnt> prof = extrusionProfilePts(face);
        if (prof.empty()) return a;
        const double ctol = std::max(tol, 0.02);
        for (const auto& sp : sk.getSplines()) {
            const auto poly = sk.sampleSpline2D(sp, 48);
            if (poly.size() < 2) continue;
            int miss = 0;
            double worst = 0.0;
            for (const auto& p : prof) {
                double d = distToPolyline(f.u(p), f.v(p), poly);
                if (d > ctol) ++miss;
                worst = std::max(worst, d);
            }
            if (std::getenv("MZR_CWALL_DEBUG") && miss > 0 && worst < 20.0) {
                gp_Pnt p0 = prof.front();
                std::fprintf(stderr, "[cwall] sk %d spline %d: miss %d/%zu "
                             "worst %.3f prof0 (%.2f,%.2f) poly0 (%.2f,%.2f) "
                             "polyN (%.2f,%.2f)\n",
                             sketchId, sp.id, miss, prof.size(), worst,
                             f.u(p0), f.v(p0), poly.front().x, poly.front().y,
                             poly.back().x, poly.back().y);
            }
            // Allow ~10% strays (interpolated-basis wiggle at trim ends);
            // resolve-side distinctness still rejects collisions.
            if (miss * 10 <= static_cast<int>(prof.size()))
                return { Anchor::CurveWall, sketchId, sp.id, ch, cu, cv };
        }
        return a;
    }

    return a; // cones/tori/bspline surfaces → unattributed (gen-kernel kinds)
}

// score(candidate, anchor): lower is better. Element already matched, so this
// only disambiguates same-element candidates (fragments, top vs bottom cap).
double score(const Anchor& cand, const Anchor& a) {
    return std::abs(cand.h - a.h) + std::hypot(cand.cu - a.cu, cand.cv - a.cv);
}

} // namespace

std::vector<Anchor> compute(const std::vector<TopoDS_Face>& faces,
                            const std::vector<SketchRef>& sketches) {
    std::vector<Anchor> out(faces.size());
    std::vector<Frame> frames;
    frames.reserve(sketches.size());
    for (const auto& [sid, sk] : sketches) frames.push_back(frameOf(*sk));

    for (double tol : { kUVTol, kDriftTol }) {
        for (size_t i = 0; i < faces.size(); ++i) {
            if (out[i].kind != Anchor::None) continue;
            for (size_t s = 0; s < sketches.size(); ++s) {
                Anchor a = classify(faces[i], sketches[s].first,
                                    *sketches[s].second, frames[s], tol);
                if (a.kind != Anchor::None) { out[i] = a; break; }
            }
        }
    }
    return out;
}

bool resolve(const std::vector<Anchor>& anchors,
             const std::vector<SketchRef>& sketches,
             const TopoDS_Shape& base, std::vector<TopoDS_Face>& out) {
    out.clear();
    if (anchors.empty()) return false;

    TopTools_IndexedMapOfShape faceMap;
    TopExp::MapShapes(base, TopAbs_FACE, faceMap);

    std::vector<TopoDS_Face> used;
    auto isUsed = [&](const TopoDS_Face& f) {
        for (const auto& u : used) if (u.IsSame(f)) return true;
        return false;
    };

    for (const Anchor& a : anchors) {
        if (a.kind == Anchor::None) return false;
        const materializr::Sketch* sk = nullptr;
        for (const auto& [sid, s] : sketches)
            if (sid == a.sketchId) { sk = s; break; }
        if (!sk) return false;
        Frame f = frameOf(*sk);

        auto pick = [&](double tol, TopoDS_Face& winner) -> bool {
            bool found = false;
            double bestScore = 0.0;
            for (int i = 1; i <= faceMap.Extent(); ++i) {
                const TopoDS_Face& cf = TopoDS::Face(faceMap(i));
                if (isUsed(cf)) continue;
                Anchor c = classify(cf, a.sketchId, *sk, f, tol);
                if (c.kind != a.kind) continue;
                if (a.kind != Anchor::Cap && c.elemId != a.elemId) continue;
                const double sc = score(c, a);
                if (!found || sc < bestScore) { winner = cf; found = true; bestScore = sc; }
            }
            return found;
        };

        TopoDS_Face best;
        if (!pick(kUVTol, best) && !pick(kDriftTol, best)) return false;
        used.push_back(best);
        out.push_back(best);
    }
    return true;
}

std::string serialize(const std::vector<Anchor>& anchors) {
    bool any = false;
    for (const auto& a : anchors) if (a.kind != Anchor::None) { any = true; break; }
    if (!any) return "";
    std::string s = "v1";
    char buf[128];
    for (const auto& a : anchors) {
        s += '~';
        switch (a.kind) {
            case Anchor::Wall:
                std::snprintf(buf, sizeof(buf), "W,%d,%d,%.6f,%.6f,%.6f",
                              a.sketchId, a.elemId, a.h, a.cu, a.cv);
                s += buf; break;
            case Anchor::Cyl:
                std::snprintf(buf, sizeof(buf), "Y,%d,%d,%.6f,%.6f,%.6f",
                              a.sketchId, a.elemId, a.h, a.cu, a.cv);
                s += buf; break;
            case Anchor::CurveWall:
                std::snprintf(buf, sizeof(buf), "E,%d,%d,%.6f,%.6f,%.6f",
                              a.sketchId, a.elemId, a.h, a.cu, a.cv);
                s += buf; break;
            case Anchor::Cap:
                std::snprintf(buf, sizeof(buf), "P,%d,%.6f,%.6f,%.6f",
                              a.sketchId, a.h, a.cu, a.cv);
                s += buf; break;
            default:
                s += 'N'; break;
        }
    }
    return s;
}

bool parse(const std::string& blob, std::vector<Anchor>& anchors) {
    anchors.clear();
    if (blob.rfind("v1", 0) != 0) return false;

    size_t pos = 0; bool first = true;
    while (pos <= blob.size()) {
        size_t t = blob.find('~', pos);
        if (t == std::string::npos) t = blob.size();
        std::string tok = blob.substr(pos, t - pos);
        pos = t + 1;
        if (first) { first = false; }
        else if (!tok.empty()) {
            Anchor a;
            switch (tok[0]) {
                case 'W': a.kind = Anchor::Wall;
                    std::sscanf(tok.c_str(), "W,%d,%d,%lf,%lf,%lf",
                                &a.sketchId, &a.elemId, &a.h, &a.cu, &a.cv); break;
                case 'Y': a.kind = Anchor::Cyl;
                    std::sscanf(tok.c_str(), "Y,%d,%d,%lf,%lf,%lf",
                                &a.sketchId, &a.elemId, &a.h, &a.cu, &a.cv); break;
                case 'E': a.kind = Anchor::CurveWall;
                    std::sscanf(tok.c_str(), "E,%d,%d,%lf,%lf,%lf",
                                &a.sketchId, &a.elemId, &a.h, &a.cu, &a.cv); break;
                case 'P': a.kind = Anchor::Cap;
                    std::sscanf(tok.c_str(), "P,%d,%lf,%lf,%lf",
                                &a.sketchId, &a.h, &a.cu, &a.cv); break;
                default:  a.kind = Anchor::None; break;
            }
            anchors.push_back(a);
        }
        if (t == blob.size()) break;
    }
    return !anchors.empty();
}

} // namespace FaceAnchor
