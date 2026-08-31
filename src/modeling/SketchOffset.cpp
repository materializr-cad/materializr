#include "SketchOffset.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom_Circle.hxx>
#include <ShapeAnalysis_Wire.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>

namespace materializr {

namespace {

// Everything here works in the sketch's own 2D coordinates on the z=0 plane.
// A 2D offset is invariant under the sketch plane's placement in space, so
// there is no reason to transform into 3D and back — SvgImport does the same.
gp_Pnt P(glm::vec2 v) { return gp_Pnt(double(v.x), double(v.y), 0.0); }
glm::vec2 V(const gp_Pnt& p) { return {float(p.X()), float(p.Y())}; }

constexpr double kMinOffsetFloor = 1e-3;   // mm
constexpr double kWeldUlps       = 8.0;

// --- source geometry, resolved from ids -------------------------------------

struct Seg {
    int id;
    bool isArc;
    glm::vec2 a, b;        // endpoints
    int aId, bId;          // the point ids they came from
    glm::vec2 center;      // arcs only
    double radius = 0.0;   // arcs only
};

bool resolveSegments(const Sketch& sk, const OffsetSource& src, std::vector<Seg>& out) {
    for (int id : src.lineIds) {
        const SketchLine* l = nullptr;
        for (const auto& c : sk.getLines()) if (c.id == id) { l = &c; break; }
        if (!l) return false;
        const SketchPoint* a = sk.getPoint(l->startPointId);
        const SketchPoint* b = sk.getPoint(l->endPointId);
        if (!a || !b) return false;
        out.push_back({id, false, a->pos, b->pos, l->startPointId, l->endPointId, {}, 0.0});
    }
    for (int id : src.arcIds) {
        const SketchArc* r = nullptr;
        for (const auto& c : sk.getArcs()) if (c.id == id) { r = &c; break; }
        if (!r) return false;
        const SketchPoint* ctr = sk.getPoint(r->centerPointId);
        const SketchPoint* a   = sk.getPoint(r->startPointId);
        const SketchPoint* b   = sk.getPoint(r->endPointId);
        if (!ctr || !a || !b) return false;
        out.push_back({id, true, a->pos, b->pos, r->startPointId, r->endPointId,
                       ctr->pos, r->radius});
    }
    return true;
}

// --- two-stage adjacency ----------------------------------------------------
// Stage 1 is exact shared point ids — the only stage geometry drawn with the
// sketch tools ever needs, and immune to translation. Stage 2 unions
// still-distinct coincident endpoints, without which an imported loop (two
// distinct ids at every junction) reads as a chain of degree-1 vertices and
// gets refused as open.

struct DSU {
    std::unordered_map<int, int> parent;
    int find(int x) {
        auto it = parent.find(x);
        if (it == parent.end()) { parent[x] = x; return x; }
        if (it->second == x) return x;
        return parent[x] = find(it->second);
    }
    void unite(int a, int b) { parent[find(a)] = find(b); }
};

void buildVertexGroups(const std::vector<Seg>& segs, DSU& dsu) {
    for (const auto& s : segs) { dsu.find(s.aId); dsu.find(s.bId); }
    // Stage 2: weld coincident-but-distinct endpoints.
    std::vector<std::pair<int, glm::vec2>> pts;
    for (const auto& s : segs) { pts.push_back({s.aId, s.a}); pts.push_back({s.bId, s.b}); }
    for (size_t i = 0; i < pts.size(); ++i)
        for (size_t j = i + 1; j < pts.size(); ++j) {
            if (pts[i].first == pts[j].first) continue;
            const double tol = std::max(weldTol(pts[i].second), weldTol(pts[j].second));
            if (glm::length(pts[i].second - pts[j].second) <= tol)
                dsu.unite(pts[i].first, pts[j].first);
        }
}

double segLength(const Seg& s) {
    if (!s.isArc) return glm::length(s.b - s.a);
    // Chord is enough to catch a degenerate arc; a full-sweep arc has a zero
    // chord but a non-zero radius, so guard on both.
    const double chord = glm::length(s.b - s.a);
    return (chord > 0.0) ? chord : s.radius;
}

// --- wire assembly ----------------------------------------------------------

TopoDS_Edge edgeFor(const Seg& s) {
    if (!s.isArc) return BRepBuilderAPI_MakeEdge(P(s.a), P(s.b));
    // Mid-point of the swept arc, so GC_MakeArcOfCircle reproduces the same
    // span rather than its complement.
    const glm::vec2 ca = s.a - s.center, cb = s.b - s.center;
    double a0 = std::atan2(double(ca.y), double(ca.x));
    double a1 = std::atan2(double(cb.y), double(cb.x));
    double sweep = a1 - a0;
    while (sweep <= -M_PI) sweep += 2.0 * M_PI;
    while (sweep >   M_PI) sweep -= 2.0 * M_PI;
    const double am = a0 + sweep * 0.5;
    const glm::vec2 mid = s.center + glm::vec2(float(std::cos(am) * s.radius),
                                               float(std::sin(am) * s.radius));
    return BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(P(s.a), P(mid), P(s.b)).Value());
}

double signedArea(const std::vector<glm::vec2>& poly) {
    double a = 0.0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const glm::vec2& p = poly[i];
        const glm::vec2& q = poly[(i + 1) % poly.size()];
        a += double(p.x) * double(q.y) - double(q.x) * double(p.y);
    }
    return 0.5 * a;
}

