#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Shape.hxx>
#include <string>
#include <utility>
#include <vector>

// An operation reconstructed from a saved project. It can't re-run the original
// modelling action (the geometric inputs aren't reconstructable), but it stores
// the exact set of bodies before and after the step, so undo/redo reproduce the
// change and the history list survives a reload.
class ReplayOp : public Operation {
public:
    using BodyState = std::vector<std::pair<int, TopoDS_Shape>>;

    ReplayOp(std::string typeId, std::string name, std::string description,
             BodyState before, BodyState after, bool fromReload = true);

    bool execute(Document& doc) override; // redo  -> restore the "after" state
    bool undo(Document& doc) override;     // undo  -> restore the "before" state
    // Diff the before/after snapshots so the project save can persist what
    // this step changed. Without this every snapshot op (revolve-rotate,
    // multi-body transform bundles, reloaded steps on re-save) wrote an EMPTY
    // diff - the step reloaded as a no-op and undoing it silently skipped to
    // the previous step.
    OperationDiff captureDiff() const override;
    std::string name() const override { return m_name; }
    std::string description() const override { return m_description; }
    void renderProperties() override;
    std::string typeId() const override { return m_typeId; }
    // Only reloaded (project-restored) instances should report as such - fresh
    // in-session batch ops (e.g. multi-body Move/Rotate/Scale) use the same
    // snapshot machinery but should not be marked "(reloaded; not editable)".
    bool isReloaded() const override { return m_fromReload; }
    // Warn (amber banner) only when a reloaded step actually shaped a body -
    // it carries before/after body snapshots. A sketch-only reloaded step
    // (e.g. a sketchedit whose params were lost) has EMPTY body states: it's
    // inert history, not a frozen feature, so it must not trigger the warning.
    bool isFrozenFeature() const override {
        return m_fromReload && !(m_before.empty() && m_after.empty());
    }

    // Carry the original op's parameter blob across save/load so a future
    // edit-by-clicking pass can pull radii / distances / etc. out of a
    // reloaded step. Empty when the project file predates the params
    // extension or the op didn't override serializeParams.
    void setStoredParams(std::string blob) { m_storedParams = std::move(blob); }
    const std::string& storedParams() const { return m_storedParams; }
    // Exposed so ProjectIO can write them back out on resave.
    std::string serializeParams() const override { return m_storedParams; }
    bool deserializeParams(const std::string& blob) override {
        m_storedParams = blob; return true;
    }

private:
    // Apply only the body changes between two snapshots (created/modified/
    // deleted), leaving bodies that ride along unchanged alone - so replaying a
    // reloaded step doesn't reset edits made to an upstream step's bodies.
    static void applyDelta(Document& doc, const BodyState& from,
                           const BodyState& to);

    std::string m_typeId, m_name, m_description;
    BodyState m_before, m_after;
    bool m_fromReload = true;
    std::string m_storedParams;
};
