#include "ui/UiTheme.h"
#include "InteractiveOpController.h"
#include "core/BodyChanges.h"
#include "touch_mode.h"
#include "../core/Document.h"
#include "../core/History.h"
#include "../core/SelectionManager.h"
#include "../core/Operation.h"
#include <imgui.h>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>

namespace materializr {

bool InteractiveOpController::begin(const IopContext& ctx) {
    // A job from a previous gesture on the same body with the same scalars
    // would carry the same key and land on this gesture: never inherit one.
    m_dispatch.reset();
    m_job.abandon();
    int body = onBegin(ctx);
    if (body == -1) return false;   // refused
    if (previewModel() == PreviewModel::HistoryEdit) {
        // No document snapshot here: the controller supplied its own pre-state
        // (the edited op's), and the whole-document guard is
        // HistoryEditPreview's job. Everything else is the controller's.
        m_bodyId = body;
        m_active = true;
        m_commitRequested = false;
        update(ctx);
        return true;
    }
    if (previewModel() == PreviewModel::LiveOp) {
        // Nothing to snapshot: the live instance's own undo() is the restore
        // path, and the target may not exist yet (a free-space extrude mints
        // its body). kNoTargetBody means "started, no body of my own".
        m_bodyId = (body == kNoTargetBody) ? -1 : body;
        m_active = true;
        m_commitRequested = false;
        m_liveOp.reset();
        m_liveApplied = false;
        update(ctx);
        return true;
    }
    if (body < 0) return false;
    try {
        m_snapshot = ctx.doc.getBody(body);
    } catch (...) { return false; }
    if (m_snapshot.IsNull()) return false;
    m_bodyId = body;
    m_active = true;
    m_commitRequested = false;
    update(ctx);
    return true;
}

void InteractiveOpController::update(const IopContext& ctx) {
    if (!m_active) return;
    // HistoryEdit controllers override update/commit/cancel outright - the
    // policy is entirely op-specific. Reaching the base here means one forgot.
    if (previewModel() == PreviewModel::HistoryEdit) return;
    if (previewModel() == PreviewModel::LiveOp) { updateLive(ctx); return; } // updateLive tracks its own changes
    if (m_bodyId < 0) return;
    materializr::BodyChangeScope trackBodies(ctx.doc, ctx.markBodyDirty, ctx.markMeshesDirty);
    if (!wantsLivePreview(ctx)) {
        // Live preview suppressed (recomputing it per change would freeze the
        // UI). Keep the snapshot shown and mark preview "ok" so Confirm still
        // computes + pushes the op once. pushOperation re-runs execute() and
        // refuses on failure, so a heavy commit that fails just does nothing.
        ctx.doc.updateBody(m_bodyId, m_snapshot);
        m_previewOk = true;
        return;
    }
    if (previewOffThread()) { updateSnapshotAsync(ctx); return; }
    updateSnapshotInline(ctx);
}

std::string InteractiveOpController::updateSnapshotInline(const IopContext& ctx) {
    // Reset to the snapshot, then run a fresh op against it so the live
    // preview tracks the current values exactly without compounding edits.
    ctx.doc.updateBody(m_bodyId, m_snapshot);
    m_previewOk = false;
    std::string key;
    try {
        std::unique_ptr<Operation> op = buildOp(ctx);
        if (op) key = op->serializeParams();
        if (op && op->execute(ctx.doc)) {
            m_previewOk = true;
        } else {
            ctx.doc.updateBody(m_bodyId, m_snapshot);
        }
    } catch (...) {
        ctx.doc.updateBody(m_bodyId, m_snapshot);
    }
    return key;
}

void InteractiveOpController::updateSnapshotAsync(const IopContext& ctx) {
    if (!m_dispatch.async()) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::string key = updateSnapshotInline(ctx);
        m_dispatch.inlinePreviewTook(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count());
        if (m_dispatch.async() && !key.empty()) {
            // The slow frame's result is on screen: record it as the applied
            // key, or the first async frame at the same value would spend a
            // whole job recomputing what is already shown.
            m_dispatch.launched(key);
            m_dispatch.finished(key);
        }
        return;
    }
    // Async: the body keeps showing the last landed preview (or the snapshot)
    // until a job for the current parameters lands - never restore the
    // snapshot per frame here, or the body would flash back on every change.
    launchSnapshotPreviewIfWanted(ctx);
}