// Order the segments into a loop and report the traversal order. Assumes the
// graph has already been validated as a single closed component.
bool orderLoop(const std::vector<Seg>& segs, DSU& dsu, std::vector<Seg>& out) {
    std::map<int, std::vector<size_t>> byVert;
    for (size_t i = 0; i < segs.size(); ++i) {
        byVert[dsu.find(segs[i].aId)].push_back(i);
        byVert[dsu.find(segs[i].bId)].push_back(i);
    }
    std::vector<bool> used(segs.size(), false);
    size_t cur = 0;
    int at = dsu.find(segs[0].bId);
    out.push_back(segs[0]);
    used[0] = true;
    for (size_t step = 1; step < segs.size(); ++step) {
        bool advanced = false;
        for (size_t idx : byVert[at]) {
            if (used[idx]) continue;
            Seg s = segs[idx];
            // Flip so the segment leaves the vertex we arrived at.
            if (dsu.find(s.bId) == at && dsu.find(s.aId) != at) {
                std::swap(s.a, s.b);
                std::swap(s.aId, s.bId);
            }
            at = dsu.find(s.bId);
            out.push_back(s);
            used[idx] = true;
            cur = idx;
            advanced = true;
            break;
        }
        if (!advanced) return false;
    }
    (void)cur;
    return true;
}

// Coarse polyline of the ordered loop — for winding and point-in-loop only.
std::vector<glm::vec2> loopPolyline(const std::vector<Seg>& ordered) {
    std::vector<glm::vec2> poly;
    for (const auto& s : ordered) {
        poly.push_back(s.a);
        if (s.isArc) {
            const glm::vec2 ca = s.a - s.center, cb = s.b - s.center;
            double a0 = std::atan2(double(ca.y), double(ca.x));
            double a1 = std::atan2(double(cb.y), double(cb.x));
            double sweep = a1 - a0;
            while (sweep <= -M_PI) sweep += 2.0 * M_PI;
            while (sweep >   M_PI) sweep -= 2.0 * M_PI;
            const int N = 8;
            for (int i = 1; i < N; ++i) {
                const double t = a0 + sweep * (double(i) / N);
                poly.push_back(s.center + glm::vec2(float(std::cos(t) * s.radius),
                                                    float(std::sin(t) * s.radius)));
            }
        }
    }
    return poly;
}

