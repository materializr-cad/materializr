#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../core/Document.h"
#include "../core/History.h"
#include "../core/SelectionManager.h"
#include "../modeling/Sketch.h"
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <vector>
#include "../modeling/LoftOp.h"
#include <TopoDS_Wire.hxx>
#include <cstdio>
#include <memory>

// N-profile sketch loft (2+).
//
// Selection contract:
//   * 2+ sketches (or regions) selected -> the action lofts through ALL of
//                              them in click order, producing a new solid.
//   * 1 sketch / region selected -> the action stashes the request as the
//                              "LoftPickSecond" interactive-op so Application
//                              can render a hint popup telling the user to
//                              Ctrl-click more sketches and click Loft again.
//
// We pull the outer wire of each sketch's outermost region - for the common
// case (one closed loop per profile sketch) that's exactly what
// BRepOffsetAPI_ThruSections needs.
//
// The button is registered under both HasSketches AND HasSketchRegions: when
// the user clicks inside a closed region the toolbar switches to the Region
// panel and HasSketches no longer matches, but SketchRegion entries carry
// their parent sketch id so we can route either selection through the same
// action.
REGISTER_PLUGIN(Loft, [](materializr::PluginContext& ctx) {
    auto action = [](materializr::PluginContext& ctx) {
        // Count distinct sketches in the selection - Application reads the
        // selection itself when it begins the loft so we don't have to ship
        // wires through the plugin context.
        const auto& sel = ctx.selection().getSelection();
        std::vector<int> sketchIds;
        auto add = [&](int id) {
            if (id < 0) return;
            for (int x : sketchIds) if (x == id) return;
            sketchIds.push_back(id);
        };
        // Faces count as sections too -- a face's outer wire is a closed loop
        // already. Counting only sketches here meant a two-face selection fell
        // through to the "pick another profile" hint and never opened the
        // popup, even though the button was offered for it.
        int faceCount = 0;
        std::vector<TopoDS_Shape> seenFaces;
        for (const auto& e : sel) {
            if ((e.type == SelectionType::Sketch ||
                 e.type == SelectionType::SketchRegion) && e.sketchId >= 0) {
                add(e.sketchId);
            } else if (e.type == SelectionType::Face && !e.shape.IsNull() &&
                       e.shape.ShapeType() == TopAbs_FACE) {
                bool dup = false;
                for (const auto& f : seenFaces) if (f.IsSame(e.shape)) { dup = true; break; }
                if (!dup) { seenFaces.push_back(e.shape); ++faceCount; }
            }
        }

        if (static_cast<int>(sketchIds.size()) + faceCount < 2) {
            ctx.requestInteractiveOp(materializr::InteractiveOp::LoftPickSecond);
            return;
        }

        // Hand off to Application, which opens the Loft popup (section list
        // with reorder + per-section Flip, Solid/Shell, Smooth/Ruled, live
        // preview, Apply / Cancel) - same architecture as Linear/Radial
        // Pattern.
        ctx.requestInteractiveOp(materializr::InteractiveOp::Loft);
    };

    const char* tooltip =
        "Loft a solid through two or more sketch profiles. Ctrl-click each "
        "sketch (or region) in order - first to last is the skinning order - "
        "then click Loft. With one selected, you'll be prompted to pick more.\n\n"
        "GUIDED MODE: select ONE closed profile plus 1-2 OPEN curves and the "
        "profile is swept up along them - draw each curve as a side "
        "silhouette rising from the base (a slanted line = straight taper, "
        "an arc = rounded side). Strokes past the peak are ignored.\n\n"
        "Best results when the profiles sit on PARALLEL planes with similar "
        "topology (all rectangles, all circles, etc.). Profiles on "
        "perpendicular planes or with very different vertex counts produce a "
        "tent / pyramid surface - that's the loft algorithm being honest, not "
        "a bug. For floor-to-vertical transitions, a Sweep along a guide curve "
        "is usually what you want instead.\n\n"
        "Sections can be SKETCHES or FACES -- pick two faces to loft straight "
        "between them, which avoids the profile-building step entirely.";

    ctx.registerToolbarButton({"Loft", "Loft",
        materializr::SelectionContext::HasSketches, 400,
        action, nullptr, tooltip});

    ctx.registerToolbarButton({"Loft", "Loft",
        materializr::SelectionContext::HasSketchRegions, 400,
        action, nullptr, tooltip});

    // Faces are sections too. A face's outer wire is already a closed loop of
    // real edges, so it skips the sketch region builder -- which is where
    // spline-heavy profiles pick up corners that do not correspond between
    // sections and loft into a self-intersecting solid (issue #83).
    ctx.registerToolbarButton({"Loft", "Loft",
        materializr::SelectionContext::HasFaces, 400,
        action, nullptr, tooltip});
})
