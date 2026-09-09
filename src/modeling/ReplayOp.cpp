#include "ReplayOp.h"
#include <imgui.h>
#include <map>
#include <set>
#include "../i18n.h"

ReplayOp::ReplayOp(std::string typeId, std::string name, std::string description,
                   BodyState before, BodyState after, bool fromReload)
    : m_typeId(std::move(typeId))
    , m_name(std::move(name))
    , m_description(std::move(description))
    , m_before(std::move(before))
    , m_after(std::move(after))
    , m_fromReload(fromReload) {}

void ReplayOp::applyDelta(Document& doc, const BodyState& from,
                          const BodyState& to) {
    // Apply ONLY what this step changed between `from` and `to`, leaving every
    // other body untouched. Reloaded steps carry full-document snapshots, but
    // re-applying the whole snapshot resets bodies this step never touched -
    // which silently discarded edits made to an UPSTREAM step (e.g. editing a
    // fillet whose body is later consumed by a union: a baked transform on an
    // unrelated body would re-slam the entire stale scene over the rebuilt
    // result). A body that rides along unchanged (IsEqual) is skipped, so the
    // edited geometry survives.
    std::map<int, const TopoDS_Shape*> fromMap;
    for (const auto& [id, s] : from) fromMap[id] = &s;
    std::set<int> toIds;
    for (const auto& [id, s] : to) {
        toIds.insert(id);
        auto it = fromMap.find(id);
        if (it == fromMap.end() || !s.IsEqual(*it->second))
            doc.putBody(id, s);          // created or genuinely changed
    }
    // Bodies present in `from` but not `to` were removed by this step.
    for (const auto& [id, s] : from)
        if (!toIds.count(id)) doc.removeBody(id);
}

bool ReplayOp::execute(Document& doc) {
    applyDelta(doc, m_before, m_after);
    return true;
}

bool ReplayOp::undo(Document& doc) {
    applyDelta(doc, m_after, m_before);
    return true;
}

OperationDiff ReplayOp::captureDiff() const {
    OperationDiff d;
    std::map<int, const TopoDS_Shape*> before;
    for (const auto& [id, s] : m_before) before[id] = &s;
    std::set<int> afterIds;
    for (const auto& [id, s] : m_after) {
        afterIds.insert(id);
        auto it = before.find(id);
        if (it == before.end()) {
            d.created.push_back(id);
        } else if (!s.IsEqual(*it->second)) {
            // Same body, different shape/placement (e.g. a rotate-body
            // revolve or a batched multi-body transform). Untouched bodies
            // that merely ride along in a full-document snapshot compare
            // IsEqual and are skipped, so the diff stays minimal.
            d.modifiedBefore.push_back({id, *it->second});
        }
    }
    for (const auto& [id, s] : m_before) {
        if (!afterIds.count(id)) d.deletedBefore.push_back({id, s});
    }
    return d;
}

void ReplayOp::renderProperties() {
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", m_description.c_str());
    if (m_fromReload) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", materializr::tr("Loaded from a saved project. Undo/redo work, but the parameters of a reloaded step can't be edited."));
    } else {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", materializr::tr("Batched transform - undo/redo restores the whole set at once."));
    }
}