double distanceToSeg(const Seg& s, glm::vec2 p) {
    if (!s.isArc) {
        const glm::vec2 ab = s.b - s.a;
        const double len2 = double(glm::dot(ab, ab));
        if (len2 <= 0.0) return double(glm::length(p - s.a));
        double t = double(glm::dot(p - s.a, ab)) / len2;
        t = std::clamp(t, 0.0, 1.0);
        return double(glm::length(p - (s.a + ab * float(t))));
    }
    // Near enough for cursor feedback: distance to the rim, clamped to the
    // endpoints when the projection falls outside the swept span.
    const double dc = double(glm::length(p - s.center));
    const double toRim = std::abs(dc - s.radius);
    const double toEnds = std::min(double(glm::length(p - s.a)), double(glm::length(p - s.b)));
    return std::min(toRim, toEnds);
}

bool pointInPolygon(const std::vector<glm::vec2>& poly, glm::vec2 p) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const auto& a = poly[i];
        const auto& b = poly[j];
        if ((a.y > p.y) != (b.y > p.y)) {
            const double x = double(a.x) + double(b.x - a.x) * double(p.y - a.y) /
                                           double(b.y - a.y);
            if (double(p.x) < x) in = !in;
        }
    }
    return in;
}

} // namespace

// --- tolerances -------------------------------------------------------------

double weldTol(glm::vec2 p) {
    const float m = std::max(std::abs(p.x), std::abs(p.y));
    const float next = std::nextafter(m, std::numeric_limits<float>::max());
    const double ulp = double(next) - double(m);
    return std::max(1e-6, kWeldUlps * ulp);
}

double minOffsetDistance(const Sketch& sk, const OffsetSource& src) {
    std::vector<Seg> segs;
    if (!resolveSegments(sk, src, segs)) return kMinOffsetFloor;
    double worst = 0.0;
    for (const auto& s : segs) {
        worst = std::max(worst, weldTol(s.a));
        worst = std::max(worst, weldTol(s.b));
    }
    for (int id : src.circleIds) {
        for (const auto& c : sk.getCircles())
            if (c.id == id)
                if (const SketchPoint* ctr = sk.getPoint(c.centerPointId))
                    worst = std::max(worst, weldTol(ctr->pos));
    }
    return std::max(kMinOffsetFloor, 4.0 * worst);
}

const char* offsetErrorMessage(OffsetError e) {
    switch (e) {
        case OffsetError::None:              return "Offset created.";
        case OffsetError::EmptySelection:    return "Select a closed loop to offset.";
        case OffsetError::UnsupportedEntity: return "Offset can't handle splines yet — select lines, arcs or a circle.";
        case OffsetError::OpenChain:         return "That selection is an open chain. Offset needs a closed loop.";
        case OffsetError::Branching:         return "That selection branches. Offset needs a single closed loop.";
        case OffsetError::Disconnected:      return "That's more than one loop. Select just the one to offset.";
        case OffsetError::DegenerateEntity:  return "That selection contains a zero-length or duplicated entity.";
        case OffsetError::SelfIntersecting:  return "That loop crosses itself, so it has no inside to offset from.";
        case OffsetError::DistanceTooSmall:  return "Offset distance is too small to build.";
        case OffsetError::OffsetFailed:      return "That offset can't be built — try a smaller distance.";
        case OffsetError::OffsetCollapsed:   return "That offset collapses the loop to nothing. Try a smaller distance.";
        case OffsetError::OffsetSplit:       return "That offset splits the loop in two. Try a smaller distance.";
        case OffsetError::UnsupportedResult: return "The offset produced geometry this sketch can't represent.";
    }
    return "Offset failed.";
}

// --- gathering --------------------------------------------------------------

OffsetError gatherSource(const Sketch& sk,
                         const std::set<int>& selLines,
                         const std::set<int>& selArcs,
                         const std::set<int>& selCircles,
                         const std::set<int>& selSplines,
                         OffsetSource& out) {
    (void)sk;
    // Refuse rather than skip: quietly dropping a spline would offset a
    // different shape than the one the user selected.
    if (!selSplines.empty()) return OffsetError::UnsupportedEntity;
    out.lineIds = selLines;
    out.arcIds = selArcs;
    out.circleIds = selCircles;
    if (out.empty()) return OffsetError::EmptySelection;
    return OffsetError::None;
}

