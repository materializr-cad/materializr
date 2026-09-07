#include "Sketch.h"

#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BOPAlgo_Builder.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <ShapeFix_Face.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Curve.hxx>
#include <GC_MakeCircle.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <Geom_BSplineCurve.hxx>
#include <ElCLib.hxx>
#include <gp_Pnt.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Pnt2d.hxx>
#include <TopoDS.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <set>

namespace materializr {

Sketch::Sketch()
    : m_plane(gp_Pln()) // default XY plane at origin
{
}

void Sketch::setPlane(const gp_Pln& plane) {
    m_plane = plane;
}

const gp_Pln& Sketch::getPlane() const {
    return m_plane;
}

// NB: the point parameter is `nearPt`, not `near`. <windef.h> still defines
// `near` (and `far`) as empty macros from the 16-bit memory-model days, so on
// MSVC a parameter called `near` is erased: `return near;` becomes `return ;`
// and `gp_Vec rel(o, near)` becomes `gp_Vec rel(o, )`. The build breaks with a
// pile of errors that never mention the macro.
gp_Pnt Sketch::latticeAnchor(const gp_Pln& plane, const gp_Pnt& nearPt,
                             double step) {
    if (step <= 0.0) return nearPt;
    const gp_Ax3& ax = plane.Position();
    const gp_Pnt o = ax.Location();
    const gp_Dir xd = ax.XDirection();
    const gp_Dir yd = ax.YDirection();
    const gp_Vec rel(o, nearPt);
    const double u = std::round(rel.Dot(gp_Vec(xd)) / step) * step;
    const double v = std::round(rel.Dot(gp_Vec(yd)) / step) * step;
    return gp_Pnt(o.X() + u * xd.X() + v * yd.X(),
                  o.Y() + u * xd.Y() + v * yd.Y(),
                  o.Z() + u * xd.Z() + v * yd.Z());
}

gp_Pnt Sketch::sketchToWorld(glm::vec2 pt2d) const {
    const gp_Ax3& ax = m_plane.Position();
    gp_Pnt origin = ax.Location();
    gp_Dir xDir = ax.XDirection();
    gp_Dir yDir = ax.YDirection();
    return gp_Pnt(
        origin.X() + pt2d.x * xDir.X() + pt2d.y * yDir.X(),
        origin.Y() + pt2d.x * xDir.Y() + pt2d.y * yDir.Y(),
        origin.Z() + pt2d.x * xDir.Z() + pt2d.y * yDir.Z()
    );
}

// Point management

int Sketch::addPoint(glm::vec2 pos, bool fromText) {
    SketchPoint pt;
    pt.id = nextId();
    pt.pos = pos;
    pt.isConstruction = false;
    pt.fromText = fromText;
    m_points.push_back(pt);
    return pt.id;
}

void Sketch::setPointOnCurve(int pointId, int curveId) {
    if (SketchPoint* pt = findPoint(pointId)) pt->onCurveId = curveId;
}

void Sketch::movePoint(int id, glm::vec2 pos) {
    SketchPoint* pt = findPoint(id);
    if (pt) {
        pt->pos = pos;
    }
}

const SketchPoint* Sketch::getPoint(int id) const {
    for (const auto& pt : m_points) {
        if (pt.id == id) return &pt;
    }
    return nullptr;
}

bool Sketch::getWorldBounds(glm::vec3& outMin, glm::vec3& outMax) const {
    if (m_points.empty()) return false;
    glm::vec3 lo( std::numeric_limits<float>::max());
    glm::vec3 hi(-std::numeric_limits<float>::max());
    auto addPt = [&](const gp_Pnt& p) {
        glm::vec3 v(static_cast<float>(p.X()), static_cast<float>(p.Y()),
                    static_cast<float>(p.Z()));
        lo = glm::min(lo, v);
        hi = glm::max(hi, v);
    };
    // An arc's centre is the centre of the (possibly enormous) circle the arc
    // lies on — for a subtle, nearly-flat arc it sits far from the drawn
    // geometry. Including it here made "frame sketch" and sketch-entry zoom
    // right out to nothing. Exclude arc centres from the framing bounds (the
    // point itself stays visible and interactive — this only affects the
    // camera box) and enclose the arc's actual swept rim instead. Circle
    // centres are kept: their rim expansion below brackets the centre exactly.
    std::unordered_set<int> arcCentres;
    for (const auto& a : m_arcs) arcCentres.insert(a.centerPointId);

    for (const auto& pt : m_points) {
        if (arcCentres.count(pt.id)) continue;
        addPt(sketchToWorld(pt.pos));
    }
    // Points only capture circle centres; expand by radius along the plane axes
    // so the rim is enclosed too.
    for (const auto& c : m_circles) {
        const SketchPoint* ctr = getPoint(c.centerPointId);
        if (!ctr) continue;
        float r = static_cast<float>(c.radius);
        addPt(sketchToWorld(ctr->pos + glm::vec2( r, 0)));
        addPt(sketchToWorld(ctr->pos + glm::vec2(-r, 0)));
        addPt(sketchToWorld(ctr->pos + glm::vec2( 0, r)));
        addPt(sketchToWorld(ctr->pos + glm::vec2( 0,-r)));
    }
    // Arc rim: sample the swept span (start->end CCW, the buildWires
    // convention) so the drawn arc — including any bulge past its endpoints —
    // is enclosed without dragging in the far centre.
    for (const auto& a : m_arcs) {
        const SketchPoint* c = getPoint(a.centerPointId);
        const SketchPoint* s = getPoint(a.startPointId);
        const SketchPoint* e = getPoint(a.endPointId);
        if (!c || !s || !e) continue;
        const glm::vec2 cp = c->pos;
        const double r = glm::length(s->pos - cp);   // actual, not stored
        double a0 = std::atan2(s->pos.y - cp.y, s->pos.x - cp.x);
        double sweep = std::atan2(e->pos.y - cp.y, e->pos.x - cp.x) - a0;
        while (sweep <= 0.0)          sweep += 2.0 * M_PI;
        while (sweep > 2.0 * M_PI)    sweep -= 2.0 * M_PI;
        const int N = 16;
        for (int i = 0; i <= N; ++i) {
            double ang = a0 + sweep * (static_cast<double>(i) / N);
            addPt(sketchToWorld(cp + glm::vec2(
                static_cast<float>(r * std::cos(ang)),
                static_cast<float>(r * std::sin(ang)))));
        }
    }
    outMin = lo;
    outMax = hi;
    return true;
}

const std::vector<SketchPoint>& Sketch::getPoints() const {
    return m_points;
}

SketchPoint* Sketch::findPoint(int id) {
    for (auto& pt : m_points) {
        if (pt.id == id) return &pt;
    }
    return nullptr;
}

// Element creation

int Sketch::addLine(int startPtId, int endPtId, bool fromText) {
    SketchLine line;
    line.id = nextId();
    line.startPointId = startPtId;
    line.endPointId = endPtId;
    line.isConstruction = false;
    line.fromText = fromText;
    m_lines.push_back(line);
    return line.id;
}

int Sketch::addCircle(int centerPtId, double radius) {
    SketchCircle circle;
    circle.id = nextId();
    circle.centerPointId = centerPtId;
    circle.radius = radius;
    circle.isConstruction = false;
    m_circles.push_back(circle);
    return circle.id;
}

void Sketch::setCircleRadius(int circleId, double r) {
    for (auto& c : m_circles) {
        if (c.id == circleId) { c.radius = std::max(r, 1e-6); return; }
    }
}

void Sketch::setArcRadius(int arcId, double r) {
    // An arc is built from its centre, both endpoints AND its stored radius:
    // buildWires places the arc's mid point at centre + (cos,sin)(midAngle) *
    // radius and hands that to GC_MakeArcOfCircle, so writing the radius alone
    // left the curve fitted through three mutually inconsistent points. The
    // endpoints have to move with it.
    //
    // resizeArc already does exactly that — slide both endpoints onto the new
    // radius along their existing bearings, which preserves the swept angle —
    // and it is what the Properties panel has always called. Delegating keeps
    // the dimension path and the panel path on one behaviour instead of two
    // that disagree in the degenerate case.
    resizeArc(arcId, r);
}

namespace {
// CCW swept angle (0, 2π] of an arc going start->end about centre c. Mirrors
// the start->end CCW convention buildWires() uses to emit the arc edge.
double ccwSweep(glm::vec2 c, glm::vec2 s, glm::vec2 e) {
    double aS = std::atan2(s.y - c.y, s.x - c.x);
    double aE = std::atan2(e.y - c.y, e.x - c.x);
    double d = aE - aS;
    while (d <= 0.0)        d += 2.0 * M_PI;
    while (d > 2.0 * M_PI)  d -= 2.0 * M_PI;
    return d;
}
} // namespace

void Sketch::moveEndpointPreservingArcs(int pointId, glm::vec2 newPos) {
    // Capture the arcs hinging on this point BEFORE moving it — the swept angle
    // has to be read from the current geometry. We also remember the opposite
    // (unmoved) endpoint's position, which stays put.
    struct Cap { int arcId; double sweep; glm::vec2 fixedPt; bool movedIsStart; };
    std::vector<Cap> caps;
    for (const auto& a : m_arcs) {
        bool isStart = (a.startPointId == pointId);
        bool isEnd   = (a.endPointId == pointId);
        if (!isStart && !isEnd) continue;
        const SketchPoint* c = getPoint(a.centerPointId);
        const SketchPoint* s = getPoint(a.startPointId);
        const SketchPoint* e = getPoint(a.endPointId);
        if (!c || !s || !e) continue;
        caps.push_back({a.id, ccwSweep(c->pos, s->pos, e->pos),
                        isStart ? e->pos : s->pos, isStart});
    }

    movePoint(pointId, newPos);

    for (const auto& cap : caps) {
        SketchArc* a = nullptr;
        for (auto& x : m_arcs) if (x.id == cap.arcId) { a = &x; break; }
        if (!a) continue;
        glm::vec2 S = cap.movedIsStart ? newPos : cap.fixedPt;
        glm::vec2 E = cap.movedIsStart ? cap.fixedPt : newPos;
        glm::vec2 chord = E - S;
        double c = glm::length(chord);
        double half = cap.sweep * 0.5;
        double sinH = std::sin(half);
        if (c < 1e-9 || std::abs(sinH) < 1e-9) continue; // degenerate; leave arc
        double R = std::abs(c / (2.0 * sinH));
        glm::vec2 mid = 0.5f * (S + E);
        glm::vec2 dir = chord / static_cast<float>(c);
        glm::vec2 n(-dir.y, dir.x);                 // left normal of the chord
        double h = R * std::cos(half);              // signed offset mid->centre
        glm::vec2 c1 = mid + n * static_cast<float>(h);
        glm::vec2 c2 = mid - n * static_cast<float>(h);
        // Two centres give the same circle; pick the one whose CCW start->end
        // sweep matches the angle we're preserving (vs. its 2π-complement).
        auto err = [&](glm::vec2 cc) {
            double d = std::abs(ccwSweep(cc, S, E) - cap.sweep);
            return std::min(d, std::abs(d - 2.0 * M_PI));
        };
        glm::vec2 center = (err(c1) <= err(c2)) ? c1 : c2;
        movePoint(a->centerPointId, center);
        a->radius = R;
    }
}

void Sketch::setLineLength(int lineId, double newLength) {
    SketchLine* l = nullptr;
    for (auto& x : m_lines) if (x.id == lineId) { l = &x; break; }
    if (!l) return;
    const SketchPoint* p1 = getPoint(l->startPointId);
    const SketchPoint* p2 = getPoint(l->endPointId);
    if (!p1 || !p2) return;
    glm::vec2 a = p1->pos, b = p2->pos;
    glm::vec2 d = b - a;
    double len = glm::length(d);
    glm::vec2 dir = (len > 1e-9) ? d / static_cast<float>(len) : glm::vec2(1.0f, 0.0f);
    glm::vec2 mid = 0.5f * (a + b);
    float half = static_cast<float>(std::max(newLength, 1e-6) * 0.5);
    moveEndpointPreservingArcs(l->startPointId, mid - dir * half);
    moveEndpointPreservingArcs(l->endPointId,   mid + dir * half);
}

void Sketch::resizeArc(int arcId, double newRadius) {
    SketchArc* a = nullptr;
    for (auto& x : m_arcs) if (x.id == arcId) { a = &x; break; }
    if (!a) return;
    const SketchPoint* c = getPoint(a->centerPointId);
    if (!c) return;
    glm::vec2 C = c->pos;
    float R = static_cast<float>(std::max(newRadius, 1e-6));
    auto reproject = [&](int pid) {
        const SketchPoint* p = getPoint(pid);
        if (!p) return;
        glm::vec2 v = p->pos - C;
        double l = glm::length(v);
        glm::vec2 dir = (l > 1e-9) ? v / static_cast<float>(l) : glm::vec2(1.0f, 0.0f);
        movePoint(pid, C + dir * R);
    };
    reproject(a->startPointId);
    reproject(a->endPointId);
    a->radius = R;
}

void Sketch::setArcChord(int arcId, double chordLen) {
    SketchArc* a = nullptr;
    for (auto& x : m_arcs) if (x.id == arcId) { a = &x; break; }
    if (!a) return;
    const SketchPoint* c = getPoint(a->centerPointId);
    const SketchPoint* s = getPoint(a->startPointId);
    const SketchPoint* e = getPoint(a->endPointId);
    if (!c || !s || !e) return;
    // Keep the SAME arc shape (sweep angle), just scaled so the endpoints are
    // `chordLen` apart: chord = 2 R sin(sweep/2)  ->  R = chord / (2 sin(sweep/2)).
    // Then resize, which holds the centre and both endpoint angles and slides
    // the ends radially — so the arc grows/shrinks symmetrically and the sweep
    // is unchanged.
    double sweep = ccwSweep(c->pos, s->pos, e->pos);
    double sinHalf = std::sin(sweep * 0.5);
    if (std::abs(sinHalf) < 1e-9) return; // ~0° or ~360°: chord undefined
    resizeArc(arcId, std::max(chordLen, 1e-6) / (2.0 * std::abs(sinHalf)));
}

void Sketch::setArcSweep(int arcId, double sweepRad) {
    SketchArc* a = nullptr;
    for (auto& x : m_arcs) if (x.id == arcId) { a = &x; break; }
    if (!a) return;
    const SketchPoint* c = getPoint(a->centerPointId);
    const SketchPoint* s = getPoint(a->startPointId);
    if (!c || !s) return;
    double sweep = std::clamp(sweepRad, 1.0 * M_PI / 180.0, 359.0 * M_PI / 180.0);
    double aS = std::atan2(s->pos.y - c->pos.y, s->pos.x - c->pos.x);
    double aE = aS + sweep; // CCW from start
    float R = static_cast<float>(a->radius);
    movePoint(a->endPointId,
              glm::vec2(c->pos.x + std::cos(aE) * R, c->pos.y + std::sin(aE) * R));
}

bool Sketch::findAxisAlignedRect(int lineId, RectInfo& out) const {
    const SketchLine* l0 = nullptr;
    for (const auto& x : m_lines) if (x.id == lineId) { l0 = &x; break; }
    if (!l0) return false;

    // Walk a closed 4-cycle of lines: each step hops to the (unique) other line
    // sharing the current point. Bail on any T-junction (a point used by >2
    // lines) or a chain that doesn't close after exactly four hops.
    auto otherLineAt = [&](int pid, int excludeLine) -> const SketchLine* {
        const SketchLine* found = nullptr;
        for (const auto& x : m_lines) {
            if (x.id == excludeLine) continue;
            if (x.startPointId == pid || x.endPointId == pid) {
                if (found) return nullptr; // ambiguous: more than one
                found = &x;
            }
        }
        return found;
    };
    auto otherEnd = [](const SketchLine& l, int pid) {
        return (l.startPointId == pid) ? l.endPointId : l.startPointId;
    };

    const SketchLine* lines[4] = {l0, nullptr, nullptr, nullptr};
    int corners[4];
    corners[0] = l0->startPointId;
    int cur = l0->endPointId;
    corners[1] = cur;
    for (int i = 1; i < 4; ++i) {
        const SketchLine* nxt = otherLineAt(cur, lines[i - 1]->id);
        if (!nxt) return false;
        lines[i] = nxt;
        cur = otherEnd(*nxt, cur);
        if (i < 3) corners[i + 1] = cur;
    }
    // The fourth line must close back onto the start point.
    if (cur != corners[0]) return false;
    // Four distinct corners.
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j)
            if (corners[i] == corners[j]) return false;

