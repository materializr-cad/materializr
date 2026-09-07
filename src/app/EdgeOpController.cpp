#include "ui/LengthField.h"
#include "EdgeOpController.h"
#include "../core/Document.h"
#include "../core/History.h"
#include "../core/NumParse.h"
#include "../core/Operation.h"
#include "../core/SelectionManager.h"
#include "../modeling/ChamferOp.h"
#include "../modeling/FilletOp.h"
#include "../ui/NumField.h"      // btnConfirm / btnCancel
#include "../ui/OpDialogGrip.h"
#include "../ui/StepperRow.h"
#include "../ui/TouchWidgets.h"
#include "../ui/UiTheme.h"       // viewportBanner / accentText
#include "../touch_mode.h"
#include <imgui.h>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"

namespace materializr {

void refreshAllEdgeOpFaces(History& hist, Document& doc) {
    for (int i = 0; i < hist.stepCount(); ++i) {
        const Operation* op = hist.getStep(i);
        if (!op || !op->isEnabled()) continue;
        auto* f = const_cast<FilletOp*>(dynamic_cast<const FilletOp*>(op));
        auto* c = const_cast<ChamferOp*>(dynamic_cast<const ChamferOp*>(op));
        if (!f && !c) continue;
        const int bodyId = f ? f->getBodyId() : c->getBodyId();
        auto refresh = [&](const TopoDS_Shape& s, int id) {
            if (f) f->refreshGeneratedFaces(s, doc.bodyFaceIds(id));
            else   c->refreshGeneratedFaces(s, doc.bodyFaceIds(id));
        };
        // A fillet/chamfer's body may have been DELETED by a later step (e.g. a
        // filleted lid that was then deleted). getBody() throws on a missing
        // id, which — uncaught — aborted the whole app on load ("Fatal error:
        // Body not found: N"). Skip any op whose body is gone.
        TopoDS_Shape own;
        try { own = doc.getBody(bodyId); } catch (...) {}
        if (!own.IsNull()) {
            try { refresh(own, bodyId); } catch (...) {}
        } else {
            // The op's own body was CONSUMED by a downstream boolean — its
            // bevel faces now live on the successor body. Refresh against every
            // current body; refreshGeneratedFaces matches by the op's stable
            // face-lineage ids (exact) or blend geometry, so only the body that
            // actually carried the faces forward updates — the rest are no-ops.
            // Without this a filleted/chamfered body that was later unioned
            // into another lost its history-hover highlight entirely.
            for (int b : doc.getAllBodyIds()) {
                try {
                    TopoDS_Shape bs = doc.getBody(b);
                    if (!bs.IsNull()) refresh(bs, b);
                } catch (...) {}
            }
        }
    }
}

namespace {
// Write a radius/distance into a history op. v2 > 0 sets an asymmetric
// chamfer's second distance; v2 <= 0 = symmetric.
void setEdgeOpParam(const Operation* opRaw, bool isFillet, float v,
                    float v2 = -1.0f) {
    if (!opRaw) return;
    if (isFillet) {
        if (auto* op = const_cast<FilletOp*>(dynamic_cast<const FilletOp*>(opRaw)))
            op->setRadius(static_cast<double>(v));
    } else {
        if (auto* op = const_cast<ChamferOp*>(dynamic_cast<const ChamferOp*>(opRaw))) {
            op->setDistance(static_cast<double>(v));
            op->setDistance2(static_cast<double>(v2));
        }
    }
}
} // namespace

// ─── Handle frame ───────────────────────────────────────────────────────────

void EdgeOpController::computeHandleFrame(bool outwardFromFaces) {
    m_hasHandle = false;
    if (m_edges.empty() || snapshot().IsNull()) return;
    try {
        BRepAdaptor_Curve curve(TopoDS::Edge(m_edges.front()));
        double t = (curve.FirstParameter() + curve.LastParameter()) * 0.5;
        gp_Pnt p; gp_Vec tan;
        curve.D1(t, p, tan);
        m_mid = glm::vec3(p.X(), p.Y(), p.Z());
        if (tan.Magnitude() <= 1e-9) return;
        m_dir = glm::normalize(glm::vec3(tan.X(), tan.Y(), tan.Z()));

        glm::vec3 out(0.0f);
        if (outwardFromFaces) {
            // Outward handle direction = the average of the two adjacent faces'
            // OUTWARD normals at the edge, made perpendicular to the edge. This
            // points the arrow the way the fillet actually grows for BOTH
            // convex (outer) edges AND concave inner corners — e.g. the inside
            // corners of a thin-wall hollow box, where the fillet bulges into
            // the cavity. The old "bbox centre → edge" heuristic was inverted
            // on concave edges (arrow faced out toward the wall).
            try {
                TopTools_IndexedDataMapOfShapeListOfShape efMap;
                TopExp::MapShapesAndAncestors(snapshot(), TopAbs_EDGE,
                                              TopAbs_FACE, efMap);
                const TopoDS_Edge& e0 = TopoDS::Edge(m_edges.front());
                if (efMap.Contains(e0)) {
                    for (const TopoDS_Shape& fs : efMap.FindFromKey(e0)) {
                        BRepGProp_Face gf(TopoDS::Face(fs));
                        Standard_Real u0, u1, v0, v1; gf.Bounds(u0, u1, v0, v1);
                        gp_Pnt fp; gp_Vec fn;
                        gf.Normal(0.5 * (u0 + u1), 0.5 * (v0 + v1), fp, fn);
                        if (fn.Magnitude() > 1e-9) {
                            fn.Normalize();
                            out += glm::vec3(fn.X(), fn.Y(), fn.Z());
                        }
                    }
                }
            } catch (...) {}
        }
        if (glm::length(out) <= 1e-5f) {   // fallback: bbox centre → edge
            Bnd_Box bb; BRepBndLib::Add(snapshot(), bb);
            if (!bb.IsVoid()) {
                double x1, y1, z1, x2, y2, z2; bb.Get(x1, y1, z1, x2, y2, z2);
                glm::vec3 c((x1 + x2) * 0.5f, (y1 + y2) * 0.5f, (z1 + z2) * 0.5f);
                out = m_mid - c;
            }
        }
        out -= glm::dot(out, m_dir) * m_dir;   // perpendicular to the edge
        if (glm::length(out) > 1e-5f) m_outDir = glm::normalize(out);
        m_hasHandle = true;
    } catch (...) {}
}

void EdgeOpController::computeFaceDirs() {
    // The two faces meeting at the first edge each get one chamfer setback.
    // For the drag arrows we want a direction lying in each face, perpendicular
    // to the edge, pointing away from the edge into the face.
    m_hasFaceDirs = false;
    m_canTwoDist = false;
    if (m_edges.empty() || snapshot().IsNull()) return;
    try {
        std::vector<TopoDS_Edge> typedEdges;
        for (const auto& e : m_edges) typedEdges.push_back(TopoDS::Edge(e));

        // Distance-1 reference face must match ChamferOp::execute. For a single
        // edge that's one of its two faces; for multiple edges it's the face
        // they ALL share (a planar edge loop). No shared face → no two-distance.
        TopoDS_Face faceA = ChamferOp::sharedReferenceFace(snapshot(), typedEdges);
        if (faceA.IsNull()) return;

        // Face B = the other face adjacent to the first edge.
        TopTools_IndexedDataMapOfShapeListOfShape edgeFaceMap;
        TopExp::MapShapesAndAncestors(snapshot(), TopAbs_EDGE, TopAbs_FACE,
                                      edgeFaceMap);
        TopoDS_Edge e0 = typedEdges.front();
        if (!edgeFaceMap.Contains(e0)) return;
        TopoDS_Shape faceB;
        for (const TopoDS_Shape& f : edgeFaceMap.FindFromKey(e0))
            if (!f.IsSame(faceA)) { faceB = f; break; }
        if (faceB.IsNull()) return;

        auto inFaceDir = [&](const TopoDS_Shape& fshape) -> glm::vec3 {
            // Centroid heuristic (perp-to-edge component of centroid − edge
            // mid) — kept only as the last-resort fallback. It points the WRONG
            // way whenever the face wraps around other features and its
            // centroid lands on the far side of the edge (the light cover's
            // shelf face flipped the yellow A-arrow, #57).
            auto centroidDir = [&]() -> glm::vec3 {
                GProp_GProps props;
                BRepGProp::SurfaceProperties(fshape, props);
                gp_Pnt cm = props.CentreOfMass();
                glm::vec3 c(cm.X(), cm.Y(), cm.Z());
                glm::vec3 d = c - m_mid;
                d -= glm::dot(d, m_dir) * m_dir;
                return (glm::length(d) > 1e-6f) ? glm::normalize(d) : m_outDir;
            };
            // Robust path (same scheme as BlendCut::analyzeEdge): the in-face
            // direction is ±(normal × edge-tangent); pick the sign by MAJORITY
            // of classifier probes sampled along the edge, so a hole under one
            // sample can't flip the arrow.
            try {
                const TopoDS_Face face = TopoDS::Face(fshape);
                BRepGProp_Face gf(face);
                Standard_Real u0, u1, v0, v1;
                gf.Bounds(u0, u1, v0, v1);
                gp_Pnt fp;
                gp_Vec nv;
                gf.Normal((u0 + u1) * 0.5, (v0 + v1) * 0.5, fp, nv);
                if (nv.Magnitude() < 1e-12) return centroidDir();
                gp_Dir n(nv);
                gp_Dir t(m_dir.x, m_dir.y, m_dir.z);
                gp_Dir cand = n.Crossed(t);
                BRepAdaptor_Curve cu(e0);
                auto votes = [&](const gp_Dir& d) {
                    int hit = 0;
                    const int N = 9;
                    for (int i = 0; i < N; ++i) {
                        const double u = (i + 0.5) / N;
                        gp_Pnt base = cu.Value(
                            cu.FirstParameter() +
                            (cu.LastParameter() - cu.FirstParameter()) * u);
                        for (double eps : {0.2, 0.05}) {
                            BRepClass_FaceClassifier cls(
                                face, base.Translated(gp_Vec(d) * eps), 1e-6);
                            if (cls.State() == TopAbs_IN ||
                                cls.State() == TopAbs_ON) { ++hit; break; }
                        }
                    }
                    return hit;
                };
                const int plus = votes(cand);
                const int minus = votes(cand.Reversed());
                if (plus == 0 && minus == 0) return centroidDir();
                gp_Dir best = (plus >= minus) ? cand : cand.Reversed();
                return glm::normalize(glm::vec3(best.X(), best.Y(), best.Z()));
            } catch (...) {
                return centroidDir();
            }
        };
        m_faceDirA = inFaceDir(faceA);
        m_faceDirB = inFaceDir(faceB);
        m_hasFaceDirs = true;
        m_canTwoDist = true;
    } catch (...) {
        m_hasFaceDirs = false;
        m_canTwoDist = false;
    }
}

// ─── Begin ──────────────────────────────────────────────────────────────────

bool EdgeOpController::beginEdgeOp(const IopContext& ctx, EdgeOpKind kind) {
    if (ctx.refuseMesh &&
        ctx.refuseMesh(kind == EdgeOpKind::Fillet ? "Fillet" : "Chamfer"))
        return false;

    int bodyId = -1;
    std::vector<TopoDS_Shape> edges;
    for (const auto& entry : ctx.selection.getSelection()) {
        if (entry.type == SelectionType::Edge && !entry.shape.IsNull()) {
            if (bodyId < 0) bodyId = entry.bodyId;
            if (entry.bodyId == bodyId) edges.push_back(entry.shape);
        }
    }
    if (bodyId < 0 || edges.empty()) return false;

    m_kind = kind;
    m_edges = std::move(edges);
    m_editingIndex = -1;      // creating new
    m_pendingBody = bodyId;
    return begin(ctx);
}

bool EdgeOpController::beginEdgeOpEdit(const IopContext& ctx, int historyIndex,
                                       int pickedBodyId) {
    const Operation* opRaw = ctx.history.getStep(historyIndex);
    if (!opRaw) return false;

    // Pull parameters from the existing op. dynamic_cast picks the right
    // sub-type; nothing else in history uses ownsFace + this typeId, so the
    // toolbar's filter is the only thing that should reach here.
    const FilletOp*  filletOp  = nullptr;
    const ChamferOp* chamferOp = nullptr;
    if (opRaw->kind() == Operation::Kind::Fillet)
        filletOp = dynamic_cast<const FilletOp*>(opRaw);
    else if (opRaw->kind() == Operation::Kind::Chamfer)
        chamferOp = dynamic_cast<const ChamferOp*>(opRaw);
    if (!filletOp && !chamferOp) return false;

    m_edges.clear();
    TopoDS_Shape preShape;
    int bodyId;
    if (filletOp) {
        m_kind = EdgeOpKind::Fillet;
        bodyId = filletOp->getBodyId();
        for (const auto& e : filletOp->getEdges()) m_edges.push_back(e);
        m_value = static_cast<float>(filletOp->getRadius());
        preShape = filletOp->getPreviousShape();
    } else {
        m_kind = EdgeOpKind::Chamfer;
        bodyId = chamferOp->getBodyId();
        for (const auto& e : chamferOp->getEdges()) m_edges.push_back(e);
        m_value = static_cast<float>(chamferOp->getDistance());
        m_twoDist = chamferOp->isAsymmetric();
        m_value2 = m_twoDist ? static_cast<float>(chamferOp->getDistance2())
                             : m_value;
        preShape = chamferOp->getPreviousShape();
    }
    if (bodyId < 0 || m_edges.empty() || preShape.IsNull()) return false;

    m_editingIndex = historyIndex;
    m_pickedBodyId = pickedBodyId;
    m_pendingBody = bodyId;
    m_editPreShape = preShape;
    return begin(ctx);
}

int EdgeOpController::onBegin(const IopContext& ctx) {
    const bool editing = m_editingIndex >= 0;

    m_grab = -1;
    m_dragging = false;
    m_inputFocus = true;
    if (!editing) {
        m_value = 0.0f;   // start at no change; drag the arrow or type a value
        m_twoDist = false;
        m_value2 = 0.0f;
    }
    materializr::formatLengthDigits(m_inputBuf, sizeof(m_inputBuf), m_value);
    materializr::formatLengthDigits(m_inputBuf2, sizeof(m_inputBuf2), m_value2);

    // Install the pre-state BEFORE computing the handle frame, which reads it.
    // CREATE: the current body (the base re-installs the same shape after
    // onBegin returns). EDIT: the edited op's own previous shape.
    if (editing) {
        setSnapshot(m_editPreShape);
    } else {
        try { setSnapshot(ctx.doc.getBody(m_pendingBody)); }
        catch (...) { return -1; }
        if (snapshot().IsNull()) return -1;
    }
    // The edit path keeps the historical bbox-centre heuristic for its arrow;
    // create uses the adjacent-faces average, which is also right on concave
    // edges.
    computeHandleFrame(/*outwardFromFaces=*/!editing);
    if (m_kind == EdgeOpKind::Chamfer) computeFaceDirs();
    else m_hasFaceDirs = false;

    if (editing) {
        m_origValue  = m_value;   // restored on cancel
        m_origValue2 = m_value2;
        // Snapshot the WHOLE document + every op's edit state BEFORE the first
        // preview replay — see HistoryEditPreview for why both halves matter.
        m_editPreview.begin(ctx.doc, ctx.history);
        // Clear the face selection so the gizmo / overlay rendering doesn't
        // fight a stale "Face Operations" panel while editing.
        ctx.selection.clear();
        // The picked body's geometry NOW, before any preview: commit compares
        // against this to spot a frozen op. Measuring it at commit instead
        // would compare "new radius" against "new radius" — the preview has
        // already moved the body — and always report "unchanged".
        m_prePickedVol = m_prePickedArea = 0.0;
        if (m_pickedBodyId >= 0) {
            try {
                TopoDS_Shape s = ctx.doc.getBody(m_pickedBodyId);
                if (!s.IsNull()) {
                    GProp_GProps gv, ga;
                    BRepGProp::VolumeProperties(s, gv);
                    BRepGProp::SurfaceProperties(s, ga);
                    m_prePickedVol  = gv.Mass();
                    m_prePickedArea = ga.Mass();
                }
            } catch (...) {}
        }
    }
    return m_pendingBody;
}

// ─── Preview ────────────────────────────────────────────────────────────────

void EdgeOpController::update(const IopContext& ctx) {
    if (!active()) return;
    if (m_editingIndex < 0) {
        // CREATE: the base's snapshot-restore-execute is exactly right.
        InteractiveOpController::update(ctx);
        return;
    }
    // EDIT: preview through the real history replay so downstream steps (a
    // chamfer stacked on this fillet) stay visible during the drag instead of
    // flickering out. Geometrically impossible values are rejected inside
    // editStep (the op snaps back to its last good parameters), so the preview
    // can never strand the model.
    if (m_value < 0.01f) return;   // don't preview "remove" mid-drag
    std::map<int, TopoDS_Shape> before;
    for (int id : ctx.doc.getAllBodyIds()) {
        try { before[id] = ctx.doc.getBody(id); } catch (...) {}
    }
    writeEditedParams(ctx, m_value, m_twoDist ? m_value2 : -1.0f);
    setPreviewOk(m_editPreview.replay(m_editingIndex, ctx.doc, ctx.history));
    // Partial remesh: re-tessellate only the bodies the replay changed.
    if (ctx.markBodyDirty) {
        std::set<int> now;
        for (int id : ctx.doc.getAllBodyIds()) {
            now.insert(id);
            auto it = before.find(id);
            TopoDS_Shape cur;
            try { cur = ctx.doc.getBody(id); } catch (...) {}
            if (it == before.end() || !it->second.IsEqual(cur))
                ctx.markBodyDirty(id);
        }
        for (auto& [id, s] : before) if (!now.count(id)) ctx.markBodyDirty(id);
    } else {
        ctx.markMeshesDirty();
    }
}

bool EdgeOpController::updateEdgeOp(const IopContext& ctx) {
    update(ctx);
    return previewOk();
}

// Only the one body being filleted changes in CREATE mode. Marking everything
// re-tessellated EVERY visible body per preview frame, which is why the op felt
// heavy with siblings shown and snappy with them hidden.
void EdgeOpController::markPreviewDirty(const IopContext& ctx) const {
    if (ctx.markBodyDirty && bodyId() >= 0) ctx.markBodyDirty(bodyId());
    else ctx.markMeshesDirty();
}

std::unique_ptr<Operation> EdgeOpController::buildOp(const IopContext& ctx) {
    if (m_value < 0.01f) return nullptr;   // no size set = nothing to preview
    std::vector<TopoDS_Edge> typedEdges;
    for (const auto& e : m_edges) typedEdges.push_back(TopoDS::Edge(e));
    if (m_kind == EdgeOpKind::Fillet) {
        auto op = std::make_unique<FilletOp>();
        op->setBody(bodyId());
        op->setEdges(typedEdges);
        op->setRadius(static_cast<double>(m_value));
        // Generative anchoring: tell the fillet which sketch drives this body
        // so a filleted corner can follow a later dimension edit. Inert unless
        // every filleted edge is a corner over a sketch vertex.
        if (ctx.sketchForBody) {
            int sid = ctx.sketchForBody(bodyId());
            if (sid >= 0) op->setSourceSketch(sid);
        }
        return op;
    }
    auto op = std::make_unique<ChamferOp>();
    op->setBody(bodyId());
    op->setEdges(typedEdges);
    op->setDistance(static_cast<double>(m_value));
    if (m_twoDist) op->setDistance2(static_cast<double>(m_value2));
    if (ctx.sketchForBody) {
        int sid = ctx.sketchForBody(bodyId());
        if (sid >= 0) op->setSourceSketch(sid);
    }
    return op;
}

void EdgeOpController::writeEditedParams(const IopContext& ctx, float v,
                                         float v2) const {
    setEdgeOpParam(ctx.history.getStep(m_editingIndex),
                   m_kind == EdgeOpKind::Fillet, v, v2);
}

// ─── Commit / cancel ────────────────────────────────────────────────────────

void EdgeOpController::finish(const IopContext& ctx) {
    m_editPreview.clear();
    ctx.selection.clear();
    ctx.markMeshesDirty();
    // Base teardown clears active/dragging/snapshot and calls onCleanup(); it
    // deliberately does NOT touch the document, which commit/cancel have
    // already put where it belongs.
    teardown();
}

void EdgeOpController::commit(const IopContext& ctx) {
    if (!active()) return;
    const bool editing = m_editingIndex >= 0;
    const bool isFillet = m_kind == EdgeOpKind::Fillet;

    // CREATE previews a transient op against the snapshot — restore it before
    // pushing the real op. EDIT previews through editStep, so the document
    // already reflects history; clobbering the body here would just be churn.
    if (!editing && bodyId() >= 0 && !snapshot().IsNull())
        ctx.doc.updateBody(bodyId(), snapshot());

    // Confirming with no size set is a no-op — cancel out. In EDIT mode a zero
    // value would mean "remove this fillet" — surprising semantics, so treat it
    // as cancel too: restore the ORIGINAL parameter (the live preview mutates
    // the real op) and replay.
    if (m_value < 0.01f) {
        if (editing) {
            writeEditedParams(ctx, m_origValue,
                              m_twoDist ? m_origValue2 : -1.0f);
            m_editPreview.replay(m_editingIndex, ctx.doc, ctx.history);
            refreshAllEdgeOpFaces(ctx.history, ctx.doc);
        }
        finish(ctx);
        return;
    }

    if (editing) {
        // Update the existing op's parameter and rerun from that point so any
        // downstream ops (cuts, fillets stacked on this one, …) recompute too.
        writeEditedParams(ctx, m_value, m_twoDist ? m_value2 : -1.0f);
        if (!m_editPreview.replay(m_editingIndex, ctx.doc, ctx.history)) {
            // The step couldn't rebuild on the current body (its edges
            // reference geometry a later feature consumed — the classic case
            // for a chamfer/fillet originally applied BEFORE those features).
            // replay() already restored the pre-edit snapshot; put the op's
            // parameter back and tell the user the honest remedy.
            writeEditedParams(ctx, m_origValue,
                              m_twoDist ? m_origValue2 : -1.0f);
            refreshAllEdgeOpFaces(ctx.history, ctx.doc);
            if (ctx.toast)
                ctx.toast(std::string(isFillet ? "This fillet" : "This chamfer")
                              .append(" can't be rebuilt on the current body "
                                      "\xE2\x80\x94 its edges reference geometry "
                                      "that a later feature changed. Left as-is; "
                                      "delete it and re-apply the feature on the "
                                      "updated body.").c_str());
            finish(ctx);
            return;
        }
        // Refresh face→op mapping after the edit so ownsFace() works on the new
        // body positions. The replay re-ran EVERY op's execute(), so every
        // fillet/chamfer (not just the edited one) needs rebinding — otherwise
        // the others' faces stay at their pre-Transform positions and become
        // un-clickable until the next reload.
        refreshAllEdgeOpFaces(ctx.history, ctx.doc);

        // Detect a frozen op: the clicked body's geometry matches what was
        // measured at begin — before any preview ran. If the commit didn't
        // change the body at all from its original pre-edit state, the op
        // likely drives a different/deleted body (save-corruption edge case).
        if (m_pickedBodyId >= 0 &&
            (m_prePickedVol != 0.0 || m_prePickedArea != 0.0)) {
            double volAfter = 0, areaAfter = 0;
            try {
                TopoDS_Shape s = ctx.doc.getBody(m_pickedBodyId);
                if (!s.IsNull()) {
                    GProp_GProps gv, ga;
                    BRepGProp::VolumeProperties(s, gv);  volAfter  = gv.Mass();
                    BRepGProp::SurfaceProperties(s, ga); areaAfter = ga.Mass();
                }
            } catch (...) {}
            const double vtol = 1e-6 * std::max(1.0, std::fabs(m_prePickedVol));
            const double atol = 1e-6 * std::max(1.0, std::fabs(m_prePickedArea));
            if (std::fabs(volAfter  - m_prePickedVol)  <= vtol &&
                std::fabs(areaAfter - m_prePickedArea) <= atol && ctx.toast) {
                ctx.toast("This fillet/chamfer is baked into the model "
                          "\xE2\x80\x94 the geometry you clicked has no editable "
                          "operation behind it. Re-apply it to make it "
                          "adjustable.");
            }
        }
        std::fprintf(stdout, "%s edited to %.1f mm\n",
                     isFillet ? "Fillet" : "Chamfer", m_value);
        finish(ctx);
        return;
    }

    std::unique_ptr<Operation> op = buildOp(ctx);
    const bool committed = op && ctx.history.pushOperation(std::move(op), ctx.doc);
    if (committed) {
        std::fprintf(stdout, "%s %.1f mm committed\n",
                     isFillet ? "Fillet" : "Chamfer", m_value);
    } else if (ctx.toast) {
        // execute() rejected the result (invalid topology / unbuildable at this
        // size) and left the body untouched — say so instead of silently doing
        // nothing.
        ctx.toast(std::string(isFillet ? "Fillet" : "Chamfer")
                      .append(" couldn't be built on those edges \xE2\x80\x94 the "
                              "result wasn't valid geometry. Try a smaller size "
                              "or fewer edges.").c_str());
    }
    finish(ctx);
}

void EdgeOpController::cancel(const IopContext& ctx) {
    if (!active()) { finish(ctx); return; }
    if (m_editingIndex >= 0) {
        // The live preview mutated the real op — restore the parameter it had
        // when the edit began, then replay so the committed state (including
        // downstream ops) returns. Replaying at the original value can itself
        // fail for a step that no longer rebuilds; replay() falls back to the
        // pre-edit snapshot, so cancelling never strands the model.
        writeEditedParams(ctx, m_origValue, m_twoDist ? m_origValue2 : -1.0f);
        m_editPreview.replay(m_editingIndex, ctx.doc, ctx.history);
    } else if (bodyId() >= 0 && !snapshot().IsNull()) {
        ctx.doc.updateBody(bodyId(), snapshot());
    }
    refreshAllEdgeOpFaces(ctx.history, ctx.doc);   // body replayed — rebind
    finish(ctx);
}

// ─── Viewport handle ────────────────────────────────────────────────────────

// Claim on left-down when the cursor is within ~12 px of the visible arrow
// line, then drag along it. Without the click claim, trackpad-mode left-orbit
// grabbed the drag-threshold frame and the arrows felt dead; with it, dragging
// from empty space orbits the camera instead of yanking the value. The minimum
// visible arrow length (1 mm single / 0.6 mm per chamfer arrow) keeps the hit
// area reachable even at value 0.
void EdgeOpController::onViewportInput(const IopViewport& vp,
                                       const IopContext& ctx) {
    if (!active() || !m_hasHandle) return;

    if (vp.clicked) {
        auto distToSeg = [&](glm::vec2 a, glm::vec2 b) {
            glm::vec2 d = b - a;
            float len2 = glm::dot(d, d);
            glm::vec2 q;
            if (len2 < 1e-6f) q = vp.mouse - a;
            else {
                float t = glm::clamp(glm::dot(vp.mouse - a, d) / len2, 0.0f, 1.0f);
                q = vp.mouse - (a + t * d);
            }
            return glm::length(q);
        };
        glm::vec2 cs;
        // Hit radius in PHYSICAL pixels: ImGui runs at device resolution
        // here (fonts are 15 * uiScale), so a bare 12 px shrank the grab
        // zone to half the arrow's apparent size on a 2x display (Steve:
        // "the grab zone on the arrow could be a little bigger").
        const float pick = (ctx.panel.imTouch ? 30.0f : 20.0f) *
                           ctx.panel.uiScale;
        if (vp.toScreen(m_mid, cs)) {
            glm::vec2 sa, sb;
            if (m_twoDist && m_hasFaceDirs) {
                glm::vec3 tipA = m_mid + m_faceDirA * std::max(m_value,  0.6f);
                glm::vec3 tipB = m_mid + m_faceDirB * std::max(m_value2, 0.6f);
                if ((vp.toScreen(tipA, sa) && distToSeg(cs, sa) < pick) ||
                    (vp.toScreen(tipB, sb) && distToSeg(cs, sb) < pick))
                    m_dragging = true;
            } else {
                glm::vec3 tip = m_mid + m_outDir * std::max(m_value, 1.0f);
                if (vp.toScreen(tip, sa) && distToSeg(cs, sa) < pick)
                    m_dragging = true;
            }
        }
        setDraggingHandle(m_dragging);
    }

    if (m_dragging && vp.dragging) {
        // Set the value from the cursor's perpendicular distance to the edge,
        // measured on a plane through the edge midpoint facing the camera.
        glm::vec3 camFwd = glm::normalize(vp.camTarget - vp.camPos);
        float denom = glm::dot(vp.rayDir, camFwd);
        if (std::abs(denom) > 1e-6f) {
            float t = glm::dot(m_mid - vp.rayOrigin, camFwd) / denom;
            glm::vec3 hit = vp.rayOrigin + vp.rayDir * t;
            if (m_twoDist && m_hasFaceDirs) {
                // Two arrows: latch the one whose tip is nearest the cursor at
                // drag start, then drag along that face's direction.
                if (m_grab < 0) {
                    auto sd = [&](glm::vec3 dir, float v) -> float {
                        glm::vec2 s;
                        if (!vp.toScreen(m_mid + dir * std::max(v, 0.6f), s))
                            return 1e18f;
                        glm::vec2 d = s - vp.mouse;
                        return glm::dot(d, d);
                    };
                    m_grab = (sd(m_faceDirA, m_value) <= sd(m_faceDirB, m_value2))
                                 ? 0 : 1;
                }
                glm::vec3 dir = (m_grab == 0) ? m_faceDirA : m_faceDirB;
                float proj = glm::dot(hit - m_mid, dir);
                float val = (proj <= 0.0f) ? 0.0f : std::max(0.1f, proj);
                val = std::round(val * 10.0f) / 10.0f;
                if (m_grab == 0) {
                    m_value = val;
                    materializr::formatLengthDigits(m_inputBuf, sizeof(m_inputBuf), val);
                } else {
                    m_value2 = val;
                    materializr::formatLengthDigits(m_inputBuf2, sizeof(m_inputBuf2), val);
                }
            } else {
                // Signed distance along the outward arrow: dragging away from
                // the edge grows the value (>= 0.1 mm); dragging back toward or
                // through the edge returns to 0 (no change).
                float proj = glm::dot(hit - m_mid, m_outDir);
                m_value = (proj <= 0.0f) ? 0.0f : std::max(0.1f, proj);
                // Quantise the drag to the displayed precision (0.1 mm): every
                // readout shows %.1f, so committing the raw float stored
                // "1.9948" behind an on-screen "2.0" — visible later in the
                // Properties editor after a reload.
                m_value = static_cast<float>(materializr::quantiseDragMm(m_value));   // display-unit step, not 0.1 mm
                materializr::formatLengthDigits(m_inputBuf, sizeof(m_inputBuf), m_value);
            }
            update(ctx);
        }
    } else if ((m_dragging || m_grab >= 0) && !vp.down) {
        m_grab = -1;        // re-pick the chamfer arrow on the next claim
        m_dragging = false; // re-claim required for the next drag
        setDraggingHandle(false);
    }
}

// ─── Overlay ────────────────────────────────────────────────────────────────

void EdgeOpController::drawOverlay(const IopOverlay& ov) const {
    if (!active() || !m_hasHandle || !ov.toScreen || !ov.triangle) return;
    const unsigned kOutline = 0xE61C1414u;   // 0xAABBGGRR
    const unsigned kAmber   = 0xFF3CC8FFu;
    const unsigned kBlue    = 0xFFFFD278u;

    // Single-head arrow drawn AT the edge with its tip pointing outward. The
    // arrow line IS the click target (onViewportInput hit-tests this same
    // line), so what you see is what you click. (Steve: arrows should point
    // AWAY from the corner, not be double-ended.)
    auto arrow = [&](glm::vec3 fromW, glm::vec3 toW, unsigned col, bool grabbed) {
        glm::vec2 a, b;
        if (!ov.toScreen(fromW, a) || !ov.toScreen(toW, b)) return;
        glm::vec2 d = b - a;
        float len = glm::length(d);
        if (len < 4.0f) return;
        d /= len;
        glm::vec2 perp(-d.y, d.x);
        const float thick = grabbed ? 4.0f : 3.0f;
        const float ah    = grabbed ? 15.0f : 13.0f;
        ov.line(a, b, kOutline, thick + 2.0f);
        ov.line(a, b, col, thick);
        glm::vec2 base = b - d * ah;
        glm::vec2 w1 = base + perp * (ah * 0.5f);
        glm::vec2 w2 = base - perp * (ah * 0.5f);
        // Slightly oversized halo behind the head.
        glm::vec2 hb = base - d * 1.6f;
        glm::vec2 hw1 = hb + perp * (ah * 0.5f + 1.6f);
        glm::vec2 hw2 = hb - perp * (ah * 0.5f + 1.6f);
        ov.triangle(b + d * 1.6f, hw1, hw2, kOutline);
        ov.triangle(b, w1, w2, col);
    };
    auto plate = [&](glm::vec2 at, const char* txt, unsigned col, float border) {
        if (!ov.rect || !ov.text || !ov.textSize) return;
        glm::vec2 ts = ov.textSize(txt);
        glm::vec2 tp(at.x + 10.0f, at.y - ts.y * 0.5f);
        glm::vec2 mn(tp.x - 5, tp.y - 3), mx(tp.x + ts.x + 5, tp.y + ts.y + 3);
        ov.rect(mn, mx, 0xEB1C1414u, 3.0f, 0.0f);
        if (border > 0.0f) ov.rect(mn, mx, col, 3.0f, border);
        ov.text(tp, txt, col);
    };

    if (m_twoDist && m_hasFaceDirs) {
        auto twoArrow = [&](glm::vec3 dir, float val, const char* tag,
                            unsigned col, bool grabbed) {
            glm::vec3 tipW = m_mid + dir * std::max(val, 0.6f);
            arrow(m_mid, tipW, col, grabbed);
            glm::vec2 sp;
            if (!ov.toScreen(tipW, sp)) return;
            char b[40];
            std::snprintf(b, sizeof(b), "%s %s", tag, materializr::fmtLength(val).c_str());
            plate(sp, b, col, grabbed ? 2.5f : 1.5f);
        };
        twoArrow(m_faceDirA, m_value,  "A", kAmber, m_grab == 0);
        twoArrow(m_faceDirB, m_value2, "B", kBlue,  m_grab == 1);
        return;
    }

    // Minimum 1 mm visible even at value 0 so the handle can be seen and
    // clicked BEFORE any value is set.
    arrow(m_mid, m_mid + m_outDir * std::max(m_value, 1.0f), kAmber, false);
    char dbuf[40];
    std::snprintf(dbuf, sizeof(dbuf), "%s", materializr::fmtLength(m_value).c_str());
    // The single-arrow readout follows the CURSOR, not the tip.
    plate(glm::vec2(ov.mouse.x + 4.0f, ov.mouse.y), dbuf, kAmber, 1.5f);
}

// ─── Panel ──────────────────────────────────────────────────────────────────

void EdgeOpController::renderEdgeOpPanel(const IopContext& ctx) {
    if (!active()) return;
    const float s = ctx.panel.uiScale;
    const bool imTouch = ctx.panel.imTouch;
    const bool isFillet = m_kind == EdgeOpKind::Fillet;
    const char* opName = isFillet ? "FILLET" : "CHAMFER";
    const char* label  = isFillet ? "Radius (%s)" : "Distance (%s)";

    materializr::viewportBanner(
        ImVec4(0.2f, 1.0f, 0.5f, 1.0f),
        materializr::touchMode()
            ? "%s - Type value or use slider, then Confirm / Cancel."
            : "%s - Type value or use slider. Enter to confirm, Escape to cancel.",
        opName);

    // im-touch: anchor the well next to the edge being rounded/cut (latched
    // midpoint — static while values change, same rule as the sketch fields);
    // other layouts keep the fixed top-right spot.
    bool anchored = false;
    if (imTouch && ctx.panel.anchorValid) {
        const ImVec2 vwp = ImGui::GetWindowPos();
        const float vww = ImGui::GetWindowWidth();
        float ax = std::min(std::max(ctx.panel.anchorX + 24.0f * s, vwp.x + 8.0f),
                            vwp.x + vww - 250.0f * s);
        float ay = std::max(ctx.panel.anchorY + 12.0f * s, vwp.y + 8.0f);
        ImGui::SetNextWindowPos(ImVec2(ax, ay), ImGuiCond_Appearing);
        anchored = true;
    }
    if (!anchored)
        ImGui::SetNextWindowPos(ImVec2(
            std::max(ImGui::GetWindowPos().x + 6.0f,
                     ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 250.0f * s),
            ImGui::GetWindowPos().y + 50), ImGuiCond_Appearing);
    // Pin the width (min == max) so moving the panel can't feed back into the
    // value field's content-avail width and ratchet the window wider.
    ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f * s, 0.0f),
                                        ImVec2(240.0f * s, 100000.0f));
    ImGui::Begin("##EdgeOpInput", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_AlwaysAutoResize);
    opDialogDragGrip(s);

    if (!imTouch) {   // im-touch: just the value well below
        ImGui::Text("%s", label);
        ImGui::Separator();
    }

    if (m_inputFocus) {
        if (!materializr::touchMode())
            ImGui::SetKeyboardFocusHere();  // touch: drag the handle, or tap to type
        m_inputFocus = false;
    }

    bool doCommit = false, doCancel = false;
    if (imTouch) {
        // im-touch: the panel is the value well (+ the chamfer's two-distance
        // controls below) — no header, hint or steppers.
        if (materializr::amountLengthField("edgeAmt", isFillet ? "Radius" : "Distance", &m_value, /*allowSign=*/false, 0.1f, 20.0f)) {
            materializr::formatLengthDigits(m_inputBuf, sizeof(m_inputBuf), m_value);
            update(ctx);
        }
        // touch: raise the keyboard on TAP, not on open (see the Extrude field,
        // issue #22).
        if (materializr::touchMode() && ImGui::IsItemClicked())
            ImGui::SetKeyboardFocusHere(-1);
    } else {
        // The member is the truth; the buffer follows it unless being typed in.
        materializr::reseedLengthBufferIfIdle("##val", m_inputBuf, sizeof(m_inputBuf), m_value);
        if (ImGui::InputText("##val", m_inputBuf, sizeof(m_inputBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            (void)materializr::parseLength(m_inputBuf, m_value);
            update(ctx);
            doCommit = true;
        } else if (materializr::lengthBufferIsActive("##val")) {
            // Only while typing — an idle re-parse wrote the buffer's rounded
            // text back over a more precise member, and reinterpreted the old
            // unit's text after a unit switch.
            float parsed = m_value;
            if (materializr::parseLength(m_inputBuf, parsed) &&
                std::abs(parsed - m_value) > 0.01f && parsed > 0.01f) {
                m_value = parsed;
                update(ctx);
            }
        }
        ImGui::SameLine();
        ImGui::Text("%s", materializr::unitSuffix());
    }

    // Quick-nudge stepper (replaces the slider). Positive-only for a radius /
    // setback; 0 shows the original body mid-preview. Confirming at 0 still
    // cancels — zero fillet = no fillet. Desktop only.
    if (!imTouch &&
        materializr::lengthStepperRow("edgeStep", &m_value,
                                /*allowNegative=*/false, 0.1f, 20.0f)) {
        materializr::formatLengthDigits(m_inputBuf, sizeof(m_inputBuf), m_value);
        update(ctx);
    }

    // Asymmetric chamfer: a second setback along the other face. Offered for
    // any chamfer whose edges share a common face (a single edge always
    // qualifies; a coplanar edge loop does too). Two arrows: A=amber, B=blue.
    if (m_kind == EdgeOpKind::Chamfer && m_canTwoDist) {
        ImGui::Spacing();
        if (ImGui::Checkbox(materializr::tr("Two distances (A / B)"), &m_twoDist)) {
            if (m_twoDist && m_value2 < 0.1f) {
                m_value2 = std::max(0.1f, m_value);   // seed B from A
                materializr::formatLengthDigits(m_inputBuf2, sizeof(m_inputBuf2), m_value2);
            }
            m_grab = -1;
            update(ctx);
        }
        if (m_twoDist) {
            if (!imTouch)
                ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Distance B (other face)"));
            if (imTouch) {
                if (materializr::amountLengthField("edgeAmt2", materializr::tr("Distance B"), &m_value2, /*allowSign=*/false, 0.1f, 20.0f)) {
                    materializr::formatLengthDigits(m_inputBuf2, sizeof(m_inputBuf2), m_value2);
                    update(ctx);
                }
            } else {
                // The member is the truth; the buffer follows it unless being typed in.
                materializr::reseedLengthBufferIfIdle("##val2", m_inputBuf2, sizeof(m_inputBuf2), m_value2);
                if (ImGui::InputText("##val2", m_inputBuf2, sizeof(m_inputBuf2),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    (void)materializr::parseLength(m_inputBuf2, m_value2);
                    update(ctx);
                    doCommit = true;
                } else if (materializr::lengthBufferIsActive("##val2")) {
                    float p2 = m_value2;
                    if (materializr::parseLength(m_inputBuf2, p2) &&
                        std::abs(p2 - m_value2) > 0.01f && p2 > 0.01f) {
                        m_value2 = p2;
                        update(ctx);
                    }
                }
                ImGui::SameLine();
                ImGui::Text("%s", materializr::unitSuffix());
            }
            if (!imTouch &&
                materializr::lengthStepperRow("edgeStep2", &m_value2,
                                        /*allowNegative=*/false, 0.1f, 20.0f)) {
                materializr::formatLengthDigits(m_inputBuf2, sizeof(m_inputBuf2), m_value2);
                update(ctx);
            }
        }
    }

    if (!ctx.cornerCommitUi) {   // im-touch: corner ✓/✗ FABs instead
        ImGui::Spacing();
        if (ImGui::Button(materializr::btnConfirm(), ImVec2(110, 0)))
            doCommit = true;
        ImGui::SameLine();
        if (ImGui::Button(materializr::btnCancel(), ImVec2(110, 0)))
            doCancel = true;
    }
    ImGui::End();
    // Commit/cancel AFTER End() — they tear down state the window is reading.
    if (doCommit) commit(ctx);
    else if (doCancel) cancel(ctx);
}

void EdgeOpController::confirmFromKey(const IopContext& ctx) {
    if (!active()) return;
    (void)materializr::parseLength(m_inputBuf, m_value);
    update(ctx);
    commit(ctx);
}

void EdgeOpController::panelBody(const IopContext&, bool&) {
    // Unused: renderEdgeOpPanel draws the whole thing (renderPanel is
    // overridden silent) because the value well anchors to the viewport.
}

void EdgeOpController::onCleanup() {
    m_kind = EdgeOpKind::None;
    m_edges.clear();
    m_editingIndex = -1;
    m_pickedBodyId = -1;
    m_pendingBody = -1;
    m_editPreShape.Nullify();
    m_hasHandle = false;
    m_hasFaceDirs = false;
    m_canTwoDist = false;
    m_twoDist = false;
    m_grab = -1;
    m_dragging = false;
    m_editPreview.clear();
}

} // namespace materializr