// --- validation -------------------------------------------------------------

OffsetError validateSource(const Sketch& sk, const OffsetSource& src) {
    if (src.empty()) return OffsetError::EmptySelection;

    // A lone circle is a closed loop by construction and skips the graph walk.
    if (!src.circleIds.empty()) {
        if (src.circleIds.size() > 1 || !src.lineIds.empty() || !src.arcIds.empty())
            return OffsetError::Disconnected;
        for (const auto& c : sk.getCircles())
            if (c.id == *src.circleIds.begin())
                return (c.radius > 0.0) ? OffsetError::None : OffsetError::DegenerateEntity;
        return OffsetError::EmptySelection;
    }

    std::vector<Seg> segs;
    if (!resolveSegments(sk, src, segs)) return OffsetError::EmptySelection;
    if (segs.size() < 2) return OffsetError::OpenChain;

    for (const auto& s : segs)
        if (segLength(s) <= weldTol(s.a)) return OffsetError::DegenerateEntity;

    DSU dsu;
    buildVertexGroups(segs, dsu);

    std::map<int, int> degree;
    for (const auto& s : segs) {
        degree[dsu.find(s.aId)]++;
        degree[dsu.find(s.bId)]++;
    }
    for (const auto& [v, d] : degree) {
        (void)v;
        if (d == 1) return OffsetError::OpenChain;
        if (d > 2)  return OffsetError::Branching;
    }

    // Connectivity: walk from the first segment and see if every one is reached.
    std::map<int, std::vector<size_t>> byVert;
    for (size_t i = 0; i < segs.size(); ++i) {
        byVert[dsu.find(segs[i].aId)].push_back(i);
        byVert[dsu.find(segs[i].bId)].push_back(i);
    }
    std::vector<bool> seen(segs.size(), false);
    std::vector<size_t> stack{0};
    seen[0] = true;
    size_t reached = 1;
    while (!stack.empty()) {
        const size_t i = stack.back(); stack.pop_back();
        for (int v : {dsu.find(segs[i].aId), dsu.find(segs[i].bId)})
            for (size_t j : byVert[v])
                if (!seen[j]) { seen[j] = true; ++reached; stack.push_back(j); }
    }
    if (reached != segs.size()) return OffsetError::Disconnected;

    // Self-intersection. A bow-tie passes every check above and still has no
    // unambiguous inside — and OCCT will happily offset it into valid-looking
    // nonsense, so this is the only place it can be caught. Delegated to
    // OCCT's own analyser rather than hand-rolled line/arc/arc-arc predicates.
    std::vector<Seg> ordered;
    if (!orderLoop(segs, dsu, ordered)) return OffsetError::Disconnected;
    try {
        BRepBuilderAPI_MakeWire mw;
        for (const auto& s : ordered) mw.Add(edgeFor(s));
        if (!mw.IsDone()) return OffsetError::SelfIntersecting;
        const TopoDS_Wire w = mw.Wire();

        BRepBuilderAPI_MakeFace mf(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), w);
        if (!mf.IsDone()) return OffsetError::SelfIntersecting;

        ShapeAnalysis_Wire saw;
        saw.Load(w);
        saw.SetFace(mf.Face());
        saw.SetPrecision(weldTol(ordered.front().a));
        if (saw.CheckSelfIntersection()) return OffsetError::SelfIntersecting;
    } catch (const Standard_Failure&) {
        return OffsetError::SelfIntersecting;
    }

    return OffsetError::None;
}

// --- direction --------------------------------------------------------------

