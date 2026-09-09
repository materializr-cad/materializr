#include "SvgExport.h"
#include "../modeling/Sketch.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace materializr {

namespace {

// One sketch element treated as a graph edge between two sketch point ids.
// Chaining by EXACT point id (not coordinates) is what lets a drawn loop
// export as one closed SVG path: the old exporter wrote every line as its own
// two-point <path>, so a 20-segment logo arrived at re-import as 20 disjoint
// fragments - duplicate corner points everywhere and no closed loop for the
// importer's circle/spline/region recovery to work with.
struct Edge {
    enum Kind { Line, Arc, Spline } kind;
    int a = -1, b = -1;   // endpoint sketch point ids
    int index = -1;       // into the sketch's line/arc/spline list
    bool used = false;
};

} // namespace

SvgExportResult SvgExport::exportSketch(const std::string& filePath, const Sketch& sketch) {
    SvgExportResult result;

    std::vector<Edge> edges;
    const auto& lines = sketch.getLines();
    const auto& arcs = sketch.getArcs();
    const auto& splines = sketch.getSplines();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].isConstruction) continue;
        if (!sketch.getPoint(lines[i].startPointId) ||
            !sketch.getPoint(lines[i].endPointId)) continue;
        edges.push_back({Edge::Line, lines[i].startPointId, lines[i].endPointId,
                         static_cast<int>(i), false});
    }
    for (size_t i = 0; i < arcs.size(); ++i) {
        if (arcs[i].isConstruction) continue;
        if (!sketch.getPoint(arcs[i].centerPointId) ||
            !sketch.getPoint(arcs[i].startPointId) ||
            !sketch.getPoint(arcs[i].endPointId)) continue;
        edges.push_back({Edge::Arc, arcs[i].startPointId, arcs[i].endPointId,
                         static_cast<int>(i), false});
    }
    for (size_t i = 0; i < splines.size(); ++i) {
        if (splines[i].isConstruction) continue;
        if (splines[i].controlPointIds.size() < 2) continue;
        if (!sketch.getPoint(splines[i].controlPointIds.front()) ||
            !sketch.getPoint(splines[i].controlPointIds.back())) continue;
        edges.push_back({Edge::Spline, splines[i].controlPointIds.front(),
                         splines[i].controlPointIds.back(),
                         static_cast<int>(i), false});
    }

    const bool haveCircles = [&] {
        for (const auto& c : sketch.getCircles())
            if (!c.isConstruction && sketch.getPoint(c.centerPointId)) return true;
        return false;
    }();
    if (edges.empty() && !haveCircles) {
        result.errorMessage = "Sketch has no exportable (non-construction) geometry.";
        return result;
    }

    // ── Walk connected chains: pick any unused edge, extend forward through
    //    shared endpoints until the chain closes on its start or dead-ends. ──
    std::multimap<int, size_t> byPoint;
    for (size_t i = 0; i < edges.size(); ++i) {
        byPoint.insert({edges[i].a, i});
        byPoint.insert({edges[i].b, i});
    }
    struct ChainStep { size_t edge; bool reversed; };
    struct Chain { std::vector<ChainStep> steps; bool closed = false; };
    std::vector<Chain> chains;
    for (size_t s = 0; s < edges.size(); ++s) {
        if (edges[s].used) continue;
        Chain ch;
        edges[s].used = true;
        ch.steps.push_back({s, false});
        const int start = edges[s].a;
        int cur = edges[s].b;
        while (cur != start) {
            bool advanced = false;
            auto range = byPoint.equal_range(cur);
            for (auto it = range.first; it != range.second; ++it) {
                Edge& e = edges[it->second];
                if (e.used) continue;
                e.used = true;
                ch.steps.push_back({it->second, e.a != cur});
                cur = (e.a == cur) ? e.b : e.a;
                advanced = true;
                break;
            }
            if (!advanced) break;
        }
        ch.closed = (cur == start && ch.steps.size() >= 2) ||
                    (ch.steps.size() == 1 && edges[s].a == edges[s].b);
        chains.push_back(std::move(ch));
    }

    // ── Bounding box over everything that will be drawn. ──
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    auto grow = [&](glm::vec2 q) {
        minX = std::min(minX, q.x); minY = std::min(minY, q.y);
        maxX = std::max(maxX, q.x); maxY = std::max(maxY, q.y);
    };
    for (const auto& e : edges) {
        if (e.kind == Edge::Line) {
            grow(sketch.getPoint(e.a)->pos);
            grow(sketch.getPoint(e.b)->pos);
        } else if (e.kind == Edge::Arc) {
            const auto& ar = arcs[e.index];
            glm::vec2 c = sketch.getPoint(ar.centerPointId)->pos;
            float r = static_cast<float>(ar.radius);
            grow(c - glm::vec2(r)); grow(c + glm::vec2(r));   // conservative
        } else {
            for (glm::vec2 q : sketch.sampleSpline2D(splines[e.index], 64)) grow(q);
        }
    }
    for (const auto& c : sketch.getCircles()) {
        if (c.isConstruction) continue;
        const SketchPoint* ctr = sketch.getPoint(c.centerPointId);
        if (!ctr) continue;
        float r = static_cast<float>(c.radius);
        grow(ctr->pos - glm::vec2(r)); grow(ctr->pos + glm::vec2(r));
    }
    const float margin = 1.0f;
    minX -= margin; minY -= margin; maxX += margin; maxY += margin;
    const float w = maxX - minX, h = maxY - minY;
    if (w <= 0.0f || h <= 0.0f) {
        result.errorMessage = "Sketch geometry is degenerate (zero extent).";
        return result;
    }

    FILE* f = std::fopen(filePath.c_str(), "w");
    if (!f) {
        result.errorMessage = "Could not open file for writing: " + filePath;
        return result;
    }

    // 1 SVG user unit = 1 mm; Y flipped (CAD Y-up -> SVG Y-down).
    auto X = [&](float x) { return x - minX; };
    auto Y = [&](float y) { return maxY - y; };
    std::fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    std::fprintf(f,
                 "<svg xmlns=\"http://www.w3.org/2000/svg\" "
                 "width=\"%.4fmm\" height=\"%.4fmm\" viewBox=\"0 0 %.4f %.4f\">\n",
                 w, h, w, h);

    int pathCount = 0;

    // Circles as REAL <circle> elements - the importer's Kåsa fit recovers a
    // native SketchCircle from them (nanosvg turns them into 4 exact cubics).
    for (const auto& c : sketch.getCircles()) {
        if (c.isConstruction) continue;
        const SketchPoint* ctr = sketch.getPoint(c.centerPointId);
        if (!ctr) continue;
        std::fprintf(f,
            "  <circle cx=\"%.4f\" cy=\"%.4f\" r=\"%.4f\" "
            "fill=\"none\" stroke=\"#000000\" stroke-width=\"0.1\"/>\n",
            X(ctr->pos.x), Y(ctr->pos.y), c.radius);
        ++pathCount;
    }

    // Each connected chain becomes ONE path: lines as L, arcs as true A
    // commands, splines as their sampled run (the importer's smooth-run
    // recovery rebuilds a native spline from the dense samples). Closed
    // chains end with Z so re-import sees a closed loop and can form regions.
    for (const auto& ch : chains) {
        std::string d;
        char buf[128];
        auto put = [&](const char* fmt, ...) {
            va_list ap;
            va_start(ap, fmt);
            std::vsnprintf(buf, sizeof(buf), fmt, ap);
            va_end(ap);
            d += buf;
        };
        bool first = true;
        for (const auto& stp : ch.steps) {
            const Edge& e = edges[stp.edge];
            const int fromId = stp.reversed ? e.b : e.a;
            const int toId   = stp.reversed ? e.a : e.b;
            glm::vec2 from = sketch.getPoint(fromId)->pos;
            glm::vec2 to   = sketch.getPoint(toId)->pos;
            if (first) { put("M%.4f %.4f ", X(from.x), Y(from.y)); first = false; }
            if (e.kind == Edge::Line) {
                put("L%.4f %.4f ", X(to.x), Y(to.y));
            } else if (e.kind == Edge::Arc) {
                // Renderer convention: CCW from start to end (end angle wraps
                // up past the start). The Y flip mirrors orientation, so a CAD
                // CCW arc is drawn CW in SVG coordinates -> sweep flag 0 when
                // traversed forward, 1 when the chain walks it backwards.
                const auto& ar = arcs[e.index];
                glm::vec2 c = sketch.getPoint(ar.centerPointId)->pos;
                glm::vec2 s = sketch.getPoint(ar.startPointId)->pos;
                glm::vec2 t = sketch.getPoint(ar.endPointId)->pos;
                float a0 = std::atan2(s.y - c.y, s.x - c.x);
                float a1 = std::atan2(t.y - c.y, t.x - c.x);
                if (a1 < a0) a1 += 2.0f * static_cast<float>(M_PI);
                const int largeArc = (a1 - a0) > static_cast<float>(M_PI) ? 1 : 0;
                const int sweep = stp.reversed ? 1 : 0;
                put("A%.4f %.4f 0 %d %d %.4f %.4f ",
                    ar.radius, ar.radius, largeArc, sweep, X(to.x), Y(to.y));
            } else {
                std::vector<glm::vec2> pts =
                    sketch.sampleSpline2D(splines[e.index], 64);
                if (stp.reversed) std::reverse(pts.begin(), pts.end());
                for (size_t i = 1; i < pts.size(); ++i)
                    put("L%.4f %.4f ", X(pts[i].x), Y(pts[i].y));
            }
        }
        if (d.empty()) continue;
        if (ch.closed) d += "Z";
        std::fprintf(f, "  <path d=\"%s\" fill=\"none\" stroke=\"#000000\" "
                        "stroke-width=\"0.1\"/>\n", d.c_str());
        ++pathCount;
    }

    std::fputs("</svg>\n", f);
    std::fclose(f);

    result.success = true;
    result.curveCount = pathCount;
    return result;
}

} // namespace materializr
