#include "BooleanOp.h"
#include "../core/UiKeepAlive.h"
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <TopTools_ListOfShape.hxx>
#include <ctime>
#include <algorithm>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include "UnifyTolerance.h"
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <TopExp_Explorer.hxx>
#include <cstdio>
#include <cstdlib>
#include <imgui.h>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"

BooleanOp::BooleanOp() = default;

void BooleanOp::setTargetBodyId(int id) {
    m_targetBodyId = id;
}

void BooleanOp::setToolBodyId(int id) {
    m_toolBodyId = id;
}

void BooleanOp::setMode(BooleanMode mode) {
    m_mode = mode;
}

bool BooleanOp::execute(Document& doc) {
    if (m_targetBodyId < 0 || m_toolBodyId < 0) {
        return false;
    }

    try {
        // Store previous shapes for undo
        m_previousTargetShape = doc.getBody(m_targetBodyId);
        m_previousToolShape = doc.getBody(m_toolBodyId);

        // Run the boolean at a given fuzzy tolerance, returning a VALID result
        // shape or null. Null/degenerate are rejected here so the caller can
        // escalate the fuzzy value instead of committing junk. (IsDone() is
        // necessary but not sufficient - OCCT can report success yet hand back a
        // null or zero-volume compound.)
        // A boolean on pathological contact geometry (a solid sitting
        // skin-tight on another, tangent B-spline walls) can grind for tens of
        // minutes single-threaded -- and this runs on the UI thread, so the
        // whole app reads as hung. Measured on robot dog cover.mzr fusing a
        // lofted skirt: 12+ minutes and counting before the user killed it,
        // and the attempt ladder below would have run FOUR of those.
        //
        // Two containments:
        //   * RunParallel -- OCCT parallelises the intersection stage well.
        //   * a hard time-box per attempt via the progress callback's user
        //     break. An attempt that cannot finish inside the budget is
        //     abandoned and the ladder moves on, so the worst case is bounded
        //     and the op fails cleanly instead of hanging forever.
        struct TimeBox : public Message_ProgressIndicator {
            std::clock_t start; double limit;
            explicit TimeBox(double seconds)
                : start(std::clock()), limit(seconds) {}
            Standard_Boolean UserBreak() override {
                // Also the UI's only breathing room during a long Build() --
                // see the matching note in ExtrudeOp's ExtrudeTimeBox.
                materializr::uiKeepAlive();
                return double(std::clock() - start) / CLOCKS_PER_SEC > limit;
            }
            void Show(const Message_ProgressScope&, const Standard_Boolean) override {
                materializr::uiKeepAlive();
            }
        };
        constexpr double kAttemptSeconds = 45.0;

        auto attempt = [&](double fuzzy) -> TopoDS_Shape {
            TopoDS_Shape s;
            try {
                switch (m_mode) {
                    case BooleanMode::Union: {
                        BRepAlgoAPI_Fuse op;
                        {
                            TopTools_ListOfShape args, tools;
                            args.Append(m_previousTargetShape);
                            tools.Append(m_previousToolShape);
                            op.SetArguments(args);
                            op.SetTools(tools);
                        }
                        op.SetRunParallel(Standard_True);
                        if (fuzzy > 0) op.SetFuzzyValue(fuzzy);
                        Handle(TimeBox) tb = new TimeBox(kAttemptSeconds);
                        op.Build(tb->Start());
                        if (tb->UserBreak())
                            std::fprintf(stderr, "[Boolean] attempt (fuzzy=%.4g) "
                                         "abandoned after %.0f s.\n", fuzzy, kAttemptSeconds);
                        if (!op.IsDone()) return TopoDS_Shape();
                        s = op.Shape();
                        // Publish face lineage from BOTH inputs so "gen" can
                        // name seam faces/edges by their generating faces.
                        m_ledger.capture(op, m_previousTargetShape, TopAbs_FACE);
                        m_ledger.captureAdd(op, m_previousToolShape, TopAbs_FACE);
                        // Merge coplanar/tangent neighbours so the union has no seam.
                        s = materializr::unifySameDomain(s, "Boolean");
                        break;
                    }
                    case BooleanMode::Subtract: {
                        BRepAlgoAPI_Cut op;
                        {
                            TopTools_ListOfShape args, tools;
                            args.Append(m_previousTargetShape);
                            tools.Append(m_previousToolShape);
                            op.SetArguments(args);
                            op.SetTools(tools);
                        }
                        op.SetRunParallel(Standard_True);
                        if (fuzzy > 0) op.SetFuzzyValue(fuzzy);
                        Handle(TimeBox) tb = new TimeBox(kAttemptSeconds);
                        op.Build(tb->Start());
                        if (tb->UserBreak())
                            std::fprintf(stderr, "[Boolean] attempt (fuzzy=%.4g) "
                                         "abandoned after %.0f s.\n", fuzzy, kAttemptSeconds);
                        if (!op.IsDone()) return TopoDS_Shape();
                        s = op.Shape();
                        // Publish face lineage from BOTH inputs so "gen" can
                        // name seam faces/edges by their generating faces.
                        m_ledger.capture(op, m_previousTargetShape, TopAbs_FACE);
                        m_ledger.captureAdd(op, m_previousToolShape, TopAbs_FACE);
                        // Merge coplanar neighbours the cut left behind. This
                        // used to happen for Union only, so a Subtract/Intersect
                        // split a flat face and the seam stayed visible (#81).
                        s = materializr::unifySameDomain(s, "Boolean");
                        break;
                    }
                    case BooleanMode::Intersect: {
                        BRepAlgoAPI_Common op;
                        {
                            TopTools_ListOfShape args, tools;
                            args.Append(m_previousTargetShape);
                            tools.Append(m_previousToolShape);
                            op.SetArguments(args);
                            op.SetTools(tools);
                        }
                        op.SetRunParallel(Standard_True);
                        if (fuzzy > 0) op.SetFuzzyValue(fuzzy);
                        Handle(TimeBox) tb = new TimeBox(kAttemptSeconds);
                        op.Build(tb->Start());
                        if (tb->UserBreak())
                            std::fprintf(stderr, "[Boolean] attempt (fuzzy=%.4g) "
                                         "abandoned after %.0f s.\n", fuzzy, kAttemptSeconds);
                        if (!op.IsDone()) return TopoDS_Shape();
                        s = op.Shape();
                        // Publish face lineage from BOTH inputs so "gen" can
                        // name seam faces/edges by their generating faces.
                        m_ledger.capture(op, m_previousTargetShape, TopAbs_FACE);
                        m_ledger.captureAdd(op, m_previousToolShape, TopAbs_FACE);
                        // Merge coplanar neighbours the cut left behind. This
                        // used to happen for Union only, so a Subtract/Intersect
                        // split a flat face and the seam stayed visible (#81).
                        s = materializr::unifySameDomain(s, "Boolean");
                        break;
                    }
                }
            } catch (...) { return TopoDS_Shape(); }
            if (s.IsNull()) return TopoDS_Shape();
            GProp_GProps gp;
            BRepGProp::VolumeProperties(s, gp);
            if (gp.Mass() < 1e-6) return TopoDS_Shape();
            // A union can never be smaller than its largest input. OCCT can
            // return a VALID solid that silently dropped one operand (seen on
            // a tangent-contact fuse: result 37045 of a 37067 body + 3513
            // tool). Volume is the one invariant that catches it.
            if (m_mode == BooleanMode::Union) {
                GProp_GProps ga, gb;
                BRepGProp::VolumeProperties(m_previousTargetShape, ga);
                BRepGProp::VolumeProperties(m_previousToolShape,  gb);
                const double biggest = std::max(ga.Mass(), gb.Mass());
                if (gp.Mass() < biggest - 1e-4 * std::max(1.0, biggest)) {
                    std::fprintf(stderr, "[Boolean] union came back SMALLER than "
                                 "an input (%.3f < %.3f) -- an operand was lost; "
                                 "rejecting.\n", gp.Mass(), biggest);
                    return TopoDS_Shape();
                }
            }
            // Reject topologically INVALID results (self-intersections, bad
            // faces) - a fuzzy boolean can return a non-null, non-zero-volume
            // shape that's still garbage. Only a valid solid is worth committing;
            // otherwise the caller escalates the fuzzy value or fails cleanly.
            if (!BRepCheck_Analyzer(s).IsValid()) return TopoDS_Shape();
            return s;
        };

        TopoDS_Shape resultShape = attempt(0.0);
        if (resultShape.IsNull()) {
            // Exact booleans fail on near-coincident / overlapping faces (a body
            // sitting flush on another, a thin sliver of overlap). A TINY fuzzy
            // tolerance usually resolves it. Keep it sub-micron-to-micron: larger
            // values (it used to go to 0.1 mm) let OCCT snap distant entities
            // together and visibly distort the model.
            for (double f : {1e-5, 1e-4, 1e-3}) {
                resultShape = attempt(f);
                if (!resultShape.IsNull()) {
                    std::fprintf(stderr, "[Boolean] %s succeeded with fuzzy=%.4g "
                                 "(target=%d tool=%d)\n",
                                 m_mode == BooleanMode::Subtract ? "Cut" :
                                 m_mode == BooleanMode::Union ? "Fuse" : "Common",
                                 f, m_targetBodyId, m_toolBodyId);
                    break;
                }
            }
        }
        if (resultShape.IsNull()) {
            std::fprintf(stderr, "[Boolean] %s failed (target=%d tool=%d) even "
                         "with fuzzy - bodies may not overlap, or the geometry is "
                         "too degenerate.\n",
                         m_mode == BooleanMode::Subtract ? "Cut" :
                         m_mode == BooleanMode::Union ? "Fuse" : "Common",
                         m_targetBodyId, m_toolBodyId);
            return false;
        }

        // Snapshot both inputs' face lineage BEFORE updateBody/removeBody
        // clear them, then propagate through the boolean's FACE ledger - a
        // split face's pieces all inherit its ancestry ids, which is what
        // keeps a chamfer's bevel traceable after a cut crosses it (#51).
        materializr::topo::FaceIdMap inTarget, inTool;
        if (const auto* im = doc.bodyFaceIds(m_targetBodyId)) inTarget = *im;
        if (const auto* im = doc.bodyFaceIds(m_toolBodyId))   inTool   = *im;
        m_prevTargetFaceIds = inTarget;   // undo restores these - a partial
        m_prevToolFaceIds   = inTool;     // replay never re-runs the minters

        // Update target body with the result
        doc.updateBody(m_targetBodyId, resultShape);
        doc.setBodyLedger(m_targetBodyId, &m_ledger);
        {
            materializr::topo::FaceIdMap next = materializr::topo::propagate(
                {{&inTarget, m_previousTargetShape},
                 {&inTool, m_previousToolShape}},
                m_ledger, resultShape);
            // COMPLETE the published map, with STABLE ids: faces with no
            // inherited ancestry (inputs carried no lineage - the common
            // fresh-extrude case) get ids reused across re-executes while
            // the uncovered count is unchanged. Without this, a downstream
            // chamfer/fillet captures pairs against ids this op re-mints
            // differently on every replay, and its lineage tier dies.
            std::vector<TopoDS_Shape> uncovered;
            for (TopExp_Explorer ex(resultShape, TopAbs_FACE); ex.More();
                 ex.Next())
                if (!materializr::topo::idsFor(next, ex.Current()))
                    uncovered.push_back(ex.Current());
            if (m_mintedIds.size() != uncovered.size()) {
                m_mintedIds.clear();
                for (size_t i = 0; i < uncovered.size(); ++i)
                    m_mintedIds.push_back(doc.mintFaceId());
            }
            for (size_t i = 0; i < uncovered.size(); ++i)
                materializr::topo::addId(next, uncovered[i], m_mintedIds[i]);
            doc.setBodyFaceIds(m_targetBodyId, std::move(next));
        }

        // Remove the tool body - unless we're keeping it (the "keep cutters"
        // option, or a cutter still needed by another target).
        if (m_keepTool) {
            m_removedToolId = -1;
        } else {
            doc.removeBody(m_toolBodyId);
            m_removedToolId = m_toolBodyId;
        }

        return true;
    } catch (...) {
        return false;
    }
}