double signedDistanceToLoop(const Sketch& sk, const OffsetSource& src, glm::vec2 p) {
    if (!src.circleIds.empty()) {
        for (const auto& c : sk.getCircles())
            if (c.id == *src.circleIds.begin())
                if (const SketchPoint* ctr = sk.getPoint(c.centerPointId)) {
                    const double d = double(glm::length(p - ctr->pos));
                    // At the exact centre the nearest point is non-unique; the
                    // signed form still resolves deterministically (negative,
                    // i.e. inside, magnitude = radius).
                    return d - c.radius;
                }
        return 0.0;
    }

    std::vector<Seg> segs;
    if (!resolveSegments(sk, src, segs) || segs.empty()) return 0.0;
    double best = std::numeric_limits<double>::max();
    for (const auto& s : segs) best = std::min(best, distanceToSeg(s, p));

    DSU dsu;
    buildVertexGroups(segs, dsu);
    std::vector<Seg> ordered;
    if (!orderLoop(segs, dsu, ordered)) return best;
    return pointInPolygon(loopPolyline(ordered), p) ? -best : best;
}

// --- offsetting -------------------------------------------------------------

OffsetError computeOffsetPlan(const Sketch& sk, const OffsetSource& src,
                              double distance, OffsetPlan& out) {
    out = OffsetPlan{};

    if (const OffsetError e = validateSource(sk, src); e != OffsetError::None) return e;

    // Before OCCT, always: 0 and 1e-9 both SUCCEED there, returning geometry
    // sitting on top of the source.
    if (!std::isfinite(distance)) return OffsetError::DistanceTooSmall;
    if (std::abs(distance) < minOffsetDistance(sk, src)) return OffsetError::DistanceTooSmall;

    try {
        TopoDS_Wire wire;
        double perform = distance;

        if (!src.circleIds.empty()) {
            const SketchCircle* c = nullptr;
            for (const auto& cc : sk.getCircles())
                if (cc.id == *src.circleIds.begin()) { c = &cc; break; }
            if (!c) return OffsetError::EmptySelection;
            const SketchPoint* ctr = sk.getPoint(c->centerPointId);
            if (!ctr) return OffsetError::EmptySelection;
            // Emitted CCW explicitly: a circle has no endpoint walk to derive
            // an orientation from, so one is imposed.
            const gp_Circ circ(gp_Ax2(P(ctr->pos), gp_Dir(0, 0, 1)), c->radius);
            BRepBuilderAPI_MakeWire mw;
            mw.Add(BRepBuilderAPI_MakeEdge(circ));
            if (!mw.IsDone()) return OffsetError::OffsetFailed;
            wire = mw.Wire();
        } else {
            std::vector<Seg> segs;
            if (!resolveSegments(sk, src, segs)) return OffsetError::EmptySelection;
            DSU dsu;
            buildVertexGroups(segs, dsu);
            std::vector<Seg> ordered;
            if (!orderLoop(segs, dsu, ordered)) return OffsetError::Disconnected;

            // Canonical winding: reorient to CCW so a positive Perform argument
            // means outward regardless of how the user happened to draw it.
            if (signedArea(loopPolyline(ordered)) < 0.0) {
                std::reverse(ordered.begin(), ordered.end());
                for (auto& s : ordered) { std::swap(s.a, s.b); std::swap(s.aId, s.bId); }
            }
            BRepBuilderAPI_MakeWire mw;
            for (const auto& s : ordered) mw.Add(edgeFor(s));
            if (!mw.IsDone()) return OffsetError::OffsetFailed;
            wire = mw.Wire();
        }

        BRepOffsetAPI_MakeOffset mk(wire, GeomAbs_Arc);
        mk.Perform(perform);

        // Six checks, not one. IsDone() alone passes a collapse; edge count
        // alone misses a split; both together still admit a non-closed or
        // invalid result at awkward distances.
        if (!mk.IsDone()) return OffsetError::OffsetFailed;
        const TopoDS_Shape shape = mk.Shape();
        if (shape.IsNull()) return OffsetError::OffsetCollapsed;

        int wires = 0;
        TopoDS_Wire result;
        for (TopExp_Explorer wx(shape, TopAbs_WIRE); wx.More(); wx.Next()) {
            if (wires == 0) result = TopoDS::Wire(wx.Current());
            ++wires;
        }
        if (wires == 0) return OffsetError::OffsetCollapsed;
        if (wires > 1)  return OffsetError::OffsetSplit;
        if (!result.Closed()) return OffsetError::UnsupportedResult;
        if (!BRepCheck_Analyzer(result).IsValid()) return OffsetError::UnsupportedResult;

        // Belt and braces with the wires==0 check above: mutation testing shows
        // either one alone catches a collapse, so neither is load-bearing on its
        // own. Both are kept because they fail differently — a shape with a wire
        // carrying no edges would slip past the first.
        int edges = 0;
        for (TopExp_Explorer ex(result, TopAbs_EDGE); ex.More(); ex.Next()) ++edges;
        if (edges == 0) return OffsetError::OffsetCollapsed;

        // Convert the WHOLE result before touching anything. Phase 1.
        for (BRepTools_WireExplorer ed(result); ed.More(); ed.Next()) {
            const TopoDS_Edge e = ed.Current();
            BRepAdaptor_Curve cu(e);
            if (cu.GetType() == GeomAbs_Line) {
                out.lines.push_back({V(cu.Value(cu.FirstParameter())),
                                     V(cu.Value(cu.LastParameter()))});
            } else if (cu.GetType() == GeomAbs_Circle) {
                const gp_Circ c = cu.Circle();
                const glm::vec2 ctr = V(c.Location());
                const gp_Pnt s = cu.Value(cu.FirstParameter());
                const gp_Pnt t = cu.Value(cu.LastParameter());
                const bool full = (std::abs(cu.LastParameter() - cu.FirstParameter())
                                   >= 2.0 * M_PI - 1e-9);
                if (full) {
                    out.circles.push_back({ctr, c.Radius()});
                } else {
                    glm::vec2 a = V(s), b = V(t);
                    // Sketch arcs are start->end CCW; an OCCT edge carries its
                    // own orientation, so a REVERSED edge must swap endpoints
                    // or the rebuilt arc takes the complementary span.
                    if (e.Orientation() == TopAbs_REVERSED) std::swap(a, b);
                    out.arcs.push_back({ctr, a, b, c.Radius()});
                }
            } else {
                out = OffsetPlan{};
                return OffsetError::UnsupportedResult;
            }
        }
        if (out.empty()) return OffsetError::OffsetCollapsed;
    } catch (const Standard_Failure&) {
        out = OffsetPlan{};
        return OffsetError::OffsetFailed;
    }

    return OffsetError::None;
}

