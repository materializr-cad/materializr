#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../core/SelectionManager.h"

// Boundary Fill - a SEPARATE feature from Loft by design. N closed sketches
// are treated as silhouettes (the body's outline seen along each sketch
// plane's normal); each is extruded through the others and the prisms are
// boolean-intersected. Order doesn't matter, planes can be anything, and
// profiles that don't overlap fail cleanly - none of the section-ordering /
// vertex-pairing sensitivity that makes a loft of perpendicular profiles
// weave through itself.
REGISTER_PLUGIN(BoundaryFill, [](materializr::PluginContext& ctx) {
    auto action = [](materializr::PluginContext& c) {
        c.requestInteractiveOp(materializr::InteractiveOp::BoundaryFill);
    };

    const char* tooltip =
        "Build a solid from silhouette sketches. Select two or more closed "
        "sketches on different planes (e.g. top + front + side outlines of "
        "the part) and click: each sketch is extruded through the others and "
        "the result is their intersection - the solid that matches every "
        "outline. Order doesn't matter.\n\n"
        "Classic use: a base footprint plus curved front/side profiles gives "
        "a tapered body with rounded sides. Also the way to reconstruct an "
        "object from traced reference photos.";

    ctx.registerToolbarButton({"Boundary Fill", "Boundary Fill",
        materializr::SelectionContext::HasSketches, 401,
        action, nullptr, tooltip});

    ctx.registerToolbarButton({"Boundary Fill", "Boundary Fill",
        materializr::SelectionContext::HasSketchRegions, 401,
        action, nullptr, tooltip});
})