    glm::vec2 P[4];
    for (int i = 0; i < 4; ++i) {
        const SketchPoint* p = getPoint(corners[i]);
        if (!p) return false;
        P[i] = p->pos;
    }
    // Every side axis-aligned (horizontal or vertical).
    const float axTol = 1e-4f;
    for (int i = 0; i < 4; ++i) {
        glm::vec2 d = P[(i + 1) % 4] - P[i];
        if (std::abs(d.x) > axTol && std::abs(d.y) > axTol) return false;
    }
    float minx = std::min(std::min(P[0].x, P[1].x), std::min(P[2].x, P[3].x));
    float maxx = std::max(std::max(P[0].x, P[1].x), std::max(P[2].x, P[3].x));
    float miny = std::min(std::min(P[0].y, P[1].y), std::min(P[2].y, P[3].y));
    float maxy = std::max(std::max(P[0].y, P[1].y), std::max(P[2].y, P[3].y));
    out.center = glm::vec2(0.5f * (minx + maxx), 0.5f * (miny + maxy));
    out.width  = maxx - minx;
    out.height = maxy - miny;
    for (int i = 0; i < 4; ++i) { out.cornerPts[i] = corners[i]; out.lineIds[i] = lines[i]->id; }
    return true;
}

void Sketch::setRectangleSize(int lineId, double width, double height) {
    RectInfo r;
    if (!findAxisAlignedRect(lineId, r)) return;
    float hw = static_cast<float>(std::max(width, 1e-6) * 0.5);
    float hh = static_cast<float>(std::max(height, 1e-6) * 0.5);
    for (int i = 0; i < 4; ++i) {
        const SketchPoint* p = getPoint(r.cornerPts[i]);
        if (!p) continue;
        glm::vec2 q = p->pos - r.center;  // keep each corner in its own quadrant
        glm::vec2 np(q.x >= 0.0f ? hw : -hw, q.y >= 0.0f ? hh : -hh);
        moveEndpointPreservingArcs(r.cornerPts[i], r.center + np);
    }
}

int Sketch::addArc(int centerPtId, int startPtId, int endPtId, double radius) {
    SketchArc arc;
    arc.id = nextId();
    arc.centerPointId = centerPtId;
    arc.startPointId = startPtId;
    arc.endPointId = endPtId;
    arc.radius = radius;
    arc.isConstruction = false;
    m_arcs.push_back(arc);
    return arc.id;
}

int Sketch::addSpline(const std::vector<int>& controlPointIds) {
    SketchSpline spline;
    spline.id = nextId();
    spline.controlPointIds = controlPointIds;
    spline.isConstruction = false;
    m_splines.push_back(spline);
    return spline.id;
}

bool Sketch::appendSplineControlPoint(int splineId, int pointId) {
    for (auto& sp : m_splines)
        if (sp.id == splineId) {
            sp.controlPointIds.push_back(pointId);
            return true;
        }
    return false;
}

bool Sketch::popSplineControlPoint(int splineId) {
    for (auto& sp : m_splines)
        if (sp.id == splineId) {
            if (sp.controlPointIds.size() <= 2) return false;
            sp.controlPointIds.pop_back();
            return true;
        }
    return false;
}

int Sketch::addPolygon(int centerPtId, double radius, int sides, double rotationRad) {
    SketchPolygon polygon;
    polygon.id = nextId();
    polygon.centerPointId = centerPtId;
    polygon.radius = radius;
    polygon.sides = sides;

    const SketchPoint* center = getPoint(centerPtId);
    if (!center) return -1;
    // COPY the centre position — `center` points INTO m_points, and the
    // addPoint calls below reallocate that vector. The dangling pointer
    // produced vertices missing the centre offset or at ±1e26 ("many
    // meters long"), and only on the FIRST polygon per session: the first
    // commit grows the vector past capacity; after an undo the capacity
    // remains, the retry doesn't reallocate, and the same clicks succeed.
    const glm::vec2 centerPos = center->pos;

    // Create N vertex points evenly spaced around center. The first vertex
    // lands at angle `rotationRad` so the caller can align it with the cursor
    // direction — used by SketchTool to snap a corner of the polygon to the
    // grid (the first vertex is exactly under the snapped cursor).
    for (int i = 0; i < sides; ++i) {
        double angle = rotationRad + 2.0 * M_PI * i / sides;
        float vx = centerPos.x + static_cast<float>(radius * std::cos(angle));
        float vy = centerPos.y + static_cast<float>(radius * std::sin(angle));
        int ptId = addPoint(glm::vec2(vx, vy));
        polygon.vertexPointIds.push_back(ptId);
    }

    // Create N lines connecting consecutive vertices (closing the loop)
    for (int i = 0; i < sides; ++i) {
        int startPtId = polygon.vertexPointIds[i];
        int endPtId = polygon.vertexPointIds[(i + 1) % sides];
        int lineId = addLine(startPtId, endPtId);
        polygon.lineIds.push_back(lineId);
    }

    polygon.isConstruction = false;
    m_polygons.push_back(polygon);
    return polygon.id;
}

int Sketch::addRectangle(glm::vec2 corner1, glm::vec2 corner2) {
    // Create 4 corner points
    glm::vec2 c1 = corner1;
    glm::vec2 c2 = glm::vec2(corner2.x, corner1.y);
    glm::vec2 c3 = corner2;
    glm::vec2 c4 = glm::vec2(corner1.x, corner2.y);

    int p1 = addPoint(c1);
    int p2 = addPoint(c2);
    int p3 = addPoint(c3);
    int p4 = addPoint(c4);

    // Create 4 lines forming a closed rectangle
    int firstLineId = addLine(p1, p2);
    int l2 = addLine(p2, p3);
    int l3 = addLine(p3, p4);
    int l4 = addLine(p4, p1);

    // Keep the rectangle rigid: top/bottom horizontal, sides vertical. Without
    // these a later dimension (e.g. a 2 mm gap to another edge) lets the naive
    // relaxation solver skew the box into a parallelogram, since nothing holds
    // its sides square. p1→p2 and p3→p4 are horizontal; p2→p3 and p4→p1 vertical.
    auto addC = [&](ConstraintType t, int a) {
        Constraint c; c.id = 0; c.type = t; c.entityA = a; c.entityB = -1;
        c.isSatisfied = true; addConstraint(c);
    };
    addC(ConstraintType::Horizontal, firstLineId);
    addC(ConstraintType::Horizontal, l3);
    addC(ConstraintType::Vertical, l2);
    addC(ConstraintType::Vertical, l4);

    return firstLineId;
}

// Element access

const std::vector<SketchLine>& Sketch::getLines() const {
    return m_lines;
}

const std::vector<SketchCircle>& Sketch::getCircles() const {
    return m_circles;
}

