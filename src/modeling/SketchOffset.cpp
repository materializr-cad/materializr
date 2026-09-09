#include "SketchOffset.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {

namespace {

constexpr float kTwoPi = 2.0f * static_cast<float>(M_PI);

float wrap2Pi(float a) {
    while (a < 0.0f)     a += kTwoPi;
    while (a >= kTwoPi)  a -= kTwoPi;
    return a;
}

float distSqPointSegment(glm::vec2 q, glm::vec2 p1, glm::vec2 p2) {
    glm::vec2 d = p2 - p1;
    float len2 = glm::dot(d, d);
    if (len2 < 1e-12f) { glm::vec2 v = q - p1; return glm::dot(v, v); }
    float t = std::clamp(glm::dot(q - p1, d) / len2, 0.0f, 1.0f);
    glm::vec2 v = q - (p1 + t * d);
    return glm::dot(v, v);
}

// True if theta lies within the arc sweeping CCW from startAngle to endAngle -
// the convention every SketchArc stores (see Sketch::addArc and the arc branch
// of SketchTool's commitMirror).
bool angleInArcCCW(float theta, float startAngle, float endAngle) {
    float s = wrap2Pi(startAngle), e = wrap2Pi(endAngle), t = wrap2Pi(theta);
    if (s <= e) return t >= s - 1e-5f && t <= e + 1e-5f;
    return t >= s - 1e-5f || t <= e + 1e-5f;
}

enum class PickKind { None, Line, Arc, Circle, Spline };

// Local pick. pickSketchElement() in SketchTool.cpp is `static` in that TU and
// is deliberately not extracted: there is no test_sketch_trim to protect a
// refactor of Trim's helpers. Splines are picked (not ignored) so that a click
// on one refuses cleanly instead of silently grabbing a line further away.
PickKind pickOffsettable(const Sketch& sk, glm::vec2 pos, float threshold,
                         int& outId) {
    float bestDistSq = threshold * threshold;
    PickKind bestKind = PickKind::None;
    outId = -1;

    for (const auto& ln : sk.getLines()) {
        if (ln.fromText) continue;
        const SketchPoint* a = sk.getPoint(ln.startPointId);
        const SketchPoint* b = sk.getPoint(ln.endPointId);
        if (!a || !b) continue;
        float dsq = distSqPointSegment(pos, a->pos, b->pos);
        if (dsq < bestDistSq) { bestDistSq = dsq; outId = ln.id; bestKind = PickKind::Line; }
    }
    for (const auto& ci : sk.getCircles()) {
        const SketchPoint* c = sk.getPoint(ci.centerPointId);
        if (!c) continue;
        float d = glm::length(pos - c->pos) - static_cast<float>(ci.radius);
        if (d * d < bestDistSq) { bestDistSq = d * d; outId = ci.id; bestKind = PickKind::Circle; }
    }
    for (const auto& ar : sk.getArcs()) {
        const SketchPoint* c = sk.getPoint(ar.centerPointId);
        const SketchPoint* s = sk.getPoint(ar.startPointId);
        const SketchPoint* e = sk.getPoint(ar.endPointId);
        if (!c || !s || !e) continue;
        glm::vec2 r = pos - c->pos;
        float theta = std::atan2(r.y, r.x);
        float sa = std::atan2(s->pos.y - c->pos.y, s->pos.x - c->pos.x);
        float ea = std::atan2(e->pos.y - c->pos.y, e->pos.x - c->pos.x);
        if (!angleInArcCCW(theta, sa, ea)) continue;
        float d = glm::length(r) - static_cast<float>(ar.radius);
        if (d * d < bestDistSq) { bestDistSq = d * d; outId = ar.id; bestKind = PickKind::Arc; }
    }
    for (const auto& sp : sk.getSplines()) {
        std::vector<glm::vec2> samp = sk.sampleSpline2D(sp, 24);
        for (size_t i = 0; i + 1 < samp.size(); ++i) {
            float dsq = distSqPointSegment(pos, samp[i], samp[i + 1]);
            if (dsq < bestDistSq) { bestDistSq = dsq; outId = sp.id; bestKind = PickKind::Spline; }
        }
    }
    return bestKind;
}

// An offsettable element reduced to "two endpoint ids", which is all the walk
// needs. Arc centres are NOT endpoints and never take part in adjacency.
enum class ElemKind { Line, Arc, Spline };

struct Elem {
    ElemKind kind = ElemKind::Line;
    int  id = -1;
    int  a = -1, b = -1; // endpoint point ids, in the element's stored order
};

std::vector<Elem> collectElems(const Sketch& sk) {
    std::vector<Elem> out;
    for (const auto& ln : sk.getLines()) {
        if (ln.fromText) continue;
        if (ln.startPointId == ln.endPointId) continue; // degenerate
        out.push_back({ElemKind::Line, ln.id, ln.startPointId, ln.endPointId});
    }
    for (const auto& ar : sk.getArcs())
        out.push_back({ElemKind::Arc, ar.id, ar.startPointId, ar.endPointId});
    // A spline joins the chain by its END control points only. Its interior
    // control points are ordinary sketch points, but nothing "meets" a curve
    // mid-span in this sketcher, so they take no part in adjacency.
    for (const auto& sp : sk.getSplines()) {
        if (sp.controlPointIds.size() < 2) continue;
        out.push_back({ElemKind::Spline, sp.id,
                       sp.controlPointIds.front(), sp.controlPointIds.back()});
    }
    return out;
}

// Build the arc's travel-order OffsetSeg. `forward` = traversed start->end
// (the stored CCW direction); otherwise the sweep is negated.
OffsetSeg makeArcSeg(const Sketch& sk, const SketchArc& ar, bool forward) {
    OffsetSeg s;
    s.kind = OffsetSeg::Kind::Arc;
    s.sourceId = ar.id;
    const SketchPoint* c = sk.getPoint(ar.centerPointId);
    const SketchPoint* a = sk.getPoint(ar.startPointId);
    const SketchPoint* b = sk.getPoint(ar.endPointId);
    s.c = c ? c->pos : glm::vec2(0.0f);
    s.r = static_cast<float>(ar.radius);
    float sa = a ? std::atan2(a->pos.y - s.c.y, a->pos.x - s.c.x) : 0.0f;
    float ea = b ? std::atan2(b->pos.y - s.c.y, b->pos.x - s.c.x) : 0.0f;
    float ccw = wrap2Pi(ea - sa);
    if (ccw < 1e-6f) ccw = kTwoPi; // coincident ends = full turn, not zero
    if (forward) {
        s.p0 = a ? a->pos : glm::vec2(0.0f);
        s.p1 = b ? b->pos : glm::vec2(0.0f);
        s.a0 = sa;
        s.sweep = ccw;
    } else {
        s.p0 = b ? b->pos : glm::vec2(0.0f);
        s.p1 = a ? a->pos : glm::vec2(0.0f);
        s.a0 = ea;
        s.sweep = -ccw;
    }
    return s;
}

OffsetSeg makeSplineSeg(const Sketch& sk, const SketchSpline& sp, bool forward) {
    OffsetSeg s;
    s.kind = OffsetSeg::Kind::Spline;
    s.sourceId = sp.id;
    s.srcCtrlCount = static_cast<int>(sp.controlPointIds.size());
    // The same B-spline sampling buildWires() emits, so the offset is measured
    // against the curve the user actually sees and extrudes.
    s.pts = sk.sampleSpline2D(sp, 12);
    if (!forward) std::reverse(s.pts.begin(), s.pts.end());
    if (!s.pts.empty()) { s.p0 = s.pts.front(); s.p1 = s.pts.back(); }
    return s;
}

OffsetSeg makeLineSeg(const Sketch& sk, const SketchLine& ln, bool forward) {
    OffsetSeg s;
    s.kind = OffsetSeg::Kind::Line;
    s.sourceId = ln.id;
    const SketchPoint* a = sk.getPoint(forward ? ln.startPointId : ln.endPointId);
    const SketchPoint* b = sk.getPoint(forward ? ln.endPointId : ln.startPointId);
    s.p0 = a ? a->pos : glm::vec2(0.0f);
    s.p1 = b ? b->pos : glm::vec2(0.0f);
    return s;
}

const SketchLine* findLine(const Sketch& sk, int id) {
    for (const auto& l : sk.getLines()) if (l.id == id) return &l;
    return nullptr;
}
const SketchArc* findArc(const Sketch& sk, int id) {
    for (const auto& a : sk.getArcs()) if (a.id == id) return &a;
    return nullptr;
}
const SketchSpline* findSpline(const Sketch& sk, int id) {
    for (const auto& s : sk.getSplines()) if (s.id == id) return &s;
    return nullptr;
}

} // anonymous namespace