bool BooleanOp::undo(Document& doc) {
    try {
        // Restore target body to previous shape
        if (m_targetBodyId >= 0 && !m_previousTargetShape.IsNull()) {
            doc.updateBody(m_targetBodyId, m_previousTargetShape);
            if (!m_prevTargetFaceIds.empty())
                doc.setBodyFaceIds(m_targetBodyId, m_prevTargetFaceIds);
        }

        // Re-add the tool body that was removed - restore it under its ORIGINAL
        // id (not a fresh addBody id). editStep rolls a boolean back then
        // re-executes the steps above it; an upstream op that targets the tool
        // body (e.g. a fillet on it) must still find it by its old id, and the
        // boolean's own re-execute looks the tool up by m_toolBodyId. putBody
        // also pulls folder/colour/visibility back from the tombstone.
        if (m_removedToolId >= 0 && !m_previousToolShape.IsNull()) {
            doc.putBody(m_toolBodyId, m_previousToolShape, "Boolean Tool (restored)");
            if (!m_prevToolFaceIds.empty())
                doc.setBodyFaceIds(m_toolBodyId, m_prevToolFaceIds);
            m_removedToolId = -1;
        }

        return true;
    } catch (...) {
        return false;
    }
}

std::string BooleanOp::description() const {
    std::string modeStr;
    switch (m_mode) {
        case BooleanMode::Union:     modeStr = "Union"; break;
        case BooleanMode::Subtract:  modeStr = "Subtract"; break;
        case BooleanMode::Intersect: modeStr = "Intersect"; break;
    }
    return "Boolean " + modeStr + " (body " + std::to_string(m_targetBodyId) +
           " with body " + std::to_string(m_toolBodyId) + ")";
}

void BooleanOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Boolean Operation"));
    ImGui::Separator();

    const char* modeItems[] = { "Union", "Subtract", "Intersect" };
    int modeIndex = static_cast<int>(m_mode);
    if (ImGui::Combo(materializr::tr("Mode"), &modeIndex, modeItems, 3)) {
        m_mode = static_cast<BooleanMode>(modeIndex);
    }

    materializr::inputNumberInt("Target Body ID", &m_targetBodyId);
    materializr::inputNumberInt("Tool Body ID", &m_toolBodyId);
}

OperationDiff BooleanOp::captureDiff() const {
    OperationDiff d;
    // The target mutates in place; the tool body is consumed by the boolean
    // unless we kept it (then it isn't deleted, so it's not in the diff).
    if (m_targetBodyId >= 0 && !m_previousTargetShape.IsNull())
        d.modifiedBefore.push_back({m_targetBodyId, m_previousTargetShape});
    if (!m_keepTool && m_toolBodyId >= 0 && !m_previousToolShape.IsNull())
        d.deletedBefore.push_back({m_toolBodyId, m_previousToolShape});
    return d;
}

std::string BooleanOp::serializeParams() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "target=%d;tool=%d;mode=%d;keeptool=%d",
                  m_targetBodyId, m_toolBodyId, static_cast<int>(m_mode),
                  m_keepTool ? 1 : 0);
    return buf;
}

