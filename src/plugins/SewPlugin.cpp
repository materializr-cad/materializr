#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../core/SelectionManager.h"

// Sew — the rung under Patch. See SewOp.h for why it has to exist: sewing runs
// inside five operations and was reachable from the UI in none of them, so a
// space bounded by several patches could never be made solid.
//
// Registered for one body as well as several: a single surface body whose faces
// already close (a shell that came in from STEP, or one patch that finished a
// lid) becomes a solid with no second selection to make.
REGISTER_PLUGIN(Sew, [](materializr::PluginContext& ctx) {
    auto action = [](materializr::PluginContext& c) {
        c.requestInteractiveOp(materializr::InteractiveOp::Sew);
    };

    const char* tooltip =
        "Stitch surfaces into one body - and into a solid when they close.\n\n"
        "Select the surfaces (Patch results, imported shells) and click. Faces "
        "that meet are joined; if the result encloses a volume you get a solid "
        "you can fillet, shell and boolean like any other. If it doesn't, you "
        "get the joined shell and a count of the edges still open, which is "
        "how many gaps are left to patch.\n\n"
        "Joined at the tightest tolerance that closes, so surfaces that only "
        "nearly meet are not silently welded together.";

    ctx.registerToolbarButton({"Sew", "Repair",
        materializr::SelectionContext::HasBodies, 403,
        action, nullptr, tooltip});
})