// --- OffsetSeg ------------------------------------------------------------

namespace {

// Locate arc-length fraction t along a polyline: returns the index of the
// segment containing it and the local fraction within that segment.
void polyLocate(const std::vector<glm::vec2>& pts, float t, size_t& idx, float& frac) {
    idx = 0; frac = 0.0f;
    if (pts.size() < 2) return;
    float total = 0.0f;
    for (size_t i = 0; i + 1 < pts.size(); ++i) total += glm::length(pts[i + 1] - pts[i]);
    if (total < 1e-9f) return;
    float want = std::clamp(t, 0.0f, 1.0f) * total;
    float run = 0.0f;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        float seg = glm::length(pts[i + 1] - pts[i]);
        if (run + seg >= want || i + 2 == pts.size()) {
            idx = i;
            frac = (seg > 1e-9f) ? std::clamp((want - run) / seg, 0.0f, 1.0f) : 0.0f;
            return;
        }
        run += seg;
    }
}

} // anonymous namespace

glm::vec2 OffsetSeg::at(float t) const {
    if (kind == Kind::Line) return p0 + (p1 - p0) * t;
    if (kind == Kind::Spline) {
        if (pts.empty()) return p0;
        if (pts.size() == 1) return pts[0];
        size_t i; float f;
        polyLocate(pts, t, i, f);
        return pts[i] + (pts[i + 1] - pts[i]) * f;
    }
    float ang = a0 + sweep * t;
    return c + glm::vec2(std::cos(ang), std::sin(ang)) * r;
}

glm::vec2 OffsetSeg::tangent(float t) const {
    if (kind == Kind::Line) {
        glm::vec2 d = p1 - p0;
        float L = glm::length(d);
        return (L > 1e-9f) ? d / L : glm::vec2(1.0f, 0.0f);
    }
    if (kind == Kind::Spline) {
        if (pts.size() < 2) return glm::vec2(1.0f, 0.0f);
        size_t i; float f;
        polyLocate(pts, t, i, f);
        (void)f;
        glm::vec2 d = pts[i + 1] - pts[i];
        float L = glm::length(d);
        return (L > 1e-9f) ? d / L : glm::vec2(1.0f, 0.0f);
    }
    float ang = a0 + sweep * t;
    // d/dt of (cos, sin) is (-sin, cos), scaled by the sweep's sign.
    glm::vec2 d(-std::sin(ang), std::cos(ang));
    return (sweep < 0.0f) ? -d : d;
}