std::string InteractiveOpController::snapshotPreviewKey(const IopContext& ctx) {
    try {
        std::unique_ptr<Operation> op = buildOp(ctx);
        return op ? op->serializeParams() : std::string();
    } catch (...) {
        return std::string();
    }
}

void InteractiveOpController::launchSnapshotPreviewIfWanted(const IopContext& ctx) {
    std::unique_ptr<Operation> op;
    try { op = buildOp(ctx); } catch (...) {}
    if (!op) {
        // Nothing to preview at these parameters (a zero thickness): show
        // the snapshot, and remember that whatever key was on screen is gone.
        ctx.doc.updateBody(m_bodyId, m_snapshot);
        m_previewOk = false;
        m_dispatch.retracted();
        return;
    }
    const std::string key = op->serializeParams();
    if (!m_dispatch.shouldLaunch(key)) return;
    std::unique_ptr<SnapshotPreviewJob> job =
        SnapshotPreviewJob::prepare(m_bodyId, m_snapshot, std::move(op));
    std::shared_ptr<SnapshotPreviewJob> shared = std::move(job);
    if (!shared || !m_job.launch([shared] { return shared->run(); })) {
        // No copy, or no thread: nothing will be previewed at these
        // parameters, so the previous preview must not stay on the body. Not
        // retried until the parameters move.
        ctx.doc.updateBody(m_bodyId, m_snapshot);
        m_previewOk = false;
        m_dispatch.refused(key);
        return;
    }
    m_dispatch.launched(key);
}

void InteractiveOpController::pollPreview(const IopContext& ctx) {
    m_job.reap(); // abandoned jobs finish whether or not a gesture is active
    if (!m_active) {
        // Deactivated without cleanup() (setActive(false), a custom lifecycle):
        // the run must not stay pending, or hasActiveWork renders forever, and
        // the dispatch must not remember it as running, or a controller that
        // reactivates through setActive(true) could never launch again.
        m_job.abandon();
        m_dispatch.reset();
        return;
    }
    if (previewModel() != PreviewModel::SnapshotBody || m_bodyId < 0) return;
    std::optional<SnapshotPreviewResult> result = m_job.take();
    if (!result) return;
    materializr::BodyChangeScope trackBodies(ctx.doc, ctx.markBodyDirty, ctx.markMeshesDirty);
    if (m_dispatch.finished(snapshotPreviewKey(ctx))) {
        if (result->ok) {
            ctx.doc.updateBody(m_bodyId, result->shape);
            m_previewOk = true;
        } else {
            // The op refused these parameters: show the gesture-start state,
            // not the last landed preview at some other value.
            ctx.doc.updateBody(m_bodyId, m_snapshot);
            m_previewOk = false;
        }
        return;
    }
    // The parameters moved while the job ran: that result is stale, ask again.
    launchSnapshotPreviewIfWanted(ctx);
}

// LiveOp preview: ONE instance, toggled against the document. Undo whatever
// it currently has applied, push the new values in, run it again. History is
// not involved until commit - and because it is the same instance every
// frame, any body it creates keeps the same id (the whole point).
void InteractiveOpController::retractLivePreview(const IopContext& ctx) {
    materializr::BodyChangeScope trackBodies(ctx.doc, ctx.markBodyDirty, ctx.markMeshesDirty);
    if (m_liveApplied && m_liveOp) {
        try { m_liveOp->undo(ctx.doc); } catch (...) {}
    }
    m_liveApplied = false;
    m_previewOk = false;
}

void InteractiveOpController::updateLive(const IopContext& ctx) {
    materializr::BodyChangeScope trackBodies(ctx.doc, ctx.markBodyDirty, ctx.markMeshesDirty);
    if (m_liveApplied && m_liveOp) {
        try { m_liveOp->undo(ctx.doc); } catch (...) {}
        m_liveApplied = false;
    }
    if (!m_liveOp) {
        try { m_liveOp = buildOp(ctx); } catch (...) { m_liveOp.reset(); }
    }
    m_previewOk = false;
    if (m_liveOp && wantsLivePreview(ctx)) {
        try {
            if (syncLiveOp(*m_liveOp) && m_liveOp->execute(ctx.doc)) {
                m_liveApplied = true;
                m_previewOk = true;
            }
        } catch (...) { m_liveApplied = false; }
    }
    markPreviewDirty(ctx);
}

