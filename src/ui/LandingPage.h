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

    enum class ActionType { None, NewProject, OpenEntry, OpenFileDialog, Dismiss,
                            ExportStep, ExportStl, OpenSettings, OpenHelp,
                            ImportParts };
    struct Action {
        ActionType type = ActionType::None;
        std::string ref, name;   // OpenEntry / ExportStep / ExportStl
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

    // Replace the tile list. Takes ownership of the entries' GL textures
    // (deleted on the next setEntries / destruction). GL context required.
    void setEntries(std::vector<Entry> entries);

    // Attach a thumbnail to an already-shown tile - the tiles go up before
    // their previews are read (peeking one costs a full project inflate, so
    // it happens off-thread). Takes ownership of `tex`; a no-op ref, or a
    // tile that already has a texture, deletes the incoming one instead of
    // leaking it. GL context required.
    void setEntryTexture(const std::string& ref, unsigned int tex);

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
};

} // namespace materializr