float OffsetSeg::length() const {
    if (kind == Kind::Line) return glm::length(p1 - p0);
    if (kind == Kind::Spline) {
        float total = 0.0f;
        for (size_t i = 0; i + 1 < pts.size(); ++i) total += glm::length(pts[i + 1] - pts[i]);
        return total;
    }
    return std::abs(sweep) * r;
}

// --- Chain walk -----------------------------------------------------------

OffsetChain walkOffsetChain(const Sketch& sk, glm::vec2 pos, float threshold) {
    OffsetChain chain;
    int hitId = -1;
    PickKind kind = pickOffsettable(sk, pos, threshold, hitId);
    if (kind == PickKind::None) return chain;

    if (kind == PickKind::Circle) {
        for (const auto& ci : sk.getCircles()) {
            if (ci.id != hitId) continue;
            const SketchPoint* c = sk.getPoint(ci.centerPointId);
            if (!c) return chain;
            OffsetSeg s;
            s.kind = OffsetSeg::Kind::Circle;
            s.sourceId = ci.id;
            s.c = c->pos;
            s.r = static_cast<float>(ci.radius);
            s.a0 = 0.0f;
            s.sweep = kTwoPi;
            s.p0 = s.p1 = c->pos + glm::vec2(s.r, 0.0f);
            chain.segs.push_back(s);
            chain.closed = true;
            return chain;
        }
        return chain;
    }

    const std::vector<Elem> elems = collectElems(sk);

    std::map<int, std::vector<size_t>> atPoint; // point id -> element indices
    for (size_t i = 0; i < elems.size(); ++i) {
        atPoint[elems[i].a].push_back(i);
        atPoint[elems[i].b].push_back(i);
    }

    const ElemKind wantKind = (kind == PickKind::Arc)    ? ElemKind::Arc
                            : (kind == PickKind::Spline) ? ElemKind::Spline
                                                         : ElemKind::Line;
    size_t startIdx = elems.size();
    for (size_t i = 0; i < elems.size(); ++i)
        if (elems[i].id == hitId && elems[i].kind == wantKind) { startIdx = i; break; }
    if (startIdx == elems.size()) return chain;

    // Ordered walk state: element index + whether it is traversed forward
    // (stored order a->b) or backward.
    struct Step { size_t idx; bool forward; };
    std::vector<Step> steps{{startIdx, true}};
    std::set<size_t> visited{startIdx};

    // The endpoint the walk currently sits on, and the element it came from.
    auto otherEnd = [&](const Elem& e, int viaPoint) {
        return (e.a == viaPoint) ? e.b : e.a;
    };

    // Continue from `pt`; returns the next element index, or elems.size() when
    // the walk must stop. Sets `closes` when it arrives back at the start.
    auto advance = [&](int pt, size_t fromIdx, bool& closes) -> size_t {
        closes = false;
        auto it = atPoint.find(pt);
        if (it == atPoint.end()) return elems.size();
        if (it->second.size() != 2) return elems.size();  // free end or branch
        for (size_t cand : it->second) {
            if (cand == fromIdx) continue;
            if (cand == startIdx) { closes = true; return elems.size(); }
            if (visited.count(cand)) return elems.size(); // non-simple loop
            return cand;
        }
        return elems.size();
    };

    // Forward from the start element's `b` end.
    int tailPt = elems[startIdx].b;
    for (;;) {
        bool closes = false;
        size_t next = advance(tailPt, steps.back().idx, closes);
        if (closes) { chain.closed = true; break; }
        if (next == elems.size()) break;
        bool fwd = (elems[next].a == tailPt);
        steps.push_back({next, fwd});
        visited.insert(next);
        tailPt = otherEnd(elems[next], tailPt);
    }

    // Backward from the start element's `a` end (skipped once closed).
    if (!chain.closed) {
        int headPt = elems[startIdx].a;
        for (;;) {
            bool closes = false;
            size_t prev = advance(headPt, steps.front().idx, closes);
            if (closes) { chain.closed = true; break; }
            if (prev == elems.size()) break;
            // Prepended, so it is traversed INTO headPt: forward when its `b`
            // end is the one we are joining at.
            bool fwd = (elems[prev].b == headPt);
            steps.insert(steps.begin(), {prev, fwd});
            visited.insert(prev);
            headPt = otherEnd(elems[prev], headPt);
        }
    }

    for (const Step& st : steps) {
        const Elem& e = elems[st.idx];
        switch (e.kind) {
        case ElemKind::Arc:
            if (const SketchArc* ar = findArc(sk, e.id))
                chain.segs.push_back(makeArcSeg(sk, *ar, st.forward));
            break;
        case ElemKind::Spline:
            if (const SketchSpline* sp = findSpline(sk, e.id)) {
                OffsetSeg seg = makeSplineSeg(sk, *sp, st.forward);
                if (seg.pts.size() >= 2) chain.segs.push_back(std::move(seg));
            }
            break;
        case ElemKind::Line:
            if (const SketchLine* ln = findLine(sk, e.id))
                chain.segs.push_back(makeLineSeg(sk, *ln, st.forward));
            break;
        }
    }
    return chain;
}

// --- Offsetting -----------------------------------------------------------