const std::vector<SketchArc>& Sketch::getArcs() const {
    return m_arcs;
}

const std::vector<SketchSpline>& Sketch::getSplines() const {
    return m_splines;
}

namespace {
// Centripetal Catmull-Rom point on the span p1->p2 at local t in [0,1].
// LOCAL control: each span only sees its two neighbours, so the curve
// hugs the clicked points instead of ballooning the way a global C2
// interpolation does around sparse points and closed seams (Steve's
// "the math is just a little off / it ends weird" vase).
glm::vec2 catmullRomPoint(glm::vec2 p0, glm::vec2 p1, glm::vec2 p2,
                          glm::vec2 p3, float t) {
    auto tj = [](float ti, glm::vec2 a, glm::vec2 b) {
        float d = glm::length(b - a);
        return ti + std::sqrt(std::max(d, 1e-6f));
    };
    float t0 = 0.0f;
    float t1 = tj(t0, p0, p1);
    float t2 = tj(t1, p1, p2);
    float t3 = tj(t2, p2, p3);
    float tt = t1 + (t2 - t1) * t;
    auto lp = [](glm::vec2 a, glm::vec2 b, float u) {
        return a * (1.0f - u) + b * u;
    };
    glm::vec2 A1 = lp(p0, p1, (tt - t0) / std::max(t1 - t0, 1e-6f));
    glm::vec2 A2 = lp(p1, p2, (tt - t1) / std::max(t2 - t1, 1e-6f));
    glm::vec2 A3 = lp(p2, p3, (tt - t2) / std::max(t3 - t2, 1e-6f));
    glm::vec2 B1 = lp(A1, A2, (tt - t0) / std::max(t2 - t0, 1e-6f));
    glm::vec2 B2 = lp(A2, A3, (tt - t1) / std::max(t3 - t1, 1e-6f));
    return lp(B1, B2, (tt - t1) / std::max(t2 - t1, 1e-6f));
}
} // anonymous

std::vector<glm::vec2> Sketch::interpolate2D(const std::vector<glm::vec2>& ctrl,
                                             int segsPerSpan, bool closed) {
    int n = static_cast<int>(ctrl.size());
    if (n < 2) return ctrl;
    auto at = [&](int i) -> glm::vec2 {
        if (closed) return ctrl[((i % n) + n) % n];
        // Open ends: reflect the end point through its neighbour for a
        // natural phantom tangent.
        if (i < 0) return ctrl[0] * 2.0f - ctrl[1];
        if (i >= n) return ctrl[n - 1] * 2.0f - ctrl[n - 2];
        return ctrl[i];
    };
    int spans = closed ? n : n - 1;
    std::vector<glm::vec2> out;
    out.reserve(spans * segsPerSpan + 1);
    for (int s = 0; s < spans; ++s) {
        for (int i = 0; i < segsPerSpan; ++i) {
            float t = static_cast<float>(i) / segsPerSpan;
            out.push_back(catmullRomPoint(at(s - 1), at(s), at(s + 1),
                                          at(s + 2), t));
        }
    }
    out.push_back(closed ? ctrl[0] : ctrl[n - 1]);
    return out;
}

std::vector<glm::vec2> Sketch::sampleSpline2D(const SketchSpline& sp,
                                              int segsPerSpan) const {
    const auto& ids = sp.controlPointIds;
    bool closedSp = ids.size() > 2 && ids.front() == ids.back();
    std::vector<glm::vec2> ctrl;
    ctrl.reserve(ids.size());
    size_t n = ids.size() - (closedSp ? 1 : 0);
    for (size_t k = 0; k < n; ++k) {
        const SketchPoint* p = getPoint(ids[k]);
        // SKIP a dangling control-point id instead of truncating: returning
        // the partial RAW control polygon (old behaviour) collapsed a whole
        // spline to a tiny un-interpolated stub whenever one referenced point
        // was gone — every classification against it then missed by miles.
        if (!p) continue;
        ctrl.push_back(p->pos);
    }
    if (ctrl.size() < 2) return {};
    return interpolate2D(ctrl, segsPerSpan, closedSp);
}

const std::vector<SketchPolygon>& Sketch::getPolygons() const {
    return m_polygons;
}

// Element removal

void Sketch::removeElement(int id) {
    m_lines.erase(
        std::remove_if(m_lines.begin(), m_lines.end(),
            [id](const SketchLine& l) { return l.id == id; }),
        m_lines.end());

    m_circles.erase(
        std::remove_if(m_circles.begin(), m_circles.end(),
            [id](const SketchCircle& c) { return c.id == id; }),
        m_circles.end());

    m_arcs.erase(
        std::remove_if(m_arcs.begin(), m_arcs.end(),
            [id](const SketchArc& a) { return a.id == id; }),
        m_arcs.end());

    m_splines.erase(
        std::remove_if(m_splines.begin(), m_splines.end(),
            [id](const SketchSpline& s) { return s.id == id; }),
        m_splines.end());

    m_polygons.erase(
        std::remove_if(m_polygons.begin(), m_polygons.end(),
            [id](const SketchPolygon& p) { return p.id == id; }),
        m_polygons.end());

    m_points.erase(
        std::remove_if(m_points.begin(), m_points.end(),
            [id](const SketchPoint& p) { return p.id == id; }),
        m_points.end());
}

int Sketch::pruneOrphanPoints() {
    // Every point id still referenced by some geometry element.
    std::unordered_set<int> used;
    for (const auto& l : m_lines) { used.insert(l.startPointId); used.insert(l.endPointId); }
    for (const auto& c : m_circles) used.insert(c.centerPointId);
    for (const auto& a : m_arcs) {
        used.insert(a.centerPointId); used.insert(a.startPointId); used.insert(a.endPointId);
    }
    for (const auto& s : m_splines)
        for (int id : s.controlPointIds) used.insert(id);
    for (const auto& g : m_polygons) {
        used.insert(g.centerPointId);
        for (int id : g.vertexPointIds) used.insert(id);
    }

    // Drop points referenced by nothing (a deleted line's stranded endpoints).
    size_t before = m_points.size();
    m_points.erase(
        std::remove_if(m_points.begin(), m_points.end(),
            [&](const SketchPoint& p) { return used.find(p.id) == used.end(); }),
        m_points.end());
    int pruned = static_cast<int>(before - m_points.size());

    // Drop constraints whose referenced entity (a point or an element) no longer
    // exists — deleting geometry would otherwise leave the solver chasing ghosts.
    std::unordered_set<int> valid;
    for (const auto& p : m_points)   valid.insert(p.id);
    for (const auto& l : m_lines)    valid.insert(l.id);
    for (const auto& c : m_circles)  valid.insert(c.id);
    for (const auto& a : m_arcs)     valid.insert(a.id);
    for (const auto& s : m_splines)  valid.insert(s.id);
    for (const auto& g : m_polygons) valid.insert(g.id);
    m_constraints.erase(
        std::remove_if(m_constraints.begin(), m_constraints.end(),
            [&](const Constraint& k) {
                return (k.entityA >= 0 && valid.find(k.entityA) == valid.end()) ||
                       (k.entityB >= 0 && valid.find(k.entityB) == valid.end());
            }),
        m_constraints.end());

    return pruned;
}

void Sketch::clear() {
    m_points.clear();
    m_lines.clear();
    m_circles.clear();
    m_arcs.clear();
    m_splines.clear();
    m_polygons.clear();
    m_constraints.clear();
    m_nextId = 1;
    m_nextConstraintId = 1;
}

bool constraintIsArcRadius(const Sketch& sk, const Constraint& c) {
    if (c.type != ConstraintType::Radius) return false;
    for (const auto& circ : sk.getCircles())
        if (circ.id == c.entityA) return false;   // a circle wins
    for (const auto& arc : sk.getArcs())
        if (arc.id == c.entityA) return true;
    return false;
}

int Sketch::addConstraint(const Constraint& c) {
    Constraint copy = c;
    copy.id = m_nextConstraintId++;
    m_constraints.push_back(copy);
    return copy.id;
}

void Sketch::removeConstraint(int id) {
    m_constraints.erase(
        std::remove_if(m_constraints.begin(), m_constraints.end(),
            [id](const Constraint& c) { return c.id == id; }),
        m_constraints.end());
}

// Build OCCT wires from closed profiles.
//
// Adjacency graph spans lines AND arc-segments-of-circles. A circle that has no
// sketch points on its perimeter contributes a standalone full-circle wire. A
// circle that *does* have points on it gets split into arc segments at those
// points so the DFS can find closed loops mixing straight and curved edges.

// The longest OPEN chain in the sketch — see the header note. Each element
// becomes a run of 2D sample points; chains are walked from degree-1 nodes so
// closed loops (which buildWires owns) never qualify.
TopoDS_Wire Sketch::buildOpenWire() const {
    struct ChainEdge {
        int a = -1, b = -1;                 // endpoint point-ids
        std::vector<glm::vec2> pts;         // samples a → b (inclusive)
        bool used = false;
        double len = 0.0;
    };
    std::vector<ChainEdge> edges;

    auto pushEdge = [&](int a, int b, std::vector<glm::vec2> pts) {
        if (pts.size() < 2) return;
        ChainEdge e;
        e.a = a; e.b = b; e.pts = std::move(pts);
        for (size_t i = 1; i < e.pts.size(); ++i)
            e.len += glm::length(e.pts[i] - e.pts[i - 1]);
        if (e.len > 1e-9) edges.push_back(std::move(e));
    };

    for (const auto& ln : m_lines) {
        if (ln.isConstruction) continue;
        const SketchPoint* a = getPoint(ln.startPointId);
        const SketchPoint* b = getPoint(ln.endPointId);
        if (!a || !b) continue;
        pushEdge(ln.startPointId, ln.endPointId, {a->pos, b->pos});
    }
    for (const auto& arc : m_arcs) {
        if (arc.isConstruction) continue;
        const SketchPoint* c = getPoint(arc.centerPointId);
        const SketchPoint* a = getPoint(arc.startPointId);
        const SketchPoint* b = getPoint(arc.endPointId);
        if (!c || !a || !b) continue;
        float sA = std::atan2(a->pos.y - c->pos.y, a->pos.x - c->pos.x);
        float eA = std::atan2(b->pos.y - c->pos.y, b->pos.x - c->pos.x);
        if (eA <= sA) eA += 2.0f * static_cast<float>(M_PI); // CCW convention
        const int N = 24;
        std::vector<glm::vec2> pts;
        pts.reserve(N + 1);
        for (int k = 0; k <= N; ++k) {
            float ang = sA + (eA - sA) * (static_cast<float>(k) / N);
            pts.emplace_back(c->pos.x + std::cos(ang) * static_cast<float>(arc.radius),
                             c->pos.y + std::sin(ang) * static_cast<float>(arc.radius));
        }
        pushEdge(arc.startPointId, arc.endPointId, std::move(pts));
    }
    for (const auto& sp : m_splines) {
        if (sp.isConstruction || sp.controlPointIds.size() < 2) continue;
        // Closed splines belong to the region machinery, not rails.
        if (sp.controlPointIds.front() == sp.controlPointIds.back()) continue;
        auto pts = sampleSpline2D(sp);
        pushEdge(sp.controlPointIds.front(), sp.controlPointIds.back(),
                 std::move(pts));
    }
    if (edges.empty()) return {};

    // Endpoint degrees.
    std::unordered_map<int, int> degree;
    for (const auto& e : edges) { ++degree[e.a]; ++degree[e.b]; }

    // Walk from every degree-1 node, following unused edges; keep the longest
    // chain by 2D length.
    std::vector<glm::vec2> best;
    double bestLen = -1.0;
    for (const auto& [startPt, deg] : degree) {
        if (deg != 1) continue;
        for (auto& e : edges) e.used = false;
        std::vector<glm::vec2> chain;
        double len = 0.0;
        int at = startPt;
        for (;;) {
            ChainEdge* next = nullptr;
            for (auto& e : edges)
                if (!e.used && (e.a == at || e.b == at)) { next = &e; break; }
            if (!next) break;
            next->used = true;
            len += next->len;
            std::vector<glm::vec2> run = next->pts;
            if (next->b == at) std::reverse(run.begin(), run.end());
            if (chain.empty()) chain = std::move(run);
            else chain.insert(chain.end(), run.begin() + 1, run.end());
            at = (next->a == at) ? next->b : next->a;
        }
        if (chain.size() >= 2 && len > bestLen) {
            bestLen = len;
            best = std::move(chain);
        }
    }
    if (best.size() < 2) return {};

    BRepBuilderAPI_MakePolygon poly;
    for (const auto& p2 : best) poly.Add(sketchToWorld(p2));
    if (!poly.IsDone()) return {};
    return poly.Wire();
}

