#include "../plugin/PluginMacro.h"
#include "../core/Document.h"
#include "../io/BrepIO.h"
#include "../io/FileDialogs.h"

// OCCT-native BREP exchange - the lossless path to/from FreeCAD and other
// OCCT-based tools (exact geometry, no tessellation, no STEP translation).
REGISTER_PLUGIN(BrepIO, [](materializr::PluginContext& ctx) {
    ctx.registerIOFormat({"BREP", {"brep"}, true, true,
        [](materializr::PluginContext& ctx, const std::string&) {
            materializr::FileDialogs::openFile("Import BREP",
                {{"BREP Files", "*.brep *.BREP"}},
                [&ctx](const std::string& path) {
                    if (path.empty()) return;
                    auto result = materializr::BrepIO::import(path, ctx.document());
                    if (result.success) {
                        ctx.markMeshesDirty();
                    }
                });
            return true;
        },
        [](materializr::PluginContext& ctx, const std::string&) {
            materializr::FileDialogs::exportFile("Export BREP", "export.brep",
                "application/octet-stream",
                {{"BREP Files", "*.brep"}},
                [&ctx](const std::string& path) {
                    return materializr::BrepIO::exportFile(path, ctx.document()).success;
                });
            return true;
        },
        // Export a caller-supplied document (the "export just these
        // bodies" path hands over a scratch doc holding copies).
        [](const Document& doc, const std::string& path) {
            return materializr::BrepIO::exportFile(path, doc).success;
        }});
})