bool BooleanOp::deserializeParams(const std::string& blob) {
    // Tolerant key=value parser (same scheme as FilletOp/ChamferOp).
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "target") { m_targetBodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "tool")   { m_toolBodyId   = std::atoi(val.c_str()); any = true; }
        else if (key == "mode")   { int m = std::atoi(val.c_str());
                                    if (m >= 0 && m <= 2) m_mode = static_cast<BooleanMode>(m);
                                    any = true; }
        else if (key == "keeptool") { m_keepTool = std::atoi(val.c_str()) != 0; any = true; }
        pos = end + 1;
    }
    return any;
}

bool BooleanOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_targetBodyId < 0 || m_toolBodyId < 0) return false;

    // Restore the pre-boolean shapes from the saved step diff: the target was
    // modified in place, the tool was consumed (deleted). Both are needed so
    // undo()/redo() and an editStep replay can roll the boolean back and re-run
    // it against the (possibly edited) upstream geometry.
    m_previousTargetShape.Nullify();
    m_previousToolShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_targetBodyId) { m_previousTargetShape = shp; break; }
    for (const auto& [id, shp] : state.deletedBefore)
        if (id == m_toolBodyId) { m_previousToolShape = shp; break; }
    if (m_previousTargetShape.IsNull()) return false;

    if (m_keepTool) {
        // The tool wasn't consumed, so it isn't in the step's deleted set - it's
        // still a live body. execute() re-fetches it; nothing to restore on undo.
        m_removedToolId = -1;
    } else {
        if (m_previousToolShape.IsNull()) return false;
        // Post-execution bookkeeping: this step consumed the tool body, so undo()
        // knows to restore it.
        m_removedToolId = m_toolBodyId;
    }
    return true;
}