namespace {

// Right of travel: rotate the tangent by -90 degrees. For a counter-clockwise
// closed chain this points outward, which is why a positive `d` reads as
// "outward" there - but the caller must still take the sign from the cursor,
// because the walk's travel direction is arbitrary.
glm::vec2 rightNormal(glm::vec2 t) { return {t.y, -t.x}; }

// Signed angular delta from `from` to `to`, taken in the direction of `sign`.
float signedDelta(float from, float to, float sign) {
    float d = wrap2Pi(to - from);
    if (d < 1e-6f || kTwoPi - d < 1e-6f) return 0.0f;
    return (sign < 0.0f) ? d - kTwoPi : d;
}

void syncArcEndpoints(OffsetSeg& s) {
    s.p0 = s.c + glm::vec2(std::cos(s.a0), std::sin(s.a0)) * s.r;
    float ae = s.a0 + s.sweep;
    s.p1 = s.c + glm::vec2(std::cos(ae), std::sin(ae)) * s.r;
}

// Move a segment's END onto x, keeping its start put.
void retargetEnd(OffsetSeg& s, glm::vec2 x) {
    if (s.kind == OffsetSeg::Kind::Spline) {
        // Walk back to the sample nearest x and pin the tail there. Trimming a
        // sampled curve is a truncation, not a reparametrisation.
        if (s.pts.size() >= 2) {
            size_t best = s.pts.size() - 1;
            float bd = -1.0f;
            for (size_t i = 0; i < s.pts.size(); ++i) {
                float dd = glm::dot(s.pts[i] - x, s.pts[i] - x);
                if (bd < 0.0f || dd < bd) { bd = dd; best = i; }
            }
            if (best < 1) best = 1;
            s.pts.resize(best + 1);
            s.pts.back() = x;
            s.p1 = x;
        }
        return;
    }
    if (s.kind == OffsetSeg::Kind::Line) { s.p1 = x; return; }
    float ax = std::atan2(x.y - s.c.y, x.x - s.c.x);
    s.sweep = signedDelta(s.a0, ax, s.sweep);
    syncArcEndpoints(s);
}

// Move a segment's START onto x, keeping its end put.
void retargetStart(OffsetSeg& s, glm::vec2 x) {
    if (s.kind == OffsetSeg::Kind::Spline) {
        if (s.pts.size() >= 2) {
            size_t best = 0;
            float bd = -1.0f;
            for (size_t i = 0; i < s.pts.size(); ++i) {
                float dd = glm::dot(s.pts[i] - x, s.pts[i] - x);
                if (bd < 0.0f || dd < bd) { bd = dd; best = i; }
            }
            if (best > s.pts.size() - 2) best = s.pts.size() - 2;
            s.pts.erase(s.pts.begin(), s.pts.begin() + static_cast<long>(best));
            s.pts.front() = x;
            s.p0 = x;
        }
        return;
    }
    if (s.kind == OffsetSeg::Kind::Line) { s.p0 = x; return; }
    float ae = s.a0 + s.sweep;
    float ax = std::atan2(x.y - s.c.y, x.x - s.c.x);
    s.a0 = ax;
    s.sweep = signedDelta(ax, ae, s.sweep);
    syncArcEndpoints(s);
}

// Infinite-line x infinite-line. False when near-parallel.
bool lineLineInf(glm::vec2 a0, glm::vec2 ad, glm::vec2 b0, glm::vec2 bd,
                 glm::vec2& out) {
    float den = ad.x * bd.y - ad.y * bd.x;
    if (std::abs(den) < 1e-9f) return false;
    float t = ((b0.x - a0.x) * bd.y - (b0.y - a0.y) * bd.x) / den;
    out = a0 + ad * t;
    return true;
}

// Infinite line x full circle; appends 0-2 roots.
void lineCircleInf(glm::vec2 p, glm::vec2 dir, glm::vec2 c, float r,
                   std::vector<glm::vec2>& out) {
    glm::vec2 f = p - c;
    float a = glm::dot(dir, dir);
    if (a < 1e-12f) return;
    float b = 2.0f * glm::dot(f, dir);
    float cc = glm::dot(f, f) - r * r;
    float disc = b * b - 4.0f * a * cc;
    if (disc < 0.0f) return;
    disc = std::sqrt(disc);
    out.push_back(p + dir * ((-b - disc) / (2.0f * a)));
    if (disc > 1e-9f) out.push_back(p + dir * ((-b + disc) / (2.0f * a)));
}

void circleCircleInf(glm::vec2 c1, float r1, glm::vec2 c2, float r2,
                     std::vector<glm::vec2>& out) {
    glm::vec2 d = c2 - c1;
    float dist = glm::length(d);
    if (dist < 1e-9f) return;                       // concentric
    if (dist > r1 + r2 + 1e-5f) return;             // apart
    if (dist < std::abs(r1 - r2) - 1e-5f) return;   // nested
    float a = (r1 * r1 - r2 * r2 + dist * dist) / (2.0f * dist);
    float h2 = r1 * r1 - a * a;
    float h = (h2 > 0.0f) ? std::sqrt(h2) : 0.0f;
    glm::vec2 mid = c1 + d * (a / dist);
    glm::vec2 perp(-d.y / dist, d.x / dist);
    out.push_back(mid + perp * h);
    if (h > 1e-9f) out.push_back(mid - perp * h);
}

// The intersection of two offset segments' underlying curves nearest the source
// vertex. Both extending (opening + Sharp) and trimming (closing) reduce to
// this same question.
bool joinPoint(const OffsetSeg& a, const OffsetSeg& b, glm::vec2 v,
               glm::vec2& out) {
    std::vector<glm::vec2> cands;
    // A sampled curve carries no analytic curve to intersect. Rather than
    // invent one, report "no join" - the caller rounds the corner instead,
    // which is exact.
    if (a.kind == OffsetSeg::Kind::Spline || b.kind == OffsetSeg::Kind::Spline)
        return false;
    const bool aLine = a.kind == OffsetSeg::Kind::Line;
    const bool bLine = b.kind == OffsetSeg::Kind::Line;

    if (aLine && bLine) {
        glm::vec2 x;
        if (!lineLineInf(a.p0, a.p1 - a.p0, b.p0, b.p1 - b.p0, x)) return false;
        out = x;
        return true;
    }
    if (aLine)       lineCircleInf(a.p0, a.p1 - a.p0, b.c, b.r, cands);
    else if (bLine)  lineCircleInf(b.p0, b.p1 - b.p0, a.c, a.r, cands);
    else             circleCircleInf(a.c, a.r, b.c, b.r, cands);

    if (cands.empty()) return false;
    float best = -1.0f;
    for (glm::vec2 q : cands) {
        float dd = glm::dot(q - v, q - v);
        if (best < 0.0f || dd < best) { best = dd; out = q; }
    }
    return true;
}

// The round corner: an arc of radius |d| centred on the source vertex, from a
// to b. Both endpoints are already exactly |d| from v, so this is always valid.
OffsetSeg roundCorner(glm::vec2 v, glm::vec2 a, glm::vec2 b, float d,
                      float cross) {
    OffsetSeg s;
    s.kind = OffsetSeg::Kind::Arc;
    s.sourceId = -1;
    s.c = v;
    s.r = std::abs(d);
    s.a0 = std::atan2(a.y - v.y, a.x - v.x);
    float a1 = std::atan2(b.y - v.y, b.x - v.x);
    // The offset normals rotate exactly as the tangents do, so the corner turns
    // the same way as the chain: CCW for a left turn.
    s.sweep = signedDelta(s.a0, a1, cross);
    syncArcEndpoints(s);
    return s;
}

} // anonymous namespace

