#include "DxfImport.h"
#include "../modeling/Sketch.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
#include <filesystem>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {

namespace {

// ── Parsed intermediate: everything lands as one of these, in FILE units. ───
struct PLine { glm::dvec2 a, b; };
struct PCircle { glm::dvec2 c; double r; };
// CCW from startAngle to endAngle (radians) — both DXF's and the sketch's
// native arc convention.
struct PArc { glm::dvec2 c; double r, a0, a1; };

struct Drawing {
    std::vector<PLine> lines;
    std::vector<PCircle> circles;
    std::vector<PArc> arcs;
    int skipped = 0;
    double unitScale = 1.0; // file units → mm
};

// One group-code/value pair per two lines; DXF's entire syntax.
struct Pair { int code; std::string value; };

// ─── Input budgets ──────────────────────────────────────────────────────────
// A DXF is untrusted input. Unlike SvgImport (32 MB + a 500k-point budget) and
// IgesIO (kMaxEntities), this importer had no ceiling of any kind: nothing
// bounded the pair count, the vertices in one polyline, the control points of
// one spline, or the primitives they expand into. Each budget is checked before
// the push_back it guards, and an over-budget file is refused with a message
// rather than silently truncated — the SvgImport precedent.
constexpr size_t kMaxDxfBytes        = 64u * 1024 * 1024;
constexpr size_t kMaxDxfLineBytes    = 4096;
constexpr size_t kMaxDxfPairs        = 4000000;
constexpr size_t kMaxDxfEntities     = 200000;
constexpr size_t kMaxPolyVertices    = 500000;
constexpr size_t kMaxSplineControls  = 100000;
constexpr size_t kMaxSplineKnots     = 100000;
constexpr size_t kMaxDxfOutputPrims  = 1000000;
// A spline degree arrives from group code 71. Bounding it is what stops
// `n + degree + 1` below from signed-overflowing, and >32 is meaningless anyway.
constexpr int    kMaxSplineDegree    = 32;

class PairReader {
public:
    explicit PairReader(std::istream& in) : m_in(in) {}
    // False at EOF, on a malformed line, or once the pair budget is spent.
    bool next(Pair& out) {
        if (m_pairs >= kMaxDxfPairs) return false;
        std::string codeLine, valueLine;
        if (!std::getline(m_in, codeLine)) return false;
        if (!std::getline(m_in, valueLine)) return false;
        // Refuse absurd lines before they become long std::strings.
        if (codeLine.size() > kMaxDxfLineBytes ||
            valueLine.size() > kMaxDxfLineBytes) return false;
        ++m_pairs;
        // Windows-authored files: strip \r (getline keeps it on Unix).
        if (!codeLine.empty() && codeLine.back() == '\r') codeLine.pop_back();
        if (!valueLine.empty() && valueLine.back() == '\r') valueLine.pop_back();
        out.code = std::atoi(codeLine.c_str());
        out.value = valueLine;
        return true;
    }

private:
    std::istream& m_in;
    size_t m_pairs = 0;
};

double insunitsToMM(int code) {
    switch (code) {
        case 1: return 25.4;     // inches
        case 2: return 304.8;    // feet
        case 4: return 1.0;      // millimeters
        case 5: return 10.0;     // centimeters
        case 6: return 1000.0;   // meters
        default: return 1.0;     // unitless / exotic → assume mm
    }
}

// A polyline vertex with its bulge (the arc-ness of the segment STARTING here).
struct PolyVertex { glm::dvec2 p; double bulge = 0.0; };

// Bulge segment → arc. bulge = tan(θ/4), θ = signed included angle (+ = CCW).
// Standard DXF conversion: radius from the chord, centre perpendicular to it.
void emitPolySegment(Drawing& d, const PolyVertex& v0, const glm::dvec2& p1) {
    const glm::dvec2 p0 = v0.p;
    if (std::abs(v0.bulge) < 1e-12) {
        if (p0 != p1) d.lines.push_back({p0, p1});
        return;
    }
    const double theta = 4.0 * std::atan(v0.bulge);
    const glm::dvec2 chord = p1 - p0;
    const double clen = glm::length(chord);
    if (clen < 1e-12) return;
    const double r = clen / (2.0 * std::abs(std::sin(theta / 2.0)));
    // Sagitta-side offset from the chord midpoint to the centre.
    const double h = (clen / 2.0) / std::tan(theta / 2.0);
    const glm::dvec2 mid = (p0 + p1) * 0.5;
    const glm::dvec2 perp(-chord.y / clen, chord.x / clen);
    const glm::dvec2 c = mid + perp * h;
    auto ang = [&](const glm::dvec2& p) { return std::atan2(p.y - c.y, p.x - c.x); };
    if (v0.bulge > 0.0) {
        d.arcs.push_back({c, r, ang(p0), ang(p1)});      // CCW as stored
    } else {
        d.arcs.push_back({c, r, ang(p1), ang(p0)});      // CW → same arc CCW
    }
}

void emitPolyline(Drawing& d, const std::vector<PolyVertex>& vs, bool closed) {
    if (vs.size() < 2) return;
    for (size_t i = 0; i + 1 < vs.size(); ++i) emitPolySegment(d, vs[i], vs[i + 1].p);
    if (closed) emitPolySegment(d, vs.back(), vs.front().p);
}

// De Boor sampling of a DXF SPLINE (degree + knots + control points; weights
// ignored — rational splines are vanishingly rare in profile files). Output
// is a polyline in `samples`. Falls back to the control polygon when the
// knot vector is inconsistent.
void sampleSpline(const std::vector<glm::dvec2>& ctrl, int degree,
                  const std::vector<double>& knots, bool closed,
                  std::vector<glm::dvec2>& samples) {
    // Bound the degree BEFORE computing expectKnots — that expression is the
    // overflow site: group code 71 can spell 2147483647, and `n + degree + 1`
    // is then signed overflow (UB) evaluated before any guard below runs.
    if (degree < 1 || degree > kMaxSplineDegree ||
        ctrl.size() > kMaxSplineControls || knots.size() > kMaxSplineKnots) {
        samples = ctrl; // fallback: control polygon
        return;
    }
    const int n = static_cast<int>(ctrl.size());
    const int expectKnots = n + degree + 1;
    if (n <= degree || static_cast<int>(knots.size()) != expectKnots) {
        samples = ctrl; // fallback: control polygon
        return;
    }
    auto deBoor = [&](double t) {
        int k = degree;
        for (int i = degree; i < n; ++i)
            if (t >= knots[i] && t <= knots[i + 1]) { k = i; break; }
        std::vector<glm::dvec2> dd(ctrl.begin() + (k - degree),
                                   ctrl.begin() + (k + 1));
        for (int r = 1; r <= degree; ++r) {
            for (int j = degree; j >= r; --j) {
                const double den = knots[j + 1 + k - r] - knots[j + k - degree];
                const double alpha = den > 1e-12
                    ? (t - knots[j + k - degree]) / den : 0.0;
                dd[j] = dd[j - 1] * (1.0 - alpha) + dd[j] * alpha;
            }
        }
        return dd[degree];
    };
    const double t0 = knots[degree], t1 = knots[n];
    const int steps = std::min(std::max(n * 8, 16), 512);
    samples.clear();
    for (int i = 0; i <= steps; ++i)
        samples.push_back(deBoor(t0 + (t1 - t0) * (double(i) / steps)));
    if (closed && glm::length(samples.front() - samples.back()) > 1e-9)
        samples.push_back(samples.front());
}

bool parse(std::istream& in, Drawing& d, std::string& error) {
    PairReader rd(in);
    Pair p;
    enum class Where { Limbo, Header, Entities } where = Where::Limbo;
    std::string section;

    // Budget tripwires. Set anywhere a cap is hit; parse() then refuses the file
    // rather than returning a silently truncated drawing.
    bool overBudget = false;
    size_t entities = 0;

    // Current-entity accumulation
    std::string ent;
    glm::dvec2 v10(0), v11(0), center(0);
    double radius = 0, a0 = 0, a1 = 0;
    bool have10 = false, have11 = false;
    std::vector<PolyVertex> poly;   // LWPOLYLINE inline / POLYLINE via VERTEX
    bool polyClosed = false, inPolyline = false;
    std::vector<glm::dvec2> splCtrl;
    std::vector<double> splKnots;
    int splDegree = 3, splFlags = 0;
    // ELLIPSE: centre in `center`, major-axis ENDPOINT (relative) in v11,
    // ratio + param range below.
    double elRatio = 1.0, elT0 = 0.0, elT1 = 2.0 * M_PI;

    auto flush = [&]() {
        if (ent.empty()) return;
        // Output budget: an entity can expand into many primitives (an ELLIPSE
        // fans into 64 segments, a SPLINE into up to 512), so cap the total
        // before the expansion rather than after it.
        if (d.lines.size() + d.circles.size() + d.arcs.size() >
            kMaxDxfOutputPrims) { overBudget = true; return; }
        if (ent == "LINE") {
            if (have10 && have11) d.lines.push_back({v10, v11});
        } else if (ent == "CIRCLE") {
            if (radius > 0) d.circles.push_back({center, radius});
        } else if (ent == "ARC") {
            if (radius > 0) {
                double s = a0 * M_PI / 180.0, e = a1 * M_PI / 180.0;
                d.arcs.push_back({center, radius, s, e});
            }
        } else if (ent == "LWPOLYLINE") {
            emitPolyline(d, poly, polyClosed);
        } else if (ent == "SPLINE") {
            std::vector<glm::dvec2> pts;
            sampleSpline(splCtrl, splDegree, splKnots, (splFlags & 1) != 0, pts);
            for (size_t i = 0; i + 1 < pts.size(); ++i)
                if (pts[i] != pts[i + 1]) d.lines.push_back({pts[i], pts[i + 1]});
        } else if (ent == "ELLIPSE") {
            // Sample: centre + major endpoint (relative) + minor/major ratio.
            const glm::dvec2 mj = v11;
            const glm::dvec2 mn(-mj.y * elRatio, mj.x * elRatio);
            const int steps = 64;
            glm::dvec2 prev;
            for (int i = 0; i <= steps; ++i) {
                const double t = elT0 + (elT1 - elT0) * (double(i) / steps);
                glm::dvec2 pt = center + mj * std::cos(t) + mn * std::sin(t);
                if (i > 0 && pt != prev) d.lines.push_back({prev, pt});
                prev = pt;
            }
        } else if (ent == "VERTEX" || ent == "SEQEND") {
            // handled inline below
        } else if (ent == "POLYLINE") {
            // vertices follow as VERTEX entities; emitted at SEQEND
        } else {
            ++d.skipped;
        }
        ent.clear();
        have10 = have11 = false;
        v10 = v11 = center = glm::dvec2(0);
        radius = a0 = a1 = 0;
        if (!inPolyline) { poly.clear(); polyClosed = false; }
        splCtrl.clear(); splKnots.clear(); splDegree = 3; splFlags = 0;
        elRatio = 1.0; elT0 = 0.0; elT1 = 2.0 * M_PI;
    };

    std::string headerVar;
    while (rd.next(p)) {
        if (p.code == 0) {
            if (p.value == "SECTION") { section.clear(); flush(); continue; }
            if (p.value == "ENDSEC") { flush(); where = Where::Limbo; continue; }
            if (p.value == "EOF") break;

            if (where == Where::Entities) {
                if (p.value == "VERTEX") {
                    // stash the previous VERTEX's data, stay in polyline mode
                    if (ent == "VERTEX" && have10) {
                        if (poly.size() >= kMaxPolyVertices) { overBudget = true; break; }
                        poly.push_back({v10, a0 /* bulge via 42 → a0 slot */});
                    }
                    ent = "VERTEX"; have10 = false; a0 = 0;
                    continue;
                }
                if (p.value == "SEQEND") {
                    if (ent == "VERTEX" && have10) {
                        if (poly.size() >= kMaxPolyVertices) { overBudget = true; break; }
                        poly.push_back({v10, a0});
                    }
                    emitPolyline(d, poly, polyClosed);
                    poly.clear(); polyClosed = false; inPolyline = false;
                    ent.clear(); have10 = false; a0 = 0;
                    continue;
                }
                flush();
                if (++entities > kMaxDxfEntities) { overBudget = true; break; }
                ent = p.value;
                if (ent == "POLYLINE") { inPolyline = true; poly.clear(); polyClosed = false; }
                if (ent == "LWPOLYLINE") { poly.clear(); polyClosed = false; }
            }
            continue;
        }

        if (p.code == 2 && section.empty()) {
            section = p.value;
            where = section == "HEADER" ? Where::Header
                  : section == "ENTITIES" ? Where::Entities : Where::Limbo;
            continue;
        }

        if (where == Where::Header) {
            if (p.code == 9) headerVar = p.value;
            else if (p.code == 70 && headerVar == "$INSUNITS")
                d.unitScale = insunitsToMM(std::atoi(p.value.c_str()));
            continue;
        }
        if (where != Where::Entities || ent.empty()) continue;

        const double v = std::atof(p.value.c_str());
        if (ent == "LWPOLYLINE") {
            // Vertices arrive as repeated 10/20 pairs; 42 = bulge of the
            // segment leaving the MOST RECENT vertex; 70 bit 0 = closed.
            switch (p.code) {
                case 10:
                    if (poly.size() >= kMaxPolyVertices) { overBudget = true; break; }
                    poly.push_back({{v, 0.0}, 0.0});
                    break;
                case 20: if (!poly.empty()) poly.back().p.y = v; break;
                case 42: if (!poly.empty()) poly.back().bulge = v; break;
                case 70: polyClosed = (std::atoi(p.value.c_str()) & 1) != 0; break;
            }
            continue;
        }
        if (ent == "SPLINE") {
            switch (p.code) {
                case 71: splDegree = std::atoi(p.value.c_str()); break;
                case 70: splFlags = std::atoi(p.value.c_str()); break;
                case 40:
                    if (splKnots.size() >= kMaxSplineKnots) { overBudget = true; break; }
                    splKnots.push_back(v);
                    break;
                case 10:
                    if (splCtrl.size() >= kMaxSplineControls) { overBudget = true; break; }
                    splCtrl.push_back({v, 0.0});
                    break;
                case 20: if (!splCtrl.empty()) splCtrl.back().y = v; break;
            }
            continue;
        }
        switch (p.code) {
            case 10:
                if (ent == "CIRCLE" || ent == "ARC" || ent == "ELLIPSE") center.x = v;
                else { v10.x = v; have10 = true; }
                break;
            case 20:
                if (ent == "CIRCLE" || ent == "ARC" || ent == "ELLIPSE") center.y = v;
                else v10.y = v;
                break;
            case 11: v11.x = v; have11 = true; break;
            case 21: v11.y = v; break;
            case 40: if (ent == "ELLIPSE") elRatio = v; else radius = v; break;
            case 41: if (ent == "ELLIPSE") elT0 = v; break;
            case 42: if (ent == "ELLIPSE") elT1 = v; else a0 = v; /* VERTEX bulge */ break;
            case 50: a0 = v; break;
            case 51: a1 = v; break;
            case 70: if (ent == "POLYLINE") polyClosed = (std::atoi(p.value.c_str()) & 1) != 0; break;
            default: break;
        }
    }
    flush();

    // Refuse, don't truncate: a partially-read DXF would import as geometry the
    // user never drew, which is worse than an honest failure.
    if (overBudget) {
        error = "DXF is too large or too complex to import safely "
                "(entity, vertex or output budget exceeded).";
        return false;
    }

    if (d.lines.empty() && d.circles.empty() && d.arcs.empty()) {
        error = d.skipped > 0
            ? "DXF contained only unsupported entities (text, dimensions, …)."
            : "No profile entities (LINE/ARC/CIRCLE/POLYLINE/SPLINE) found.";
        return false;
    }
    return true;
}

} // namespace

