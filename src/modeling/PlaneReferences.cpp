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

#include <cmath>
#include <cstdint>
#include <unordered_set>

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
        for (const auto& s : refs.splines) {
            std::int64_t k = static_cast<std::int64_t>(s.control.size());
            for (const glm::vec2& q : s.control) k = k * 1000003LL ^ key(q);
            m_splines.insert(k);
        }
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
        std::int64_t k = static_cast<std::int64_t>(s.control.size());
        for (const glm::vec2& q : s.control) k = k * 1000003LL ^ key(q);
        if (m_splines.insert(k).second) { m_refs.splines.push_back(std::move(s)); ++splines; }
    }
    int points = 0, lines = 0, circles = 0, splines = 0;

private:
    static std::int64_t key(glm::vec2 p) {
        const std::int64_t x = std::llround(p.x * 1e3), y = std::llround(p.y * 1e3);
        return (x << 32) ^ (y & 0xffffffffLL);
    }
    // Centre + radius + SPAN: a plane through a pipe returns each wall circle as
    // two arcs, and keying on centre/radius alone dropped the second one.
    static std::int64_t circleKey(const Sketch::FaceReference::Circle& c) {
        std::int64_t k = key(c.center) ^ (std::int64_t(std::lround(c.radius * 1e3)) << 1);
        k = k * 1000003LL ^ std::int64_t(std::llround(c.startAngle * 1e3));
        return k * 1000003LL ^ std::int64_t(std::llround(c.sweep * 1e3));
    }
    static std::int64_t lineKey(glm::vec2 a, glm::vec2 b) {
        const std::int64_t ka = key(a), kb = key(b);
        return ka < kb ? (ka * 1000003LL) ^ kb : (kb * 1000003LL) ^ ka;   // order-free
    }
    Sketch::FaceReference& m_refs;
    std::unordered_set<std::int64_t> m_points, m_lines, m_circles, m_splines;
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
