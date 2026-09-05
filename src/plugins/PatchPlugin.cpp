#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../core/SelectionManager.h"

// Patch — fit one surface across a ring of picked edges and, where those edges
// bound an opening in a body, sew it back in so the void is actually closed.
//
// The reason this is its own tool rather than a mode of Loft: a loft skins a
// stack of sections along an axis and cares about their order and their vertex
// correspondence. A patch has no sections and no order — it is a variational
// fit to whatever boundary it is given, which is what "fill this hole" needs.
//
// Faces may be selected alongside the edges; they are read as explicit tangency
// supports for the boundary that lies on them. That only matters where an edge
// has a face on both sides (a notch, a bridge between two lumps) and the app
// would otherwise have to guess which one to blend into.
REGISTER_PLUGIN(Patch, [](materializr::PluginContext& ctx) {
    auto action = [](materializr::PluginContext& c) {
        c.requestInteractiveOp(materializr::InteractiveOp::Patch);
    };

    const char* tooltip =
        "Fill an opening with a single fitted surface. Ctrl-click the ring of "
        "edges around the void and click Patch: the surface is fitted across "
        "them and, if they bound a hole in one body, sewn back in so the body "
        "closes.\n\n"
        "CONTINUITY is the point of the tool. Position just plugs the hole. "
        "Tangent leaves the rim along the surrounding faces, so there is no "
        "crease where the patch meets them. Curvature matches how those faces "
        "are bending as well, for a blend that reads as one surface.\n\n"
        "Tangency needs something to be tangent TO: where the faces around the "
        "opening stand square to it — a flat lid on vertical walls — there is "
        "no tangent surface to find, and the panel says so rather than "
        "pretending. Slope those walls even a few degrees and it works.\n\n"
        "Select faces as well as edges to say which side a bridging patch "
        "should blend into.";

    ctx.registerToolbarButton({"Patch", "Patch",
        materializr::SelectionContext::HasEdges, 402,
        action, nullptr, tooltip});
})