OffsetResult offsetChain(const OffsetChain& ch, float d, OffsetCorners corners) {
    OffsetResult res;
    res.closed = ch.closed;
    res.distance = d;
    if (!ch.valid()) { res.rejectReason = "Nothing to offset"; return res; }
    if (std::abs(d) < 1e-6f) { res.rejectReason = "Offset distance is zero"; return res; }

    // A circle is its own chain: no corners, just a radius change.
    if (ch.segs.size() == 1 && ch.segs[0].kind == OffsetSeg::Kind::Circle) {
        OffsetSeg s = ch.segs[0];
        s.r += d;
        if (s.r <= 1e-4f) {
            res.rejectReason = "Offset distance is larger than the circle";
            return res;
        }
        s.sourceId = -1;
        syncArcEndpoints(s);
        res.segs.push_back(s);
        res.valid = true;
        return res;
    }

    // 1. Offset each segment on its own.
    struct Raw { OffsetSeg seg; bool alive; };
    std::vector<Raw> raw;
    raw.reserve(ch.segs.size());
    for (const OffsetSeg& src : ch.segs) {
        OffsetSeg s = src;
        s.sourceId = -1;
        if (src.kind == OffsetSeg::Kind::Spline) {
            // No exact offset of a B-spline exists, so offset the sampled curve
            // point by point along its normal. Normals come from the ADJACENT
            // segment directions averaged at each interior sample, which keeps
            // a smooth curve smooth instead of faceting it.
            const std::vector<glm::vec2>& q = src.pts;
            s.pts.clear();
            s.pts.reserve(q.size());
            for (size_t i = 0; i < q.size(); ++i) {
                glm::vec2 t(0.0f);
                if (i > 0)             t += glm::normalize(q[i] - q[i - 1]);
                if (i + 1 < q.size())  t += glm::normalize(q[i + 1] - q[i]);
                float tl = glm::length(t);
                if (tl < 1e-9f) continue;
                s.pts.push_back(q[i] + rightNormal(t / tl) * d);
            }
            bool alive = s.pts.size() >= 2;
            if (alive) { s.p0 = s.pts.front(); s.p1 = s.pts.back(); }
            raw.push_back({s, alive});
            continue;
        }
        if (src.kind == OffsetSeg::Kind::Line) {
            glm::vec2 n = rightNormal(src.tangent(0.0f));
            s.p0 = src.p0 + n * d;
            s.p1 = src.p1 + n * d;
            raw.push_back({s, glm::length(s.p1 - s.p0) > 1e-6f});
        } else {
            // The right-of-travel normal on a CCW arc points outward, so the
            // offset radius grows; on a CW arc it points inward and shrinks.
            s.r = src.r + d * (src.sweep >= 0.0f ? 1.0f : -1.0f);
            bool alive = s.r > 1e-4f;
            if (alive) syncArcEndpoints(s);
            raw.push_back({s, alive});
        }
    }

    // 2. Fix up each corner. A closed chain also joins last -> first.
    const size_t n = raw.size();
    const size_t corner_count = ch.closed ? n : (n > 0 ? n - 1 : 0);
    std::vector<OffsetSeg> inserted(corner_count);   // round-corner arcs
    std::vector<bool>      hasInsert(corner_count, false);

    for (size_t i = 0; i < corner_count; ++i) {
        const size_t j = (i + 1) % n;
        if (!raw[i].alive || !raw[j].alive) continue;

        const glm::vec2 v    = ch.segs[i].p1;                 // source vertex
        const glm::vec2 tin  = ch.segs[i].tangent(1.0f);
        const glm::vec2 tout = ch.segs[j].tangent(0.0f);
        const float cross = tin.x * tout.y - tin.y * tout.x;
        const float dot   = glm::dot(tin, tout);

        // Tangent join: the offsets already meet, nothing to bridge.
        if (std::abs(cross) < 1e-6f && dot > 0.0f) continue;

        const bool opening = (cross * d) > 0.0f;
        // A full reversal (cusp) has no miter - it meets at infinity - so it is
        // always rounded regardless of the requested style.
        const bool reversal = std::abs(cross) < 1e-6f && dot <= 0.0f;

        if (opening && (corners == OffsetCorners::Round || reversal)) {
            inserted[i] = roundCorner(v, raw[i].seg.p1, raw[j].seg.p0, d,
                                      reversal ? d : cross);
            hasInsert[i] = true;
            continue;
        }

        glm::vec2 x;
        if (!joinPoint(raw[i].seg, raw[j].seg, v, x)) {
            // No miter exists (parallel), and for a closing corner no crossing
            // exists either. Round what we can; otherwise leave the gap for
            // pruneOffset to resolve.
            if (opening) {
                inserted[i] = roundCorner(v, raw[i].seg.p1, raw[j].seg.p0, d, cross);
                hasInsert[i] = true;
            }
            continue;
        }
        retargetEnd(raw[i].seg, x);
        retargetStart(raw[j].seg, x);
    }

    // 3. Emit in travel order, dropping collapsed segments.
    for (size_t i = 0; i < n; ++i) {
        if (raw[i].alive) res.segs.push_back(raw[i].seg);
        if (i < corner_count && hasInsert[i]) res.segs.push_back(inserted[i]);
    }

    if (res.segs.empty()) {
        res.rejectReason = "Offset distance is too large for this profile";
        return res;
    }
    res.valid = true;
    return res;
}

