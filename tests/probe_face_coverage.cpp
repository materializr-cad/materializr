// Diagnostic probe (experiment/face-anchors): load a real project, and for
// every body classify EVERY face against every sketch in the document, report
// coverage by kind, then self-resolve the anchors against the same body (the
// sanity floor: each anchored face must re-find a distinct face even with the
// sketch unchanged). This is the ground-truth loop for gauging how well
// generative FACE naming covers real geometry - the dual of
// probe_anchor_coverage.
//
// Usage: probe_face_coverage <project.materializr>

#include "core/Document.h"
#include "io/ProjectIO.h"
#include "modeling/FaceAnchor.h"
#include "modeling/Sketch.h"

#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

static const char* surfName(GeomAbs_SurfaceType t) {
    switch (t) {
        case GeomAbs_Plane:            return "plane";
        case GeomAbs_Cylinder:         return "cylinder";
        case GeomAbs_Cone:             return "cone";
        case GeomAbs_Sphere:           return "sphere";
        case GeomAbs_Torus:            return "torus";
        case GeomAbs_BezierSurface:    return "bezier";
        case GeomAbs_BSplineSurface:   return "bspline";
        case GeomAbs_SurfaceOfRevolution: return "revol";
        case GeomAbs_SurfaceOfExtrusion:  return "extr";
        default:                       return "other";
    }
}

using materializr::ProjectHistory;
using materializr::ProjectIO;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <project>\n", argv[0]); return 2; }

    Document doc;
    ProjectHistory hist;
    auto res = ProjectIO::load(argv[1], doc, &hist);
    if (!res.success) {
        std::fprintf(stderr, "load failed: %s\n", res.errorMessage.c_str());
        return 1;
    }
    std::printf("loaded '%s' (saved by %s), %d bodies, %zu sketches\n",
                argv[1], res.savedByVersion.c_str(), res.bodiesLoaded,
                doc.getAllSketchIds().size());

    std::vector<FaceAnchor::SketchRef> refs;
    for (int sid : doc.getAllSketchIds())
        if (auto sk = doc.getSketch(sid)) refs.push_back({ sid, sk.get() });
    if (refs.empty()) { std::printf("no sketches - nothing to attribute\n"); return 0; }

    int grandTot = 0, grandNamed = 0;
    for (int bid : doc.getAllBodyIds()) {
        TopoDS_Shape body;
        try { body = doc.getBody(bid); } catch (...) { continue; }
        if (body.IsNull()) continue;

        std::vector<TopoDS_Face> faces;
        for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next())
            faces.push_back(TopoDS::Face(ex.Current()));

        auto anchors = FaceAnchor::compute(faces, refs);
        int wall = 0, cyl = 0, cwall = 0, cap = 0, none = 0;
        for (const auto& a : anchors)
            switch (a.kind) {
                case FaceAnchor::Anchor::Wall:      ++wall;  break;
                case FaceAnchor::Anchor::Cyl:       ++cyl;   break;
                case FaceAnchor::Anchor::CurveWall: ++cwall; break;
                case FaceAnchor::Anchor::Cap:       ++cap;   break;
                default: ++none; break;
            }
        const int named = wall + cyl + cwall + cap;
        std::printf("\nbody %d '%s': %zu faces  ->  %d wall  %d cyl  %d cwall  %d cap  %d NONE"
                    "  (%d/%zu named)\n",
                    bid, doc.getBodyName(bid).c_str(), faces.size(),
                    wall, cyl, cwall, cap, none, named, faces.size());

        // Categorize the unattributed faces by surface type - this is what
        // separates "add another face kind" (cone/torus) from "needs the
        // general generation-map kernel" (blend/boolean/loft/revolve faces a
        // sketch scheme fundamentally can't name).
        std::map<std::string, int> noneBy;
        for (size_t i = 0; i < anchors.size(); ++i)
            if (anchors[i].kind == FaceAnchor::Anchor::None) {
                GeomAbs_SurfaceType st = GeomAbs_OtherSurface;
                try { st = BRepAdaptor_Surface(faces[i]).GetType(); } catch (...) {}
                noneBy[surfName(st)]++;
            }
        if (!noneBy.empty()) {
            std::printf("    NONE by surface: ");
            for (const auto& [k, n] : noneBy) std::printf("%s=%d ", k.c_str(), n);
            std::printf("\n");
        }

        // Self-resolve: only the named subset (resolve() is all-or-nothing, so
        // feed it a vector with just the attributed faces + their anchors).
        std::vector<FaceAnchor::Anchor> namedAnchors;
        for (const auto& a : anchors)
            if (a.kind != FaceAnchor::Anchor::None) namedAnchors.push_back(a);
        std::vector<TopoDS_Face> resolved;
        bool ok = FaceAnchor::resolve(namedAnchors, refs, body, resolved);
        std::printf("  self-resolve (%d named): %s (%zu faces)\n",
                    (int)namedAnchors.size(), ok ? "OK" : "FAILED",
                    resolved.size());

        grandTot += (int)faces.size();
        grandNamed += named;
    }

    std::printf("\n=== TOTAL: %d/%d faces named (%.0f%%) ===\n",
                grandNamed, grandTot,
                grandTot ? 100.0 * grandNamed / grandTot : 0.0);
    return 0;
}
