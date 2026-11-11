#include "PlaneReferences.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GeomAbs_CurveType.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <array>
#include <set>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {

namespace {

constexpr double kPlaneTol = 1e-3;     // mm: "lies on the plane"
constexpr size_t kMaxEdges = 60000;    // a body past this is not scanned
constexpr int    kSectionMaxFaces = 400; // cross-section only below this
constexpr size_t kMaxRefPoints = 20000;

// De-duplicating collector over a FaceReference, hashed on a 1 micron grid so a
// dense body does not turn the merge into an O(n^2) scan.
class Collector {
public:
    explicit Collector(Sketch::FaceReference& refs) : m_refs(refs) {
        for (const auto& p : refs.points) m_points.insert(key(p));
        for (const auto& l : refs.lines) m_lines.insert(lineKey(l.first, l.second));
        for (const auto& c : refs.circles) m_circles.insert(circleKey(c));
        for (const auto& s : refs.splines) m_splines.insert(splineKey(s.control));
    }
    void point(glm::vec2 p) {
        if (m_refs.points.size() >= kMaxRefPoints) return;
        if (m_points.insert(key(p)).second) { m_refs.points.push_back(p); ++points; }
    }
    void line(glm::vec2 a, glm::vec2 b) {
        if (glm::length(a - b) < 1e-4f) return;
        if (m_lines.insert(lineKey(a, b)).second) { m_refs.lines.emplace_back(a, b); ++lines; }
        point(0.5f * (a + b));
    }
    void circle(const Sketch::FaceReference::Circle& c) {
        if (m_circles.insert(circleKey(c)).second) { m_refs.circles.push_back(c); ++circles; }
    }
    void spline(Sketch::FaceReference::Spline s) {
        if (s.control.size() < 2 || s.poly.size() < 2) return;
        if (m_splines.insert(splineKey(s.control)).second) { m_refs.splines.push_back(std::move(s)); ++splines; }
    }
    int points = 0, lines = 0, circles = 0, splines = 0;

private:
    // EXACT keys - the quantised values themselves, not a hash of them. These
    // were folded into one int64 each, and the fold collided for mirror-image
    // geometry: (-5,0)-(-5,10) and (5,0)-(5,10) hashed to the same number, so a
    // symmetric body lost one of its two walls from the references (the
    // cylinder's missing side). A set of the real values cannot do that.
    using PointKey  = std::pair<std::int64_t, std::int64_t>;
    using LineKey   = std::array<std::int64_t, 4>;
    using CircleKey = std::array<std::int64_t, 5>;
    static std::int64_t q(float v) { return static_cast<std::int64_t>(std::llround(static_cast<double>(v) * 1e3)); }
    static PointKey key(glm::vec2 p) { return { q(p.x), q(p.y) }; }
    // Centre + radius + SPAN: a plane through a pipe returns each wall circle as
    // two arcs, and keying on centre/radius alone dropped the second one.
    static CircleKey circleKey(const Sketch::FaceReference::Circle& c) {
        return { q(c.center.x), q(c.center.y), q(c.radius), q(c.startAngle), q(c.sweep) };
    }
    static LineKey lineKey(glm::vec2 a, glm::vec2 b) {
        const PointKey ka = key(a), kb = key(b);   // order-free: smaller end first
        return ka < kb ? LineKey{ ka.first, ka.second, kb.first, kb.second }
                       : LineKey{ kb.first, kb.second, ka.first, ka.second };
    }
    static std::vector<std::int64_t> splineKey(const std::vector<glm::vec2>& control) {
        std::vector<std::int64_t> k;
        k.reserve(control.size() * 2);
        for (const glm::vec2& p : control) { k.push_back(q(p.x)); k.push_back(q(p.y)); }
        return k;
    }
    Sketch::FaceReference& m_refs;
    std::set<PointKey> m_points;
    std::set<LineKey> m_lines;
    std::set<CircleKey> m_circles;
    std::set<std::vector<std::int64_t>> m_splines;
};

struct Frame {
    gp_Pnt O; gp_Dir X, Y, N;
    explicit Frame(const gp_Pln& pln)
        : O(pln.Position().Location()), X(pln.Position().XDirection()),
          Y(pln.Position().YDirection()), N(pln.Position().Direction()) {}
    glm::vec2 project(const gp_Pnt& p) const {
        const double dx = p.X() - O.X(), dy = p.Y() - O.Y(), dz = p.Z() - O.Z();
        return { static_cast<float>(dx * X.X() + dy * X.Y() + dz * X.Z()),
                 static_cast<float>(dx * Y.X() + dy * Y.Y() + dz * Y.Z()) };
    }
    double dist(const gp_Pnt& p) const {
        return (p.X() - O.X()) * N.X() + (p.Y() - O.Y()) * N.Y() + (p.Z() - O.Z()) * N.Z();
    }
};

// One edge, if it lies on the plane. Lines become reference lines, circles that
// lie IN the plane stay true circles (continuous perimeter snap), anything else
// is sampled to points.
void addEdge(const TopoDS_Edge& edge, const Frame& fr, Collector& out) {
    BRepAdaptor_Curve curve(edge);
    const double f = curve.FirstParameter(), l = curve.LastParameter();
    if (!std::isfinite(f) || !std::isfinite(l)) return;
    gp_Pnt pS, pE, pM;
    curve.D0(f, pS); curve.D0(l, pE); curve.D0(0.5 * (f + l), pM);
    if (std::abs(fr.dist(pS)) > kPlaneTol || std::abs(fr.dist(pE)) > kPlaneTol ||
        std::abs(fr.dist(pM)) > kPlaneTol)
        return;
    const glm::vec2 a = fr.project(pS), b = fr.project(pE);
    const float twoPi = 2.0f * static_cast<float>(M_PI);

    switch (curve.GetType()) {
    case GeomAbs_Line:
        out.line(a, b);
        out.point(a); out.point(b);
        return;
    case GeomAbs_Circle: {
        const gp_Circ circ = curve.Circle();
        out.point(fr.project(circ.Location()));
        const bool inPlane = std::abs(circ.Axis().Direction().Dot(fr.N)) > 0.999 &&
                             std::abs(fr.dist(circ.Location())) < 1e-4;
        if (inPlane) {
            Sketch::FaceReference::Circle fc;
            fc.center = fr.project(circ.Location());
            fc.radius = static_cast<float>(circ.Radius());
            if (std::abs((l - f) - 2.0 * M_PI) < 1e-6) {
                fc.startAngle = 0.0f; fc.sweep = twoPi;
            } else {
                const glm::vec2 m2 = fr.project(pM);
                const float a0 = std::atan2(a.y - fc.center.y, a.x - fc.center.x);
                const float a1 = std::atan2(b.y - fc.center.y, b.x - fc.center.x);
                const float am = std::atan2(m2.y - fc.center.y, m2.x - fc.center.x);
                auto ccw = [&](float from, float to) {
                    float d = to - from;
                    while (d < 0.0f) d += twoPi;
                    while (d >= twoPi) d -= twoPi;
                    return d;
                };
                if (ccw(a0, am) <= ccw(a0, a1)) { fc.startAngle = a0; fc.sweep = ccw(a0, a1); }
                else                            { fc.startAngle = a1; fc.sweep = ccw(a1, a0); }
                out.point(a); out.point(b);
            }
            out.circle(fc);
            return;
        }
        break;   // foreshortened: sampled below
    }
    case GeomAbs_Ellipse:
        out.point(fr.project(curve.Ellipse().Location()));
        break;
    case GeomAbs_BSplineCurve:
    case GeomAbs_BezierCurve: {
        // A free-form edge lying in the plane (every sample on it, not just the
        // ends and middle already tested): offer the WHOLE curve, not only its
        // points. Tilted-out-of-plane curves fall through to point sampling.
        constexpr int kN = 48;
        Sketch::FaceReference::Spline sp;
        bool flat = true;
        for (int i = 0; i <= kN && flat; ++i) {
            gp_Pnt p;
            curve.D0(f + (l - f) * (double(i) / kN), p);
            if (std::abs(fr.dist(p)) > kPlaneTol) { flat = false; break; }
            sp.poly.push_back(fr.project(p));
        }
        if (flat) {
            constexpr int kCtrl = 12;
            for (int i = 0; i <= kCtrl; ++i)
                sp.control.push_back(sp.poly[static_cast<size_t>(i) * kN / kCtrl]);
            out.spline(std::move(sp));
        }
        break;
    }
    default:
        break;
    }
    for (int i = 0; i <= 8; ++i) {
        gp_Pnt p;
        curve.D0(f + (l - f) * (double(i) / 8.0), p);
        out.point(fr.project(p));
    }
}

// Which side(s) of the plane a bounding box reaches: -1 below, +1 above, 0 none.
void sides(const Bnd_Box& bb, const Frame& fr, bool& below, bool& above) {
    below = above = false;
    double x0, y0, z0, x1, y1, z1;
    bb.Get(x0, y0, z0, x1, y1, z1);
    for (int c = 0; c < 8; ++c) {
        const double d = fr.dist(gp_Pnt((c & 1) ? x1 : x0, (c & 2) ? y1 : y0, (c & 4) ? z1 : z0));
        if (d < -kPlaneTol) below = true;
        if (d >  kPlaneTol) above = true;
    }
}

// Arcs of ONE circle that together close it become that circle. A round body's
// edges and its plane section arrive split at the seam (a sphere's meridian, a
// cylinder's seam line): each half is a separate reference arc, so picking one
// copied half the outline - hover lit the half under the cursor, which looked
// right, and the click then took only that half. Contiguous arcs of the same
// centre and radius are also joined into one longer arc.
void mergeCircleArcs(std::vector<Sketch::FaceReference::Circle>& circles) {
    using Circle = Sketch::FaceReference::Circle;
    const float twoPi = 2.0f * static_cast<float>(M_PI);
    const float full = twoPi - 1e-3f;
    auto wrap = [&](float a) {
        while (a < 0.0f) a += twoPi;
        while (a >= twoPi) a -= twoPi;
        return a;
    };
    auto sameCircle = [](const Circle& a, const Circle& b) {
        return glm::length(a.center - b.center) < 2e-3f && std::abs(a.radius - b.radius) < 2e-3f;
    };
    std::vector<Circle> out;
    std::vector<bool> done(circles.size(), false);
    for (size_t i = 0; i < circles.size(); ++i) {
        if (done[i]) continue;
        std::vector<Circle> group;
        for (size_t j = i; j < circles.size(); ++j)
            if (!done[j] && sameCircle(circles[i], circles[j])) { done[j] = true; group.push_back(circles[j]); }
        const Circle base = group.front();
        bool whole = false;
        for (const auto& g : group) if (g.sweep >= full) whole = true;
        if (whole || group.size() == 1) {
            if (whole) { Circle c = base; c.startAngle = 0.0f; c.sweep = twoPi; out.push_back(c); }
            else out.push_back(base);
            continue;
        }
        // Chain arcs end to start, round and round, until no more join.
        std::sort(group.begin(), group.end(), [&](const Circle& a, const Circle& b) {
            return wrap(a.startAngle) < wrap(b.startAngle);
        });
        std::vector<Circle> chains;
        std::vector<bool> used(group.size(), false);
        const float eps = 2e-3f;
        for (size_t k = 0; k < group.size(); ++k) {
            if (used[k]) continue;
            used[k] = true;
            Circle cur = group[k];
            cur.startAngle = wrap(cur.startAngle);
            bool grew = true;
            while (grew && cur.sweep < full) {
                grew = false;
                const float end = cur.startAngle + cur.sweep;
                for (size_t m = 0; m < group.size(); ++m) {
                    if (used[m]) continue;
                    float d = wrap(group[m].startAngle - end);
                    if (d > twoPi - eps) d = 0.0f;
                    if (d < eps) {
                        cur.sweep += group[m].sweep;
                        used[m] = true;
                        grew = true;
                        break;
                    }
                }
            }
            // A chain that also meets its own start has closed the circle.
            if (cur.sweep >= full) { cur.startAngle = 0.0f; cur.sweep = twoPi; }
            chains.push_back(cur);
        }
        // The last chain can run into the first across the wrap: join them.
        for (size_t a = 0; a < chains.size(); ++a) {
            for (size_t b = 0; b < chains.size(); ++b) {
                if (a == b || chains[a].sweep >= full || chains[b].sweep >= full) continue;
                float d = wrap(chains[b].startAngle - (chains[a].startAngle + chains[a].sweep));
                if (d > twoPi - eps) d = 0.0f;
                if (d < eps) {
                    chains[a].sweep += chains[b].sweep;
                    if (chains[a].sweep >= full) { chains[a].startAngle = 0.0f; chains[a].sweep = twoPi; }
                    chains.erase(chains.begin() + static_cast<long>(b));
                    if (b < a) --a;
                    b = static_cast<size_t>(-1);   // rescan from the start
                }
            }
        }
        for (const auto& c : chains) out.push_back(c);
    }
    circles = std::move(out);
}

} // namespace

