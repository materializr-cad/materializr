#include "EdgeAnchor.h"
#include "Sketch.h"
#include <BRepAdaptor_Curve.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace EdgeAnchor {
namespace {

constexpr double kUVTol   = 1e-3;  // sketch-plane position match tolerance
constexpr double kAxisDot = 0.999; // |dir·axis| above this = parallel
// Loose second-pass tolerance: a project saved AFTER a failed cascade holds
// sketches slightly ahead of the body (uncommitted moves/edits), so retrofit
// capture may pair stale geometry with a drifted sketch. Applied only to
// edges the strict pass leaves unattributed.
constexpr double kDriftTol = 1.0;

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

// ── per-edge geometric classification against ONE sketch frame ──────────────

// Corner = line parallel to the sketch normal. Sets its midpoint (u,v) and
// mid height h.
bool cornerUVH(const TopoDS_Edge& e, const Frame& f,
               double& u, double& v, double& h) {
    BRepAdaptor_Curve c;
    try { c.Initialize(e); } catch (...) { return false; }
    if (c.GetType() != GeomAbs_Line) return false;
    if (std::abs(c.Line().Direction().Dot(f.axis)) < kAxisDot) return false;
    gp_Pnt m = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
    u = f.u(m); v = f.v(m); h = f.h(m);
    return true;
}

// Rim = straight edge at constant height along the sketch normal. Returns its
// endpoints in (u,v) and its height h.
bool rimEnds(const TopoDS_Edge& e, const Frame& f,
             double& u1, double& v1, double& u2, double& v2, double& h) {
    BRepAdaptor_Curve c;
    try { c.Initialize(e); } catch (...) { return false; }
    if (c.GetType() != GeomAbs_Line) return false;
    gp_Pnt p1 = c.Value(c.FirstParameter());
    gp_Pnt p2 = c.Value(c.LastParameter());
    double h1 = f.h(p1), h2 = f.h(p2);
    if (std::abs(h1 - h2) > kUVTol) return false; // not at one cap height
    u1 = f.u(p1); v1 = f.v(p1);
    u2 = f.u(p2); v2 = f.v(p2);
    h = 0.5 * (h1 + h2);
    return true;
}

// Does the (u1,v1)-(u2,v2) edge lie ON segment A-B (within tol)? Features that
// intersect each other clip rim edges into FRAGMENTS of the sketch line, so
// endpoint-pair equality is too strict - containment is the real relation.
// Sets t = the edge midpoint's fraction along A-B (for fragment disambiguation).
bool onSegment(double u1, double v1, double u2, double v2,
               double ax, double ay, double bx, double by,
               double tol, double& t) {
    const double dx = bx - ax, dy = by - ay;
    const double len2 = dx * dx + dy * dy;
    if (len2 < tol * tol) return false; // degenerate sketch line
    const double len = std::sqrt(len2);
    auto onIt = [&](double px, double py) {
        const double s = ((px - ax) * dx + (py - ay) * dy) / len2;
        if (s < -tol / len || s > 1.0 + tol / len) return false; // outside A-B
        const double cx = ax + s * dx, cy = ay + s * dy;         // closest point
        return near2(px, py, cx, cy, tol);
    };
    if (!onIt(u1, v1) || !onIt(u2, v2)) return false;
    const double mx = 0.5 * (u1 + u2), my = 0.5 * (v1 + v2);
    t = ((mx - ax) * dx + (my - ay) * dy) / len2;
    return true;
}

// Arc/Circle = circular OR elliptical edge cut from the cylinder swept by a
// sketch arc/circle: center on the extrusion axis over (cu,cv), radius (minor
// radius for oblique = elliptical sections - a plane section of a cylinder is
// an ellipse whose minor radius IS the cylinder radius) equal to the sketch
// radius. Sets h = center height along the normal.
bool cylUVRH(const TopoDS_Edge& e, const Frame& f,
             double& u, double& v, double& r, double& h) {
    BRepAdaptor_Curve c;
    try { c.Initialize(e); } catch (...) { return false; }
    gp_Pnt ctr;
    if (c.GetType() == GeomAbs_Circle) {
        ctr = c.Circle().Location();
        r = c.Circle().Radius();
    } else if (c.GetType() == GeomAbs_Ellipse) {
        ctr = c.Ellipse().Location();
        r = c.Ellipse().MinorRadius();
    } else return false;
    u = f.u(ctr); v = f.v(ctr); h = f.h(ctr);
    return true;
}

// Classify one edge against one sketch, matching positions within `tol`.
// Returns a None anchor on no match.
Anchor classify(const TopoDS_Edge& e, int sketchId,
                const materializr::Sketch& sk, const Frame& f, double tol) {
    Anchor a;
    double u, v, h;
    if (cornerUVH(e, f, u, v, h)) {
        for (const auto& p : sk.getPoints())
            if (near2(u, v, p.pos.x, p.pos.y, tol))
                return { Anchor::Corner, sketchId, p.id, h, 0.5 };
        return a; // parallel to this sketch's normal but over no vertex
    }
    double u1, v1, u2, v2;
    if (rimEnds(e, f, u1, v1, u2, v2, h)) {
        for (const auto& L : sk.getLines()) {
            const auto* pa = sk.getPoint(L.startPointId);
            const auto* pb = sk.getPoint(L.endPointId);
            if (!pa || !pb) continue;
            double t;
            if (onSegment(u1, v1, u2, v2, pa->pos.x, pa->pos.y,
                          pb->pos.x, pb->pos.y, tol, t))
                return { Anchor::Rim, sketchId, L.id, h, t };
        }
    }
    double r;
    if (cylUVRH(e, f, u, v, r, h)) {
        for (const auto& A : sk.getArcs()) {
            const auto* pc = sk.getPoint(A.centerPointId);
            if (!pc) continue;
            if (near2(u, v, pc->pos.x, pc->pos.y, tol) &&
                std::abs(r - A.radius) < tol)
                return { Anchor::Arc, sketchId, A.id, h, 0.5 };
        }
        for (const auto& C : sk.getCircles()) {
            const auto* pc = sk.getPoint(C.centerPointId);
            if (!pc) continue;
            if (near2(u, v, pc->pos.x, pc->pos.y, tol) &&
                std::abs(r - C.radius) < tol)
                return { Anchor::Circle, sketchId, C.id, h, 0.5 };
        }
    }
    return a;
}

// Candidate scan for resolve(): all edges of `base` matching anchor `a`'s
// sketch element, scored by |h - a.h| (rims tie-break on |t - a.t|). The
// element's CURRENT geometry is used, so a moved/resized sketch re-finds the
// edge at its new position.
struct Candidate { TopoDS_Edge edge; double score; };

std::vector<Candidate> candidatesFor(const Anchor& a,
                                     const materializr::Sketch& sk,
                                     const Frame& f,
                                     const TopTools_IndexedMapOfShape& edges,
                                     double tol) {
    std::vector<Candidate> out;
    for (int i = 1; i <= edges.Extent(); ++i) {
        const TopoDS_Edge& e = TopoDS::Edge(edges(i));
        Anchor c = classify(e, a.sketchId, sk, f, tol);
        if (c.kind != a.kind || c.elemId != a.elemId) continue;
        double score = std::abs(c.h - a.h);
        if (a.kind == Anchor::Rim) score += 0.1 * std::abs(c.t - a.t);
        out.push_back({ e, score });
    }
    return out;
}

} // namespace

