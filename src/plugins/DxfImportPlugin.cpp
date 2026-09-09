#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../core/Document.h"
#include "../modeling/Sketch.h"
#include "../io/DxfImport.h"
#include "../io/FileDialogs.h"
#include <cstdio>
#include <memory>

// File → Import → DXF. Same flow as the SVG import plugin - the profile
// lands as a NEW sketch on the ground plane - but DXF is a DIMENSIONED
// format, so unlike SVG there is no width clamp or rescale: a 100 mm part
// imports at exactly 100 mm ($INSUNITS handles inch-authored files), only
// the drawing's offset is normalized to the origin.
REGISTER_PLUGIN(DxfImport, [](materializr::PluginContext& ctx) {
    ctx.registerIOFormat({"DXF", {"dxf"}, true, false,
        [](materializr::PluginContext& ctx, const std::string&) {
            materializr::FileDialogs::openFile("Import DXF",
                {{"DXF Files", "*.dxf *.DXF"}},
                [&ctx](const std::string& path) {
                    if (path.empty()) return;
                    auto sk = std::make_shared<materializr::Sketch>();
                    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0),
                                               gp_Dir(0, 1, 0),
                                               gp_Dir(1, 0, 0))));
                    auto result = materializr::DxfImport::importFile(path, *sk);
                    if (!result.success) {
                        std::fprintf(stderr, "[DXF] import failed: %s\n",
                                     result.errorMessage.c_str());
                        return;
                    }
                    std::string name = path;
                    auto slash = name.find_last_of("/\\");
                    if (slash != std::string::npos) name = name.substr(slash + 1);
                    auto dot = name.find_last_of('.');
                    if (dot != std::string::npos) name = name.substr(0, dot);

                    ctx.document().addSketch(sk, name);
                    ctx.markMeshesDirty();
                    std::fprintf(stderr,
                                 "[DXF] imported '%s' as sketch (%d entities, "
                                 "%d unsupported skipped)\n",
                                 name.c_str(), result.entityCount,
                                 result.skippedCount);
                });
            return true;
        },
        nullptr});
})