// --- applying ---------------------------------------------------------------

void applyOffsetPlan(Sketch& sk, const OffsetPlan& plan,
                     std::set<int>* outPoints, std::set<int>* outEntities) {
    if (plan.empty()) return;   // a refused offset must be a no-op

    // Weld RESULT-to-RESULT only, at weldTol. Deliberately not
    // findCoincidentPoint: its 0.3*snapScale() UI radius would weld a valid
    // sub-snap offset straight onto the source it was offset from.
    std::vector<std::pair<glm::vec2, int>> made;
    auto point = [&](glm::vec2 p) {
        for (const auto& [pos, id] : made)
            if (glm::length(pos - p) <= weldTol(p)) return id;
        const int id = sk.addPoint(p);
        made.push_back({p, id});
        if (outPoints) outPoints->insert(id);
        return id;
    };

    for (const auto& l : plan.lines) {
        const int a = point(l.a), b = point(l.b);
        if (a != b) { const int id = sk.addLine(a, b); if (outEntities) outEntities->insert(id); }
    }
    for (const auto& a : plan.arcs) {
        const int c = point(a.center), s = point(a.start), e = point(a.end);
        if (s != e) { const int id = sk.addArc(c, s, e, a.radius); if (outEntities) outEntities->insert(id); }
    }
    for (const auto& c : plan.circles) {
        const int ctr = point(c.center);
        const int id = sk.addCircle(ctr, c.radius);
        if (outEntities) outEntities->insert(id);
    }
}

} // namespace materializr