std::vector<TopoDS_Wire> Sketch::buildWires() const {
    std::vector<TopoDS_Wire> wires;

    // Helper edge spec used by the adjacency walker
    struct EdgeSpec {
        bool isArc = false;
        int startPtId = -1;
        int endPtId = -1;
        // Arc-only fields:
        int circleId = -1;           // index into m_circles
        float startAngle = 0.0f;     // radians, around circle center
        float endAngle = 0.0f;       // radians, CCW from startAngle (may exceed 2pi)
        // Spline-only: index into m_splines. A spline participates in
        // adjacency as a single edge between its first and last control
        // points; emitOcctEdge interpolates a smooth B-spline through ALL
        // its control points.
        int splineIdx = -1;
        // Spline sub-edge: when a point lands on the spline and splits it, a
        // sub-edge emits only samp[splineSampStart..splineSampEnd] of the sampled
        // curve (sample density must match emitOcctEdge). -1/-1 = whole spline.
        int splineSampStart = -1;
        int splineSampEnd = -1;
    };
    std::vector<EdgeSpec> edges;

    // Coordinate table for every adjacency node: real sketch points plus the
    // synthetic crossing points generated just below. Keyed by id. Synthetic
    // ids start well past the live id counter so they can't collide with real
    // ones. emitOcctEdge() and the splitting loops below resolve coordinates
    // through this table rather than getPoint(), so synthetic nodes work too.
    std::unordered_map<int, glm::vec2> coord;
    for (const auto& pt : m_points) coord[pt.id] = pt.pos;
    int nextSynthId = m_nextId + 100000;

    const float onLineTol = 1e-2f;

    // Register a synthetic point at each proper line-line crossing (an
    // intersection strictly interior to BOTH segments). This lets closed shapes
    // formed by *crossing* lines be detected even when the user never placed a
    // vertex at the crossing — matching what the trim tool produces, but without
    // requiring a trim first. Crossings at shared endpoints are excluded (the
    // interior-parameter test rejects them), so already-working profiles like a
    // plain rectangle are untouched.
    {
        auto cross2 = [](glm::vec2 p, glm::vec2 q) { return p.x * q.y - p.y * q.x; };
        const int nL = static_cast<int>(m_lines.size());
        std::vector<glm::vec2> A(nL), B(nL);
        std::vector<bool> ok(nL, false);
        for (int i = 0; i < nL; ++i) {
            auto ia = coord.find(m_lines[i].startPointId);
            auto ib = coord.find(m_lines[i].endPointId);
            if (ia != coord.end() && ib != coord.end()) {
                A[i] = ia->second; B[i] = ib->second; ok[i] = true;
            }
        }
        const float pe = 1e-4f; // keep crossings off the segment endpoints
        for (int i = 0; i < nL; ++i) {
            if (!ok[i]) continue;
            for (int j = i + 1; j < nL; ++j) {
                if (!ok[j]) continue;
                glm::vec2 di = B[i] - A[i];
                glm::vec2 dj = B[j] - A[j];
                float den = cross2(di, dj);
                if (std::abs(den) < 1e-9f) continue; // parallel / collinear
                glm::vec2 off = A[j] - A[i];
                float t = cross2(off, dj) / den;
                float u = cross2(off, di) / den;
                if (t < pe || t > 1.0f - pe || u < pe || u > 1.0f - pe) continue;
                glm::vec2 ip = A[i] + t * di;
                // Reuse a nearby existing node (real or synthetic) so three lines
                // meeting at one point share a single graph node.
                bool dup = false;
                for (const auto& kv : coord) {
                    if (glm::length(kv.second - ip) < onLineTol) { dup = true; break; }
                }
                if (!dup) coord[nextSynthId++] = ip;
            }
        }
    }

    // Add line edges. If any point (real sketch point OR synthetic crossing)
    // lies on the interior of a line, emit sub-edges so adjacency can route
    // through that point. Without this, the line stays one edge from corner to
    // corner and any element ending mid-line can never form a closed region.
    for (const auto& line : m_lines) {
        auto ai = coord.find(line.startPointId);
        auto bi = coord.find(line.endPointId);
        if (ai == coord.end() || bi == coord.end()) {
            edges.push_back({false, line.startPointId, line.endPointId, -1, 0, 0});
            continue;
        }
        glm::vec2 aPos = ai->second;
        glm::vec2 dir = bi->second - aPos;
        float len2 = glm::dot(dir, dir);
        if (len2 < 1e-12f) {
            edges.push_back({false, line.startPointId, line.endPointId, -1, 0, 0});
            continue;
        }

        struct Onseg { float t; int id; };
        std::vector<Onseg> onseg;
        onseg.push_back({0.0f, line.startPointId});
        onseg.push_back({1.0f, line.endPointId});
        for (const auto& kv : coord) {
            if (kv.first == line.startPointId || kv.first == line.endPointId) continue;
            glm::vec2 v = kv.second - aPos;
            float t = glm::dot(v, dir) / len2;
            if (t < 1e-3f || t > 1.0f - 1e-3f) continue;
            glm::vec2 proj = aPos + t * dir;
            if (glm::length(kv.second - proj) > onLineTol) continue;
            onseg.push_back({t, kv.first});
        }
        std::sort(onseg.begin(), onseg.end(),
                  [](const Onseg& x, const Onseg& y) { return x.t < y.t; });
        for (size_t i = 0; i + 1 < onseg.size(); ++i) {
            edges.push_back({false, onseg[i].id, onseg[i + 1].id, -1, 0, 0});
        }
    }

    // For each circle, check for points lying on its perimeter
    const float perimeterEpsilon = 1e-3f;
    for (size_t ci = 0; ci < m_circles.size(); ++ci) {
        const auto& circle = m_circles[ci];
        const SketchPoint* center = getPoint(circle.centerPointId);
        if (!center) continue;

        struct OnPerim { int ptId; float angle; };
        std::vector<OnPerim> onPerim;
        for (const auto& pt : m_points) {
            if (pt.id == circle.centerPointId) continue;
            float d = glm::length(pt.pos - center->pos);
            if (std::abs(d - static_cast<float>(circle.radius)) < perimeterEpsilon) {
                float a = std::atan2(pt.pos.y - center->pos.y, pt.pos.x - center->pos.x);
                if (a < 0.0f) a += 2.0f * static_cast<float>(M_PI);
                onPerim.push_back({pt.id, a});
            }
        }

        if (onPerim.size() < 2) {
            // A circle needs at least TWO perimeter junctions to be split into
            // arcs that route through them. With zero (a standalone circle) or
            // ONE (a stray point sitting on the rim — e.g. a line's endpoint,
            // or the circle's own drag-release point), splitting would emit a
            // degenerate point->itself 360° arc that never closes into a wire,
            // so the loop — and any region it bounds, like a concentric
            // annulus — silently vanished. Emit the clean full-circle wire.
            gp_Pnt center3d = sketchToWorld(center->pos);
            gp_Dir normal = m_plane.Position().Direction();
            gp_Circ gpCircle(gp_Ax2(center3d, normal), circle.radius);
            BRepBuilderAPI_MakeEdge edgeMaker(gpCircle);
            if (edgeMaker.IsDone()) {
                BRepBuilderAPI_MakeWire wireMaker(edgeMaker.Edge());
                if (wireMaker.IsDone()) wires.push_back(wireMaker.Wire());
            }
            continue;
        }

        // Split the circle into arc segments at perimeter points (CCW)
        std::sort(onPerim.begin(), onPerim.end(),
                  [](const OnPerim& a, const OnPerim& b){ return a.angle < b.angle; });
        for (size_t i = 0; i < onPerim.size(); ++i) {
            const OnPerim& cur = onPerim[i];
            const OnPerim& nxt = onPerim[(i + 1) % onPerim.size()];
            float endAng = nxt.angle;
            if (endAng <= cur.angle) endAng += 2.0f * static_cast<float>(M_PI);
            edges.push_back({true, cur.ptId, nxt.ptId, static_cast<int>(ci), cur.angle, endAng});
        }
    }

    // Existing arcs: include them in adjacency too. Same intermediate-point
    // split as for lines — any sketch point lying on the arc's perimeter
    // inside the arc's angle range becomes a virtual split, so adjacency can
    // route through it.
    for (const auto& arc : m_arcs) {
        const SketchPoint* center = getPoint(arc.centerPointId);
        const SketchPoint* s = getPoint(arc.startPointId);
        const SketchPoint* e = getPoint(arc.endPointId);
        if (!center || !s || !e) continue;
        float sA = std::atan2(s->pos.y - center->pos.y, s->pos.x - center->pos.x);
        float eA = std::atan2(e->pos.y - center->pos.y, e->pos.x - center->pos.x);
        if (sA < 0.0f) sA += 2.0f * static_cast<float>(M_PI);
        if (eA < 0.0f) eA += 2.0f * static_cast<float>(M_PI);
        if (eA <= sA) eA += 2.0f * static_cast<float>(M_PI);
        int arcEncoded = -2 - static_cast<int>(&arc - &m_arcs[0]);

        struct Onarc { float angle; int id; };
        std::vector<Onarc> onarc;
        onarc.push_back({sA, arc.startPointId});
        onarc.push_back({eA, arc.endPointId});
        for (const auto& pt : m_points) {
            if (pt.id == arc.startPointId || pt.id == arc.endPointId ||
                pt.id == arc.centerPointId) continue;
            float d = glm::length(pt.pos - center->pos);
            if (std::abs(d - static_cast<float>(arc.radius)) > perimeterEpsilon) continue;
            float a = std::atan2(pt.pos.y - center->pos.y, pt.pos.x - center->pos.x);
            if (a < 0.0f) a += 2.0f * static_cast<float>(M_PI);
            // Bring `a` into the [sA, sA + 2π) range so it can be compared against eA.
            while (a < sA - 1e-4f) a += 2.0f * static_cast<float>(M_PI);
            if (a < sA + 1e-3f || a > eA - 1e-3f) continue;
            onarc.push_back({a, pt.id});
        }
        std::sort(onarc.begin(), onarc.end(),
                  [](const Onarc& x, const Onarc& y) { return x.angle < y.angle; });
        for (size_t i = 0; i + 1 < onarc.size(); ++i) {
            edges.push_back({true, onarc[i].id, onarc[i + 1].id, arcEncoded,
                              onarc[i].angle, onarc[i + 1].angle});
        }
    }

    // Splines: one adjacency edge from first to last control point. A
    // CLOSED spline (first == last point, e.g. the user finished by
    // clicking the start point) forms a loop on its own and the walker
    // closes it immediately.
    for (size_t si = 0; si < m_splines.size(); ++si) {
        const auto& sp = m_splines[si];
        if (sp.isConstruction) continue;
        if (sp.controlPointIds.size() < 2) continue;
        const bool closedSp = sp.controlPointIds.size() > 2 &&
                              sp.controlPointIds.front() == sp.controlPointIds.back();
        // Sample the curve (density MUST match emitOcctEdge's sampleSpline2D) and
        // find any adjacency node lying ON it other than the spline's own control
        // points — a line endpoint landed there, and the spline must split so the
        // loop-walker (and a dividing line) can route through that point.
        std::vector<glm::vec2> samp = sampleSpline2D(sp, 24);
        const int ns = static_cast<int>(samp.size());
        std::unordered_set<int> ownCtrl(sp.controlPointIds.begin(), sp.controlPointIds.end());
        // A landed point sits ANYWHERE on the curve (between samples), so project
        // each candidate onto the sample SEGMENTS, not just the vertices — a
        // vertex-only test misses a point on a chord and the loop never closes.
        struct SplineSplit { int seg; float t; int ptId; };   // on segment [seg, seg+1]
        std::vector<SplineSplit> splits;
        if (ns >= 3) {
            for (const auto& kv : coord) {
                if (ownCtrl.count(kv.first)) continue;
                int bestSeg = -1; float bestT = 0.0f, bestD = onLineTol;
                for (int i = 0; i + 1 < ns; ++i) {
                    glm::vec2 a = samp[i], ab = samp[i + 1] - a;
                    float len2 = glm::dot(ab, ab);
                    if (len2 < 1e-12f) continue;
                    float t = glm::clamp(glm::dot(kv.second - a, ab) / len2, 0.0f, 1.0f);
                    float d = glm::length(kv.second - (a + ab * t));
                    if (d < bestD) { bestD = d; bestSeg = i; bestT = t; }
                }
                if (bestSeg >= 0) splits.push_back({bestSeg, bestT, kv.first});
            }
            std::sort(splits.begin(), splits.end(),
                      [](const SplineSplit& a, const SplineSplit& b) {
                          return a.seg != b.seg ? a.seg < b.seg : a.t < b.t; });
        }
        if (splits.empty()) {
            EdgeSpec es;
            es.splineIdx = static_cast<int>(si);
            es.startPtId = sp.controlPointIds.front();
            es.endPtId = sp.controlPointIds.back();
            edges.push_back(es);
            continue;
        }
        // Contiguous sub-edges front -> cut0 -> ... -> back. A cut on segment s
        // ends the previous sub-edge at sample s and starts the next at sample
        // s+1; the exact cut point is pinned in emitOcctEdge.
        int prevStart = 0, prevPt = sp.controlPointIds.front();
        for (const auto& s : splits) {
            EdgeSpec es;
            es.splineIdx = static_cast<int>(si);
            es.startPtId = prevPt; es.endPtId = s.ptId;
            es.splineSampStart = prevStart; es.splineSampEnd = s.seg;
            edges.push_back(es);
            prevStart = s.seg + 1; prevPt = s.ptId;
        }
        EdgeSpec es;
        es.splineIdx = static_cast<int>(si);
        es.startPtId = prevPt;
        es.endPtId = closedSp ? sp.controlPointIds.front() : sp.controlPointIds.back();
        es.splineSampStart = prevStart; es.splineSampEnd = ns - 1;
        edges.push_back(es);
    }

    if (edges.empty()) return wires;

    // Prune dangling edges. An edge hanging off a free end — a vertex touched by
    // only one edge — can never be part of a closed loop, so iteratively drop
    // such edges until only the cycle core remains. This keeps the greedy walker
    // below from wandering down a tail, marking its edges used, failing to
    // close, and rolling back: e.g. a triangle drawn with three *crossing* lines
    // has six dangling tails past the corners that would otherwise trap it.
    // Profiles with no free ends (a plain rectangle, a circle) are unaffected.
    std::vector<bool> alive(edges.size(), true);
    {
        bool changed = true;
        while (changed) {
            changed = false;
            std::unordered_map<int, int> degree;
            for (size_t i = 0; i < edges.size(); ++i) {
                if (!alive[i]) continue;
                degree[edges[i].startPtId]++;
                degree[edges[i].endPtId]++;
            }
            for (size_t i = 0; i < edges.size(); ++i) {
                if (!alive[i]) continue;
                if (degree[edges[i].startPtId] < 2 || degree[edges[i].endPtId] < 2) {
                    alive[i] = false;
                    changed = true;
                }
            }
        }
    }

    auto emitOcctEdge = [&](const EdgeSpec& es, int fromPt, BRepBuilderAPI_MakeWire& wm) -> bool {
        // Resolve through the coord table so synthetic crossing nodes (which have
        // no SketchPoint) work alongside real points.
        auto sc = coord.find(es.startPtId);
        auto ec = coord.find(es.endPtId);
        if (sc == coord.end() || ec == coord.end()) return false;
        gp_Pnt p1 = sketchToWorld(sc->second);
        gp_Pnt p2 = sketchToWorld(ec->second);
        if (p1.Distance(p2) < 1e-10 && !es.isArc && es.splineIdx < 0)
            return false; // (a CLOSED spline legitimately starts where it ends)

        if (es.splineIdx >= 0) {
            // The SAME centripetal Catmull-Rom curve the renderer draws, densely
            // sampled and fitted with a B-spline — what you see is what extrudes.
            // A sub-edge (splineSampStart/End set, from a point landing on the
            // spline and splitting it) emits only that slice, with its ends pinned
            // to the shared points so the pieces + the landing line meet exactly.
            const SketchSpline& sp = m_splines[es.splineIdx];
            std::vector<glm::vec2> full = sampleSpline2D(sp, 24); // density matches adjacency
            const bool subEdge = (es.splineSampStart >= 0 && es.splineSampEnd >= 0);
            std::vector<glm::vec2> samp;
            if (subEdge) {
                int a = std::max(0, es.splineSampStart);
                int b = std::min(static_cast<int>(full.size()) - 1, es.splineSampEnd);
                samp.push_back(sc->second);                     // exact start (pinned)
                for (int i = a; i <= b; ++i) samp.push_back(full[i]);
                samp.push_back(ec->second);                     // exact end (pinned)
                // Drop a duplicate where a pinned end coincides with its sample
                // (front/back sub-edges begin/end ON a control-point sample).
                std::vector<glm::vec2> ded;
                for (const auto& q : samp)
                    if (ded.empty() || glm::length(q - ded.back()) > 1e-6f) ded.push_back(q);
                samp.swap(ded);
            } else {
                samp = std::move(full);
            }
            if (samp.size() < 2) return false;
            bool closedSp = !subEdge && sp.controlPointIds.size() > 2 &&
                            sp.controlPointIds.front() == sp.controlPointIds.back();
            if (fromPt == es.endPtId && !closedSp)
                std::reverse(samp.begin(), samp.end());
            try {
                TColgp_Array1OfPnt arr(1, static_cast<int>(samp.size()));
                for (size_t k = 0; k < samp.size(); ++k)
                    arr.SetValue(static_cast<int>(k) + 1, sketchToWorld(samp[k]));
                int degMin = std::min(3, static_cast<int>(samp.size()) - 1);
                if (degMin < 1) return false;
                GeomAPI_PointsToBSpline fit(arr, degMin, 8, GeomAbs_C2, 1.0e-3);
                if (!fit.IsDone()) return false;
                BRepBuilderAPI_MakeEdge mk(fit.Curve());
                if (!mk.IsDone()) return false;
                wm.Add(mk.Edge());
                return true;
            } catch (...) { return false; }
        }

        if (!es.isArc) {
            BRepBuilderAPI_MakeEdge mk(p1, p2);
            if (!mk.IsDone()) return false;
            wm.Add(mk.Edge());
            return true;
        }

        // Arc segment: figure out center/radius from either a circle index or an arc index
        glm::vec2 center2d(0); double radius = 0.0;
        if (es.circleId >= 0 && es.circleId < static_cast<int>(m_circles.size())) {
            const SketchCircle& c = m_circles[es.circleId];
            const SketchPoint* cp = getPoint(c.centerPointId);
            if (!cp) return false;
            center2d = cp->pos;
            radius = c.radius;
        } else {
            int arcIdx = -es.circleId - 2;
            if (arcIdx < 0 || arcIdx >= static_cast<int>(m_arcs.size())) return false;
            const SketchArc& a = m_arcs[arcIdx];
            const SketchPoint* cp = getPoint(a.centerPointId);
            if (!cp) return false;
            center2d = cp->pos;
            radius = a.radius;
        }

        // Walk direction: if we're entering from startPtId go CCW (start->end angles),
        // otherwise go the other way.
        float aStart = es.startAngle;
        float aEnd = es.endAngle;
        if (fromPt == es.endPtId) std::swap(aStart, aEnd);
        float aMid = 0.5f * (aStart + aEnd);
        glm::vec2 mid2d(center2d.x + std::cos(aMid) * static_cast<float>(radius),
                        center2d.y + std::sin(aMid) * static_cast<float>(radius));
        gp_Pnt mid3d = sketchToWorld(mid2d);

        // p1 must correspond to fromPt and p2 to the other end
        gp_Pnt fromPnt = (fromPt == es.startPtId) ? p1 : p2;
        gp_Pnt toPnt   = (fromPt == es.startPtId) ? p2 : p1;

        GC_MakeArcOfCircle arcMaker(fromPnt, mid3d, toPnt);
        if (!arcMaker.IsDone()) return false;
        BRepBuilderAPI_MakeEdge mk(arcMaker.Value());
        if (!mk.IsDone()) return false;
        wm.Add(mk.Edge());
        return true;
    };

    // ── Planar face traversal ──────────────────────────────────────────────
    // Trace every minimal face of the planar graph via half-edges: each
    // undirected edge becomes two directed half-edges, and at each vertex the
    // face turns to the next edge in angular order. Every DIRECTED half-edge is
    // used once, so two faces can share an edge — a bump attached to a loop, or
    // a region split by a chord — which the old one-use-per-edge greedy walker
    // could not (it consumed the shared edge for one face and starved the other,
    // merging the bump or breaking the main loop). The unbounded outer face
    // comes out clockwise and is dropped by its signed area.

    // A self-loop edge (a closed spline with no split points) is already a
    // complete closed wire — emit it directly and keep it out of the graph.
    std::vector<bool> edgeDead(edges.size(), false);
    for (size_t i = 0; i < edges.size(); ++i) {
        if (!alive[i]) { edgeDead[i] = true; continue; }
        if (edges[i].startPtId == edges[i].endPtId) {
            edgeDead[i] = true;
            BRepBuilderAPI_MakeWire wm;
            if (emitOcctEdge(edges[i], edges[i].startPtId, wm) && wm.IsDone())
                wires.push_back(wm.Wire());
        }
    }

    // COINCIDENT STRAIGHT EDGES collapse to one. Drawing a rectangle whose side
    // lands on an existing line — inevitable on a busy face, and what the 0.3mm
    // point snap encourages — produces two edges between the SAME pair of nodes.
    // The half-edge walker then sees two outgoing half-edges at an identical
    // angle, so nextHE's "clockwise of the twin" can pick the duplicate instead
    // of turning the corner: the walk escapes along the twin, the enclosed area
    // never closes as its own face, and its half-edges are absorbed into the
    // surrounding one. The visible result is a rectangle with NO selectable
    // region, so clicking inside it picks the whole surrounding face — and a
    // push/pull there cut an entire box wall away (Steve's antenna tracker).
    //
    // Curves are excluded: two arcs (or an arc and a line) between the same two
    // points are genuinely different edges bounding real area between them.
    {
        std::set<std::pair<int, int>> seenStraight;
        for (size_t i = 0; i < edges.size(); ++i) {
            if (edgeDead[i]) continue;
            const EdgeSpec& e = edges[i];
            if (e.isArc || e.splineIdx >= 0) continue;
            const int lo = std::min(e.startPtId, e.endPtId);
            const int hi = std::max(e.startPtId, e.endPtId);
            if (!seenStraight.insert({lo, hi}).second) edgeDead[i] = true;
        }
    }

    // 2D polyline of an edge from its `from` endpoint to its `to` endpoint,
    // curves sampled — used both for the outgoing tangent (so a spline's two
    // sub-arcs leaving a shared point are ordered right) and for the face's TRUE
    // signed area (a corners-only polygon has ~zero area for a 2-spline loop).
    auto edgePolyline = [&](const EdgeSpec& es, int fromPt) -> std::vector<glm::vec2> {
        std::vector<glm::vec2> pts;
        auto sc = coord.find(es.startPtId), ec = coord.find(es.endPtId);
        glm::vec2 sPos = (sc != coord.end()) ? sc->second : glm::vec2(0.0f);
        glm::vec2 ePos = (ec != coord.end()) ? ec->second : glm::vec2(0.0f);
        if (es.splineIdx >= 0) {
            const SketchSpline& sp = m_splines[es.splineIdx];
            std::vector<glm::vec2> full = sampleSpline2D(sp, 24);
            if (es.splineSampStart >= 0 && es.splineSampEnd >= 0 && !full.empty()) {
                int a = std::max(0, es.splineSampStart);
                int b = std::min(static_cast<int>(full.size()) - 1, es.splineSampEnd);
                pts.push_back(sPos);
                for (int i = a; i <= b; ++i) pts.push_back(full[i]);
                pts.push_back(ePos);
                std::vector<glm::vec2> ded;
                for (const auto& q : pts)
                    if (ded.empty() || glm::length(q - ded.back()) > 1e-6f) ded.push_back(q);
                pts.swap(ded);
            } else {
                pts = full;
            }
        } else if (es.isArc) {
            glm::vec2 center(0); double radius = 0; bool ok = false;
            if (es.circleId >= 0 && es.circleId < static_cast<int>(m_circles.size())) {
                if (const SketchPoint* cp = getPoint(m_circles[es.circleId].centerPointId)) {
                    center = cp->pos; radius = m_circles[es.circleId].radius; ok = true; }
            } else {
                int ai = -es.circleId - 2;
                if (ai >= 0 && ai < static_cast<int>(m_arcs.size()))
                    if (const SketchPoint* cp = getPoint(m_arcs[ai].centerPointId)) {
                        center = cp->pos; radius = m_arcs[ai].radius; ok = true; }
            }
            if (ok && radius > 1e-9) {
                const int N = 12;
                for (int k = 0; k <= N; ++k) {
                    float ang = es.startAngle + (es.endAngle - es.startAngle) * (static_cast<float>(k) / N);
                    pts.push_back(center + glm::vec2(std::cos(ang), std::sin(ang)) * static_cast<float>(radius));
                }
            }
        }
        if (pts.size() < 2) pts = {sPos, ePos};
        if (fromPt == es.endPtId) std::reverse(pts.begin(), pts.end());
        return pts;
    };
    auto outgoingDir = [&](const EdgeSpec& es, int vpt) -> glm::vec2 {
        std::vector<glm::vec2> poly = edgePolyline(es, vpt);
        if (poly.size() < 2) return glm::vec2(1.0f, 0.0f);
        glm::vec2 d = poly[1] - poly[0];
        float l = glm::length(d);
        return l > 1e-9f ? d / l : glm::vec2(1.0f, 0.0f);
    };

    // Half-edges: 2*i = start->end, 2*i+1 = end->start (twin = index ^ 1).
    struct HE { int edge; int from; int to; float ang; };
    std::vector<HE> he(edges.size() * 2, HE{-1, -1, -1, 0.0f});
    for (size_t i = 0; i < edges.size(); ++i) {
        if (edgeDead[i]) continue;
        glm::vec2 ds = outgoingDir(edges[i], edges[i].startPtId);
        glm::vec2 de = outgoingDir(edges[i], edges[i].endPtId);
        he[2 * i]     = {static_cast<int>(i), edges[i].startPtId, edges[i].endPtId, std::atan2(ds.y, ds.x)};
        he[2 * i + 1] = {static_cast<int>(i), edges[i].endPtId, edges[i].startPtId, std::atan2(de.y, de.x)};
    }
    std::unordered_map<int, std::vector<int>> outHE;
    for (int i = 0; i < static_cast<int>(he.size()); ++i)
        if (he[i].edge >= 0) outHE[he[i].from].push_back(i);
    for (auto& kv : outHE)
        std::sort(kv.second.begin(), kv.second.end(),
                  [&](int a, int b) { return he[a].ang < he[b].ang; });

    // Face's next half-edge: at the arrival vertex, the edge immediately
    // clockwise of the twin in the CCW angular order.
    auto nextHE = [&](int h) -> int {
        int v = he[h].to, t = h ^ 1;       // twin is an outgoing half-edge at v
        auto it = outHE.find(v);
        if (it == outHE.end()) return -1;
        const auto& lst = it->second;
        int pos = -1, n = static_cast<int>(lst.size());
        for (int i = 0; i < n; ++i) if (lst[i] == t) { pos = i; break; }
        if (pos < 0) return -1;
        return lst[(pos - 1 + n) % n];
    };

    std::vector<bool> heUsed(he.size(), false);
    for (int h0 = 0; h0 < static_cast<int>(he.size()); ++h0) {
        if (he[h0].edge < 0 || heUsed[h0]) continue;
        std::vector<int> face;
        int h = h0; bool closed = false;
        for (int step = 0; step <= static_cast<int>(he.size()); ++step) {
            if (h < 0 || heUsed[h]) break;
            heUsed[h] = true; face.push_back(h);
            int nh = nextHE(h);
            if (nh == h0) { closed = true; break; }
            h = nh;
        }
        if (!closed || face.empty()) continue;
        // TRUE signed area from the sampled face outline (a corners-only polygon
        // is ~zero for a 2-spline loop): keep CCW interior faces, drop the CW
        // unbounded outer face and any zero-area degenerate.
        std::vector<glm::vec2> poly;
        for (int hh : face) {
            std::vector<glm::vec2> ep = edgePolyline(edges[he[hh].edge], he[hh].from);
            for (size_t k = (poly.empty() ? 0 : 1); k < ep.size(); ++k) poly.push_back(ep[k]);
        }
        double area2 = 0;
        for (size_t k = 0; k < poly.size(); ++k) {
            const glm::vec2& A = poly[k];
            const glm::vec2& B = poly[(k + 1) % poly.size()];
            area2 += static_cast<double>(A.x) * B.y - static_cast<double>(B.x) * A.y;
        }
        if (area2 <= 1e-7) continue;
        BRepBuilderAPI_MakeWire wireMaker;
        bool valid = true;
        for (int hh : face) {
            const EdgeSpec& es = edges[he[hh].edge];
            // Skip a zero-length line (degenerate duplicate points) rather than
            // failing the whole wire (that used to drop SVG circles).
            if (!es.isArc && es.splineIdx < 0) {
                auto sc = coord.find(es.startPtId), ec = coord.find(es.endPtId);
                if (sc != coord.end() && ec != coord.end() &&
                    glm::length(sc->second - ec->second) < 1e-6f) continue;
            }
            if (!emitOcctEdge(es, he[hh].from, wireMaker)) { valid = false; break; }
        }
        if (valid && wireMaker.IsDone()) wires.push_back(wireMaker.Wire());
    }

    return wires;
}

