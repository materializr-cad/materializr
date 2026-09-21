#include "../plugin/PluginMacro.h"
#include "../core/Document.h"
#include "../io/IgesIO.h"
#include "../io/FileDialogs.h"
#include <cstdio>

REGISTER_PLUGIN(IgesIO, [](materializr::PluginContext& ctx) {
    ctx.registerIOFormat({"IGES", {"iges", "igs"}, true, true,
        [](materializr::PluginContext& ctx, const std::string&) {
            materializr::FileDialogs::openFile("Import IGES",
                {{"IGES Files", "*.iges *.igs *.IGES *.IGS"}},
                [&ctx](const std::string& path) {
                    if (path.empty()) return;
                    // A large assembly's parse + tessellation can take seconds;
                    // queueHeavyImport runs it deferred, under the same
                    // pool+pump mesh path project load uses, instead of
                    // freezing the window on the next full mesh rebuild.
                    // Mirrors StepIOPlugin.cpp's identical fix.
                    ctx.queueHeavyImport("Importing IGES\xE2\x80\xA6", [&ctx, path]() {
                        return materializr::IgesIO::import(path, ctx.document()).success;
                    });
                });
            return true;
        },
        [](materializr::PluginContext& ctx, const std::string&) {
            materializr::FileDialogs::exportFile("Export IGES", "export.iges",
                "application/octet-stream",
                {{"IGES Files", "*.iges *.igs"}},
                [&ctx](const std::string& path) {
                    return materializr::IgesIO::exportFile(path, ctx.document()).success;
                });
            return true;
        },
        // Export a caller-supplied document (the "export just these
        // bodies" path hands over a scratch doc holding copies).
        [](const Document& doc, const std::string& path) {
            return materializr::IgesIO::exportFile(path, doc).success;
        }});
})
