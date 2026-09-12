#include "PluginContext.h"
#include "PluginRegistry.h"
#include "../core/EventBus.h"
#include "../core/Document.h"
#include "../core/History.h"
#include "../core/SelectionManager.h"
#include "../viewport/Camera.h"

namespace materializr {

Document& PluginContext::document() { return *m_document; }
History& PluginContext::history() { return *m_history; }
SelectionManager& PluginContext::selection() { return *m_selection; }
EventBus& PluginContext::events() { return *m_eventBus; }
const Camera& PluginContext::camera() const { return *m_camera; }

void PluginContext::markMeshesDirty() {
    if (m_meshesDirtyFlag) *m_meshesDirtyFlag = true;
}

void PluginContext::markDocumentDirty() {
    if (m_markDirtyFn) m_markDirtyFn();
}

bool PluginContext::isInSketchMode() const {
    return m_sketchModeFlag && *m_sketchModeFlag;
}

void PluginContext::requestInteractiveOp(InteractiveOp op) {
    m_pendingInteractiveOp = op;
}

InteractiveOp PluginContext::takeRequestedInteractiveOp() {
    const InteractiveOp taken = m_pendingInteractiveOp;
    m_pendingInteractiveOp = InteractiveOp::None;
    return taken;
}

void PluginContext::registerToolbarButton(ToolbarContribution contrib) {
    PluginRegistry::instance().toolbarContributions().push_back(std::move(contrib));
}

void PluginContext::registerCommand(CommandContribution contrib) {
    PluginRegistry::instance().commandContributions().push_back(std::move(contrib));
}

void PluginContext::registerMenuItem(MenuContribution contrib) {
    PluginRegistry::instance().menuContributions().push_back(std::move(contrib));
}

void PluginContext::registerIOFormat(IOFormatContribution contrib) {
    PluginRegistry::instance().ioFormats().push_back(std::move(contrib));
}

void PluginContext::registerRenderPass(RenderPassContribution contrib) {
    PluginRegistry::instance().renderPasses().push_back(std::move(contrib));
}

void PluginContext::registerPropertySection(PropertyContribution contrib) {
    PluginRegistry::instance().propertyContributions().push_back(std::move(contrib));
}

void PluginContext::registerOverlay(OverlayContribution contrib) {
    PluginRegistry::instance().overlayContributions().push_back(std::move(contrib));
}

const AppSettings::AiSettings& PluginContext::aiSettings() const {
    static const AppSettings::AiSettings kEmpty;
    return m_aiSettings ? *m_aiSettings : kEmpty;
}

void PluginContext::_bind(Document* doc, History* hist, SelectionManager* sel,
                          EventBus* bus, Camera* cam, bool* meshesDirtyFlag,
                          const bool* sketchModeFlag,
                          const AppSettings::AiSettings* aiSettings,
                          std::function<void()> markDirtyFn) {
    m_document = doc;
    m_history = hist;
    m_selection = sel;
    m_eventBus = bus;
    m_camera = cam;
    m_meshesDirtyFlag = meshesDirtyFlag;
    m_sketchModeFlag = sketchModeFlag;
    m_aiSettings = aiSettings;
    if (markDirtyFn) m_markDirtyFn = std::move(markDirtyFn);
}

} // namespace materializr