PlaneReferenceStats addBodyPlaneReferences(Sketch& sk,
                                           const std::vector<TopoDS_Shape>& bodies,
                                           bool crossSections) {
    PlaneReferenceStats st;
    Sketch::FaceReference refs = sk.getFaceReferences();
    Collector out(refs);
    const gp_Pln& pln = sk.getPlane();
    const Frame fr(pln);

    for (const TopoDS_Shape& body : bodies) {
        if (body.IsNull()) continue;
        try {
            Bnd_Box bb;
            BRepBndLib::Add(body, bb);
            if (bb.IsVoid()) continue;
            bool below, above;
            sides(bb, fr, below, above);
            // A plane the body does not reach has nothing on it - unless the box
            // merely TOUCHES it (all corners on one side, one face coplanar).
            // That case still has in-plane edges, so only reject a box that sits
            // wholly clear of the plane.
            Bnd_Box grown = bb; grown.Enlarge(kPlaneTol * 2.0);
            bool gb, ga;
            sides(grown, fr, gb, ga);
            if (!(gb && ga)) continue;

            TopTools_IndexedMapOfShape edges, verts, faces;
            TopExp::MapShapes(body, TopAbs_EDGE, edges);
            if (static_cast<size_t>(edges.Extent()) > kMaxEdges) { st.skippedHeavy = true; continue; }
            for (int i = 1; i <= edges.Extent(); ++i)
                addEdge(TopoDS::Edge(edges(i)), fr, out);
            TopExp::MapShapes(body, TopAbs_VERTEX, verts);
            for (int i = 1; i <= verts.Extent(); ++i) {
                const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(verts(i)));
                if (std::abs(fr.dist(p)) <= kPlaneTol) out.point(fr.project(p));
            }

            // The plane cuts through the body: the outline of the cut is
            // snappable too. A boolean section, so only for modest bodies.
            if (crossSections && below && above) {
                TopExp::MapShapes(body, TopAbs_FACE, faces);
                if (faces.Extent() > kSectionMaxFaces) { st.skippedHeavy = true; continue; }
                BRepAlgoAPI_Section sec(body, pln, Standard_False);
                sec.Build();
                if (sec.IsDone() && !sec.Shape().IsNull()) {
                    TopTools_IndexedMapOfShape secEdges;
                    TopExp::MapShapes(sec.Shape(), TopAbs_EDGE, secEdges);
                    for (int i = 1; i <= secEdges.Extent(); ++i)
                        addEdge(TopoDS::Edge(secEdges(i)), fr, out);
                    if (secEdges.Extent() > 0) st.crossSection = true;
                }
            }
        } catch (...) {
            // A body OCCT cannot bound or section simply contributes nothing.
        }
    }
    mergeCircleArcs(refs.circles);
    st.points = out.points; st.lines = out.lines; st.circles = out.circles;
    sk.setFaceReferences(std::move(refs));
    return st;
}