void InteractiveOpController::commit(const IopContext& ctx) {
    if (!m_active) return;
    materializr::BodyChangeScope trackBodies(ctx.doc, ctx.markBodyDirty, ctx.markMeshesDirty);
    if (previewModel() == PreviewModel::HistoryEdit) { cleanup(); return; }
    if (previewModel() == PreviewModel::LiveOp) {
        // A different op to record? Undo the preview and push it properly,
        // so History executes it against the un-previewed document.
        std::unique_ptr<Operation> alt;
        try { alt = buildCommitOp(ctx); } catch (...) {}
        if (alt) {
            if (m_liveApplied && m_liveOp) {
                try { m_liveOp->undo(ctx.doc); } catch (...) {}
                m_liveApplied = false;
            }
            m_liveOp.reset();
            // Inline unless the controller asks otherwise. Deferring used to
            // be worthless here: no LiveOp operation reported progress, so a
            // deferred boolean drew no window, offered no Cancel and pumped no
            // events - the same freeze one frame later, plus a frame of the
            // un-previewed body. PushPullOp now drives a progress range, and
            // the un-previewed frame does not arise in the case that defers
            // (a ghosted gesture applied no preview to undo).
            //
            // Still inline on a threaded body, which is the override's job to
            // exclude: History reflows this op beneath the Thread step, and
            // moving the push out from under that hook is what produced a
            // partially re-cut thread before (see ResizeCylindricalController,
            // which overrides wantsDeferredCommit to false for the same
            // reason).
            if (!wantsDeferredCommit(ctx) || !deferCommit(ctx, alt))
                ctx.history.pushOperation(std::move(alt), ctx.doc);
        } else if (m_liveApplied && m_liveOp) {
            // The preview IS the result - record it without re-running it.
            ctx.history.pushExecuted(std::move(m_liveOp));
        }
        // Anything else (nothing applied - a zero-distance gesture) records
        // nothing, which is right: the document is already untouched.
        ctx.selection.clear();
        cleanup();
        return;
    }
    // Roll back the preview; History::pushOperation re-runs the op cleanly
    // against the snapshot.
    ctx.doc.updateBody(m_bodyId, m_snapshot);
    // In an async gesture m_previewOk describes the last LANDED job, which
    // may be for other parameters than the ones being committed (or none has
    // landed yet), so it cannot gate the commit: pushOperation re-runs the op
    // and refuses on failure, which is the same clean no-op cancel() gives.
    if (!m_previewOk && !m_dispatch.async()) {
        cancel(ctx);
        return;
    }
    std::unique_ptr<Operation> op = buildOp(ctx);
    if (op) {
        // An op that turned its live preview off because it is slow (Project
        // Sketch) runs BETWEEN frames with a progress reporter, so the window
        // stays alive and the user can cancel; a cancel makes execute() fail,
        // pushOperation refuses, and the body stays at the snapshot (a clean
        // no-op). Only ops that actually report progress belong here - see the
        // LiveOp branch above for why "slow" alone is not a reason to defer.
        if (!wantsDeferredCommit(ctx) || !deferCommit(ctx, op))
            ctx.history.pushOperation(std::move(op), ctx.doc);
    }
    ctx.selection.clear();
    cleanup();
}