namespace {

// Sample a 2D point on a wire (used as a "representative" for containment tests).
// We pick the midpoint of the first edge in sketch-plane coordinates.
glm::vec2 sampleWirePoint(const TopoDS_Wire& wire, const gp_Pln& plane) {
    gp_Ax3 ax = plane.Position();
    gp_Pnt origin = ax.Location();
    gp_Dir xd = ax.XDirection();
    gp_Dir yd = ax.YDirection();

    auto toSketch2D = [&](const gp_Pnt& w) {
        gp_Vec v(origin, w);
        return glm::vec2(static_cast<float>(v.Dot(gp_Vec(xd))),
                         static_cast<float>(v.Dot(gp_Vec(yd))));
    };

    BRepTools_WireExplorer ex(wire);
    if (!ex.More()) return glm::vec2(0);
    TopoDS_Edge edge = TopoDS::Edge(ex.Current());
    double f, l;
    Handle(Geom_Curve) curve = BRep_Tool::Curve(edge, f, l);
    if (curve.IsNull()) return glm::vec2(0);
    gp_Pnt mid;
    curve->D0((f + l) * 0.5, mid);
    return toSketch2D(mid);
}

// Densely sample a wire's perimeter into 2D sketch-plane points.
void densifyWire2D(const TopoDS_Wire& wire, const gp_Pln& plane,
                   std::vector<glm::vec2>& out, int samplesPerEdge = 32) {
    gp_Ax3 ax = plane.Position();
    gp_Pnt origin = ax.Location();
    gp_Dir xd = ax.XDirection();
    gp_Dir yd = ax.YDirection();
    auto toSketch2D = [&](const gp_Pnt& w) {
        gp_Vec v(origin, w);
        return glm::vec2(static_cast<float>(v.Dot(gp_Vec(xd))),
                         static_cast<float>(v.Dot(gp_Vec(yd))));
    };

    for (BRepTools_WireExplorer ex(wire); ex.More(); ex.Next()) {
        TopoDS_Edge edge = TopoDS::Edge(ex.Current());
        double f, l;
        Handle(Geom_Curve) curve = BRep_Tool::Curve(edge, f, l);
        if (curve.IsNull()) continue;
        // Sample in the WIRE's traversal direction, not the curve's natural
        // f->l one. BRepTools_WireExplorer walks edges head-to-tail, but a
        // TopAbs_REVERSED edge (which the BOP region-builder routinely emits
        // for multi-loop sketches) runs opposite to its curve parameter — so
        // sampling f->l appends its points backwards, producing a self-
        // intersecting polygon that makes the even-odd point-in-polygon test
        // miscount. Honouring the orientation keeps the polygon simple so
        // interior clicks land. (Same scramble noted in getSourceFaceCentroid.)
        const bool rev = (ex.Orientation() == TopAbs_REVERSED);
        for (int i = 0; i < samplesPerEdge; ++i) {
            double frac = double(i) / samplesPerEdge;
            double t = rev ? l - (l - f) * frac : f + (l - f) * frac;
            gp_Pnt p;
            curve->D0(t, p);
            out.push_back(toSketch2D(p));
        }
    }
}

// Standard ray-cast point-in-polygon (winding-independent, counts crossings).
bool pointInPolygon2D(const std::vector<glm::vec2>& poly, glm::vec2 p) {
    bool inside = false;
    size_t n = poly.size();
    if (n < 3) return false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const auto& a = poly[i];
        const auto& b = poly[j];
        bool intersect = ((a.y > p.y) != (b.y > p.y)) &&
            (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y + 1e-30f) + a.x);
        if (intersect) inside = !inside;
    }
    return inside;
}

} // anonymous

