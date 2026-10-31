// Diagnostic probe for a tester report against a lathe + push/pull-holes
// project: "editing Sketch 2's circles doesn't update / leaves holes blocked,
// Redo of a deleted circle never brings its hole back, a hidden construction
// plane reappears after a Sketch 1 edit". Loads the project, rebuilds history
// the way Application::rebuildHistoryFromProject does, and replays the reported
// edits headlessly (SketchEditOp + the sketch cascade, as the app commits them),
// probing every hole of the push/pull sketch for solid material after each.
//
// Usage: probe_report_holes <project> [holeSketchId=2] [depth=20]

#include "core/Document.h"
#include "core/History.h"
#include "core/Operation.h"
#include "core/SketchCascadeTarget.h"
#include "io/ProjectIO.h"
#include "modeling/OperationFactory.h"
#include "modeling/PushPullOp.h"
#include "modeling/ReplayOp.h"
#include "modeling/Sketch.h"
#include "modeling/SketchEditOp.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <gp_Pln.hxx>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace materializr;

namespace {

int g_holeSketch = 2;
double g_depth = 20.0;

double ms(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Headless mirror of Application::cascadeFromSketchEdit.
void cascade(History& h, Document& doc, int sid) {
    int matched = 0;
    const int earliest = rederiveSketchDrivenOps(h, doc, sid, &matched);
    if (earliest < 0) {
        std::printf("    cascade(sketch %d): %d matched, none re-derivable\n", sid, matched);
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (auto sk = doc.getSketch(sid))
        doc.setCascadeSketchOverride(sid, std::make_shared<Sketch>(*sk));
    bool ok = h.editStep(earliest, doc, /*transactional=*/true);
    std::vector<int> disabled;
    while (!ok && h.lastEditFailStep() >= 0 && disabled.size() < 8) {
        const int bad = h.lastEditFailStep();
        std::printf("    cascade: step %d (%s) failed -> DISABLED, retrying\n",
                    bad + 1, h.getStep(bad)->description().c_str());
        h.setStepEnabled(bad, false, doc);
        disabled.push_back(bad);
        ok = h.editStep(earliest, doc, /*transactional=*/true);
    }
    if (!ok && !disabled.empty())
        for (int i : disabled) h.setStepEnabled(i, true, doc);
    doc.clearCascadeSketchOverrides();
    std::printf("    cascade(sketch %d) from step %d: %s in %.0f ms\n", sid, earliest + 1,
                ok ? "applied" : "REVERTED", ms(t0));
}

// What the app does on an in-sketch commit: SketchEditOp, then the cascade.
void sketchEdit(History& h, Document& doc, int sid, const std::function<void(Sketch&)>& fn) {
    auto live = doc.getSketch(sid);
    auto before = std::make_shared<Sketch>(*live);
    fn(*live);
    auto after = std::make_shared<Sketch>(*live);
    auto op = std::make_unique<SketchEditOp>(live, before, after);
    op->setSketchId(sid);
    h.pushExecuted(std::move(op));
    cascade(h, doc, sid);
}

void undoRedo(History& h, Document& doc, bool undo) {
    const bool ok = undo ? h.undo(doc) : h.redo(doc);
    const int idx = undo ? h.lastUndoneStep() : h.lastRedoneStep();
    std::printf("    %s -> ok=%d step %d\n", undo ? "undo" : "redo", ok, idx + 1);
    if (ok) dispatchUndoRedoCascade(idx, h.getStep(idx), doc, -1,
                                    [&](int sid) { cascade(h, doc, sid); });
}

bool solidAt(const Document& doc, const gp_Pnt& p) {
    for (int id : doc.getAllBodyIds()) {
        TopoDS_Shape b = doc.getBody(id);
        if (b.IsNull()) continue;
        BRepClass3d_SolidClassifier c(b, p, 1e-6);
        if (c.State() == TopAbs_IN) return true;
    }
    return false;
}

// One row per circle of the hole sketch: '#' solid, '.' open, sampled down the
// axis (5 depths) and on a ring at 2/3 radius (8 angles) at mid depth.
void holes(const char* tag, const Document& doc) {
    std::printf("  [%s] bodies:", tag);
    for (int id : doc.getAllBodyIds()) {
        const TopoDS_Shape b = doc.getBody(id);
        GProp_GProps g; BRepGProp::VolumeProperties(b, g);
        int solids = 0, shells = 0, faces = 0;
        for (TopExp_Explorer e(b, TopAbs_SOLID); e.More(); e.Next()) ++solids;
        for (TopExp_Explorer e(b, TopAbs_SHELL); e.More(); e.Next()) ++shells;
        for (TopExp_Explorer e(b, TopAbs_FACE); e.More(); e.Next()) ++faces;
        Bnd_Box bb; BRepBndLib::Add(b, bb);
        double x0, y0, z0, x1, y1, z1; bb.Get(x0, y0, z0, x1, y1, z1);
        std::printf(" #%d vol=%.2f solids=%d shells=%d faces=%d valid=%d bbox=(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f) far=%s",
                    id, g.Mass(), solids, shells, faces, (int)BRepCheck_Analyzer(b).IsValid(),
                    x0, y0, z0, x1, y1, z1, solidAt(doc, gp_Pnt(500, 500, 500)) ? "IN(!)" : "out");
    }
    std::printf("\n");
    auto sk = doc.getSketch(g_holeSketch);
    if (!sk) return;
    const gp_Pln pl = sk->getPlane();
    const gp_Ax3& ax = pl.Position();
    auto world = [&](double u, double v, double d) {
        return ax.Location().XYZ() + u * ax.XDirection().XYZ() +
               v * ax.YDirection().XYZ() - d * ax.Direction().XYZ();
    };
    for (const auto& c : sk->getCircles()) {
        const SketchPoint* ctr = sk->getPoint(c.centerPointId);
        if (!ctr) continue;
        std::string axis, ring;
        for (int k = 1; k <= 5; ++k)
            axis += solidAt(doc, gp_Pnt(world(ctr->pos.x, ctr->pos.y, g_depth * (k - 0.5) / 5))) ? '#' : '.';
        for (int a = 0; a < 8; ++a) {
            const double t = a * M_PI / 4, r = c.radius * 2.0 / 3.0;
            ring += solidAt(doc, gp_Pnt(world(ctr->pos.x + r * std::cos(t), ctr->pos.y + r * std::sin(t),
                                              g_depth / 2))) ? '#' : '.';
        }
        std::printf("    circle %2d @(%7.3f,%7.3f) r=%.2f  axis %s  ring %s%s\n", c.id,
                    ctr->pos.x, ctr->pos.y, c.radius, axis.c_str(), ring.c_str(),
                    (axis + ring).find('#') != std::string::npos ? "   <-- BLOCKED" : "");
    }
}

void planes(const char* tag, const Document& doc) {
    std::printf("  [%s] planes:", tag);
    for (int id : doc.getAllPlaneIds())
        std::printf(" #%d '%s' %s", id, doc.getPlaneName(id).c_str(),
                    doc.isPlaneVisible(id) ? "VISIBLE" : "hidden");
    std::printf("\n");
}

void pushPullParams(const History& h) {
    for (int i = 0; i < h.stepCount(); ++i)
        if (auto* pp = dynamic_cast<const PushPullOp*>(h.getStep(i)))
            std::printf("  step %d push/pull params: %s\n", i + 1, pp->serializeParams().c_str());
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { std::fprintf(stderr, "usage: %s <project> [holeSketchId] [depth]\n", argv[0]); return 2; }
    if (argc > 2) g_holeSketch = std::atoi(argv[2]);
    if (argc > 3) g_depth = std::atof(argv[3]);

    Document doc;
    ProjectHistory loaded;
    auto res = ProjectIO::load(argv[1], doc, &loaded);
    if (!res.success) { std::fprintf(stderr, "load failed: %s\n", res.errorMessage.c_str()); return 1; }

    // History rebuild - same as probe_undo_redo / Application::rebuildHistoryFromProject.
    History hist;
    std::map<int, TopoDS_Shape> running;
    for (const auto& [id, shp] : loaded.initialState) running[id] = shp;
    auto toVec = [](const std::map<int, TopoDS_Shape>& m) {
        ReplayOp::BodyState v; for (const auto& [id, s] : m) v.push_back({id, s}); return v;
    };
    for (size_t i = 0; i < loaded.steps.size(); ++i) {
        const auto& st = loaded.steps[i];
        ReplayOp::BodyState before = toVec(running);
        Operation::ReloadState rs;
        for (const auto& [id, shape] : st.changed) {
            if (running.find(id) == running.end()) {
                rs.created.push_back(id); rs.createdAfter.push_back({id, shape});
            } else {
                rs.modifiedBefore.push_back({id, running[id]});
                rs.modifiedAfter.push_back({id, shape});
            }
        }
        for (int id : st.deleted) {
            auto it = running.find(id);
            if (it != running.end()) rs.deletedBefore.push_back({id, it->second});
        }
        for (const auto& [id, shape] : st.changed) running[id] = shape;
        for (int id : st.deleted) running.erase(id);
        ReplayOp::BodyState after = toVec(running);

        std::unique_ptr<Operation> op;
        if (st.typeId == "sketchedit" && !st.params.empty())
            op = ProjectIO::rehydrateSketchEditOp(st.params, doc);
        if (!op && !st.params.empty()) {
            auto cand = OperationFactory::create(st.typeId);
            if (cand && cand->deserializeParams(st.params) && cand->rehydrateFromReload(rs, doc))
                op = std::move(cand);
        }
        if (!op) {
            if (st.typeId != "sketchedit")
                std::printf("step %zu %s: BAKED (ReplayOp)\n", i + 1, st.typeId.c_str());
            auto r = std::make_unique<ReplayOp>(st.typeId, st.name, st.description,
                                                std::move(before), std::move(after));
            if (!st.params.empty()) r->setStoredParams(st.params);
            op = std::move(r);
        } else if (st.typeId != "sketchedit") {
            std::printf("step %zu %s: editable%s\n", i + 1, st.typeId.c_str(),
                        st.enabled ? "" : " (DISABLED)");
        }
        op->setEnabled(st.enabled);
        hist.pushExecuted(std::move(op));
    }

    if (argc > 4 && std::string(argv[4]) == "--bench") {
        // The Lathe subtract's boolean in isolation: step-32 body minus Sketch 4
        // revolved about Y, under different OCCT options.
        while (hist.canUndo()) hist.undo(doc);
        while (hist.canRedo() && hist.currentStep() + 1 < hist.stepCount() - 1) hist.redo(doc);
        const TopoDS_Shape body = doc.getBody(1);
        auto s4 = doc.getSketch(4);
        auto regs = s4->buildRegions();
        TopoDS_Shape tool = BRepPrimAPI_MakeRevol(regs.at(0).face,
                                                  gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0))).Shape();
        struct V { const char* name; bool par; bool obb; double fuzzy; };
        for (V v : {V{"exact serial", false, false, 0}, V{"exact parallel", true, false, 0},
                    V{"exact par+OBB", true, true, 0}, V{"fuzzy 1e-3 par", true, false, 1e-3},
                    V{"fuzzy 1e-2 serial", false, false, 1e-2}, V{"fuzzy 1e-2 par", true, false, 1e-2},
                    V{"fuzzy 1e-2 par+OBB", true, true, 1e-2}}) {
            const auto t0 = std::chrono::steady_clock::now();
            BRepAlgoAPI_Cut cut;
            TopTools_ListOfShape a, t; a.Append(body); t.Append(tool);
            cut.SetArguments(a); cut.SetTools(t);
            cut.SetRunParallel(v.par); cut.SetUseOBB(v.obb);
            if (v.fuzzy > 0) cut.SetFuzzyValue(v.fuzzy);
            cut.Build();
            const double tb = ms(t0);
            double vol = 0; int valid = 0;
            if (cut.IsDone()) {
                GProp_GProps g; BRepGProp::VolumeProperties(cut.Shape(), g); vol = g.Mass();
                valid = BRepCheck_Analyzer(cut.Shape()).IsValid();
            }
            std::printf("  %-20s %6.0f ms  done=%d valid=%d vol=%.1f\n", v.name, tb,
                        (int)cut.IsDone(), valid, vol);
        }
        return 0;
    }

    if (argc > 4 && std::string(argv[4]) == "--walk") {
        // Undo to the bottom, then redo one step at a time, printing every body.
        while (hist.canUndo()) hist.undo(doc);
        while (hist.canRedo()) {
            const auto tr0 = std::chrono::steady_clock::now();
            const bool ok = hist.redo(doc);
            const double tStep = ms(tr0);
            const int i = hist.lastRedoneStep();
            const Operation* op = hist.getStep(i);
            if (op && op->typeId() == "sketchedit") continue;
            char tag[160];
            std::snprintf(tag, sizeof tag, "step %d %s ok=%d en=%d %.0fms", i + 1,
                          op ? op->description().c_str() : "?", ok, op ? op->isEnabled() : 0, tStep);
            std::printf("  [%s] bodies:", tag);
            for (int id : doc.getAllBodyIds()) {
                const TopoDS_Shape b = doc.getBody(id);
                GProp_GProps g; BRepGProp::VolumeProperties(b, g);
                int faces = 0;
                for (TopExp_Explorer e(b, TopAbs_FACE); e.More(); e.Next()) ++faces;
                std::printf(" #%d vol=%.1f faces=%d valid=%d", id, g.Mass(), faces,
                            (int)BRepCheck_Analyzer(b).IsValid());
            }
            if (i >= 0 && i < (int)loaded.steps.size())
                for (const auto& [id, b] : loaded.steps[i].changed) {
                    GProp_GProps g; BRepGProp::VolumeProperties(b, g);
                    int faces = 0;
                    for (TopExp_Explorer e(b, TopAbs_FACE); e.More(); e.Next()) ++faces;
                    std::printf("   | SAVED #%d vol=%.1f faces=%d valid=%d", id, g.Mass(), faces,
                                (int)BRepCheck_Analyzer(b).IsValid());
                }
            std::printf("\n");
            if (op && (op->typeId() == "pushpull" || op->typeId() == "boolean" ||
                       op->typeId() == "revolve"))
                holes(tag, doc);
        }
        return 0;
    }

    if (argc > 4 && std::string(argv[4]) == "--budget") {
        // Mirror Application::cascadeFromSketchEdit with an in-sketch budget:
        // how long until the over-budget replay gives up, and is the model intact?
        const double budget = argc > 5 ? std::atof(argv[5]) : 400.0;
        cascade(hist, doc, g_holeSketch);   // settle once (fixes the saved invalid body)
        holes("before", doc);
        auto sk = doc.getSketch(g_holeSketch);
        int big = -1; double r = -1;
        for (const auto& c : sk->getCircles()) if (c.radius > r) { r = c.radius; big = c.id; }
        sk->setCircleRadius(big, r + 0.5);
        const auto t0 = std::chrono::steady_clock::now();
        auto over = [t0, budget]() { return ms(t0) > budget; };
        int earliest = rederiveSketchDrivenOps(hist, doc, g_holeSketch);
        hist.setEditAbort(over);
        for (int i = earliest; i <= hist.currentStep(); ++i)
            const_cast<Operation*>(hist.getStep(i))->setProgressReporter(
                [over](float, const char*) { return over(); });
        doc.setCascadeSketchOverride(g_holeSketch, std::make_shared<Sketch>(*sk));
        const bool ok = hist.editStep(earliest, doc, /*transactional=*/true);
        doc.clearCascadeSketchOverrides();
        std::printf("  budget %.0f ms: ok=%d aborted=%d after %.0f ms\n", budget, ok,
                    hist.lastEditAborted(), ms(t0));
        holes("after abort (must equal before)", doc);
        hist.setEditAbort({});
        for (int i = 0; i < hist.stepCount(); ++i)
            const_cast<Operation*>(hist.getStep(i))->setProgressReporter({});
        std::printf("  deferred run at Finish:\n");
        cascade(hist, doc, g_holeSketch);
        holes("after Finish", doc);
        return 0;
    }

    std::printf("\n== As loaded ==\n");
    pushPullParams(hist);
    holes("loaded", doc);
    planes("loaded", doc);

    std::printf("\n== Rebuild with no change (re-derive the hole sketch) ==\n");
    cascade(hist, doc, g_holeSketch);
    pushPullParams(hist);
    holes("rebuilt", doc);

    auto hs = doc.getSketch(g_holeSketch);
    if (!hs || hs->getCircles().empty()) return 0;
    // The biggest circle is "the central circle"; the last small one is deleted.
    int bigId = -1, smallId = -1; double bigR = -1;
    for (const auto& c : hs->getCircles()) {
        if (c.radius > bigR) { bigR = c.radius; bigId = c.id; }
    }
    for (const auto& c : hs->getCircles()) if (c.id != bigId) smallId = c.id;

    std::printf("\n== Central circle %d: radius %.2f -> %.2f ==\n", bigId, bigR, bigR + 1.0);
    sketchEdit(hist, doc, g_holeSketch, [&](Sketch& s) { s.setCircleRadius(bigId, bigR + 1.0); });
    holes("bigger centre", doc);

    if (smallId >= 0) {
        std::printf("\n== Delete small circle %d ==\n", smallId);
        sketchEdit(hist, doc, g_holeSketch, [&](Sketch& s) { s.removeElement(smallId); });
        pushPullParams(hist);
        holes("deleted", doc);
        std::printf("\n== Undo the delete (circle comes back) ==\n");
        undoRedo(hist, doc, /*undo=*/true);
        pushPullParams(hist);
        holes("restored", doc);
    }

    std::printf("\n== Hide every plane, then nudge a Sketch 1 point by 1 micron ==\n");
    for (int id : doc.getAllPlaneIds()) doc.setPlaneVisible(id, false);
    planes("hidden", doc);
    if (auto s1 = doc.getSketch(1); s1 && !s1->getPoints().empty()) {
        // A point well off the lathe axis: nudging one ON it across the axis
        // breaks the revolve by itself.
        SketchPoint p = s1->getPoints().front();
        for (const auto& q : s1->getPoints()) if (std::abs(q.pos.x) > 5.0f) { p = q; break; }
        sketchEdit(hist, doc, 1, [&](Sketch& s) { s.movePoint(p.id, p.pos + glm::vec2(0.001f, 0.0f)); });
    }
    planes("after sketch 1 edit", doc);
    holes("after sketch 1 edit", doc);
    return 0;
}
