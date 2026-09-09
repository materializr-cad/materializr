#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"

// Primitives plugin: one toolbar button per OCCT primitive (Box / Cylinder /
// Sphere / Cone / Torus). Each button fires a requestInteractiveOp that
// Application picks up and opens a parameter popup for - same flow the
// Construction Plane / Axis plugins use. The actual PrimitiveOp construction
// happens in Application::commitPrimitivePopup so we only have to maintain
// one place that knows about the kind-specific parameter set.

// No toolbar buttons here - the empty-canvas "Primitives..." fold-out lives
// in Toolbar::renderPrimitivesMenu so the five kinds don't sprawl across the
// sidebar. The plugin still owns: the PrimitiveOp registration (via
// ForceLink + OperationFactory) AND the Command Palette entries so power
// users can fire each kind by name without hunting the menu.
REGISTER_PLUGIN(Primitives, [](materializr::PluginContext& ctx) {
    using namespace materializr;

    ctx.registerCommand({"New Box", "",
        [](PluginContext& c) { c.requestInteractiveOp(materializr::InteractiveOp::PrimitiveBox); }});
    ctx.registerCommand({"New Cylinder", "",
        [](PluginContext& c) { c.requestInteractiveOp(materializr::InteractiveOp::PrimitiveCylinder); }});
    ctx.registerCommand({"New Sphere", "",
        [](PluginContext& c) { c.requestInteractiveOp(materializr::InteractiveOp::PrimitiveSphere); }});
    ctx.registerCommand({"New Cone", "",
        [](PluginContext& c) { c.requestInteractiveOp(materializr::InteractiveOp::PrimitiveCone); }});
    ctx.registerCommand({"New Torus", "",
        [](PluginContext& c) { c.requestInteractiveOp(materializr::InteractiveOp::PrimitiveTorus); }});
})
