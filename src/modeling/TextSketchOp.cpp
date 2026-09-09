#include "TextSketchOp.h"
#include "Sketch.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <vector>
#include <Font_BRepFont.hxx>
#include <Font_BRepTextBuilder.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <NCollection_String.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>

namespace materializr {

namespace {

// Sample one wire into an ordered polyline (glyph units, z dropped).
// WireExplorer walks edges in connection order; a REVERSED edge's samples
// arrive backwards and get flipped so the loop stays continuous.
std::vector<glm::vec2> sampleWire(const TopoDS_Wire& w, double deflection) {
    std::vector<glm::vec2> pts;
    for (BRepTools_WireExplorer ex(w); ex.More(); ex.Next()) {
        const TopoDS_Edge& e = ex.Current();
        BRepAdaptor_Curve c(e);
        GCPnts_QuasiUniformDeflection d(c, deflection);
        if (!d.IsDone() || d.NbPoints() < 2) continue;
        std::vector<glm::vec2> seg;
        seg.reserve(d.NbPoints());
        for (int i = 1; i <= d.NbPoints(); ++i) {
            gp_Pnt p = d.Value(i);
            seg.push_back(glm::vec2(static_cast<float>(p.X()),
                                    static_cast<float>(p.Y())));
        }
        if (e.Orientation() == TopAbs_REVERSED)
            std::reverse(seg.begin(), seg.end());
        // skip the junction point shared with the previous edge
        size_t start = pts.empty() ? 0 : 1;
        for (size_t i = start; i < seg.size(); ++i) pts.push_back(seg[i]);
    }
    // drop the closing duplicate (loop closure is an explicit line later)
    if (pts.size() >= 2 &&
        glm::length(pts.front() - pts.back()) < 1e-6f * 64.0f) {
        pts.pop_back();
    }
    return pts;
}

} // namespace

namespace {

// Shared glyph-shape build: font load, cap-height probe, text render.
// Returns false (and logs) when nothing renders. `scale` converts glyph
// units to mm for the requested capital height.
bool buildGlyphShape(const std::string& text, const std::string& fontPath,
                     float heightMm, TopoDS_Shape& shapeOut, float& scaleOut,
                     double& emOut) {
    if (text.empty() || heightMm <= 0.01f || fontPath.empty()) return false;

    // Render at a fixed em size and scale the sampled points afterwards -
    // cap height isn't the em size (DejaVu's 'H' is ~71% of it), and the
    // user asked for a letter height, not a typographic unit.
    const double em = 64.0;
    Font_BRepFont font;
    if (!font.Init(NCollection_String(fontPath.c_str()), em, 0)) {
        std::fprintf(stderr, "[Text] cannot load font '%s'\n",
                     fontPath.c_str());
        return false;
    }
    Font_BRepTextBuilder builder;

    double capH = em * 0.7; // fallback if the probe fails
    {
        TopoDS_Shape probe =
            builder.Perform(font, NCollection_String("H"), gp_Ax3());
        if (!probe.IsNull()) {
            Bnd_Box bb;
            BRepBndLib::Add(probe, bb);
            if (!bb.IsVoid()) {
                double x0, y0, z0, x1, y1, z1;
                bb.Get(x0, y0, z0, x1, y1, z1);
                if (y1 - y0 > 1.0) capH = y1 - y0;
            }
        }
    }

    TopoDS_Shape shape;
    try {
        shape = builder.Perform(font, NCollection_String(text.c_str()),
                                gp_Ax3());
    } catch (...) {
        std::fprintf(stderr, "[Text] glyph build threw\n");
        return false;
    }
    if (shape.IsNull()) {
        std::fprintf(stderr, "[Text] glyph build produced nothing\n");
        return false;
    }
    shapeOut = shape;
    scaleOut = heightMm / static_cast<float>(capH);
    emOut = em;
    return true;
}

} // namespace

int TextSketch::generate(Sketch* sketch, const std::string& text,
                         const std::string& fontPath, glm::vec2 pos,
                         float heightMm, float angleDeg) {
    if (!sketch) return 0;
    TopoDS_Shape shape;
    float scale = 1.0f;
    double em = 64.0;
    if (!buildGlyphShape(text, fontPath, heightMm, shape, scale, em))
        return 0;

    const float a = glm::radians(angleDeg);
    const float ca = std::cos(a), sa = std::sin(a);
    auto place = [&](glm::vec2 p) {
        p *= scale;
        return pos + glm::vec2(p.x * ca - p.y * sa, p.x * sa + p.y * ca);
    };

    // 0.2%% of the em - ~0.02 mm chord error on 10 mm letters.
    const double deflection = em * 0.002;

    int loops = 0;
    for (TopExp_Explorer fx(shape, TopAbs_FACE); fx.More(); fx.Next()) {
        const TopoDS_Face& f = TopoDS::Face(fx.Current());
        for (TopExp_Explorer wx(f, TopAbs_WIRE); wx.More(); wx.Next()) {
            std::vector<glm::vec2> pts =
                sampleWire(TopoDS::Wire(wx.Current()), deflection);
            if (pts.size() < 3) continue;

            // NOTE: ids only - SketchPoint* would dangle across addPoint
            // reallocations (the polygon-tool lesson).
            std::vector<int> ids;
            ids.reserve(pts.size());
            for (const glm::vec2& p : pts)
                ids.push_back(sketch->addPoint(place(p), /*fromText=*/true));
            for (size_t i = 0; i + 1 < ids.size(); ++i)
                sketch->addLine(ids[i], ids[i + 1], /*fromText=*/true);
            sketch->addLine(ids.back(), ids.front(), /*fromText=*/true);
            loops++;
        }
    }
    std::fprintf(stderr, "[Text] '%s' h=%.2fmm: %d loops\n", text.c_str(),
                 heightMm, loops);
    return loops;
}

bool TextSketch::measure(const std::string& text,
                         const std::string& fontPath, float heightMm,
                         glm::vec2& bbMin, glm::vec2& bbMax) {
    TopoDS_Shape shape;
    float scale = 1.0f;
    double em = 64.0;
    if (!buildGlyphShape(text, fontPath, heightMm, shape, scale, em))
        return false;
    Bnd_Box bb;
    BRepBndLib::Add(shape, bb);
    if (bb.IsVoid()) return false;
    double x0, y0, z0, x1, y1, z1;
    bb.Get(x0, y0, z0, x1, y1, z1);
    bbMin = glm::vec2(static_cast<float>(x0), static_cast<float>(y0)) * scale;
    bbMax = glm::vec2(static_cast<float>(x1), static_cast<float>(y1)) * scale;
    return true;
}

bool TextSketch::outline(const std::string& text, const std::string& fontPath,
                         float heightMm,
                         std::vector<std::vector<glm::vec2>>& loops) {
    loops.clear();
    TopoDS_Shape shape;
    float scale = 1.0f;
    double em = 64.0;
    if (!buildGlyphShape(text, fontPath, heightMm, shape, scale, em))
        return false;
    const double deflection = em * 0.002; // match generate()'s chord error
    for (TopExp_Explorer fx(shape, TopAbs_FACE); fx.More(); fx.Next()) {
        const TopoDS_Face& f = TopoDS::Face(fx.Current());
        for (TopExp_Explorer wx(f, TopAbs_WIRE); wx.More(); wx.Next()) {
            std::vector<glm::vec2> pts =
                sampleWire(TopoDS::Wire(wx.Current()), deflection);
            if (pts.size() < 3) continue;
            for (glm::vec2& p : pts) p *= scale; // local, unrotated (measure() space)
            loops.push_back(std::move(pts));
        }
    }
    return !loops.empty();
}

} // namespace materializr
