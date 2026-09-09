// Diagnostic probe (experiment/generative-edges): load a real project file,
// rehydrate its ops the way Application::rebuildHistoryFromProject does, then
// report - for every fillet/chamfer - how each selected edge classifies
// against EVERY sketch in the document. This is the ground-truth loop for
// extending EdgeAnchor to cover real multi-sketch bodies.
//
// Usage: probe_anchor_coverage <project.materializr>

#include "core/Document.h"
#include "core/Operation.h"
#include "io/ProjectIO.h"
#include "modeling/FilletOp.h"
#include "modeling/ChamferOp.h"
#include "modeling/EdgeAnchor.h"
#include "modeling/Sketch.h"

#include <BRepAdaptor_Curve.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>
#include <cstdio>
#include <map>
#include <memory>
#include <vector>

using materializr::ProjectHistory;
using materializr::ProjectIO;

static const char* curveTypeName(GeomAbs_CurveType t) {
    switch (t) {
        case GeomAbs_Line: return "line";
        case GeomAbs_Circle: return "circle";
        case GeomAbs_Ellipse: return "ellipse";
        case GeomAbs_BSplineCurve: return "bspline";
        case GeomAbs_BezierCurve: return "bezier";
        default: return "other";
    }
}

// Dump one edge's raw geometry, plus its projection into a sketch frame.
static void dumpEdge(size_t i, const TopoDS_Edge& e) {
    BRepAdaptor_Curve c;
    try { c.Initialize(e); } catch (...) { std::printf("  edge #%zu: <bad>\n", i); return; }
    gp_Pnt p1 = c.Value(c.FirstParameter());
    gp_Pnt p2 = c.Value(c.LastParameter());
    std::printf("  edge #%zu: %-8s (%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)",
                i, curveTypeName(c.GetType()),
                p1.X(), p1.Y(), p1.Z(), p2.X(), p2.Y(), p2.Z());
    if (c.GetType() == GeomAbs_Circle) {
        gp_Circ ci = c.Circle();
        std::printf("  C=(%.2f,%.2f,%.2f) R=%.3f ax=(%.2f,%.2f,%.2f)",
                    ci.Location().X(), ci.Location().Y(), ci.Location().Z(),
                    ci.Radius(),
                    ci.Axis().Direction().X(), ci.Axis().Direction().Y(),
                    ci.Axis().Direction().Z());
    } else if (c.GetType() == GeomAbs_Ellipse) {
        gp_Elips el = c.Ellipse();
        std::printf("  C=(%.2f,%.2f,%.2f) R=%.3f/%.3f ax=(%.2f,%.2f,%.2f)",
                    el.Location().X(), el.Location().Y(), el.Location().Z(),
                    el.MajorRadius(), el.MinorRadius(),
                    el.Axis().Direction().X(), el.Axis().Direction().Y(),
                    el.Axis().Direction().Z());
    }
    std::printf("\n");
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <project>\n", argv[0]); return 2; }

    Document doc;
    ProjectHistory hist;
    auto res = ProjectIO::load(argv[1], doc, &hist);
    if (!res.success) {
        std::fprintf(stderr, "load failed: %s\n", res.errorMessage.c_str());
        return 1;
    }
    std::printf("loaded '%s' (saved by %s), %d bodies, history=%d steps\n",
                argv[1], res.savedByVersion.c_str(), res.bodiesLoaded,
                (int)hist.steps.size());

    // Sketch inventory.
    for (int sid : doc.getAllSketchIds()) {
        auto sk = doc.getSketch(sid);
        if (!sk) continue;
        const gp_Pln& pl = sk->getPlane();
        std::printf("sketch %d '%s': %zu pts %zu lines %zu circles %zu arcs  "
                    "plane O=(%.2f,%.2f,%.2f) N=(%.2f,%.2f,%.2f)\n",
                    sid, doc.getSketchName(sid).c_str(),
                    sk->getPoints().size(), sk->getLines().size(),
                    sk->getCircles().size(), sk->getArcs().size(),
                    pl.Location().X(), pl.Location().Y(), pl.Location().Z(),
                    pl.Axis().Direction().X(), pl.Axis().Direction().Y(),
                    pl.Axis().Direction().Z());
        auto toWorld = [&](double u, double v) {
            gp_Vec w = gp_Vec(pl.XAxis().Direction()) * u +
                       gp_Vec(pl.YAxis().Direction()) * v;
            gp_Pnt p = pl.Location().Translated(w);
            return p;
        };
        for (const auto& A : sk->getArcs()) {
            const auto* pc = sk->getPoint(A.centerPointId);
            if (!pc) continue;
            gp_Pnt w = toWorld(pc->pos.x, pc->pos.y);
            std::printf("  arc id=%d R=%.3f center uv=(%.2f,%.2f) world=(%.2f,%.2f,%.2f)\n",
                        A.id, A.radius, pc->pos.x, pc->pos.y, w.X(), w.Y(), w.Z());
        }
    }

    // Replay the factory path from rebuildHistoryFromProject (simplified: no
    // ReplayOp fallback, no legacy param synthesis - we only care about ops
    // that rehydrate for real).
    std::map<int, TopoDS_Shape> running;
    for (const auto& [id, shape] : hist.initialState) running[id] = shape;

    int stepIdx = -1;
    for (const auto& st : hist.steps) {
        ++stepIdx;
        Operation::ReloadState reload;
        for (const auto& [id, shape] : st.changed) {
            if (running.find(id) == running.end()) reload.created.push_back(id);
            else {
                reload.modifiedBefore.push_back({id, running[id]});
                reload.modifiedAfter.push_back({id, shape});
            }
        }
        for (int id : st.deleted) {
            auto it = running.find(id);
            if (it != running.end()) reload.deletedBefore.push_back({id, it->second});
        }
        for (const auto& [id, shape] : st.changed) running[id] = shape;
        for (int id : st.deleted) running.erase(id);

        if (st.typeId != "fillet" && st.typeId != "chamfer") continue;
        std::printf("\n=== step %d: %s '%s' (%s) ===\n", stepIdx,
                    st.typeId.c_str(), st.name.c_str(), st.description.c_str());
        if (st.params.empty()) { std::printf("  (no params - baked)\n"); continue; }

        std::unique_ptr<Operation> op;
        if (st.typeId == "fillet") op = std::make_unique<FilletOp>();
        else op = std::make_unique<ChamferOp>();
        if (!op->deserializeParams(st.params) ||
            !op->rehydrateFromReload(reload, doc)) {
            std::printf("  (rehydrate FAILED)\n");
            continue;
        }

        std::vector<TopoDS_Edge> edges;
        int bodyId = -1;
        if (auto* f = dynamic_cast<FilletOp*>(op.get())) {
            edges = f->getEdges(); bodyId = f->getBodyId();
        } else if (auto* c = dynamic_cast<ChamferOp*>(op.get())) {
            edges = c->getEdges(); bodyId = c->getBodyId();
        }
        std::printf("  body %d, %zu edges:\n", bodyId, edges.size());
        for (size_t i = 0; i < edges.size(); ++i) dumpEdge(i, edges[i]);

        // Classification against ALL sketches (the real compute() path).
        std::vector<EdgeAnchor::SketchRef> refs;
        for (int sid : doc.getAllSketchIds())
            if (auto sk = doc.getSketch(sid)) refs.push_back({ sid, sk.get() });
        auto anchors = EdgeAnchor::compute(edges, refs);
        int corner = 0, rim = 0, arc = 0, circ = 0, none = 0;
        for (size_t i = 0; i < anchors.size(); ++i) {
            const auto& a = anchors[i];
            const char* k = "-";
            switch (a.kind) {
                case EdgeAnchor::Anchor::Corner: ++corner; k = "Corner"; break;
                case EdgeAnchor::Anchor::Rim:    ++rim;    k = "Rim";    break;
                case EdgeAnchor::Anchor::Arc:    ++arc;    k = "Arc";    break;
                case EdgeAnchor::Anchor::Circle: ++circ;   k = "Circle"; break;
                default: ++none; k = "NONE"; break;
            }
            std::printf("  anchor #%zu: %-6s sketch=%d elem=%d h=%.2f t=%.3f\n",
                        i, k, a.sketchId, a.elemId, a.h, a.t);
        }
        std::printf("  TOTAL: %d corner %d rim %d arc %d circle %d none of %zu\n",
                    corner, rim, arc, circ, none, anchors.size());

        // Round-trip resolve against the op's own pre-shape: every anchor must
        // re-find a distinct edge even with UNCHANGED sketches (sanity floor).
        TopoDS_Shape preShape;
        if (auto* f = dynamic_cast<FilletOp*>(op.get())) preShape = f->getPreviousShape();
        else if (auto* c = dynamic_cast<ChamferOp*>(op.get())) preShape = c->getPreviousShape();
        std::vector<TopoDS_Edge> resolved;
        bool ok = EdgeAnchor::resolve(anchors, refs, preShape, resolved);
        std::printf("  self-resolve: %s (%zu edges)\n", ok ? "OK" : "FAILED",
                    resolved.size());
    }
    return 0;
}