PlaneReferenceStats addSketchPlaneReferences(Sketch& sk,
                                             const std::vector<const Sketch*>& others) {
    PlaneReferenceStats st;
    Sketch::FaceReference refs = sk.getFaceReferences();
    Collector out(refs);
    const Frame mine(sk.getPlane());
    const float twoPi = 2.0f * static_cast<float>(M_PI);

    for (const Sketch* o : others) {
        if (!o || o == &sk) continue;
        const Frame theirs(o->getPlane());
        // Same plane: parallel normals (either facing) and the other's origin
        // on this plane.
        if (std::abs(theirs.N.Dot(mine.N)) < 0.9999) continue;
        if (std::abs(mine.dist(theirs.O)) > kPlaneTol) continue;

        auto toMine = [&](glm::vec2 q) {
            const gp_Pnt w(theirs.O.X() + theirs.X.X() * q.x + theirs.Y.X() * q.y,
                           theirs.O.Y() + theirs.X.Y() * q.x + theirs.Y.Y() * q.y,
                           theirs.O.Z() + theirs.X.Z() * q.x + theirs.Y.Z() * q.y);
            return mine.project(w);
        };
        for (const auto& p : o->getPoints())
            if (!p.fromText) out.point(toMine(p.pos));
        for (const auto& l : o->getLines()) {
            if (l.fromText) continue;
            const SketchPoint* a = o->getPoint(l.startPointId);
            const SketchPoint* b = o->getPoint(l.endPointId);
            if (a && b) out.line(toMine(a->pos), toMine(b->pos));
        }
        for (const auto& c : o->getCircles()) {
            const SketchPoint* ctr = o->getPoint(c.centerPointId);
            if (!ctr) continue;
            Sketch::FaceReference::Circle fc;
            fc.center = toMine(ctr->pos);
            fc.radius = static_cast<float>(c.radius);
            fc.startAngle = 0.0f; fc.sweep = twoPi;
            out.circle(fc);
        }
        // Arcs and splines: points along the curve (a mirrored plane would flip
        // an arc's sweep, and nothing here depends on the arc being whole).
        for (const auto& a : o->getArcs()) {
            const SketchPoint* c = o->getPoint(a.centerPointId);
            const SketchPoint* s = o->getPoint(a.startPointId);
            const SketchPoint* e = o->getPoint(a.endPointId);
            if (!c || !s || !e) continue;
            float a0 = std::atan2(s->pos.y - c->pos.y, s->pos.x - c->pos.x);
            float a1 = std::atan2(e->pos.y - c->pos.y, e->pos.x - c->pos.x);
            while (a1 <= a0) a1 += twoPi;
            for (int i = 0; i <= 8; ++i) {
                const float t = a0 + (a1 - a0) * (float(i) / 8.0f);
                out.point(toMine(c->pos + glm::vec2(std::cos(t), std::sin(t)) *
                                              static_cast<float>(a.radius)));
            }
        }
        for (const auto& sp : o->getSplines()) {
            for (glm::vec2 q : o->sampleSpline2D(sp, 6)) out.point(toMine(q));
            // The curve itself too: copied by running a new spline through the
            // same control points.
            Sketch::FaceReference::Spline rs;
            for (int id : sp.controlPointIds)
                if (const SketchPoint* cp = o->getPoint(id)) rs.control.push_back(toMine(cp->pos));
            for (glm::vec2 q : o->sampleSpline2D(sp, 12)) rs.poly.push_back(toMine(q));
            out.spline(std::move(rs));
        }
    }
    st.points = out.points; st.lines = out.lines; st.circles = out.circles;
    sk.setFaceReferences(std::move(refs));
    return st;
}

} // namespace materializr
