#pragma once
#include "../platform_defs.h"
#include <string>
#include <vector>
#include <functional>

namespace materializr {

struct FileFilter {
    std::string description;
    std::string pattern;
};

class FileDialogs {
public:
    // Open a file browser (non-blocking, renders via ImGui)
    static void openFile(const std::string& title,
                         const std::vector<FileFilter>& filters,
                         std::function<void(const std::string&)> callback);

    // Open a save browser (non-blocking)
    static void saveFile(const std::string& title,
                         const std::string& defaultName,
                         const std::vector<FileFilter>& filters,
                         std::function<void(const std::string&)> callback);

    // Call every frame from the main loop to render the active dialog
    static void render();

    // Unified export entry point. `writeFn(path)` writes the file to `path`
    // and returns success. On desktop this drives a Save dialog (filters apply,
    // `mime` ignored); on Android it pops a Share / Save-to-device sheet
    // (`title`/`filters` ignored, `mime` used). Keeping the platform branch
    // here lets plugins call one function with no #if - important because the
    // REGISTER_PLUGIN macro stringifies its argument, and a preprocessor
    // directive inside a macro argument is ill-formed under MSVC.
    static void exportFile(const std::string& title,
                           const std::string& defaultName,
                           const std::string& mime,
                           const std::vector<FileFilter>& filters,
                           std::function<bool(const std::string&)> writeFn);

#if defined(MZ_MOBILE)
    // Android export: pop a Share / Save-to-device sheet. writeFn(path) writes
    // the file to a temp path (returns success); Share hands it to the system
    // share sheet, Save copies it to a SAF destination. Desktop keeps saveFile.
    static void mobileExportShareOrSave(const std::string& suggestedName,
                                         const std::string& mime,
                                         std::function<bool(const std::string&)> writeFn);
#endif

    // Is a dialog currently open?
    static bool isOpen();

    // Desktop file dialogs shell out to zenity / kdialog / qarma / matedialog;
    // if NONE is installed they silently do nothing (a fresh/minimal Linux box
    // often lacks them). Returns whether a backend exists - always true on
    // Android (SAF). Lets the app warn instead of failing mute.
    static bool dialogsAvailable();
    // Invoked in place of showing a dialog when openFile/saveFile is requested
    // with no backend, so the app can surface guidance (e.g. a toast).
    static void setUnavailableNotifier(std::function<void()> cb);

    // Last directory the picker landed in. Application syncs this with
    // AppSettings::lastFileDir at load / save time so the value survives
    // a relaunch. Updated automatically when a non-empty path comes back
    // from openFile / saveFile.
    static void setLastDir(const std::string& dir);
    static const std::string& getLastDir();
};

} // namespace materializr