std::vector<Sketch::Region> Sketch::buildRegions() const {
    const uint64_t h = geometryHash();
    if (m_regionCacheValid && h == m_regionHash) return m_regionCache;
    m_regionCache = buildRegionsUncached();
    m_regionHash = h;
    m_regionCacheValid = true;
    return m_regionCache;
}

bool Sketch::regionsCached() const {
    return m_regionCacheValid && geometryHash() == m_regionHash;
}

// FNV-1a over everything region construction depends on. ~10 µs on a text
// sketch — noise next to the general fuse this guards (tens of ms).
uint64_t Sketch::geometryHash() const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* data, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    auto mixI = [&](int v) { mix(&v, sizeof v); };
    auto mixF = [&](float v) { mix(&v, sizeof v); };
    auto mixD = [&](double v) { mix(&v, sizeof v); };

    const gp_Ax3& ax = m_plane.Position();
    mixD(ax.Location().X()); mixD(ax.Location().Y()); mixD(ax.Location().Z());
    mixD(ax.Direction().X()); mixD(ax.Direction().Y()); mixD(ax.Direction().Z());
    mixD(ax.XDirection().X()); mixD(ax.XDirection().Y()); mixD(ax.XDirection().Z());

    for (const auto& p : m_points) {
        mixI(p.id); mixF(p.pos.x); mixF(p.pos.y);
        mixI(p.isConstruction ? 1 : 0);
    }
    for (const auto& l : m_lines) {
        mixI(l.id); mixI(l.startPointId); mixI(l.endPointId);
        mixI(l.isConstruction ? 1 : 0);
    }
    for (const auto& c : m_circles) {
        mixI(c.id); mixI(c.centerPointId); mixD(c.radius);
        mixI(c.isConstruction ? 1 : 0);
    }
    for (const auto& a : m_arcs) {
        mixI(a.id); mixI(a.centerPointId); mixI(a.startPointId);
        mixI(a.endPointId); mixD(a.radius);
    }
    for (const auto& s : m_splines) {
        mixI(s.id);
        for (int id : s.controlPointIds) mixI(id);
    }
    for (const auto& g : m_polygons) {
        mixI(g.id); mixI(g.centerPointId); mixD(g.radius); mixI(g.sides);
    }
    mixI(m_sourceBodyId);
    // Source face identity: the TShape pointer changes whenever the host
    // body is regenerated, which is exactly when grafted holes can change.
    const void* tsh =
        m_sourceFace.IsNull() ? nullptr : m_sourceFace.TShape().get();
    mix(&tsh, sizeof tsh);
    // Plane pose: the 2D geometry projects to 3D through the plane, so the
    // cached 3D regions must rebuild when the sketch moves (e.g. Move Face
    // translates the plane) even though the 2D ids are unchanged.
    {
        gp_Pnt o = m_plane.Location();
        gp_Dir z = m_plane.Axis().Direction();
        gp_Dir x = m_plane.XAxis().Direction();
        double pv[9] = { o.X(), o.Y(), o.Z(), z.X(), z.Y(), z.Z(),
                         x.X(), x.Y(), x.Z() };
        mix(pv, sizeof pv);
    }
    return h;
}