// --- Prune ----------------------------------------------------------------

namespace {

float distToSeg(const OffsetSeg& s, glm::vec2 q) {
    if (s.kind == OffsetSeg::Kind::Line) {
        return std::sqrt(distSqPointSegment(q, s.p0, s.p1));
    }
    if (s.kind == OffsetSeg::Kind::Spline) {
        float best = std::numeric_limits<float>::max();
        for (size_t i = 0; i + 1 < s.pts.size(); ++i)
            best = std::min(best, distSqPointSegment(q, s.pts[i], s.pts[i + 1]));
        return (best == std::numeric_limits<float>::max()) ? best : std::sqrt(best);
    }
    const float dc = glm::length(q - s.c);
    if (s.kind == OffsetSeg::Kind::Circle) return std::abs(dc - s.r);

    // Inside the swept span the nearest point is radial; outside it is one of
    // the two ends.
    float rel = wrap2Pi(std::atan2(q.y - s.c.y, q.x - s.c.x) - s.a0);
    bool inSpan = (s.sweep >= 0.0f) ? (rel <= s.sweep + 1e-5f)
                                    : ((rel - kTwoPi) >= s.sweep - 1e-5f);
    if (inSpan) return std::abs(dc - s.r);
    return std::min(glm::length(q - s.p0), glm::length(q - s.p1));
}

// The piece of `s` between parameters t0 and t1.
OffsetSeg subSeg(const OffsetSeg& s, float t0, float t1) {
    OffsetSeg o = s;
    if (s.kind == OffsetSeg::Kind::Line) {
        o.p0 = s.at(t0);
        o.p1 = s.at(t1);
        return o;
    }
    if (s.kind == OffsetSeg::Kind::Spline) {
        // Resample the kept span at the source density so a trimmed curve keeps
        // its fidelity instead of degrading to a chord.
        const int n = std::max(2, static_cast<int>(s.pts.size()));
        o.pts.clear();
        o.pts.reserve(static_cast<size_t>(n) + 1);
        for (int i = 0; i <= n; ++i)
            o.pts.push_back(s.at(t0 + (t1 - t0) * static_cast<float>(i) / n));
        o.p0 = o.pts.front();
        o.p1 = o.pts.back();
        return o;
    }
    // A partially-pruned circle is no longer a circle.
    o.kind = OffsetSeg::Kind::Arc;
    o.a0 = s.a0 + s.sweep * t0;
    o.sweep = s.sweep * (t1 - t0);
    syncArcEndpoints(o);
    return o;
}

} // anonymous namespace

float distanceToChain(const OffsetChain& ch, glm::vec2 q) {
    float best = std::numeric_limits<float>::max();
    for (const OffsetSeg& s : ch.segs) best = std::min(best, distToSeg(s, q));
    return best;
}

float signedDistanceToChain(const OffsetChain& ch, glm::vec2 q) {
    float best = std::numeric_limits<float>::max();
    size_t bi = 0;
    float  bt = 0.0f;

    for (size_t i = 0; i < ch.segs.size(); ++i) {
        const OffsetSeg& s = ch.segs[i];
        float t = 0.0f, dist = 0.0f;
        if (s.kind == OffsetSeg::Kind::Line) {
            glm::vec2 d = s.p1 - s.p0;
            float len2 = glm::dot(d, d);
            t = (len2 < 1e-12f) ? 0.0f
                                : std::clamp(glm::dot(q - s.p0, d) / len2, 0.0f, 1.0f);
            dist = glm::length(q - s.at(t));
        } else if (s.kind == OffsetSeg::Kind::Spline) {
            float bestSq = std::numeric_limits<float>::max();
            size_t bi = 0;
            for (size_t k = 0; k + 1 < s.pts.size(); ++k) {
                float dd = distSqPointSegment(q, s.pts[k], s.pts[k + 1]);
                if (dd < bestSq) { bestSq = dd; bi = k; }
            }
            if (bestSq == std::numeric_limits<float>::max()) continue;
            dist = std::sqrt(bestSq);
            // Arc-length parameter of the containing sample, good enough to
            // pick a tangent for the side test.
            float total = s.length(), run = 0.0f;
            for (size_t k = 0; k < bi; ++k) run += glm::length(s.pts[k + 1] - s.pts[k]);
            t = (total > 1e-9f) ? std::clamp(run / total, 0.0f, 1.0f) : 0.0f;
        } else {
            float rel = wrap2Pi(std::atan2(q.y - s.c.y, q.x - s.c.x) - s.a0);
            bool inSpan = (s.sweep >= 0.0f) ? (rel <= s.sweep + 1e-5f)
                                            : ((rel - kTwoPi) >= s.sweep - 1e-5f);
            if (inSpan && std::abs(s.sweep) > 1e-9f) {
                t = std::clamp(((s.sweep >= 0.0f) ? rel : rel - kTwoPi) / s.sweep,
                               0.0f, 1.0f);
                dist = std::abs(glm::length(q - s.c) - s.r);
            } else {
                float d0 = glm::length(q - s.p0), d1 = glm::length(q - s.p1);
                t = (d0 <= d1) ? 0.0f : 1.0f;
                dist = std::min(d0, d1);
            }
        }
        if (dist < best) { best = dist; bi = i; bt = t; }
    }
    if (ch.segs.empty()) return 0.0f;

    glm::vec2 n = rightNormal(ch.segs[bi].tangent(bt));
    // Sign from the normal, magnitude from the true distance: at a clamped end
    // the offset is not perpendicular, so the projection would understate it.
    return (glm::dot(q - ch.segs[bi].at(bt), n) >= 0.0f) ? best : -best;
}

