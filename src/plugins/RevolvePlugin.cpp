#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"

// Revolve is a body-targeting interactive op. The actual workflow (popup,
// arc-drag gizmo, live preview, applyRevolve) lives in Application because
// it needs deep document access and per-frame render-pass coupling that
// plugins don't get. The plugin's job here is purely the discovery surface:
// register a toolbar button that hands the request over to Application via
// the same requestInteractiveOp channel ConstructionPlane / ConstructionAxis
// use. Same pattern as those two - keeps the plumbing consistent.
// Revolve's toolbar slot is hardcoded inside Toolbar::renderBodyTools
// next to Mirror so it shares the Transform row visually (plugin-section
// rendering can't merge with hardcoded body-tools buttons today). The
// plugin entry here registers the Command Palette action only - keeps the
// architecture file-discoverable and gives Application a single
// requestInteractiveOp channel for future programmatic invocations.
REGISTER_PLUGIN(Revolve, [](materializr::PluginContext& ctx) {
    ctx.registerCommand({"Revolve", "",
        [](materializr::PluginContext& c) {
            c.requestInteractiveOp(materializr::InteractiveOp::Revolve);
        }});
})