TopoDS_Shape Sketch::buildProfileShape() const {
    auto wires = buildWires();
    if (wires.empty()) return TopoDS_Shape();
    const size_t n = wires.size();

    // Containment matrix from densified 2D polygons. n is small (a busy
    // SVG is a few dozen wires); the n² point-in-polygon pass is trivial
    // next to the boolean work below.
    std::vector<std::vector<glm::vec2>> polys(n);
    for (size_t i = 0; i < n; ++i)
        densifyWire2D(wires[i], m_plane, polys[i]);

    std::vector<std::vector<bool>> inside(n, std::vector<bool>(n, false));
    std::vector<int> depth(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (polys[i].size() < 3) continue;
        const size_t stepI = std::max<size_t>(1, polys[i].size() / 24);
        for (size_t j = 0; j < n; ++j) {
            if (i == j || polys[j].size() < 3) continue;
            // i ⊂ j  iff MOST of i's boundary points lie inside j. The old test
            // used a single vertex (polys[i][0]), which flips on float luck when
            // a counter grazes its glyph's edge — that's why A/R counters were
            // detected inconsistently. Sampling many points is robust, and keeps
            // concentric rings right (an outer ring's points sit OUTSIDE its
            // own counter, so it's never nested in its own hole).
            size_t in = 0, tested = 0;
            for (size_t k = 0; k < polys[i].size(); k += stepI) {
                ++tested;
                if (pointInPolygon2D(polys[j], polys[i][k])) ++in;
            }
            if (tested && in * 2 > tested) { inside[i][j] = true; depth[i]++; }
        }
    }
    // Direct parent of an odd-depth wire: its container one level up.
    std::vector<int> parent(n, -1);
    for (size_t i = 0; i < n; ++i) {
        if (depth[i] % 2 == 0) continue;
        for (size_t j = 0; j < n; ++j) {
            if (inside[i][j] && depth[j] == depth[i] - 1) {
                parent[i] = static_cast<int>(j);
                break;
            }
        }
    }

    // One face per island; holes removed by boolean cut (no wire-winding
    // coordination — the lesson from the projection op's aperture bug).
    auto wireFace = [&](TopoDS_Wire w) -> TopoDS_Face {
        for (int attempt = 0; attempt < 2; ++attempt) {
            BRepBuilderAPI_MakeFace mf(m_plane, w);
            if (!mf.IsDone()) return TopoDS_Face();
            ShapeFix_Face fix(mf.Face());
            fix.FixOrientationMode() = 1;
            fix.FixWireMode() = 1;
            fix.Perform();
            TopoDS_Face cand = fix.Face();
            GProp_GProps g;
            BRepGProp::SurfaceProperties(cand, g);
            if (g.Mass() > 0.0 && BRepCheck_Analyzer(cand).IsValid())
                return cand;
            w.Reverse();
        }
        return TopoDS_Face();
    };

    TopoDS_Compound comp;
    BRep_Builder bb;
    bb.MakeCompound(comp);
    int islands = 0;
    for (size_t i = 0; i < n; ++i) {
        if (depth[i] % 2 != 0) continue; // hole, consumed by its parent
        // #60: an even-depth island nested >=2 levels deep floats inside
        // another island's hole — a "plug" (e.g. the inner circle of a
        // stepped/counterbore hole, which sits inside both the part outline
        // AND the counterbore circle). Even-odd parity fills it solid, but a
        // single extruded profile must be connected material: sweeping a
        // floating plug drops a disconnected lump into the body. Skip it — the
        // hole stays open, matching the intent of a stepped hole.
        if (depth[i] >= 2) continue;
        TopoDS_Face outer = wireFace(wires[i]);
        if (outer.IsNull()) continue;
        TopTools_ListOfShape holeFaces;
        for (size_t j = 0; j < n; ++j) {
            if (parent[j] != static_cast<int>(i)) continue;
            TopoDS_Face hf = wireFace(wires[j]);
            if (!hf.IsNull()) holeFaces.Append(hf);
        }
        TopoDS_Shape island = outer;
        if (!holeFaces.IsEmpty()) {
            try {
                BRepAlgoAPI_Cut cut;
                TopTools_ListOfShape args;
                args.Append(outer);
                cut.SetArguments(args);
                cut.SetTools(holeFaces);
                cut.Build();
                if (cut.IsDone() && !cut.Shape().IsNull())
                    island = cut.Shape();
            } catch (...) {}
        }
        for (TopExp_Explorer fx(island, TopAbs_FACE); fx.More(); fx.Next()) {
            bb.Add(comp, fx.Current());
            islands++;
        }
    }
    if (islands == 0) return TopoDS_Shape();
    return comp;
}