void pruneOffset(OffsetResult& res, const OffsetChain& src, float d) {
    if (!res.valid) return;
    const float ad = std::abs(d);
    // The invariant can only be enforced to the accuracy of the representation.
    // A line or arc offset is exact, so the band is razor-thin. A SPLINE offset
    // is a sampled approximation whose error goes as curvature x spacing^2 -
    // comfortably more than 1e-3 mm - so an analytic epsilon condemns perfectly
    // good curve and chops one smooth offset into three abutting pieces.
    bool hasSpline = false;
    for (const OffsetSeg& s : src.segs)
        if (s.kind == OffsetSeg::Kind::Spline) { hasSpline = true; break; }
    const float eps = hasSpline ? std::max(0.02f, 0.01f * ad)
                                : std::max(1e-4f, 1e-3f * ad);
    const float floorDist = ad - eps;

    auto ok = [&](const OffsetSeg& s, float t) {
        return distanceToChain(src, s.at(t)) >= floorDist;
    };

    std::vector<OffsetSeg> kept;
    bool anyChange = false;

    for (const OffsetSeg& s : res.segs) {
        // Sample density follows the segment's length relative to the offset
        // distance: fine enough to catch a crossing, cheap enough for a
        // per-frame preview.
        const float step = std::max(0.1f, ad * 0.25f);
        int n = static_cast<int>(std::ceil(s.length() / step));
        n = std::clamp(n, 16, 128);

        std::vector<bool> good(n + 1);
        for (int i = 0; i <= n; ++i) good[i] = ok(s, static_cast<float>(i) / n);

        // Refine a valid/invalid boundary between samples i and i+1 to the
        // parameter where the offset crosses the band.
        auto refine = [&](int i, bool wantEndOfValidRun) {
            float lo = static_cast<float>(i) / n;
            float hi = static_cast<float>(i + 1) / n;
            for (int k = 0; k < 12; ++k) {
                float mid = 0.5f * (lo + hi);
                bool m = ok(s, mid);
                if (m == wantEndOfValidRun) lo = mid; else hi = mid;
            }
            return wantEndOfValidRun ? lo : hi;
        };

        int i = 0;
        while (i <= n) {
            if (!good[i]) { ++i; continue; }
            int runStart = i;
            while (i + 1 <= n && good[i + 1]) ++i;
            int runEnd = i;

            float t0 = (runStart == 0) ? 0.0f : refine(runStart - 1, false);
            float t1 = (runEnd == n)   ? 1.0f : refine(runEnd, true);

            if (t1 - t0 > 1e-3f) {
                if (t0 > 1e-6f || t1 < 1.0f - 1e-6f) anyChange = true;
                kept.push_back(subSeg(s, t0, t1));
            } else {
                anyChange = true; // run too short to be real geometry
            }
            ++i;
        }
    }

    if (kept.size() != res.segs.size()) anyChange = true;
    res.segs = std::move(kept);

    if (res.segs.empty()) {
        res.valid = false;
        res.closed = false;
        res.rejectReason = "Offset distance is too large for this profile";
        return;
    }
    if (anyChange) res.closed = false;
}

// --- Apply ----------------------------------------------------------------

namespace {

// Distance from q to a polyline.
float distToPolyline(const std::vector<glm::vec2>& poly, glm::vec2 q) {
    float best = std::numeric_limits<float>::max();
    for (size_t i = 0; i + 1 < poly.size(); ++i)
        best = std::min(best, distSqPointSegment(q, poly[i], poly[i + 1]));
    return (best == std::numeric_limits<float>::max()) ? best : std::sqrt(best);
}

// Choose control points for a spline through `curve`.
//
// The offset of a B-spline is not a B-spline, so this is a FIT, not an
// identity: pick n control points evenly along the offset curve, interpolate
// through them the way Sketch does, and measure how far that lands from the
// curve we actually want. Double n until it is inside `tol` (or the cap is
// hit). Starting at the source's own control-point count means an unremarkable
// offset reproduces the source's density exactly.
std::vector<glm::vec2> fitSplineCtrl(const std::vector<glm::vec2>& curve,
                                     int srcCtrlCount, float tol, int cap) {
    int n = std::max(3, srcCtrlCount > 0 ? srcCtrlCount : 4);
    if (n > cap) n = cap;
    std::vector<glm::vec2> ctrl;

    auto sampleAt = [&](int count) {
        std::vector<glm::vec2> out;
        out.reserve(static_cast<size_t>(count));
        OffsetSeg probe;                       // reuse the arc-length walker
        probe.kind = OffsetSeg::Kind::Spline;
        probe.pts = curve;
        for (int i = 0; i < count; ++i)
            out.push_back(probe.at(static_cast<float>(i) / (count - 1)));
        return out;
    };

    for (;;) {
        ctrl = sampleAt(n);
        if (n >= cap) break;
        // How far does the curve Sketch would actually build land from ours?
        std::vector<glm::vec2> got = Sketch::interpolate2D(ctrl, 12, false);
        float worst = 0.0f;
        for (glm::vec2 q : got) worst = std::max(worst, distToPolyline(curve, q));
        if (worst <= tol) break;
        n = std::min(cap, n * 2);
    }
    return ctrl;
}

} // anonymous namespace

