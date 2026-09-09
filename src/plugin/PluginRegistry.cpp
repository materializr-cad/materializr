#include "PluginRegistry.h"
#include "PluginContext.h"
#include <algorithm>
#include <cstdio>

namespace materializr {

PluginRegistry& PluginRegistry::instance() {
    static PluginRegistry s_instance;
    return s_instance;
}

void PluginRegistry::add(PluginDescriptor desc) {
    m_plugins.push_back(std::move(desc));
}

void PluginRegistry::initAll(PluginContext& ctx) {
    // Start from a clean slate. The registry is a process-lifetime singleton,
    // but on Android SDL can re-enter SDL_main in the SAME process (the
    // activity is recreated - system-bar transitions, backing out and
    // reopening, surface loss). Each plugin's init then pushes its
    // contributions AGAIN, and the toolbar/menus/IO formats show every entry
    // two or three times. Clearing here makes initAll idempotent; m_plugins
    // itself is fine (static auto-registration really does run once).
    m_activeTool.reset();
    m_toolbar.clear();
    m_commands.clear();
    m_menus.clear();
    m_ioFormats.clear();
    m_renderPasses.clear();
    m_properties.clear();
    m_overlays.clear();

    for (auto& plugin : m_plugins) {
        if (plugin.init) {
            plugin.init(ctx);
        }
    }

    std::sort(m_toolbar.begin(), m_toolbar.end(),
        [](const auto& a, const auto& b) { return a.priority < b.priority; });
    std::sort(m_commands.begin(), m_commands.end(),
        [](const auto& a, const auto& b) { return a.priority < b.priority; });
    std::sort(m_menus.begin(), m_menus.end(),
        [](const auto& a, const auto& b) { return a.priority < b.priority; });
    std::sort(m_renderPasses.begin(), m_renderPasses.end(),
        [](const auto& a, const auto& b) { return a.priority < b.priority; });
    std::sort(m_properties.begin(), m_properties.end(),
        [](const auto& a, const auto& b) { return a.priority < b.priority; });
    std::sort(m_overlays.begin(), m_overlays.end(),
        [](const auto& a, const auto& b) { return a.priority < b.priority; });

    for (auto& rp : m_renderPasses) {
        if (rp.initialize && !rp.initialize()) {
            fprintf(stderr, "Failed to initialize render pass: %s\n", rp.name.c_str());
        }
    }
}

void PluginRegistry::shutdownAll() {
    m_activeTool.reset();
    for (auto& plugin : m_plugins) {
        if (plugin.shutdown) {
            plugin.shutdown();
        }
    }
}

std::vector<ToolbarContribution>& PluginRegistry::toolbarContributions() { return m_toolbar; }
std::vector<CommandContribution>& PluginRegistry::commandContributions() { return m_commands; }
std::vector<MenuContribution>& PluginRegistry::menuContributions() { return m_menus; }
std::vector<IOFormatContribution>& PluginRegistry::ioFormats() { return m_ioFormats; }
std::vector<RenderPassContribution>& PluginRegistry::renderPasses() { return m_renderPasses; }
std::vector<PropertyContribution>& PluginRegistry::propertyContributions() { return m_properties; }
std::vector<OverlayContribution>& PluginRegistry::overlayContributions() { return m_overlays; }

void PluginRegistry::activateTool(std::unique_ptr<InteractiveTool> tool, PluginContext& ctx) {
    if (m_activeTool) {
        m_activeTool->cancel(ctx);
    }
    m_activeTool = std::move(tool);
    if (m_activeTool) {
        m_activeTool->begin(ctx);
    }
}

InteractiveTool* PluginRegistry::activeTool() {
    return m_activeTool.get();
}

void PluginRegistry::deactivateTool(PluginContext& ctx) {
    if (m_activeTool) {
        m_activeTool->cancel(ctx);
        m_activeTool.reset();
    }
}

void PluginRegistry::finishActiveTool() {
    // No cancel(): the tool already finished itself (commit or cancel) before
    // signalling done, so cancelling again would undo a committed operation.
    m_activeTool.reset();
}

} // namespace materializr
