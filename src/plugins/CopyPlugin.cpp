#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../core/Document.h"
#include "../core/History.h"
#include "../core/SelectionManager.h"
#include "../modeling/CopyOp.h"

#include <algorithm>
#include <vector>

// Duplicate-in-place action shared by the toolbar button and the Ctrl+D
// shortcut handler in Application. Pushes a CopyOp with zero offset, then
// re-selects the JUST-CREATED body so the user can immediately Move it.
// Two identical bodies stacked on top of each other are indistinguishable;
// landing on the new one (rather than leaving both selected) keeps the
// gizmo unambiguous and matches Ctrl+D / "duplicate in place" UX in other
// CAD tools.
namespace materializr { namespace dup_in_place {
inline void run(PluginContext& ctx) {
    // Every selected body is duplicated (one CopyOp each - every step stays a
    // real, replayable op), then ALL the new copies are selected.
    std::vector<int> srcIds;
    for (const auto& e : ctx.selection().getSelection()) {
        if (e.bodyId < 0) continue;
        if (std::find(srcIds.begin(), srcIds.end(), e.bodyId) == srcIds.end())
            srcIds.push_back(e.bodyId);
    }
    if (srcIds.empty()) return;
    std::vector<int> newIds;
    for (int srcId : srcIds) {
        int srcFolder = ctx.document().getBodyFolder(srcId);
        auto op = std::make_unique<CopyOp>();
        op->setSourceBodyId(srcId);
        op->setOffset(0.0, 0.0, 0.0);
        CopyOp* opPtr = op.get(); // op moves into history; raw pointer stays valid
        if (!ctx.history().pushOperation(std::move(op), ctx.document())) continue;
        int newId = opPtr->getCreatedBodyId();
        if (newId < 0) continue;
        // Inherit the source's folder so the duplicate lands beside the
        // original in the Items panel instead of dropping to root.
        if (srcFolder >= 0) ctx.document().setBodyFolder(newId, srcFolder);
        newIds.push_back(newId);
    }
    bool first = true;
    for (int newId : newIds) {
        SelectionEntry entry;
        entry.type = SelectionType::Body;
        entry.bodyId = newId;
        try { entry.shape = ctx.document().getBody(newId); } catch (...) {}
        if (first) { ctx.selection().select(entry); first = false; }
        else       ctx.selection().addToSelection(entry);
    }
    ctx.markMeshesDirty();
}
}} // namespace materializr::dup_in_place

REGISTER_PLUGIN(Copy, [](materializr::PluginContext& ctx) {
    ctx.registerToolbarButton({"Duplicate", "Edit",
        materializr::SelectionContext::HasBodies, 800,
        [](materializr::PluginContext& ctx) {
            materializr::dup_in_place::run(ctx);
        }, nullptr,
        "Add an exact copy of the selected body on top of itself. Use Move "
        "to reposition. Ctrl+D for keyboard."});
})
