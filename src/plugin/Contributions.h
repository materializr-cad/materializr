#pragma once
#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <glm/glm.hpp>

class Document;   // global namespace, like the rest of the core headers use it

namespace materializr {

class PluginContext;
class InteractiveTool;

enum class SelectionContext {
    Always,
    NoSelection,
    HasBodies,
    HasFaces,
    HasEdges,
    HasSketches,
    HasSketchRegions,
    InSketchMode,
    MultipleBodies
};

struct ToolbarContribution {
    std::string name;
    std::string section;
    SelectionContext context = SelectionContext::Always;
    int priority = 100;
    std::function<void(PluginContext&)> action;
    std::function<std::unique_ptr<InteractiveTool>()> toolFactory;
    // Optional hover-description shown when "Show toolbar tooltips" is on.
    // Empty = no tooltip for this button.
    std::string tooltip;
};

struct CommandContribution {
    std::string name;
    std::string shortcut;
    std::function<void(PluginContext&)> action;
    int priority = 100;
};

struct MenuContribution {
    std::string path;
    std::string shortcut;
    int priority = 100;
    std::function<void(PluginContext&)> action;
    std::function<bool(const PluginContext&)> enabled;
};

struct IOFormatContribution {
    std::string name;
    std::vector<std::string> extensions;
    bool canImport = false;
    bool canExport = false;
    std::function<bool(PluginContext&, const std::string& path)> importFn;
    std::function<bool(PluginContext&, const std::string& path)> exportFn;
    // Export a SPECIFIC document to an explicit path - no picker, no reading
    // the live document. This is what "export just these bodies" runs: the
    // host builds a scratch Document holding copies of the chosen bodies and
    // hands it over. exportFn can't serve that, because it owns its own file
    // dialog whose callback fires frames later and reads ctx.document() at
    // that point - by then the host has moved on. Optional: a format without
    // it simply doesn't appear in the per-body export menu.
    std::function<bool(const Document&, const std::string& path)> exportDocFn;
};

struct RenderPassContribution {
    std::string name;
    int priority = 500;
    std::function<void(PluginContext&, const glm::mat4& view, const glm::mat4& proj)> render;
    std::function<bool()> initialize;
};

struct PropertyContribution {
    std::string name;
    SelectionContext context = SelectionContext::Always;
    int priority = 100;
    std::function<bool(PluginContext&)> render;
};

// A free-floating, per-frame ImGui overlay. The host calls `render` every frame
// (after the docked panels, so it draws on top) inside the ImGui frame - the
// plugin is free to Begin/End its own window(s). Unlike an InteractiveTool this
// is non-modal: it doesn't capture input or get cancelled when a tool starts, so
// it suits persistent, optional UI like a tutorial/onboarding panel.
struct OverlayContribution {
    std::string name;
    int priority = 100;
    std::function<void(PluginContext&)> render;
};

} // namespace materializr