std::vector<Sketch::Region> Sketch::buildRegionsUncached() const {
    std::vector<Region> regions;

    // Build a planar face from every closed wire the sketch forms. Each
    // sketch face is then augmented with any holes from the source face
    // that fall fully inside it — BOPAlgo_Builder doesn't split coplanar
    // faces when their edges don't intersect, so a sketch drawn AROUND an
    // existing hole would otherwise come out as a solid disk and a
    // push/pull would yield a solid bar instead of a tube.
    auto wires = buildWires();
    std::vector<TopoDS_Wire> sourceHoleWires;
    TopoDS_Wire sourceOuterWire;
    if (!m_sourceFace.IsNull()) {
        sourceOuterWire = BRepTools::OuterWire(m_sourceFace);
        for (TopExp_Explorer we(m_sourceFace, TopAbs_WIRE); we.More(); we.Next()) {
            TopoDS_Wire w = TopoDS::Wire(we.Current());
            if (!sourceOuterWire.IsNull() && !w.IsSame(sourceOuterWire)) {
                sourceHoleWires.push_back(w);
            }
        }
    }
    // Densify a wire to 2D sketch-plane points so we can do polygon-in-polygon tests.
    auto wireTo2D = [&](const TopoDS_Wire& w) -> std::vector<glm::vec2> {
        std::vector<glm::vec2> poly;
        densifyWire2D(w, m_plane, poly);
        return poly;
    };
    std::vector<std::vector<glm::vec2>> holePolys;
    holePolys.reserve(sourceHoleWires.size());
    for (const auto& hw : sourceHoleWires) holePolys.push_back(wireTo2D(hw));

    std::vector<TopoDS_Face> faces;
    for (const auto& w : wires) {
        BRepBuilderAPI_MakeFace mf(m_plane, w);
        if (!mf.IsDone()) continue;
        TopoDS_Face f = mf.Face();
        // Subtract any source-face holes that lie fully inside this sketch face.
        std::vector<glm::vec2> outerPoly = wireTo2D(w);
        int grafted = 0;
        for (size_t i = 0; i < sourceHoleWires.size(); ++i) {
            const auto& holePoly = holePolys[i];
            if (holePoly.empty()) continue;
            // Sample test: every densified point of the hole must lie inside
            // the sketch's outer polygon. (densifyWire2D produces enough
            // samples that a clean "all inside" check is reliable.)
            bool allInside = true;
            for (const auto& p : holePoly) {
                if (!pointInPolygon2D(outerPoly, p)) { allInside = false; break; }
            }
            if (!allInside) continue;
            // Graft this hole into the sketch face as an inner wire.
            BRepBuilderAPI_MakeFace addHole(f);
            addHole.Add(TopoDS::Wire(sourceHoleWires[i].Reversed()));
            if (addHole.IsDone()) {
                f = addHole.Face();
                ++grafted;
            }
        }
        faces.push_back(f);
        (void)grafted; // counter retained in case a debug log is re-added later
    }
    // Include the source face so leftover area outside the drawn shapes
    // (but inside the host face) is also selectable.
    if (!m_sourceFace.IsNull()) {
        faces.push_back(m_sourceFace);
    }

    if (faces.empty()) return regions;

    // Partition the union of all faces into atomic, non-overlapping regions.
    // General-fusing the faces splits every overlap into its own face — the lens
    // where two shapes cross, the surrounding crescents, an annulus where one
    // shape sits inside another — so each piece can be selected and push/pulled
    // independently. (This is what makes intersecting shapes selectable.) A lone
    // face has nothing to fuse, and a one-argument general fuse is invalid, so
    // use it directly in that case.
    std::vector<TopoDS_Face> atomic;
    if (faces.size() == 1) {
        atomic.push_back(faces.front());
    } else {
        try {
            BOPAlgo_Builder gf;
            for (const auto& f : faces) gf.AddArgument(f);
            gf.Perform();
            if (!gf.HasErrors()) {
                for (TopExp_Explorer ex(gf.Shape(), TopAbs_FACE); ex.More(); ex.Next())
                    atomic.push_back(TopoDS::Face(ex.Current()));
            }
        } catch (...) {}
        if (atomic.empty()) atomic = faces; // fall back to the un-fused faces
    }

    // Split those faces by every open sketch curve, so an interior curve that
    // divides a region (e.g. a wall splitting a room) yields separate selectable
    // cells. Curves that merely trace a face boundary are no-ops here; only those
    // crossing a face's interior actually subdivide it. (GF above handles closed
    // overlaps/holes; this handles open dividing lines, ARCS and splines — the
    // greedy wire-walker in buildWires can miss interior bands bounded by two
    // arcs, so feeding the arcs to the splitter recovers those cells.)
    {
        TopTools_ListOfShape toolEdges;
        // A tool curve should only SUBDIVIDE a face when it crosses the interior;
        // one that merely traces the boundary must be a no-op. BOPAlgo honours
        // that for straight edges, but a boundary ARC gets imprinted and carves a
        // thin sliver off each rounded corner (newly visible once SVG import
        // recovers real arcs). So only feed a curve whose midpoint has face
        // interior on BOTH sides — true for an interior divider, false for a
        // boundary edge.
        std::vector<std::vector<glm::vec2>> atomicPolys;
        for (const auto& f : atomic) {
            std::vector<glm::vec2> poly;
            densifyWire2D(BRepTools::OuterWire(f), m_plane, poly);
            if (!poly.empty()) atomicPolys.push_back(std::move(poly));
        }
        glm::vec2 abMin(0.0f), abMax(0.0f); bool haveAB = false;
        for (const auto& poly : atomicPolys)
            for (const auto& q : poly) {
                if (!haveAB) { abMin = abMax = q; haveAB = true; }
                else { abMin.x = std::min(abMin.x, q.x); abMin.y = std::min(abMin.y, q.y);
                       abMax.x = std::max(abMax.x, q.x); abMax.y = std::max(abMax.y, q.y); }
            }
        const float divEps = std::max(1e-4f,
            0.01f * glm::length(abMax - abMin));
        auto insideAny = [&](glm::vec2 p) {
            for (const auto& poly : atomicPolys)
                if (pointInPolygon2D(poly, p)) return true;
            return false;
        };
        auto isDivider = [&](glm::vec2 mid, glm::vec2 nrm) {
            float nl = glm::length(nrm);
            if (nl < 1e-9f) return true;           // degenerate: leave as-is
            nrm /= nl;
            return insideAny(mid + nrm * divEps) && insideAny(mid - nrm * divEps);
        };
        for (const auto& line : m_lines) {
            const SketchPoint* a = getPoint(line.startPointId);
            const SketchPoint* b = getPoint(line.endPointId);
            if (!a || !b) continue;
            gp_Pnt p1 = sketchToWorld(a->pos);
            gp_Pnt p2 = sketchToWorld(b->pos);
            if (p1.Distance(p2) < 1e-9) continue;
            glm::vec2 mid = 0.5f * (a->pos + b->pos);
            glm::vec2 nrm(-(b->pos.y - a->pos.y), b->pos.x - a->pos.x);
            if (!isDivider(mid, nrm)) continue;
            BRepBuilderAPI_MakeEdge mk(p1, p2);
            if (mk.IsDone()) toolEdges.Append(mk.Edge());
        }
        // Arcs as dividing tools — same three-point construction as emitOcctEdge.
        for (const auto& arc : m_arcs) {
            const SketchPoint* c = getPoint(arc.centerPointId);
            const SketchPoint* s = getPoint(arc.startPointId);
            const SketchPoint* e = getPoint(arc.endPointId);
            if (!c || !s || !e) continue;
            float sA = std::atan2(s->pos.y - c->pos.y, s->pos.x - c->pos.x);
            float eA = std::atan2(e->pos.y - c->pos.y, e->pos.x - c->pos.x);
            if (eA <= sA) eA += 2.0f * static_cast<float>(M_PI);
            float midA = 0.5f * (sA + eA);
            glm::vec2 mid2d(c->pos.x + std::cos(midA) * static_cast<float>(arc.radius),
                            c->pos.y + std::sin(midA) * static_cast<float>(arc.radius));
            gp_Pnt p1 = sketchToWorld(s->pos);
            gp_Pnt pm = sketchToWorld(mid2d);
            gp_Pnt p2 = sketchToWorld(e->pos);
            if (p1.Distance(p2) < 1e-9 && p1.Distance(pm) < 1e-9) continue;
            if (!isDivider(mid2d, glm::vec2(mid2d.x - c->pos.x, mid2d.y - c->pos.y)))
                continue;
            try {
                GC_MakeArcOfCircle arcMaker(p1, pm, p2);
                if (!arcMaker.IsDone()) continue;
                BRepBuilderAPI_MakeEdge mk(arcMaker.Value());
                if (mk.IsDone()) toolEdges.Append(mk.Edge());
            } catch (...) {}
        }
        // Splines as dividing tools — same B-spline fit as emitOcctEdge.
        for (const auto& sp : m_splines) {
            if (sp.isConstruction || sp.controlPointIds.size() < 2) continue;
            std::vector<glm::vec2> samp = sampleSpline2D(sp, 24);
            if (samp.size() < 2) continue;
            {   // boundary splines (e.g. an imported outline) must not sliver
                size_t mi = samp.size() / 2;
                size_t lo = mi > 0 ? mi - 1 : mi;
                size_t hiK = mi + 1 < samp.size() ? mi + 1 : mi;
                glm::vec2 tang = samp[hiK] - samp[lo];
                if (!isDivider(samp[mi], glm::vec2(-tang.y, tang.x))) continue;
            }
            try {
                TColgp_Array1OfPnt arr(1, static_cast<int>(samp.size()));
                for (size_t k = 0; k < samp.size(); ++k)
                    arr.SetValue(static_cast<int>(k) + 1, sketchToWorld(samp[k]));
                GeomAPI_PointsToBSpline fit(arr, 3, 8, GeomAbs_C2, 1.0e-3);
                if (!fit.IsDone()) continue;
                BRepBuilderAPI_MakeEdge mk(fit.Curve());
                if (mk.IsDone()) toolEdges.Append(mk.Edge());
            } catch (...) {}
        }
        if (!toolEdges.IsEmpty() && !atomic.empty()) {
            try {
                BRepAlgoAPI_Splitter sp;
                TopTools_ListOfShape args;
                for (const auto& f : atomic) args.Append(f);
                sp.SetArguments(args);
                sp.SetTools(toolEdges);
                sp.Build();
                if (!sp.HasErrors()) {
                    std::vector<TopoDS_Face> split;
                    for (TopExp_Explorer ex(sp.Shape(), TopAbs_FACE); ex.More(); ex.Next())
                        split.push_back(TopoDS::Face(ex.Current()));
                    if (!split.empty()) atomic.swap(split);
                }
            } catch (...) {}
        }
    }

    // Project a 3D point onto sketch-plane 2D coordinates.
    const gp_Ax3& ax = m_plane.Position();
    gp_Pnt planeOrigin = ax.Location();
    gp_Dir xd = ax.XDirection();
    gp_Dir yd = ax.YDirection();
    auto toSketch2D = [&](const gp_Pnt& w) {
        gp_Vec v(planeOrigin, w);
        return glm::vec2(static_cast<float>(v.Dot(gp_Vec(xd))),
                         static_cast<float>(v.Dot(gp_Vec(yd))));
    };

    for (const auto& f : atomic) {
        Region region;
        region.face = f;
        region.outerWire = BRepTools::OuterWire(f);
        for (TopExp_Explorer wexp(f, TopAbs_WIRE); wexp.More(); wexp.Next()) {
            TopoDS_Wire w = TopoDS::Wire(wexp.Current());
            if (!region.outerWire.IsNull() && !w.IsSame(region.outerWire))
                region.holeWires.push_back(w);
        }

        // Representative point: the face's area centroid in 2D. For a ring/annulus
        // the centroid can fall in the hole, so fall back to a point on the outer
        // wire when the centroid isn't actually inside the region.
        glm::vec2 rep(0);
        try {
            GProp_GProps props;
            BRepGProp::SurfaceProperties(f, props);
            rep = toSketch2D(props.CentreOfMass());
        } catch (...) {}
        if (region.outerWire.IsNull() || !isPointInRegion(region, rep)) {
            if (!region.outerWire.IsNull()) rep = sampleWirePoint(region.outerWire, m_plane);
        }
        region.representativePoint = rep;

        regions.push_back(std::move(region));
    }

    return regions;
}

bool Sketch::isPointInRegion(const Region& region, glm::vec2 p) const {
    std::vector<glm::vec2> outerPoly;
    densifyWire2D(region.outerWire, m_plane, outerPoly);
    if (!pointInPolygon2D(outerPoly, p)) return false;
    for (const auto& h : region.holeWires) {
        std::vector<glm::vec2> holePoly;
        densifyWire2D(h, m_plane, holePoly);
        if (pointInPolygon2D(holePoly, p)) return false;
    }
    return true;
}

bool Sketch::isPointInOrNearRegion(const Region& region, glm::vec2 p, float tol) const {
    if (isPointInRegion(region, p)) return true;
    if (tol <= 0.0f) return false;

    auto distToSeg = [](glm::vec2 q, glm::vec2 a, glm::vec2 b) {
        glm::vec2 ab = b - a;
        float len2 = glm::dot(ab, ab);
        float t = len2 > 1e-12f ? glm::clamp(glm::dot(q - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        return glm::length(q - (a + t * ab));
    };
    auto nearWire = [&](const TopoDS_Wire& w) {
        std::vector<glm::vec2> poly;
        densifyWire2D(w, m_plane, poly);
        for (size_t i = 0; i + 1 < poly.size(); ++i) {
            if (distToSeg(p, poly[i], poly[i + 1]) <= tol) return true;
        }
        return false;
    };
    if (nearWire(region.outerWire)) return true;
    for (const auto& h : region.holeWires) {
        if (nearWire(h)) return true;
    }
    return false;
}

bool Sketch::getSourceFaceCentroid(glm::vec2& out) const {
    if (m_sourceFace.IsNull()) return false;

    // Let OCCT compute the true area centroid (centre of mass for a uniform
    // density surface). Doing this ourselves from densified polygon vertices is
    // fragile for faces whose outer wire has any reversed edges — the resulting
    // vertex sequence is scrambled and the polygon-area formula returns garbage.
    //
    // NOTE this is the centroid of the TRIMMED face, so holes count: an
    // off-centre pocket pulls the point away from itself. That is what a centre
    // of mass means, and it is what the green centre marker shows.
    if (!m_centroid3dValid) {
        try {
            GProp_GProps props;
            BRepGProp::SurfaceProperties(m_sourceFace, props);
            if (props.Mass() <= 0.0) return false;
            m_centroid3d = props.CentreOfMass();
            m_centroid3dValid = true;
        } catch (...) { return false; }
    }

    // Project to sketch-plane 2D on every call rather than caching the 2D
    // result. The centroid belongs to the face, so its world position holds
    // until the face is replaced (setSourceFace clears the cache) — but its
    // 2D coordinates are relative to the PLANE, and the plane moves whenever
    // the sketch is moved. Caching the 2D value left the centre marker (and
    // the snap that follows it) behind by exactly the distance moved, since
    // setPlane has no way to know the cache existed. Two dot products per call
    // is nothing next to the BRepGProp pass this still avoids.
    const gp_Ax3& ax = m_plane.Position();
    gp_Vec v(ax.Location(), m_centroid3d);
    out = glm::vec2(static_cast<float>(v.Dot(gp_Vec(ax.XDirection()))),
                    static_cast<float>(v.Dot(gp_Vec(ax.YDirection()))));
    return true;
}

int Sketch::elementCount() const {
    return static_cast<int>(m_lines.size() + m_circles.size() + m_arcs.size() +
                            m_splines.size() + m_polygons.size());
}

int Sketch::pointCount() const {
    return static_cast<int>(m_points.size());
}

} // namespace materializr
