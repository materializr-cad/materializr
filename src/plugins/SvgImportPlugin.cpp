#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../core/Document.h"
#include "../modeling/Sketch.h"
#include "../modeling/SvgImport.h"
#include "../io/FileDialogs.h"
#include <cstdio>
#include <algorithm>
#include <memory>

// File → Import → SVG. Drops the artwork as a NEW sketch on the ground
// plane at its natural size (clamped to something sane), centred on the
// origin - the quick "get my logo into the project" path. For interactive
// placement (click-to-position, width, rotation, live ghost preview), the
// sketch toolbar's "Import SVG" tool is the richer flow; it needs the
// active SketchTool, which plugins deliberately can't reach.
REGISTER_PLUGIN(SvgImport, [](materializr::PluginContext& ctx) {
    ctx.registerIOFormat({"SVG", {"svg"}, true, false,
        [](materializr::PluginContext& ctx, const std::string&) {
            materializr::FileDialogs::openFile("Import SVG",
                {{"SVG Files", "*.svg *.SVG"}},
                [&ctx](const std::string& path) {
                    if (path.empty()) return;
                    materializr::SvgPaths svg;
                    if (!materializr::SvgImport::load(path, svg)) return;

                    // Ground plane in the USER's axis convention (Z up):
                    // world XZ with +Y normal - the same plane the ground
                    // sketch button uses.
                    auto sk = std::make_shared<materializr::Sketch>();
                    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0),
                                               gp_Dir(0, 1, 0),
                                               gp_Dir(1, 0, 0))));
                    // Natural size when sane, else a printable default.
                    float w = svg.size().x > 1e-6f ? svg.size().x
                                                   : svg.size().y;
                    w = std::clamp(w, 5.0f, 300.0f);
                    int loops = materializr::SvgImport::place(
                        sk.get(), svg, {0.0f, 0.0f}, w, 0.0f);
                    if (loops <= 0) return;

                    // Derive a sketch name from the file name.
                    std::string name = path;
                    auto slash = name.find_last_of("/\\");
                    if (slash != std::string::npos) name = name.substr(slash + 1);
                    auto dot = name.find_last_of('.');
                    if (dot != std::string::npos) name = name.substr(0, dot);

                    ctx.document().addSketch(sk, name);
                    ctx.markMeshesDirty();
                    std::fprintf(stderr,
                                 "[SVG] imported '%s' as sketch (%d loops, "
                                 "%.0f mm wide) on the ground plane\n",
                                 name.c_str(), loops, w);
                });
            return true;
        },
        nullptr});
})