bool InteractiveOpController::deferCommit(const IopContext& ctx,
                                          std::unique_ptr<Operation>& op) {
    if (!op || !ctx.progress || !ctx.deferHeavy) return false;
    op->setProgressReporter(ctx.progress);
    History* hist = &ctx.history;
    Document* doc = &ctx.doc;
    auto markDirty = ctx.markMeshesDirty;
    // The task outlives this controller (a commit tears it down at once), so
    // it may capture nothing owned by `this`.
    auto markBody = ctx.markBodyDirty;
    // The op travels in a shared_ptr because std::function needs a copyable
    // target, and a queued task that never runs (the app quits, the startup
    // restore path clears the queue) must still release the operation and the
    // geometry it holds. Moving out of the held pointer also makes a second
    // invocation a no-op.
    auto held = std::make_shared<std::unique_ptr<Operation>>(std::move(op));
    ctx.deferHeavy([hist, doc, held, markDirty, markBody]() {
        std::unique_ptr<Operation> o = std::move(*held);
        if (!o) return;
        // The scope that tracked this edit closed with the frame that
        // confirmed it, so the task diffs the document itself: a heavy commit
        // is exactly when the project is big enough that re-tessellating every
        // body afterwards hurts.
        materializr::BodySnapshot before;
        if (markBody) before = materializr::snapshotBodies(*doc);
        hist->pushOperation(std::move(o), *doc);
        if (markBody) {
            for (int id : materializr::changedBodies(before, *doc)) markBody(id);
        } else if (markDirty) {
            markDirty();
        }
    });
    return true;
}

void InteractiveOpController::cancel(const IopContext& ctx) {
    materializr::BodyChangeScope trackBodies(ctx.doc, ctx.markBodyDirty, ctx.markMeshesDirty);
    if (previewModel() == PreviewModel::HistoryEdit) { cleanup(); return; }
    if (previewModel() == PreviewModel::LiveOp) {
        if (m_liveApplied && m_liveOp) {
            try { m_liveOp->undo(ctx.doc); } catch (...) {}
        }
    } else if (m_bodyId >= 0 && !m_snapshot.IsNull()) {
        ctx.doc.updateBody(m_bodyId, m_snapshot);
    }
    cleanup();
}

void InteractiveOpController::cleanup() {
    m_active = false;
    m_commitRequested = false;
    m_previewOk = false;
    // A latched handle must not survive the op - the viewport reads this to
    // suppress camera orbit and picking, so leaving it set would wedge both.
    m_draggingHandle = false;
    m_bodyId = -1;
    m_snapshot.Nullify();
    m_liveOp.reset();
    m_liveApplied = false;
    m_dispatch.reset();
    m_job.abandon(); // finishes on its own, reaped by a later poll
    onCleanup();
}

void InteractiveOpController::renderPanel(const IopContext& ctx) {
    if (!m_active) return;

    // Seed the panel's RIGHT edge a fixed margin inside the viewport (pivot 1,0)
    // and let it grow leftward. The panel is AlwaysAutoResize and, on touch, its
    // padded content is wider than panelWidth() - anchoring the LEFT edge by a
    // fixed offset (winWidth - w - 20) let that extra width run off the right
    // edge on the tablet. Right-anchoring keeps it on-screen at any scale.
    //
    // ImGuiCond_Appearing + no NoMove flag: same as the Pattern / Edit-Diameter
    // popups - seed the position on first appearance, then let the user drag the
    // panel somewhere convenient (it otherwise landed over the top-left menu in
    // im-touch with no way to move it - issue #29). Dragging the panel body
    // (there's no title bar) moves it.
    const float w = panelWidth();
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x +
                                       ImGui::GetWindowWidth() - 20.0f,
                                   ImGui::GetWindowPos().y + 50.0f),
                            ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Appearing);
    char id[64];
    std::snprintf(id, sizeof(id), "##iop_%s", title());
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                 ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::TextColored(materializr::accentText(), "%s", title());
    ImGui::Separator();

    bool changed = false;
    panelBody(ctx, changed);
    if (changed) update(ctx);

    ImGui::Spacing();
    bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    bool esc   = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    bool doCommit = m_commitRequested || enter;
    bool doCancel = esc;
    if (!ctx.cornerCommitUi) {
        doCommit = doCommit ||
                   ImGui::Button(materializr::btnConfirm(), ImVec2(120, 0));
        if (!doCommit) {
            ImGui::SameLine();
            doCancel = doCancel ||
                       ImGui::Button(materializr::btnCancel(), ImVec2(120, 0));
        }
    }
    doCancel = doCancel && !doCommit;
    ImGui::End();

    if (doCommit) commit(ctx);
    else if (doCancel) cancel(ctx);
}

} // namespace materializr
