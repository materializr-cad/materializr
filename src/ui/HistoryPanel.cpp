#include "UiTheme.h"
#include "HistoryPanel.h"
#include <cctype>
#include "../core/History.h"
#include "../core/Document.h"
#include "../core/Operation.h"
#include "../core/EventBus.h"
#include "../core/Events.h"
#include "../modeling/SketchEditOp.h"
#include "../modeling/SketchTransformOp.h"
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <string>
#include <tuple>
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"

namespace materializr {

HistoryPanel::HistoryPanel() = default;

void HistoryPanel::setHistory(History* history) {
    m_history = history;
}

void HistoryPanel::setDocument(Document* doc) {
    m_document = doc;
}

bool HistoryPanel::render() {
    m_showUndoRedo = true;   // the desktop window always shows its own row
    ImGui::Begin("History", nullptr, ImGuiWindowFlags_NoCollapse);
    const bool modified = renderContent();
    ImGui::End();
    return modified;
}

// Panel body without the window wrapper — see ItemsPanel::renderContent().
bool HistoryPanel::renderContent() {
    bool modified = false;
    m_hoveredStep = -1; // recomputed below from whichever row the cursor is over

    if (!m_history || !m_document) {
        ImGui::TextColored(materializr::dimText(), "%s", materializr::tr("No history available."));
        return false;
    }

    int stepCount = m_history->stepCount();
    int currentStep = m_history->currentStep();
    int breakpoint = m_history->getBreakpoint();

    ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Operation History"));
    if (!m_showUndoRedo) {
        // No bottom button row (the host provides undo/redo) — the step
        // counter rides beside the label instead.
        ImGui::SameLine();
        ImGui::TextColored(materializr::dimText(), "%d/%d",
                           currentStep + 1, stepCount);
    }
    ImGui::Separator();

    // If any step came from a reopened project, explain that those steps replay
    // saved geometry and can't have their parameters re-edited.
    bool anyReloaded = false;
    for (int i = 0; i < stepCount; ++i) {
        const Operation* op = m_history->getStep(i);
        if (op && op->isFrozenFeature()) { anyReloaded = true; break; }
    }
    if (anyReloaded) {
        // Keep the banner to a single sentence so the panel stays tidy; the
        // how-to-fix detail lives in a hover tooltip. (Honest wording: frozen
        // just means the step reloaded without editable parameters — old files
        // are ONE cause, but a step type the reload path can't yet rebuild
        // produces the same state on a brand-new save — a bug to report, not an
        // old file.)
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.0f), "%s", materializr::tr("Amber (frozen) steps reloaded without editable parameters. (hover for more)"));
        ImGui::PopTextWrapPos();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", materializr::tr("Undo/redo still work; to change one, select its feature and\nuse Remove Feature, then redo it.\n(Usual cause: a save from an older version.)"));
        ImGui::Separator();
    }

    // A step that failed to recompute after an upstream edit sits above the
    // current index with its geometry missing from the viewport — say so,
    // and say how to get it back, instead of leaving it silently absent.
    int failedAt = m_history->lastReplayFailure();
    if (failedAt >= 0) {
        const Operation* fop = m_history->getStep(failedAt);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
            materializr::tr("Step %d (%s) couldn't recompute — the geometry it referenced no longer exists after an upstream step was edited or disabled. Re-enable the disabled step, edit an upstream step (it retries automatically), edit this step's parameters, or delete it."),
            failedAt + 1, fop ? fop->name().c_str() : "?");
        ImGui::PopTextWrapPos();
        ImGui::Separator();
    }

    // Step list. When a step's properties editor is open below, shrink the
    // list so the editor + its pinned Apply button stay on-screen instead of
    // overflowing past the panel bottom (previously: type, scroll, THEN
    // apply — every single edit).
    const bool propsOpen =
        m_showProperties && m_editingStep >= 0 && m_editingStep < stepCount &&
        m_history->getStep(m_editingStep) != nullptr;
    // The properties block SIZES TO THE OP'S FIELDS (measured last frame) plus
    // the fixed chrome (separator + header + Apply button). A fixed box made a
    // two-distance chamfer — one field taller than a plain op — scroll. Capped
    // so the step list keeps a usable floor; past the cap the props child
    // scrolls internally (Apply stays pinned) rather than growing further.
    const float kPropsChrome = 66.0f;
    float propsBlockH = 0.0f;
    if (propsOpen) {
        const float panelH = ImGui::GetContentRegionAvail().y;
        const float wanted =
            (m_stepPropsH > 1.0f ? m_stepPropsH : 150.0f) + kPropsChrome;
        const float maxBlock = std::max(
            panelH - 60.0f - ImGui::GetFrameHeightWithSpacing() * 4.0f, 120.0f);
        propsBlockH = std::min(std::max(wanted, 96.0f), maxBlock);
    }
    ImGui::BeginChild("StepList", ImVec2(0, -(60.0f + propsBlockH)), true);

    int deleteIndex = -1; // set by the context menu, applied after the loop

    // Render a single step row (used both for ungrouped steps and the
    // members of an expanded group). Kept as a lambda to avoid duplicating
    // the body when the surrounding iteration loop branches between
    // single-step and group-render paths.
    auto renderOneStep = [&](int i) {
        const Operation* op = m_history->getStep(i);
        if (!op) return;
        // Breakpoint marker before this step
        if (breakpoint >= 0 && i == breakpoint + 1) {
            ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::Separator();
            ImGui::PopStyleColor();
        }
        bool isAboveBreakpoint = (breakpoint >= 0 && i > breakpoint);
        bool isCurrentlyEditing = (i == m_editingStep);
        bool isDisabled = !op->isEnabled();
        bool isAboveCurrent = (i > currentStep);
        bool isFrozen = op->isFrozenFeature(); // baked BODY feature; sketch-only reloads are inert, not flagged

        // Soft highlight: the step owning the viewport-selected sketch element.
        // Orange to match the in-viewport element highlight; editing (blue) wins.
        bool isHighlighted = (i == m_highlightStep) && !isCurrentlyEditing;

        ImGui::PushID(i);
        if (isCurrentlyEditing) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.3f, 0.5f, 1.0f, 0.3f));
        } else if (isHighlighted) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(1.0f, 0.55f, 0.1f, 0.30f));
        }
        // Colour precedence: dim (inactive/disabled) wins so the active row set
        // reads clearly; otherwise a frozen step is amber so it's easy to spot.
        bool pushedText = true;
        if (isAboveBreakpoint || isAboveCurrent || isDisabled) {
            ImGui::PushStyleColor(ImGuiCol_Text, materializr::dimText());
        } else if (isFrozen) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.3f, 1.0f));
        } else {
            pushedText = false;
        }
        // Step label uses the op's description() when available — gives
        // dimension steps a useful caption ("Add Distance 25 mm") instead
        // of the generic name().
        std::string detail = op->description();
        if (detail.empty()) detail = op->name();
        // Descriptions are COMPOSED English ("Boolean Union (body 2 with body
        // 3)") built by 36 different ops, so they cannot be catalogue keys.
        // Translate the leading word run -- the op name -- when the catalogue
        // knows it; the numeric tail stays as-is. Cheap, safe (a miss changes
        // nothing), and it is the op name that carries the meaning.
        {
            std::size_t cut = 0;
            while (cut < detail.size() &&
                   (std::isalpha(static_cast<unsigned char>(detail[cut])) ||
                    detail[cut] == ' ' || detail[cut] == '/' || detail[cut] == '-'))
                ++cut;
            while (cut > 0 && !std::isalpha(static_cast<unsigned char>(detail[cut - 1])))
                --cut;
            if (cut >= 2) {
                const std::string head = detail.substr(0, cut);
                const char* t = materializr::tr(head.c_str());
                if (t != nullptr && head != t) detail = t + detail.substr(cut);
            }
        }
        char label[256];
        std::snprintf(label, sizeof(label), "%d. %s%s%s",
                      i + 1,
                      detail.c_str(),
                      isDisabled ? " [disabled]" : "",
                      isFrozen ? " (frozen)" : "");
        bool selected = (i == m_editingStep) || isHighlighted;
        if (ImGui::Selectable(label, selected)) {
            // Re-clicking the active step toggles it off, clearing the orange
            // viewport highlight (which tracks the editing step) — otherwise
            // there's no way to dismiss it.
            if (i == m_editingStep) {
                m_editingStep = -1;
                m_showProperties = false;
            } else {
                m_editingStep = i;
                m_showProperties = true;
            }
            m_deleteConflict = false;
        }
        if (ImGui::IsItemHovered()) m_hoveredStep = i; // drives the viewport preview
        if (pushedText) {
            ImGui::PopStyleColor();
        }
        if (isCurrentlyEditing || isHighlighted) {
            ImGui::PopStyleColor();
        }
        if (i == m_enableFailStep) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.2f, 1.0f));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 250.0f);
            ImGui::TextWrapped("%s", materializr::tr("Re-enabled, but this step still can't find the geometry it referenced on the current body, so it produces nothing. Delete it and re-apply the feature on the updated body."));
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        if (ImGui::BeginPopupContextItem("StepContextMenu")) {
            if (ImGui::MenuItem(materializr::tr("Edit Parameters"))) {
                m_editingStep = i;
                m_showProperties = true;
            }
            if (ImGui::MenuItem(op->isEnabled() ? "Disable" : "Enable")) {
                // In-place toggle — preserves base bodies the op modifies
                // (replayAll's doc.clear() would delete them).
                const bool enabling = !op->isEnabled();
                const bool okTog =
                    m_history->setStepEnabled(i, enabling, *m_document);
                // Re-enabling a step whose references are gone re-executes,
                // fails, and gets SKIPPED — which read as "does nothing"
                // (#54). Say so, inline under the step.
                m_enableFailStep = (enabling && !okTog) ? i : -1;
                modified = true;
            }
            if (ImGui::MenuItem(materializr::tr("Set Breakpoint Here"))) {
                m_history->setBreakpoint(i);
                modified = true;
            }
            if (breakpoint == i && ImGui::MenuItem(materializr::tr("Clear Breakpoint"))) {
                m_history->setBreakpoint(-1);
                modified = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(materializr::tr("Delete"))) {
                deleteIndex = i;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    };

    // Group steps by calendar date — "Today", "Yesterday", or a date string
    // for older sessions. Each date is a collapsible header so a 100+ step
    // project doesn't dominate the panel. Default state: today's bucket is
    // expanded, all older buckets are collapsed (so the panel boots
    // minimised).
    using ymd_t = std::tuple<int, int, int>; // year, month, day (0-based mon)
    auto computeStepDate = [&](int idx) -> ymd_t {
        const Operation* op = m_history->getStep(idx);
        if (!op) return {1970, 0, 1};
        std::time_t tt = std::chrono::system_clock::to_time_t(op->timestamp());
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &tt);
#else
        localtime_r(&tt, &local);
#endif
        return {local.tm_year + 1900, local.tm_mon, local.tm_mday};
    };
    // Step timestamps never change, so their calendar dates are cached and
    // rebuilt only when history mutates — the grouping loop below reads the
    // date of EVERY step EVERY frame, which was 150+ localtime_r calls per
    // frame on a long history. ("Today"/"Yesterday" labels still use a fresh
    // `today` below, so the midnight rollover renames buckets correctly.)
    static std::vector<ymd_t> s_stepDates;
    static unsigned s_stepDatesRev = ~0u;
    if (s_stepDatesRev != m_history->revision() ||
        static_cast<int>(s_stepDates.size()) != stepCount) {
        s_stepDatesRev = m_history->revision();
        s_stepDates.resize(stepCount);
        for (int k = 0; k < stepCount; ++k) s_stepDates[k] = computeStepDate(k);
    }
    auto stepDate = [&](int idx) -> ymd_t {
        return (idx >= 0 && idx < static_cast<int>(s_stepDates.size()))
                   ? s_stepDates[idx]
                   : ymd_t{1970, 0, 1};
    };
    auto dateLabel = [](ymd_t d, ymd_t today, ymd_t yest) -> std::string {
        if (d == today) return materializr::tr("Today");
        if (d == yest)  return materializr::tr("Yesterday");
        char buf[32];
        static const char* months[] = {
            "Jan","Feb","Mar","Apr","May","Jun",
            "Jul","Aug","Sep","Oct","Nov","Dec" };
        std::snprintf(buf, sizeof(buf), "%s %d, %d",
                      months[std::get<1>(d)], std::get<2>(d), std::get<0>(d));
        return buf;
    };
    // Build the "today" / "yesterday" reference dates.
    auto now = std::chrono::system_clock::now();
    auto toYmd = [](std::chrono::system_clock::time_point t) -> ymd_t {
        std::time_t tt = std::chrono::system_clock::to_time_t(t);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &tt);