DxfImportResult DxfImport::importFile(const std::string& filePath, Sketch& sketch) {
    DxfImportResult result;
    std::ifstream in(filePath, std::ios::binary);
    if (!in.good()) {
        result.errorMessage = "Could not open file: " + filePath;
        return result;
    }
    // Absolute input cap, matching SvgImport::load. The per-container budgets in
    // parse() bound what the file can expand into; this bounds the file itself.
    {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(filePath, ec);
        if (!ec && sz > kMaxDxfBytes) {
            result.errorMessage = "DXF file is too large to import (over 64 MB).";
            return result;
        }
    }
    Drawing d;
    if (!parse(in, d, result.errorMessage)) return result;

    // Scale to mm, then centre the drawing's bounding box on the sketch
    // origin (dimensions preserved exactly; only the offset is normalized —
    // real drawings frequently live thousands of units from their origin).
    const double s = d.unitScale;
    glm::dvec2 mn(std::numeric_limits<double>::max());
    glm::dvec2 mx(std::numeric_limits<double>::lowest());
    auto grow = [&](const glm::dvec2& p) { mn = glm::min(mn, p); mx = glm::max(mx, p); };
    for (const auto& l : d.lines) { grow(l.a); grow(l.b); }
    for (const auto& c : d.circles) { grow(c.c - glm::dvec2(c.r)); grow(c.c + glm::dvec2(c.r)); }
    for (const auto& a : d.arcs) { grow(a.c - glm::dvec2(a.r)); grow(a.c + glm::dvec2(a.r)); }
    const glm::dvec2 offset = (mn + mx) * 0.5;

    auto xform = [&](const glm::dvec2& p) {
        return glm::vec2(static_cast<float>((p.x - offset.x) * s),
                         static_cast<float>((p.y - offset.y) * s));
    };

    for (const auto& l : d.lines) {
        int a = sketch.addPoint(xform(l.a));
        int b = sketch.addPoint(xform(l.b));
        sketch.addLine(a, b);
        ++result.entityCount;
    }
    for (const auto& c : d.circles) {
        int ctr = sketch.addPoint(xform(c.c));
        sketch.addCircle(ctr, c.r * s);
        ++result.entityCount;
    }
    for (const auto& a : d.arcs) {
        const glm::dvec2 ps(a.c.x + a.r * std::cos(a.a0), a.c.y + a.r * std::sin(a.a0));
        const glm::dvec2 pe(a.c.x + a.r * std::cos(a.a1), a.c.y + a.r * std::sin(a.a1));
        int ctr = sketch.addPoint(xform(a.c));
        int s0 = sketch.addPoint(xform(ps));
        int s1 = sketch.addPoint(xform(pe));
        sketch.addArc(ctr, s0, s1, a.r * s);
        ++result.entityCount;
    }

    result.skippedCount = d.skipped;
    result.success = true;
    return result;
}

} // namespace materializr
