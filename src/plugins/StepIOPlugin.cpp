#include "../plugin/PluginMacro.h"
#include "../core/Document.h"
#include "../io/StepIO.h"
#include "../io/FileDialogs.h"
#include <cstdio>

REGISTER_PLUGIN(StepIO, [](materializr::PluginContext& ctx) {
    ctx.registerIOFormat({"STEP", {"step", "stp"}, true, true,
        [](materializr::PluginContext& ctx, const std::string&) {
            materializr::FileDialogs::openFile("Import STEP",
                {{"STEP Files", "*.step *.stp *.STEP *.STP"}},
                [&ctx](const std::string& path) {
                    if (path.empty()) return;
                    // A large assembly's parse + tessellation can take seconds;
                    // queueHeavyImport runs it deferred, under the same
                    // pool+pump mesh path project load uses, instead of
                    // freezing the window on the next full mesh rebuild.
                    ctx.queueHeavyImport("Importing STEP\xE2\x80\xA6", [&ctx, path]() {
                        return materializr::StepIO::import(path, ctx.document()).success;
                    });
                });
            return true;
        },
        [](materializr::PluginContext& ctx, const std::string&) {
            materializr::FileDialogs::exportFile("Export STEP", "export.step",
                "application/octet-stream",
                {{"STEP Files", "*.step *.stp"}},
                [&ctx](const std::string& path) {
                    return materializr::StepIO::exportFile(path, ctx.document()).success;
                });
            return true;
        },
        // Export a caller-supplied document (the "export just these
        // bodies" path hands over a scratch doc holding copies).
        [](const Document& doc, const std::string& path) {
            return materializr::StepIO::exportFile(path, doc).success;
        }});
})
