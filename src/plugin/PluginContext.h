#pragma once
#include "Contributions.h"
#include "InteractiveOp.h"
#include "../io/Settings.h"
#include <memory>
#include <string>

class Document;
class History;
class SelectionManager;

namespace materializr {

class EventBus;
class Camera;
class InteractiveTool;

class PluginContext {
public:
    Document& document();
    History& history();
    SelectionManager& selection();
    EventBus& events();
    const Camera& camera() const;
    // The AI Assistant's provider/key/model config. A pointer, not a value,
    // because it must reflect the LIVE settings if the user edits them in
    // the Settings dialog mid-session - the plugin re-reads it on every
    // send, it doesn't cache it.
    const AppSettings::AiSettings& aiSettings() const;

    void markMeshesDirty();
    // For a plugin mutation outside History (e.g. MatePlugin moving a body
    // via the solver, not an undoable op) - otherwise nothing marks the
    // project unsaved and a quit/reopen silently drops the change.
    void markDocumentDirty();

    // True while the host Application has the sketch editor active. Plugins
    // can use this to suppress decorations that would clutter the sketch
    // canvas - the Construction Plane plugin hides plane quads when
    // sketching in ortho so the user has a clean drawing surface.
    bool isInSketchMode() const;

    // True once _bind has set a live Document - document()/history() have no
    // null check of their own, so a caller holding a PluginContext* that
    // might not have gone through _bind yet (a future headless/test harness
    // path, or a refactor that changes wireDocumentConsumers()'s ordering)
    // needs a way to tell before dereferencing. Not currently reachable in
    // the app proper: Application always binds before any plugin render()
    // callback can run.
    bool isBound() const { return m_document != nullptr; }

    // Request that the host Application start an interactive popup-driven op
    // (which the plugin can't run on its own - those need viewport + UI plumbing
    // that lives in Application). Application picks it up via
    // takeRequestedInteractiveOp() once per frame and dispatches. Calling this
    // from a toolbar action defers the actual popup to the next frame, which is
    // exactly when Application checks for it.
    //
    // The id is a typed InteractiveOp, not a string: the old free-form channel
    // silently ignored anything the dispatcher didn't recognise, so a typo on
    // either side was a dead button with no diagnostic (discussion #72).
    // takeRequestedInteractiveOp() returns InteractiveOp::None when idle.
    void requestInteractiveOp(InteractiveOp op);
    InteractiveOp takeRequestedInteractiveOp();

    void registerToolbarButton(ToolbarContribution contrib);
    void registerCommand(CommandContribution contrib);
    void registerMenuItem(MenuContribution contrib);
    void registerIOFormat(IOFormatContribution contrib);
    void registerRenderPass(RenderPassContribution contrib);
    void registerPropertySection(PropertyContribution contrib);
    void registerOverlay(OverlayContribution contrib);

    void _bind(Document* doc, History* hist, SelectionManager* sel,
               EventBus* bus, Camera* cam, bool* meshesDirtyFlag,
               const bool* sketchModeFlag,
               const AppSettings::AiSettings* aiSettings,
               std::function<void()> markDirtyFn = {});

private:
    Document* m_document = nullptr;
    History* m_history = nullptr;
    SelectionManager* m_selection = nullptr;
    EventBus* m_eventBus = nullptr;
    Camera* m_camera = nullptr;
    bool* m_meshesDirtyFlag = nullptr;
    const bool* m_sketchModeFlag = nullptr;
    const AppSettings::AiSettings* m_aiSettings = nullptr;
    std::function<void()> m_markDirtyFn;
    InteractiveOp m_pendingInteractiveOp = InteractiveOp::None;
};

} // namespace materializr