std::vector<Anchor> compute(const std::vector<TopoDS_Edge>& edges,
                            const std::vector<SketchRef>& sketches) {
    std::vector<Anchor> out(edges.size());
    std::vector<Frame> frames;
    frames.reserve(sketches.size());
    for (const auto& [sid, sk] : sketches) frames.push_back(frameOf(*sk));

    // Two passes: exact first, then a drift-tolerant retry for edges the
    // strict pass leaves unattributed (see kDriftTol). Running the strict
    // pass over ALL sketches before any loose match prevents a drifted
    // sketch from stealing an edge that matches another sketch exactly.
    for (double tol : { kUVTol, kDriftTol }) {
        for (size_t i = 0; i < edges.size(); ++i) {
            if (out[i].kind != Anchor::None) continue;
            for (size_t s = 0; s < sketches.size(); ++s) {
                Anchor a = classify(edges[i], sketches[s].first,
                                    *sketches[s].second, frames[s], tol);
                if (a.kind != Anchor::None) { out[i] = a; break; }
            }
        }
    }
    return out;
}

bool resolve(const std::vector<Anchor>& anchors,
             const std::vector<SketchRef>& sketches,
             const TopoDS_Shape& base, std::vector<TopoDS_Edge>& out) {
    out.clear();
    if (anchors.empty()) return false;

    TopTools_IndexedMapOfShape edgeMap;
    TopExp::MapShapes(base, TopAbs_EDGE, edgeMap);

    // Each anchor takes the best-scoring edge nobody else has claimed, so two
    // fragments of the same sketch line at the same height stay distinct.
    std::vector<TopoDS_Edge> used;
    auto isUsed = [&](const TopoDS_Edge& e) {
        for (const auto& u : used) if (u.IsSame(e)) return true;
        return false;
    };

    for (const Anchor& a : anchors) {
        if (a.kind == Anchor::None) return false;
        const materializr::Sketch* sk = nullptr;
        for (const auto& [sid, s] : sketches)
            if (sid == a.sketchId) { sk = s; break; }
        if (!sk) return false;
        Frame f = frameOf(*sk);

        // Strict pass first; the loose pass only fires when a desynced body
        // (retrofit against uncommitted sketch drift) hides the exact match.
        auto cands = candidatesFor(a, *sk, f, edgeMap, kUVTol);
        if (cands.empty()) cands = candidatesFor(a, *sk, f, edgeMap, kDriftTol);
        const Candidate* best = nullptr;
        for (const auto& c : cands) {
            if (isUsed(c.edge)) continue;
            if (!best || c.score < best->score) best = &c;
        }
        if (!best) return false;
        used.push_back(best->edge);
        out.push_back(best->edge);
    }
    return true;
}

