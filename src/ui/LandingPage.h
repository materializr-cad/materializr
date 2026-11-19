#pragma once
#include <string>
#include <vector>

namespace materializr {

// Startup landing page: a full-work-area screen shown before a project is
// open. A grid of tiles - "New Project" first, then the recent projects with
// the project name above its embedded thumbnail (ProjectIO::peekThumbnail;
// legacy saves have none and show a placeholder until their next save).
// Modals (recovery prompts, welcome, tours) are ImGui popups and always draw
// above it, so the startup dialog turn-taking is untouched.
class LandingPage {
public:
    struct Entry {
        std::string ref;       // what re-opens the project (path / content: URI)
        std::string name;      // display label
        unsigned int tex = 0;  // GL texture of the thumbnail; 0 = placeholder
    };

    // A project that is already open in a tab. The page used to say nothing
    // about these: with several open, the only ways off it were the small x in
    // the header or opening / creating something else (#184).
    struct OpenTab {
        size_t index = 0;      // the app's session index, handed back on a click
        std::string name;
        bool active = false;   // the tab the page is laid over
        bool dirty = false;    // unsaved changes
    };

    enum class ActionType { None, NewProject, OpenEntry, OpenFileDialog, Dismiss,
                            ExportStep, ExportStl, OpenSettings, OpenHelp,
                            ImportParts, DeleteEntry, Exit, SwitchTab };
    struct Action {
        ActionType type = ActionType::None;
        std::string ref, name;   // OpenEntry / ExportStep / ExportStl / DeleteEntry
        size_t tab = 0;          // SwitchTab: OpenTab::index
    };

    ~LandingPage();

    void setVisible(bool vis);
    bool isVisible() const { return m_visible; }

    // App logo drawn in the header (the same texture the layouts' top-bar
    // chip uses). 0 = text-only header. Set by the caller each show/frame;
    // NOT owned by this class (unlike the entry thumbnails).
    void setLogoTexture(unsigned int tex) { m_logoTex = tex; }

    // Whether the header shows a close (×) button. False at startup - there
    // is nothing behind the page, so closing it equals New Project. True when
    // reopened over a live session (File → Home Screen): the × is the way
    // back to the open project.
    void setCanDismiss(bool v) { m_canDismiss = v; }

    // The projects open in tabs right now, shown as a strip above the tiles
    // (empty = no strip). Cheap to refresh every frame.
    void setOpenTabs(std::vector<OpenTab> tabs) { m_openTabs = std::move(tabs); }

    // Replace the tile list. Takes ownership of the entries' GL textures
    // (deleted on the next setEntries / destruction). GL context required.
    void setEntries(std::vector<Entry> entries);

    // Attach a thumbnail to an already-shown tile - the tiles go up before
    // their previews are read (peeking one costs a full project inflate, so
    // it happens off-thread). Takes ownership of `tex`; a no-op ref, or a
    // tile that already has a texture, deletes the incoming one instead of
    // leaking it. GL context required.
    void setEntryTexture(const std::string& ref, unsigned int tex);

    // Drop one tile (its project was deleted / forgotten) and free its texture.
    // Safe to call between render()s, never from inside one. GL context required.
    void removeEntry(const std::string& ref);

    // Draw (when visible) and report what the user picked this frame. The
    // caller owns the consequences - this class never loads or creates
    // projects itself.
    Action render();

private:
    void destroyTextures();

    bool m_visible = false;
    bool m_takeFocus = false;   // one-shot on show: lift above the panels
    bool m_canDismiss = false;  // see setCanDismiss
    unsigned int m_logoTex = 0; // not owned
    std::vector<Entry> m_entries;
    std::vector<OpenTab> m_openTabs;
};

} // namespace materializr