#else
        localtime_r(&tt, &local);
#endif
        return {local.tm_year + 1900, local.tm_mon, local.tm_mday};
    };
    ymd_t today = toYmd(now);
    ymd_t yest  = toYmd(now - std::chrono::hours{24});

    int i = 0;
    while (i < stepCount) {
        ymd_t bucket = stepDate(i);
        int runEnd = i;
        while (runEnd + 1 < stepCount && stepDate(runEnd + 1) == bucket) ++runEnd;
        // Date header is collapsible. Group "key" is the start index so
        // m_collapsedGroupStarts indexing still works. Default collapse state:
        // today's bucket expanded, everything older collapsed (so loading a
        // big project doesn't dump 100 rows on the user). We seed the default
        // by inserting historical buckets into m_collapsedGroupStarts the
        // first time we see them; subsequent toggles by the user persist.
        static const int kSentinel = -1;
        bool firstSeen = (m_seenGroupStarts.find(i) == m_seenGroupStarts.end());
        if (firstSeen) {
            m_seenGroupStarts.insert(i);
            if (bucket != today) m_collapsedGroupStarts.insert(i);
        }
        (void)kSentinel;
        bool isCollapsed = (m_collapsedGroupStarts.count(i) > 0);
        int runLen = runEnd - i + 1;

        ImGui::PushID(200000 + i);
        if (ImGui::SmallButton(isCollapsed ? "\xE2\x96\xB6" : "\xE2\x96\xBC")) {
            if (isCollapsed) m_collapsedGroupStarts.erase(i);
            else             m_collapsedGroupStarts.insert(i);
        }
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::TextColored(materializr::accentText(),
                           materializr::tr("%s  (%d step%s)"),
                           dateLabel(bucket, today, yest).c_str(),
                           runLen, runLen == 1 ? "" : "s");
        if (!isCollapsed) {
            // A modest indent (not the full default ~21px) — enough to nest the
            // steps under their date header without stranding the numbers in a
            // wide empty left margin.
            const float stepIndent = ImGui::GetFontSize() * 0.6f;
            ImGui::Indent(stepIndent);
            for (int k = i; k <= runEnd; ++k) renderOneStep(k);
            ImGui::Unindent(stepIndent);
        }
        i = runEnd + 1;
    }

    // Apply a queued delete now that we're done iterating the (about-to-change)
    // list. removeStep rebuilds in place and refuses (returns false) if a later
    // operation depends on the one being removed.
    if (deleteIndex >= 0) {
        if (m_history->removeStep(deleteIndex, *m_document)) {
            if (m_editingStep == deleteIndex) { m_editingStep = -1; m_showProperties = false; }
            else if (m_editingStep > deleteIndex) m_editingStep--;
            m_deleteConflict = false;
        } else {
            m_deleteConflict = true; // a dependent step blocked the removal
        }
        modified = true;
    }

    if (m_deleteConflict) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", materializr::tr("Can't delete: a later operation depends on it."));
    }

    // Draw breakpoint line at the end if breakpoint is at last step
    if (breakpoint >= 0 && breakpoint == stepCount - 1) {
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::Separator();
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();

    // Properties sub-section — the parameter widgets live in a bounded,
    // scrollable child; the Apply button is pinned BELOW it so it's always
    // visible no matter how many fields the op renders. Enter anywhere in
    // the editor commits too.
    if (propsOpen) {
        const Operation* op = m_history->getStep(m_editingStep);
        if (op) {
            ImGui::Separator();
            ImGui::TextColored(materializr::accentText(), materializr::tr("Properties: %s"),
                               op->name().c_str());
            ImGui::BeginChild("StepProps",
                              ImVec2(0, propsBlockH - kPropsChrome), true);
            const float propsTop = ImGui::GetCursorPosY();
            // Ops render "InputInt/InputDouble" with a LABEL on the right; the
            // default item width eats most of the row, so the field is
            // absurdly wide for a 2-3 digit id and the label runs off the
            // panel edge. Size the input for ~7 digits PLUS the -/+ step
            // buttons (which InputDouble carves out of the item width — a flat
            // char count left "12.500"-style values clipped). Ops wanting a
            // full-width control still PushItemWidth themselves inside.
            ImGui::PushItemWidth(
                ImGui::CalcTextSize("0000000").x +
                2.0f * (ImGui::GetFrameHeight() +
                        ImGui::GetStyle().ItemInnerSpacing.x));
            // First frame on a newly selected step: remember its params so a
            // failed Apply can snap the fields back (they bind live to the op).
            if (m_paramsSnapStep != m_editingStep) {
                m_paramsSnapStep = m_editingStep;
                m_paramsSnap = op->serializeParams();
            }
            const_cast<Operation*>(op)->renderProperties();
            ImGui::PopItemWidth();
            // Measure the fields so next frame's block sizes to them. In a
            // scrolling child the cursor tracks the full content, so this is
            // right whether the content fit or overflowed. Add the child's
            // top+bottom WindowPadding (the content region is inset by it) plus
            // a couple px, or the last field clips by ~15px.
            m_stepPropsH = (ImGui::GetCursorPosY() - propsTop) +
                           ImGui::GetStyle().WindowPadding.y * 2.0f + 4.0f;
            bool enterInProps =
                ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
                (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                 ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
            ImGui::EndChild();

            if (ImGui::Button(materializr::tr("Apply Changes"), ImVec2(-1, 0)) || enterInProps) {
                // Carry any inline circle-diameter edit forward into the later
                // full-snapshot sketchedit steps FIRST — otherwise the next
                // snapshot overwrites it before the extrude/pushpull reads it.
                m_history->propagateSketchValueEdits(m_editingStep, *m_document);
                // Transactional: a failed replay restores the model wholesale
                // rather than leaving it half-built.
                bool applied = m_history->editStep(m_editingStep, *m_document,
                                                   /*transactional=*/true);
                modified = true;
                if (!applied && !m_paramsSnap.empty()) {
                    // The replay was rolled back — snap the fields back to the
                    // pre-edit values too. The inputs bind live to op members,
                    // so without this the REJECTED value silently sticks in
                    // the panel while the geometry shows the old state.
                    const_cast<Operation*>(op)->deserializeParams(m_paramsSnap);
                } else if (applied) {
                    m_paramsSnap = op->serializeParams();   // new baseline
                }
                // If the edited step is a SketchEditOp, publish a cascade
                // event so any downstream Extrude / Push-Pull that consumed
                // this sketch re-runs with the new constraint values. We
                // publish from here (not from inside editStep) so generic
                // history shuffles — undo/redo, push/pull drag previews —
                // don't trigger spurious cascades. Skip if the edit didn't
                // apply (model was reverted) — cascading a reverted edit is wrong.
                if (applied && m_eventBus) {
                    auto* sketchOp = dynamic_cast<const materializr::SketchEditOp*>(op);
                    if (sketchOp) {
                        auto target = sketchOp->getTarget();
                        if (target && m_document) {
                            int sid = m_document->findSketchId(target.get());
                            if (sid >= 0) {
                                m_eventBus->publish(SketchEditedEvent{sid});
                            }
                        }
                    }
                }
            }
        }
    }

    // Bottom section: Undo/Redo + step counter
    ImGui::Separator();

    // After an undo/redo of a SketchEditOp, the body built from that sketch is
    // updated through the cascade (editStep), which the op's own undo/redo
    // doesn't drive — publish a SketchEditedEvent so the body follows. Mirrors
    // the Ctrl+Z/Ctrl+Y handlers in Application.
    auto publishIfSketchEdit = [&](const Operation* op) {
        if (!op || !m_eventBus || !m_document) return;
        if (auto* se = dynamic_cast<const materializr::SketchEditOp*>(op)) {
            if (auto target = se->getTarget()) {
                int sid = m_document->findSketchId(target.get());
                if (sid >= 0) m_eventBus->publish(SketchEditedEvent{sid});
            }
        } else if (auto* st = dynamic_cast<const materializr::SketchTransformOp*>(op)) {
            // A linked 3D sketch move updated its body via the cascade — re-run it
            // so the body follows the reverted/re-applied plane.
            if (st->getSketchId() >= 0)
                m_eventBus->publish(SketchEditedEvent{st->getSketchId()});
        }
    };

    if (m_showUndoRedo) {
        ImGui::BeginDisabled(m_historyLocked || !m_history->canUndo());
        if (ImGui::Button(materializr::tr("Undo"))) {
            const Operation* undone =
                m_history->getStep(m_history->currentStep());
            m_history->undo(*m_document);
            publishIfSketchEdit(undone);
            modified = true;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();

        ImGui::BeginDisabled(m_historyLocked || !m_history->canRedo());
        if (ImGui::Button(materializr::tr("Redo"))) {
            m_history->redo(*m_document);
            publishIfSketchEdit(m_history->getStep(m_history->currentStep()));
            modified = true;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();

        // Step counter
        char stepText[64];
        std::snprintf(stepText, sizeof(stepText), "Step %d/%d", currentStep + 1, stepCount);
        ImGui::Text("%s", stepText);
    }

    return modified;
}

} // namespace materializr