std::string serialize(const std::vector<Anchor>& anchors) {
    bool any = false;
    for (const auto& a : anchors) if (a.kind != Anchor::None) { any = true; break; }
    if (!any) return "";
    std::string s = "v2";
    char buf[96];
    for (const auto& a : anchors) {
        s += '~';
        switch (a.kind) {
            case Anchor::Corner:
                std::snprintf(buf, sizeof(buf), "C,%d,%d,%.6f", a.sketchId, a.elemId, a.h);
                s += buf; break;
            case Anchor::Rim:
                std::snprintf(buf, sizeof(buf), "R,%d,%d,%.6f,%.6f", a.sketchId, a.elemId, a.h, a.t);
                s += buf; break;
            case Anchor::Arc:
                std::snprintf(buf, sizeof(buf), "A,%d,%d,%.6f", a.sketchId, a.elemId, a.h);
                s += buf; break;
            case Anchor::Circle:
                std::snprintf(buf, sizeof(buf), "O,%d,%d,%.6f", a.sketchId, a.elemId, a.h);
                s += buf; break;
            default:
                s += 'N'; break;
        }
    }
    return s;
}

bool parse(const std::string& blob, std::vector<Anchor>& anchors) {
    anchors.clear();
    if (blob.empty()) return false;

    // Legacy v1: "<sketchId>~C,<pid>~R,<lid>,<h>~N" - header sketch id applies
    // to every token. v2 leads with the literal "v2".
    const bool v2 = blob.rfind("v2", 0) == 0 &&
                    (blob.size() == 2 || blob[2] == '~');
    int legacySid = -1;

    size_t pos = 0; bool first = true;
    while (pos <= blob.size()) {
        size_t t = blob.find('~', pos);
        if (t == std::string::npos) t = blob.size();
        std::string tok = blob.substr(pos, t - pos);
        pos = t + 1;
        if (first) {
            first = false;
            if (!v2) legacySid = std::atoi(tok.c_str());
        } else if (!tok.empty()) {
            Anchor a;
            if (v2) {
                switch (tok[0]) {
                    case 'C': a.kind = Anchor::Corner;
                        std::sscanf(tok.c_str(), "C,%d,%d,%lf", &a.sketchId, &a.elemId, &a.h); break;
                    case 'R': a.kind = Anchor::Rim;
                        std::sscanf(tok.c_str(), "R,%d,%d,%lf,%lf", &a.sketchId, &a.elemId, &a.h, &a.t); break;
                    case 'A': a.kind = Anchor::Arc;
                        std::sscanf(tok.c_str(), "A,%d,%d,%lf", &a.sketchId, &a.elemId, &a.h); break;
                    case 'O': a.kind = Anchor::Circle;
                        std::sscanf(tok.c_str(), "O,%d,%d,%lf", &a.sketchId, &a.elemId, &a.h); break;
                    default:  a.kind = Anchor::None; break;
                }
            } else {
                a.sketchId = legacySid;
                if (tok[0] == 'C') { a.kind = Anchor::Corner; a.elemId = std::atoi(tok.c_str() + 2); }
                else if (tok[0] == 'R') { a.kind = Anchor::Rim; std::sscanf(tok.c_str(), "R,%d,%lf", &a.elemId, &a.h); }
                else a.kind = Anchor::None;
            }
            anchors.push_back(a);
        }
        if (t == blob.size()) break;
    }
    return !first;
}

} // namespace EdgeAnchor