void applyOffset(Sketch& sk, const OffsetResult& res,
                 const std::function<int(glm::vec2)>& weld,
                 std::set<int>& outPoints, std::set<int>& outElements) {
    if (!res.valid) return;

    // Segments that meet must share ONE point id, or the committed geometry is
    // a pile of disconnected edges and buildRegions() sees no loop.
    struct Made { glm::vec2 pos; int id; };
    std::vector<Made> made;

    auto ptFor = [&](glm::vec2 p) {
        for (const Made& m : made)
            if (glm::dot(m.pos - p, m.pos - p) < 1e-8f) return m.id;
        int id = weld ? weld(p) : -1;
        if (id < 0) id = sk.addPoint(p);
        made.push_back({p, id});
        outPoints.insert(id);
        return id;
    };

    for (const OffsetSeg& s : res.segs) {
        switch (s.kind) {
        case OffsetSeg::Kind::Line: {
            int a = ptFor(s.p0), b = ptFor(s.p1);
            if (a >= 0 && b >= 0 && a != b) outElements.insert(sk.addLine(a, b));
            break;
        }
        case OffsetSeg::Kind::Circle: {
            int c = ptFor(s.c);
            if (c >= 0) outElements.insert(sk.addCircle(c, s.r));
            break;
        }
        case OffsetSeg::Kind::Spline: {
            if (s.pts.size() < 2) break;
            // The CAP matters more than the tolerance here. Chasing a very
            // tight fit produced ~40 control points for a 5-point source - a
            // solid mass of vertex markers that is horrible to edit and hides
            // the curve. An offset of a 5-point spline should be about a
            // 5-point spline, so allow at most double the source's density and
            // accept the (sub-0.1 mm) deviation that comes with it.
            const float tol = std::max(0.02f, 0.02f * std::abs(res.distance));
            const int   cap = std::clamp(2 * std::max(1, s.srcCtrlCount), 8, 48);
            std::vector<glm::vec2> ctrl = fitSplineCtrl(s.pts, s.srcCtrlCount, tol, cap);
            std::vector<int> ids;
            ids.reserve(ctrl.size());
            for (glm::vec2 q : ctrl) {
                int id = ptFor(q);
                // Consecutive duplicates would make a degenerate span.
                if (ids.empty() || ids.back() != id) ids.push_back(id);
            }
            if (ids.size() >= 2) outElements.insert(sk.addSpline(ids));
            break;
        }
        case OffsetSeg::Kind::Arc: {
            int c = ptFor(s.c);
            // Sketch arcs are stored sweeping CCW from start to end, so a
            // clockwise-travelled arc is committed with its ends swapped -
            // geometrically the same arc, and arcs are undirected for region
            // building. (Same rule as commitMirror's reflected arcs.)
            glm::vec2 sp = (s.sweep >= 0.0f) ? s.p0 : s.p1;
            glm::vec2 ep = (s.sweep >= 0.0f) ? s.p1 : s.p0;
            int a = ptFor(sp), b = ptFor(ep);
            if (c >= 0 && a >= 0 && b >= 0)
                outElements.insert(sk.addArc(c, a, b, s.r));
            break;
        }
        }
    }
}

// --- Densify --------------------------------------------------------------

void densifySegs(const std::vector<OffsetSeg>& segs,
                 std::vector<std::vector<glm::vec2>>& out) {
    out.clear();
    std::vector<glm::vec2> run;
    const float kJoinTol2 = 1e-6f;

    auto flush = [&]() {
        if (run.size() >= 2) out.push_back(run);
        run.clear();
    };

    for (const OffsetSeg& s : segs) {
        // Start a new polyline whenever this segment does not continue the
        // previous one - a pruned offset legitimately has gaps.
        if (!run.empty()) {
            glm::vec2 d = s.p0 - run.back();
            if (glm::dot(d, d) > kJoinTol2) flush();
        }
        if (run.empty()) run.push_back(s.at(0.0f));

        if (s.kind == OffsetSeg::Kind::Line) {
            run.push_back(s.at(1.0f));
        } else if (s.kind == OffsetSeg::Kind::Spline) {
            for (size_t i = 1; i < s.pts.size(); ++i) run.push_back(s.pts[i]);
        } else {
            // One segment per ~7.5 degrees, at least 8 across any arc.
            int n = std::max(8, static_cast<int>(
                        std::ceil(std::abs(s.sweep) / (static_cast<float>(M_PI) / 24.0f))));
            for (int i = 1; i <= n; ++i)
                run.push_back(s.at(static_cast<float>(i) / static_cast<float>(n)));
        }
    }
    flush();
}

void densifyChain(const OffsetChain& ch,
                  std::vector<std::vector<glm::vec2>>& out) {
    densifySegs(ch.segs, out);
}

} // namespace materializr
