#pragma once
#include <functional>
#include <set>
#include <string>
#include <vector>

class Document;
class SelectionManager;
class History;
struct SelectionEntry;   // global namespace, like SelectionManager itself

namespace materializr {

class ItemsPanel {
public:
    ItemsPanel();

    void setDocument(Document* doc);
    void setSelectionManager(SelectionManager* sel);
    void setHistory(History* hist);
    // The sketch currently being drawn, if any, can appear as a normal row
    // here the moment a mid-edit save registers it with the Document - but
    // it's still live in Application::m_activeSketch, uncoordinated with this
    // panel. Delete/Edit Sketch on that row would desync the two (stale
    // m_activeSketchId, clobbered undo floor); gate them while it's active.
    void setActiveSketchContext(bool inSketchMode, int activeSketchId) {
        m_sketchModeActive = inSketchMode;
        m_activeSketchId = activeSketchId;
    }

    // True if the panel was hovered last frame - the touch input layer uses this
    // to arm long-press (right-click) over the panel's rows for their context
    // menus, the same way it does over the viewport.
    bool isHovered() const { return m_hovered; }
    // Called whenever a rename / non-history mutation happens so the
    // Application can mark the project dirty (otherwise closing without a
    // manual Save silently drops the change).
    void setDirtyCallback(std::function<void()> cb) { m_markDirty = std::move(cb); }
    // Called when the user picks "Export STL…" from a body's context menu.
    // ItemsPanel doesn't own STL I/O, so the Application wires this up to
    // route the click into its own per-body export flow.
    void setExportStlCallback(std::function<void(int)> cb) { m_exportStl = std::move(cb); }
    // The formats the "Export" submenu offers, in menu order, from the plugin
    // registry - a new export plugin shows up here without touching this
    // panel. The callback below gets the bodies to export (the whole
    // selection when the clicked body is part of one) and the chosen name.
    // A PROVIDER, not a fixed list: the panel is wired up before the plugins
    // register their IO formats, so anything captured at wiring time would be
    // empty forever. Asked at menu-open instead, which also survives any
    // future re-ordering of startup.
    void setExportFormatsProvider(std::function<std::vector<std::string>()> p) {
        m_exportFormats = std::move(p);
    }
    void setExportBodiesCallback(
        std::function<void(const std::vector<int>&, const std::string&)> cb) {
        m_exportBodies = std::move(cb);
    }
    // "Export to New Project" on a body's context menu - routes to
    // Application::exportBodiesToNewProject, which opens the parts in a new
    // tab as an unsaved project. Takes the whole body selection, same rule
    // as the Export submenu.
    void setExportToProjectCallback(
        std::function<void(const std::vector<int>&)> cb) {
        m_exportToProject = std::move(cb);
    }
    // Called when the user picks "Edit Sketch" from a sketch's right-click
    // menu. Routes to Application::editSketch which enters sketch mode on
    // that sketch - the only way to re-enter a sketch that was created in
    // a previous session.
    void setEditSketchCallback(std::function<void(int)> cb) { m_editSketch = std::move(cb); }
    // Called when the user picks "Export as SVG…" from a sketch's right-click
    // menu. Routes to Application::exportSketchAsSvg (1:1-mm polyline SVG for
    // laser / 2.5D CNC). Sketch-only by design - a File-menu export would also
    // catch non-planar geometry, which SVG can't represent.
    void setExportSketchSvgCallback(std::function<void(int)> cb) { m_exportSketchSvg = std::move(cb); }
    void setExportSketchDxfCallback(std::function<void(int)> cb) { m_exportSketchDxf = std::move(cb); }
    // Called when the user picks "Duplicate Sketch" - makes an independent copy.
    // Routes to Application::duplicateSketch.
    void setDuplicateSketchCallback(std::function<void(int)> cb) { m_duplicateSketch = std::move(cb); }
    // Called when the user picks "Combine sketches" - merges the selected
    // coplanar sketches into the first. Routes to Application::combineSketches.
    void setCombineSketchesCallback(std::function<void(const std::vector<int>&)> cb) {
        m_combineSketches = std::move(cb);
    }
    // Called when the user picks "Rotate About Axis…" from a construction
    // plane's right-click menu. Routes to Application, which opens the
    // rotate-plane-about-axis popup targeting the given plane id.
    void setRotatePlaneCallback(std::function<void(int)> cb) { m_rotatePlane = std::move(cb); }

    // Returns true if a body was deleted (caller must rebuild meshes)
    bool render();
    // Panel body without the "Items" window wrapper - for hosting inside
    // another container (im-touch shell right panel). Same return contract.
    bool renderContent();

private:
    Document* m_document = nullptr;
    SelectionManager* m_selection = nullptr;
    History* m_history = nullptr;
    std::function<void()> m_markDirty;
    std::function<void(int)> m_exportStl;
    std::function<void(const std::vector<int>&)> m_exportToProject;
    std::function<std::vector<std::string>()> m_exportFormats;
    std::function<void(const std::vector<int>&, const std::string&)> m_exportBodies;
    std::function<void(int)> m_editSketch;
    std::function<void(int)> m_exportSketchSvg;
    std::function<void(int)> m_exportSketchDxf;
    std::function<void(int)> m_duplicateSketch;
    std::function<void(const std::vector<int>&)> m_combineSketches;
    std::function<void(int)> m_rotatePlane;
    bool m_sketchModeActive = false;
    int m_activeSketchId = -1;
    int m_renamingId = -1;
    char m_renameBuffer[128] = {};
    // Selected body ids, rebuilt once at the top of render() - renderBodyRow
    // reads it per row instead of rescanning the whole selection.
    std::set<int> m_selectedBodyIdsFrame;
    bool m_showBodies = true;
    bool m_showSketches = true;
    bool m_showPlanes = true;
    bool m_bodyDeleted = false;
    bool m_hovered = false;   // panel hovered last frame (for touch long-press arming)
    // Auto-scroll: when the selected body / sketch changes (e.g. a viewport
    // pick), scroll its row into view. -1 = no pending scroll.
    int m_lastSelectedBodyId = -1;
    int m_lastSelectedSketchId = -1;
    // Anchor body for shift-click range selection in the Items panel. Set
    // whenever a plain click (no Ctrl, no Shift) selects a body.
    int m_anchorBodyId = -1;
    // "New folder…" submenu prompts for a name - kept across frames until the
    // user confirms / cancels via Enter / Esc. The body being moved is
    // remembered so we can assign it once the folder exists.
    bool m_newFolderPopupOpen = false;
    bool m_newFolderFocusInput = false; // first-frame focus only - else the
                                        // input steals focus from Create/Cancel
                                        // every frame and the popup locks up.
    char m_newFolderName[128] = {};
    // Bodies to move into the newly-created folder once its name is confirmed.
    // Empty = create the folder empty (e.g. "+ Folder" header button).
    std::vector<int> m_newFolderForBodyIds;

    // Click behaviour for the sketch / plane / axis rows: a plain click
    // selects just this item, Ctrl+click toggles it in or out of whatever is
    // already selected. Body rows keep their own version because they also
    // support Shift range-select. Without this the non-body rows always
    // REPLACED the selection, so Ctrl+clicking a sketch silently dropped the
    // bodies you had picked - while Ctrl+clicking a body afterwards did
    // extend, which is what made the behaviour look arbitrary.
    void applyRowClick(const SelectionEntry& entry);

    // Renders one body row (visibility + name + colour + context menu).
    // Pulled out of render() so it can be called both at the root level and
    // inside each folder's expanded content.
    bool renderBodyRow(int id, bool& colorChanged);
};

} // namespace materializr
