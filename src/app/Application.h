#pragma once
#include "modeling/MoveHoleOp.h"
#include "app/MoveFaceState.h"
#include "../platform_defs.h"

#include <memory>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <vector>
#include <functional>
#include <string>
#include <set>
#include <map>
#include <glm/glm.hpp>
#include "io/ImageDecode.h"   // DecodedImage — thumbnail peek results
#include "ui/UpdateChecker.h"
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>

#include "app/InteractiveOpController.h"
#include "app/FaceOpControllers.h"
#include "app/ExtrudeController.h"
#include "app/PushPullController.h"
#include "app/EdgeOpController.h"
#include "app/SplitController.h"
#include "app/LiveOpPreview.h"
#include "app/CylindricalPick.h"
#include <array>
#include "modeling/ExtrudeOp.h" // for ExtrudeMode
#include "modeling/SketchConstraints.h" // for ConstraintType (applySketchConstraint)
#include "modeling/Unfold.h" // for FlatPattern (m_unfoldPattern)
#include "modeling/TopoName.h" // for topo::Ref (m_threadFaceRef)
#include "viewport/SectionView.h" // SectionView::Result (async section compute)
#include "core/SheetSpec.h" // for SheetMaterial (m_unfoldMaterial)
#include "io/Settings.h" // for AppSettings::RecentProject (m_recentProjects)
#include <cstddef>   // size_t

// Global (non-namespaced) op, forward-declared for configureFaceOp's signature.
class MoveFaceOp;

namespace materializr {

struct AppSettings;
class Window;
class Viewport;
class Grid;
class ShapeRenderer;
class SketchRenderer;
class EdgeRenderer;
class BackgroundRenderer;
class ViewCube;
class Picker;
class Gizmo;
class SelectionHighlight;
class BoxSelect;
class SectionView;
class Toolbar;
class HistoryPanel;
class AboutDialog;
class WelcomeScreen;
class ShortcutsPanel;
class HelpPanel;
class MeasureTool;
class ItemsPanel;
class StatusBar;
class ThemeManager;
class PropertiesPanel;
class LandingPage;
struct ProjectSession;
class Sketch;
class SketchSolver;
class SketchTool;
struct PendingDimension;
class EventBus;
class PluginContext;

} // namespace materializr

class Document;
class History;
class SelectionManager;
class ThreadOp;
class PushPullOp;

namespace materializr {

struct ProjectHistory; // io/ProjectIO.h

class Application {
public:
    // `safeMode` is set by `--safe-mode` on the CLI; loadAppSettings honours
    // it by stomping the persisted "potentially-expensive" fields back to
    // known-safe defaults (MSAA off, mesh quality Low, default lights,
    // autosave off, auto-open-last-project off) and writing them out, so the
    // *next* normal launch is recovered without further action.
    explicit Application(bool safeMode = false, float uiScaleOverride = 0.0f);
    ~Application();

    void run();

private:
    void initImGui();
    void shutdownImGui();
    // Restore the default panel/dock layout live (Settings → Appearance).
    void resetLayout();
    // Locate a TTF from assets/fonts across AppImage / dev / Windows-zip
    // layouts; "" when missing. Used by the UI font load + the Text tool.
    std::string resolveBundledFont(const std::string& fname) const;
    void renderTextToolPanel(); // sketch Text tool settings (floating)
    // Transient centered toast (threads-last guidance etc.) — shown for a
    // few seconds, doesn't fight the per-frame status-bar message.
    void showThreadsLastToast();
    void showToast(const std::string& text, double seconds = 4.0);
    void renderTransientToast();
    std::string m_toastText;
    double m_toastExpiry = 0.0;
    void renderSvgToolPanel();  // SVG placement settings (floating)
    void renderAirfoilToolPanel(); // airfoil chord / points / trailing edge
    std::string m_airfoilSource;   // path the profile came from, for re-reading
    int  m_airfoilPointBudget = 40; // spline control points per surface
    void renderMirrorToolPanel(); // interactive mirror line controls (floating)
    void renderOffsetToolPanel(); // offset distance / side / corner style (floating)
    // Camera-upright default rotation for Text/SVG placement.
    void seedUprightPlacementAngle();
    void initRenderers();
    void setupCommands();
    void beginFrame();
    void endFrame();
    void renderSplashFrame(const char* status);
    void noteHeavyPumpGap();
    // Self-contained progress frame for long operations, rendered between main
    // frames (via m_deferredHeavyTask). Returns true if the user hit Cancel.
    // fraction<0 = indeterminate; fraction<=0 also resets the cancel latch.
    bool renderProgressFrame(float fraction, const char* label);
    // A left→right sweeping marquee bar at the current ImGui cursor (shared by
    // the projection progress overlay and the thread-cutting modal).
    void drawIndeterminateBar();
    bool m_progressCancelled = false;
    // Set once the first splash frame has been rendered to BOTH swap-chain
    // buffers (see renderSplashFrame) — kills the intermittent black flash from
    // the undefined back buffer being presented before the first swap.
    bool m_splashPrimed = false;
    // A heavy op deferred from a controller commit to run between frames, where
    // renderProgressFrame can pump its own frames without nesting ImGui frames.
    std::function<void()> m_deferredHeavyTask;
    // What the UI keep-alive should redraw while a heavy task blocks the main
    // thread (see core/UiKeepAlive.h). The task updates these as it advances —
    // the history replay sets a real step fraction — and the keep-alive repaints
    // from wherever the block happens to be, including from inside OCCT.
    float m_heavyProgressFrac = -1.0f;      // <0 = indeterminate
    std::string m_heavyProgressLabel;
    // Diagnostics for the main-loop stall watchdog: how many times the
    // keep-alive pumped and redrew during the last heavy task, and whether the
    // iteration it is about to judge ran one at all.
    int  m_heavyPumps = 0;
    int  m_heavyDraws = 0;
    bool m_heavyRanThisIter = false;
    // Longest stretch of the last heavy task with NO event pump — the single
    // number that says whether the window could have been flagged unresponsive
    // (desktops give up at around 5 s). Reported by the stall watchdog so a
    // slow machine's log answers that question directly instead of inviting
    // another round of guessing.
    std::chrono::steady_clock::time_point m_lastHeavyPump{};
    int  m_heavyWorstGapMs = 0;
    // Earliest time the keep-alive may draw another progress frame. Pumping the
    // event queue is nearly free and happens every time; DRAWING one is not,
    // and its cost is not ours to predict: with the window unmapped or occluded
    // the compositor sends no frame callback and Mesa's swap sits out its ~1 s
    // fallback timeout. A frame per history step cost a full second each that
    // way -- a 16 s project load stretched past 70 s. So the draw rate backs
    // off from its own measured cost, and answering the compositor never
    // depends on it.
    std::chrono::steady_clock::time_point m_nextHeavyDraw{};
    // Idle-render throttle: counts down frames to render after the last event
    // or active-work wakeup. Zero = skip the frame and sleep for the next event.
    int m_wakeFrames = 0;
    // True only while a load is tessellating in the deferred slot: tells
    // rebuildMeshes to pump a per-body progress frame (safe between frames).
    bool m_pumpMeshProgress = false;
    // Launch-time update check, run on a worker so a slow/unreachable network
    // can't freeze startup (it was a synchronous call with a 10 s timeout —
    // the real cause of the "not responding" on launch). Polled each frame.
    std::future<materializr::UpdateChecker::Result> m_updateCheckFuture;
    void renderDockspace();
    void renderViewport();
    void renderMenuBar();
    // Menu bodies shared by the desktop menu bar and the im-touch overflow.
    // withSettings=false drops the nested "Settings..." item — the touch
    // overflow exposes Settings at the top level, so it shouldn't also nest it
    // under File. The desktop menu bar keeps it (default true).
    void renderFileMenuItems(bool withSettings = true);
    void renderEditMenuItems();
    // Derived construction Plane/Axis items — the popup behind the touch rail's
    // "Construct" group. Selection-aware (mirrors Toolbar's Add Plane/Axis).
    void renderConstructionMenuItems();
    void renderViewMenuItems();
    void renderHelpMenuItems();
    // Per-layout chrome (src/app/layout/<name>/). Modern: top app bar + tool
    // rail + right panel (layout/modern/ModernLayout.cpp). im-touch: near-zero
    // chrome, floating overlays (layout/imtouch/ImTouchLayout.cpp). Both
    // compute the viewport rect renderViewport() pins to. Classic's menu bar
    // is renderMenuBar() above (layout/classic/ClassicLayout.cpp).
    void renderModernLayout();
    // Modern panel/viewport pop-in/out edge tabs — submitted AFTER the
    // viewport (see run()) so they sit on top of it, not under it.
    void renderModernEdgeTabs();
    void renderImTouchLayout();
    // Polygon in the tool rail opens a side-count popout (matching the
    // classic sketch toolbar). Shared by the modern rail and im-touch bar.
    void renderRailPolygonSidesPopup(bool clicked);
    void renderTouchOverflowPopup(); // shared ⋯/☰ menu popup (modern + im-touch)
    // Undo/redo with the sketch-edit cascade (shared by the Edit menu, the
    // touch shell's top bar, and nothing else — the Ctrl+Z shortcut has its
    // own copy in handleShortcuts pending a merge).
    // Sketch id mutated by a history step (SketchTransformOp/SketchEditOp),
    // or -1 — so undo/redo outside sketch mode can re-cascade the driven body.
    int sketchIdEditedBy(const Operation* op) const;
    void undoWithCascade();
    void redoWithCascade();
    // Sketch-aware undo for the touch shell's top-bar Undo button: in sketch
    // mode it mirrors the Ctrl+Z sketch behaviour (cancel an in-progress shape
    // first, then undo committed sketch edits but NEVER past the sketch's own
    // entry into history — rolling the host body back while the sketch renders
    // against it crashes). Outside sketch mode it's plain undoWithCascade().
    // touchCanUndo() is the matching enabled-state for the button.
    void touchUndo();
    bool touchCanUndo() const;
    void renderInteractionsPanel();
    void renderSettings();
    void loadAppSettings();   // restore persisted preferences at startup
    void saveAppSettings();   // write persisted preferences
    void exportSettings();    // Settings dialog → Export…  (write current prefs as JSON)
    void importSettings();    // Settings dialog → Import…  (load prefs from JSON, apply live)
    AppSettings currentSettings() const;     // gather current prefs into a struct
    void        applyAppSettings(const AppSettings& s); // push prefs onto the live members
    void renderMirrorPopup();
    void renderUpdatePopup();
    void renderMultiTransformPanel();
    void applyMultiBodyRotation();
    void renderScalePanel();
    void handleToolAction(int action);
    void handleShortcuts();
    void handleViewCubeAction(int action);
    void rebuildMeshes();
    glm::vec2 screenToSketch(float sx, float sy, float vpW, float vpH);

    void importStepFile();
    void exportStepFile();
    // Per-body STL export: opens a save dialog with the body's current name
    // (from the Items panel) as the default filename and writes JUST that
    // body's mesh. Triggered from the viewport right-click menu and the
    // Items-panel context menu so users can dump individual parts of a
    // multi-body project without juggling visibility for the file-menu
    // "Export STL" (which writes every visible body to one file).
    void exportBodyAsStl(int bodyId);
    // Export the given bodies to any format the plugin registry can
    // write, as ONE file with their relative positions intact (a
    // print-in-place assembly is several bodies that must stay put).
    void exportBodiesAs(const std::vector<int>& bodyIds,
                        const std::string& formatName);
    void exportSketchAsSvg(int sketchId);
    void exportSketchAsDxf(int sketchId);
    // Zoom-fit the camera onto the selection (or all visible bodies when
    // nothing is selected). Bound to F and View > Frame Selection — the menu
    // item is the touch path.
    void frameSelection();
    // Delete the sketch tool's selected elements (points + lines), history-
    // wrapped, and sweep orphan points. Bound to Delete and the sketch context
    // bar's Delete button — the latter is the touch path.
    void deleteSelectedSketchElements();
    void saveProject();
    std::string projectDisplayName() const;    // name or basename or "New project"         // Save dialog (Save As behavior)
    void saveProjectQuick();    // Save to current path if known, else falls through to saveProject
    // Register the sketch currently being drawn into the Document so a save
    // taken mid-sketch actually contains it. Idempotent; see the definition.
    void flushActiveSketchToDocument();
    // Call after a save that included a flushed sketch actually succeeds on
    // disk — drops the now-redundant crash-recovery draft. See the definition
    // for why this is separate from flushActiveSketchToDocument().
    void acknowledgeSketchDraftCommitted();
    // Render the "home view" of the project (visible bodies only — no
    // sketches, planes, axes, grid or overlays; reset isometric camera,
    // zoom-fit) into an offscreen 512px square and PNG-encode it. Embedded
    // in the save file as the landing-page tile. False when there is nothing
    // to show (no visible bodies) — the save then simply carries no thumbnail.
    // Main thread only (needs the GL context).
    bool captureProjectThumbnailPNG(std::vector<uint8_t>& pngOut);
    // Landing page: rebuild the tile list from m_recentProjects (peeking each
    // file's embedded thumbnail into a GL texture) and show it. Rendered by
    // renderLandingPage() each frame; actions (new/open/dismiss) are handled
    // there. The page hides itself whenever a project load succeeds.
    // `fromStartup` hides the header's close button: at startup there is no
    // session behind the page to go back to (closing would equal New Project).
    void showLandingPage(bool fromStartup = false);
    // File → Home Screen. Leaving for the home screen IS leaving the project:
    // the save/discard prompt fires HERE (not later when a tile is clicked),
    // the project closes, and the page shows over an empty workspace.
    void goToHomeScreen();
    void renderLandingPage();
    // True while the landing page owns the screen. Layout chrome (panels,
    // toolbars, shells, edge tabs, status bar) checks this and stands down —
    // relying on z-order alone is fragile because any later focus event can
    // lift a chrome window above the page.
    bool landingPageUp() const;

    // ── Project sessions (tabs) ──────────────────────────────────────────
    // Make m_sessions[idx] the active project: stash the outgoing session's
    // working state (path/name/dirty/camera), repoint the m_document /
    // m_history / m_selection mirrors, restore the incoming session's state
    // and rewire every consumer that caches document pointers.
    void adoptSession(size_t idx);
    // The two halves of adoptSession. stash writes the working copies back
    // into the active session; apply repoints the mirrors at m_sessions[idx],
    // restores its state and rewires consumers. closeSession uses apply alone
    // (the outgoing session is being destroyed — nothing to stash into).
    void stashActiveSessionState();
    void applySessionState(size_t idx);
    // Forced (undebounced) recovery snapshot of the active session; called
    // right before a tab deactivates so inactive tabs always have an exact
    // crash-recovery file.
    void writeSessionRecoveryNow();

    // ── Tab lifecycle (UI lands in phase 3; menu + Ctrl+Tab already wired) ──
    // Fresh empty session; does NOT switch to it. Recovery indices recycle
    // (smallest unused, capped at the scanner's 0..15 namespace) so a closed
    // tab's snapshot file can never fall outside the startup scan.
    size_t createSession();
    int nextFreeRecoveryIndex() const;
    // Full user-visible switch: refuses mid-sketch and mid-thread-recut
    // (toast explains), cancels live previews, shelves the outgoing tab's GPU
    // meshes (all platforms), swaps state, queues the incoming rebuild.
    bool switchToSession(size_t idx);
    // Tear down a session: recovery file cleared, session destroyed; the last
    // tab is replaced by a fresh empty one (there is always >= 1). Unsaved-
    // changes prompting is the CALLER's job (route through guardedOpen).
    void closeSession(size_t idx);

    // ── Tab UI (phase 3) ─────────────────────────────────────────────────
    // Display label for a tab: explicit name → file basename → "Untitled".
    std::string sessionDisplayLabel(size_t i) const;
    // Unsaved-changes state, valid for active AND inactive sessions.
    bool sessionDirty(size_t i) const;
    // Make tab i active if it isn't (switch may refuse → false + toast).
    // Every per-tab menu action funnels through this so Save/Close always
    // operate on the ACTIVE session's machinery.
    bool activateTabFor(size_t i);
    // Shared per-tab dropdown body: Save / Save As / Close Tab.
    void renderTabMenuItems(size_t i);
    // Render-pass split point: below this draws BEHIND the bodies (reference
    // photos), at/above draws IN FRONT (construction planes, axes).
    static constexpr int kBodyPassPriority = 500;
    // Does this body id still resolve in the ACTIVE document? Document::getBody
    // throws on a miss, and callers that only want a yes/no answer kept writing
    // their own try/catch (or, worse, forgot to — see the sketch-attachment
    // gating, which treated a dead id as "still attached").
    bool bodyExists(int bodyId) const;
    // Create a fresh tab AND make it active; on a refused switch (mid-sketch
    // etc.) the just-created session is cleaned up again. False = nothing
    // happened (the refusal already toasted).
    bool openNewTab();
    // True when the active tab is an untouched empty workspace (no project,
    // no geometry, no history) — i.e. safe to load into without displacing
    // anything the user still wants.
    bool activeSessionIsScratch() const;
    // The open tab already holding this project, or m_sessions.size() for
    // none. Compares the ACTIVE tab against m_currentProjectPath — an inactive
    // session's own `projectPath` is only refreshed when it's stashed on the
    // way out — the same rule saveAppSettings uses to write the tab list.
    size_t sessionForProjectRef(const std::string& ref) const;
    // One project, one tab. If `ref` is already open, focus that tab and
    // return true; the caller must then NOT load it again. Nothing checked
    // this before, so re-opening a project you already had open (a home-screen
    // tile, an Open Recent entry) silently made a SECOND tab of the same file
    // — and the duplicate went on to be written into the session list and
    // faithfully restored on the next launch, which looked like a restore bug.
    // Discards the empty tab a caller may have just created for the load.
    bool focusExistingProject(const std::string& ref);
    // Reopen a previous session's projects, one per tab, and focus the tab
    // that was in front. Runs from the deferred startup slot when "open last
    // project on launch" is on. Missing projects are skipped, not fatal.
    void restoreSessionTabs(const std::vector<std::string>& paths,
                            size_t activeIndex);
    // The "+" button's dropdown, shared by all three layouts: New Project /
    // Open Project... / Open Recent — every flavor lands in its own new tab.
    void renderNewTabMenuBody();
    // Classic: dock-style tab bar pinned to the top of the Viewport window —
    // deliberately NOT a dock node, so tabs can't be dragged into the panel
    // docks (Steve: "only bound to the viewport to keep it from getting
    // weird"). Also hosts the trailing "+".
    void renderViewportTabBar();
    // Im-touch: the open-projects sheet the project-name chip opens.
    void renderTouchTabsSheet();
    // Set when the active session changed OUTSIDE the classic tab bar
    // (Ctrl+Tab, menus, a refused switch snapping back) so the bar re-asserts
    // the visual selection exactly once instead of fighting user clicks.
    bool m_tabSelectionSync = true;
    // One-shot: raise the Settings window on the next render. Set by every
    // explicit open request — an already-open window buried under the home
    // page otherwise never surfaces.
    bool m_settingsRaise = false;
    // (Re)apply the current mirrors to everything that holds a Document /
    // History / SelectionManager pointer: panels, event-bus binds, plugin
    // context, per-History callbacks. Called from the ctor and every adopt.
    void wireDocumentConsumers();
    ProjectSession& currentSession() { return *m_sessions[m_activeSession]; }
    // Landing-page tile context menu: load a recent project's BAKED bodies
    // into a scratch Document and export them as STEP/STL. Deliberately not
    // parametric — final shapes only.
    void exportRecentProjectAs(const std::string& ref, const std::string& name,
                               bool asStl);
    // Thumbnail side-cache (SDL pref path /thumbs, keyed by hash of the
    // recent ref). Exists for refs the landing page can't peek directly —
    // Android content:// documents — and doubles as a fallback everywhere.
    // Written on every successful save and on mobile recent-opens.
    void cacheProjectThumbnail(const std::string& ref,
                               const std::vector<uint8_t>& png);
    bool readCachedThumbnail(const std::string& ref, std::vector<uint8_t>& png);
    // Cache entry at least as new as the project file it came from.
    bool thumbCacheFresh(const std::string& ref) const;

    // Off-thread thumbnail peeks for landing tiles the cache couldn't serve.
    // ProjectIO::peekThumbnail gunzips an ENTIRE project to read one line, so
    // a full recents list of these blocked startup for seconds; the worker
    // decodes to RGBA and the main thread uploads (GL is not shareable here).
    struct ThumbResult { std::string ref; DecodedImage img; };
    struct ThumbJob {
        std::mutex mutex;
        std::vector<ThumbResult> done;
        std::atomic<bool> cancel{false};
    };
    void startThumbnailPeeks(const std::vector<std::string>& refs);
    void drainThumbnailPeeks();   // main thread, while the landing page is up
    std::shared_ptr<ThumbJob> m_thumbJob;

    // Cross-project parts: scratch-load `ref` and open the "Import Parts"
    // modal listing its bodies + sketches. `intoNewProject` (landing-tile
    // flow) clears the workspace first; otherwise parts land in the current
    // document. Copies are BAKED (bodies) / independent (sketches severed
    // from their source body) — nothing parametric crosses files.
    void openPartsPicker(const std::string& ref, const std::string& name,
                         bool intoNewProject);
    void renderPartsPickerDialog();
    // Items-panel body context menu: write one body into a fresh project
    // file (and add it to Open Recent so it shows on the landing page).
    // Open the given bodies in a NEW TAB as an unsaved project — the "use
    // this part elsewhere" flow. Not a file write: you see what you got
    // first, and save it (or not) like any other project.
    void exportBodiesToNewProject(const std::vector<int>& bodyIds);
    void loadProject();         // File dialog → loadProjectAt
    // Load a project file directly by path. Used by loadProject() and by the
    // "auto-open last project on launch" path.
    bool loadProjectAt(const std::string& path);
    // Like loadProjectAt but shows a loading bar and tessellates up front,
    // pumping frames so the window stays responsive (no OS "not responding").
    // Must run from the deferred-heavy-task slot (between frames), never inside
    // a live ImGui frame. Used for the auto-open-on-launch path.
    void loadProjectWithProgress(const std::string& path);

    // Open Recent: a persisted, most-recent-first list of projects. `ref` is a
    // filesystem path (desktop) or a SAF content:// URI (Android); `name` is the
    // display label. addRecentProject records a successful open/save;
    // openRecentProject re-opens one (resolving the URI on Android).
    void addRecentProject(const std::string& ref, const std::string& name);
    void openRecentProject(const AppSettings::RecentProject& r);
    void removeRecentProject(const std::string& ref);
    // Run `doOpen` now if the document is clean; otherwise route through the
    // unsaved-changes save prompt and run it once that resolves. All project
    // opens (dialog + Open Recent) go through here so none silently discard work.
    void guardedOpen(std::function<void()> doOpen);
    // File → Close Project. Prompts to save if dirty (unless autosave is on),
    // then clears the document/history/selection and resets the project path.
    void closeProject();
    void doCloseProject();      // the actual clear; called from closeProject + save prompt
    // Snapshot the operation history (parameters + per-step body diffs) for the
    // project file, and rebuild a replayable history from a loaded project.
    // Cancels live interactive previews first by default: the snapshot seeds
    // from the CURRENT doc body, so an uncommitted preview (e.g. a shell being
    // dragged) would otherwise bake into the previous step's snapshot with no
    // op behind it — a hollow body that reloads un-editable and can't be
    // re-shelled. The background recovery autosave passes false so it doesn't
    // yank an active preview out from under the user mid-drag.
    ProjectHistory captureProjectHistory(bool cancelPreviews = true);
    void rebuildHistoryFromProject(const ProjectHistory& hist,
                                   const std::string& savedByVersion = "");

    // Dirty tracking + unsaved-changes prompt
    bool isDirty() const;
    void markDirty();           // for changes that don't go through History
    // The single caller of materializr::setCurrentUnit. Guards the ImGui side
    // effect so it is safe during settings-apply, which runs before a context
    // exists.
    void applyDisplayUnitChange(int unit);
    void markSaved();
    void renderSavePrompt();
    void requestClose();        // called when the user clicks the window X

    // Merge coplanar sketches into the first (Items panel). Non-coplanar ones
    // are skipped; refuses (toast) if fewer than two end up coplanar.
    void combineSketches(const std::vector<int>& ids);
    // Make an independent copy of a sketch (Items panel → Duplicate Sketch).
    void duplicateSketch(int sketchId);
    void enterSketchMode();
    void enterSketchOnPlane(const gp_Pln& plane);
    void enterSketchOnFace(const TopoDS_Face& face, int sourceBodyId = -1);
    void editSketch(int sketchId);
    // True centre of a threaded host body on `pln` (the Thread step's axis
    // piercing the plane), in the plane's 2D frame. False when the body has
    // no enabled Thread step whose axis is perpendicular to the plane.
    bool threadAxisCenter2d(int bodyId, const gp_Pln& pln,
                            glm::vec2& out) const;
    // Body owning a planar face coplanar with pln — re-adopts a severed
    // sketch-body link (sourceBody saved as -1).
    int findBodyUnderRegionlessPlane(const gp_Pln& pln) const;
    void extrudeSketchById(int sketchId, ExtrudeMode mode = ExtrudeMode::NewBody);
    // Interactive subtract of a single sketch region from the body the sketch
    // was drawn on (red preview). Used by the region toolbar where viewport
    // clicks land, since clicking a sketch selects a region, not the whole sketch.
    void subtractSketchRegion(int sketchId, int regionIndex);
    TopoDS_Face buildSketchProfileFace(const Sketch& sketch) const;
    void exitSketchMode();

    // Snapshot the active sketch, run `mutator`, and if the element count
    // changed, push a SketchEditOp so the user can Ctrl+Z drawing actions.
    void recordSketchMutation(const std::function<void()>& mutator);

    // Touch sketch context-bar actions for chain tools (line/spline):
    //   Back   — drop the most recently placed segment / control point, keep
    //            drawing the chain.
    //   Cancel — discard the whole chain being drawn (every segment placed
    //            since the chain started), then end placement.
    void sketchChainBack();
    void sketchChainCancel();

    // Flag a single body as needing a mesh refresh. Call sites that already
    // know which body changed should prefer this over `m_meshesDirty = true`
    // — the next rebuildMeshes pass updates just this body via setBodyMesh,
    // leaving the rest of the (potentially 100+) bodies untouched. Critical
    // for push/pull preview smoothness on complex projects.
    void markBodyDirty(int bodyId) { if (bodyId >= 0) m_dirtyBodyIds.insert(bodyId); }

    // If `sketchId`'s Sketch has a sourceBodyId but no sourceFace (typical
    // for a sketch reloaded from a project file), walk the source body's
    // faces and bind the planar face whose plane coincides with the sketch's
    // plane. Without a sourceFace, Sketch::buildRegions doesn't union the
    // host face's wires (holes, fillets) into the sketch — and a push/pull
    // of a "circle around an existing hole" wrongly produces a solid bar.
    void ensureSketchSourceFace(int sketchId);

    // Re-adopt the body a free-floating sketch sits flat ON. When a sketch has
    // no body link (never linked, e.g. drawn on a construction plane and used
    // to cut a hole) but its plane coincides with a visible body's planar face
    // and the region overlaps that face, PUSH/PULL fuses/cuts that body in
    // place instead of spawning a separate solid that overlaps and z-fights
    // it. (Extrude keeps its always-new-body semantics — this helper is only
    // consulted by beginPushPull.) Returns the body id to adopt, or -1 if the
    // region isn't sitting on a body face (a genuine free-space sketch —
    // leave it free-floating). `region` is the sketch-region face; `plane` is
    // the sketch plane.
    int findBodyUnderRegion(const TopoDS_Face& region, const gp_Pln& plane) const;

    // True if `face` is geometrically flat (a real plane, or a flat trapezoid on
    // a ruled/BSpline surface). Uses GeomLib_IsPlanarSurface, so it accepts
    // flat-but-not-Geom_Plane faces and rejects genuinely curved ones (cylinder /
    // sphere / fillet). Gates push/pull off rounded faces (fillets freak it out).
    bool faceIsPlanar(const TopoDS_Face& face) const;

    // Apply a sketch constraint of the given type to the current
    // SketchTool element selection. Inspects the selection counts to decide
    // which arity to use (e.g. Coincident chains pairs of selected points;
    // Parallel pairs each line with the first one). No-op if the selection
    // doesn't match the constraint's requirements. Routed from the toolbar
    // Constraints section; constraints are always opt-in.
    void applySketchConstraint(ConstraintType type);

    // Commit the Dimension tool's resolved pending dimension: one undoable
    // constraint add (or value+label update when the same pair is already
    // dimensioned), then open the ##DimEdit popup on it for value entry.
    void applyPendingDimension();

    // Sketch-space auto anchor of a dimension's label: line/pair midpoint,
    // circle/arc center, or the midpoint of the point-to-line perpendicular
    // foot segment. Label offsets are stored relative to this.
    glm::vec2 dimensionAutoAnchor(const PendingDimension& pd) const;

    // Align the orbit camera to look straight at the active sketch's plane in ortho.
    // Called when entering sketch mode / editing an existing sketch.
    void alignCameraToActiveSketch();

    // Sketch region hover/pick + Push/Pull. buildIfCold=false makes the pick
    // SKIP sketches whose region cache would need the heavy OCCT fuse —
    // required on the per-frame hover path (a cold complex sketch would
    // freeze the app on the first mouse move after being unhidden); click
    // frames pass true and build as before.
    struct SketchRegionHit { int sketchId = -1; int regionIndex = -1; glm::vec3 worldPoint{0.0f}; };
    SketchRegionHit pickSketchRegion(float screenX, float screenY,
                                     float vpW, float vpH,
                                     bool buildIfCold = true) const;
    // Thin delegates — the tool lives in PushPullController now (slice 2).
    // NB: no blanket m_meshesDirty here. Push/Pull's preview marks only the
    // bodies it touched (markPreviewDirty) — a full rebuild per frame is both
    // the dense-project perf hazard and what erases the ghost tool volume.
    void beginPushPull() {
        cancelActiveIops();
        m_ppCtl.beginPushPull(iopContext());
    }
    // applySnap=false bypasses the grid snap for that update — the stepper
    // buttons are an explicit fine override (a 0.1 nudge under a 1 mm grid must
    // actually move), so they call updatePushPull(false).
    void updatePushPull(bool applySnap = true) {
        m_ppCtl.updatePushPull(iopContext(), applySnap);
    }
    void commitPushPull() { m_ppCtl.commit(iopContext()); m_meshesDirty = true; }
    void cancelPushPull() { m_ppCtl.cancel(iopContext()); m_meshesDirty = true; }
    // ── Move Face (face transform → body follows via loft; see MoveFaceOp) ──
    // The face transform this gesture applies (Move / Rotate / Scale share the
    // same loft engine + deferred silhouette; only the gizmo + drag math differ).
    using FaceXform = materializr::FaceXform;
    // Thin delegates — the tool lives in MoveFaceController now (slice 2b).
    void beginMoveFace(FaceXform kind = FaceXform::Translate) {
        cancelAllInteractivePreviews();
        m_moveFaceCtl.beginMoveFace(iopContext(), kind);
    }
    // Configure a MoveFaceOp with the current gesture's kind + params, and test
    // whether the gesture has anything to apply (defined in the .cpp where the
    // op type is complete).
    bool faceXformNontrivial() const { return m_moveFaceCtl.faceXformNontrivial(); }
    // Total tilt = the live ring drag composed onto the accumulated tilts.
    glm::mat3 faceRotTotal() const { return m_moveFaceCtl.faceRotTotal(); }
    void bakeFaceRotationDrag() { m_moveFaceCtl.bakeFaceRotationDrag(); }
    void updateMoveFace() { m_moveFaceCtl.updateMoveFace(iopContext()); }
    void commitMoveFace() { m_moveFaceCtl.commitMoveFace(iopContext()); }
    void cancelMoveFace() { m_moveFaceCtl.cancelMoveFace(iopContext()); }
    void moveFaceSlideSketches(const glm::vec3& v) {
        m_moveFaceCtl.moveFaceSlideSketches(iopContext(), v);
    }
    materializr::MoveFaceController m_moveFaceCtl;
    void beginInteractiveExtrude(const TopoDS_Shape& profile,
                                 ExtrudeMode mode = ExtrudeMode::NewBody,
                                 int targetBody = -1,
                                 int sourceSketchId = -1);

    // Re-execute every enabled ExtrudeOp in history that was originally built
    // from this sketch — called when the user edits a constraint value via
    // the Properties → Constraints panel or the History → Apply Changes
    // path. Downstream ops (Fillet, Pattern, Push/Pull face-references) are
    // intentionally NOT re-run; that's a separate toponaming-heavy future
    // release. Result: simple "sketch -> extrude -> done" chains follow the
    // sketch immediately; chained workflows leave downstream ops on their
    // old body shape (user re-does manually).
    void cascadeFromSketchEdit(int sketchId);
    // Map each sketch id to the body ids it drives (created/modified through a
    // sketch-sourced extrude / push-pull). Used by the gizmo commit to tell a
    // unison move (body + its driving sketch) from a lone move that de-links.
    // MEMOIZED on History::revision(): the Properties panel asks for the link
    // hint every frame while a body/sketch is selected (the normal working
    // state), and rebuilding this walks the whole history + captureDiff per
    // op — hundreds of map/set node allocations per frame on a long history.
    const std::map<int, std::set<int>>& sketchBodyLinks() const;
    // Human-readable parametric-link summary for the Properties panel: for a body
    // (isBody=true) which sketch drives it, for a sketch which body it drives, plus
    // whether the link is live or was broken by an independent 3D move. "" = none.
    std::string linkHintFor(bool isBody, int id) const;
    // True if `bodyId` can be safely rebuilt by re-running history from its
    // sketch (`viaSketchId`) — i.e. nothing but that sketch's own extrude/
    // push-pull touches it. A fillet/chamfer/boolean/other feature downstream
    // can't re-bind after the geometry moves, so those bodies must move rigidly
    // (and de-link) instead of re-deriving.
    bool bodySafelyRederivable(int bodyId, int viaSketchId) const;
    // sketchBodyLinks() memo — see its declaration. ~0u forces the first build.
    mutable std::map<int, std::set<int>> m_linkMapCache;
    mutable unsigned m_linkMapRevision = ~0u;
    // Re-establish the parametric link of a detached sketch (Properties-panel
    // "Re-link"): clears the detached flag so editing the sketch drives its body
    // again. isBody=true re-links every detached sketch driving that body.
    // Geometry is left as-is — re-link resumes parametric control, it doesn't move.
    void relinkSketch(bool isBody, int id);
    void updateInteractiveExtrude(bool applySnap = true);
    void commitInteractiveExtrude();
    void cancelInteractiveExtrude();

    std::unique_ptr<Window> m_window;
    std::unique_ptr<Viewport> m_viewport;
    std::unique_ptr<Grid> m_grid;
    std::unique_ptr<ShapeRenderer> m_shapeRenderer;
    std::unique_ptr<SketchRenderer> m_sketchRenderer;
    std::unique_ptr<EdgeRenderer> m_edgeRenderer;
    std::unique_ptr<BackgroundRenderer> m_backgroundRenderer;
    std::unique_ptr<ViewCube> m_viewCube;
    std::unique_ptr<Picker> m_picker;
    std::unique_ptr<Gizmo> m_gizmo;
    std::unique_ptr<SelectionHighlight> m_selectionHighlight;
    std::unique_ptr<BoxSelect> m_boxSelect;
    std::unique_ptr<SectionView> m_sectionView;
    // The open projects ("tabs"). Sessions OWN the document/history/selection;
    // the raw pointers below are mirrors into m_sessions[m_activeSession],
    // repointed by adoptSession() so the existing ~900 `m_document->` sites
    // compile (and stay tab-correct) unchanged. Declared HERE, where the old
    // unique_ptrs sat, so destruction order relative to the renderers and the
    // event bus is exactly what it was single-session.
    std::vector<std::unique_ptr<ProjectSession>> m_sessions;
    size_t m_activeSession = 0;
    Document* m_document = nullptr;
    History* m_history = nullptr;
    SelectionManager* m_selection = nullptr;
    std::unique_ptr<EventBus> m_eventBus;
    std::unique_ptr<PluginContext> m_pluginContext;

    // UI panels
    std::unique_ptr<Toolbar> m_toolbar;
    std::unique_ptr<HistoryPanel> m_historyPanel;
    std::unique_ptr<ItemsPanel> m_itemsPanel;
    std::unique_ptr<StatusBar> m_statusBar;
    std::unique_ptr<ThemeManager> m_themeManager;
    std::unique_ptr<PropertiesPanel> m_propertiesPanel;
    std::unique_ptr<AboutDialog> m_aboutDialog;
    std::unique_ptr<WelcomeScreen> m_welcomeScreen;
    std::unique_ptr<LandingPage> m_landingPage;
    // Parts-picker modal state (see openPartsPicker). The scratch document
    // pins the source shapes alive until the modal resolves.
    std::shared_ptr<Document> m_partsPickerDoc;
    std::string m_partsPickerSource;              // display name
    bool m_partsPickerOpen = false;
    bool m_partsPickerIntoNew = false;
    std::vector<std::pair<int, bool>> m_partsPickerBodies;   // id, checked
    std::vector<std::pair<int, bool>> m_partsPickerSketches; // id, checked
    std::unique_ptr<ShortcutsPanel> m_shortcutsPanel;
    std::unique_ptr<HelpPanel> m_helpPanel;
    std::unique_ptr<MeasureTool> m_measureTool;

    // Update-check popup state (Help → Check for Updates).
    bool m_showUpdatePopup = false;
    bool m_updateChecked = false;
    std::string m_updateCurrent;
    std::string m_updateLatest;
    std::string m_updateMessage;
    std::string m_updateReleaseUrl;
    bool m_updateAvailable = false;

private:
    // Sketch
    std::shared_ptr<Sketch> m_activeSketch;
    // Deferred "before" snapshot from a line-chain anchor click (first click,
    // only the start point placed). Held so the first segment's undo step
    // absorbs the anchor into ONE step; see recordSketchMutation.
    std::shared_ptr<Sketch> m_deferredSketchBefore;
    size_t m_deferredSketchBeforeSig = 0;
    Sketch* m_deferredSketchOwner = nullptr;
    // Snapshot taken at left-mouse-down in Select mode so a point/line drag
    // (which only moves positions, no structural change) can be committed to
    // history on mouse-up.
    std::shared_ptr<Sketch> m_sketchDragBefore;

    // Sketch Move/Rotate gizmo (drawn on the selection centroid in Select mode):
    // axis arrows + free-move dot + rotate ring. Held-drag — clicking a handle
    // arms the corresponding op, releasing commits. Rotate also pops a small
    // type-in panel on release so the angle can be set exactly.
    enum class SketchGizmoHandle { None, MoveX, MoveY, MoveFree, Rotate };
    SketchGizmoHandle m_sketchGizmoHandle = SketchGizmoHandle::None;
    glm::vec2 m_sketchGizmoCenter{0.0f};   // centroid at drag start (rotate pivot)
    glm::vec2 m_sketchGizmoAnchor{0.0f};   // cursor sketch pos at drag start
    std::shared_ptr<Sketch> m_sketchGizmoBefore;
    std::vector<std::pair<int, glm::vec2>> m_sketchGizmoOriginals;
    // Rotate handle: snap drag to 15°; on release enter "adjusting" mode where
    // the popup is shown with the current angle pre-filled. Apply (or Enter)
    // commits the typed angle; Cancel / Esc reverts.
    bool m_sketchGizmoRotateAdjusting = false;
    float m_sketchGizmoRotateDegrees = 0.0f;
    char m_sketchGizmoRotateBuf[32] = "0.0";
    // Screen-space anchor for the rotate adjust popup, updated each frame the
    // gizmo is on screen so the popup tracks the centroid if the camera moves.
    glm::vec2 m_sketchGizmoAdjustAnchor{0.0f};

    // Sketch box-select: when in Select mode and the user clicks on empty space,
    // begin a rectangle drag; on release, sketch elements whose screen-space
    // projection lies inside the rectangle are added to the selection. Reuses
    // the shared m_boxSelect overlay so the rectangle visuals are identical to
    // the 3D mode's.
    bool m_sketchBoxSelectActive = false;

    // Right-click in the sketch viewport with at least one element selected
    // opens a context menu (currently: Add Constraint ▸ submenu). Set by the
    // input handler; consumed by the popup-render block on the next frame so
    // ImGui's OpenPopup happens inside the same window stack as the popup.
    bool m_sketchCtxMenuPending = false;

    std::unique_ptr<SketchSolver> m_sketchSolver;
    std::unique_ptr<SketchTool> m_sketchTool;
    bool m_inSketchMode = false;
    // Wall-clock (SDL_GetTicks seconds) until which interactive states (sketch,
    // push/pull, gizmo, edge/face ops, …) keep rendering continuously after the
    // last input; past it they idle like the main viewport (the live preview
    // stays on screen, frozen, and wakes instantly on the next event). Refreshed
    // on every event in the main loop. See hasActiveWork().
    double m_interactiveGraceUntil = 0.0;
    // History step index immediately before the current sketch was entered.
    // The "Exit Sketch (discard)" button rewinds history back to this step
    // so the user can bail out of a half-built sketch without keeping any
    // partial content. -1 = no sketch in progress (or pre-sketch state
    // wasn't capturable).
    int m_sketchEntryHistoryStep = -1;
    int m_activeSketchId = -1; // document id of the sketch being edited, or -1 if new

    // In-progress-sketch crash/kill recovery (see io/SketchRecovery). While in
    // sketch mode the active sketch is periodically written to a sidecar draft
    // (it isn't in the saved project until Finish Sketch). On launch a surviving
    // draft means last session ended mid-sketch; m_pendingSketchRecovery drives
    // the restore prompt.
    double m_lastDraftWrite = 0.0;       // ImGui time of last draft write
    int    m_lastDraftElemCount = -1;    // element count at last write (skip no-ops)
    bool   m_pendingSketchRecovery = false;
    void writeSketchDraftIfDue();        // throttled per-frame draft write
    void renderSketchRecoveryPrompt();   // startup "restore unfinished sketch?" modal
    void restoreSketchDraftNow();        // re-enter sketch mode with the saved draft

    // Whole-project crash/hang recovery (see io/ProjectRecovery). Independent of
    // the user-facing autosave (which only writes a SAVED file): the committed
    // model — bodies + full history — is snapshotted to a sidecar even for an
    // UNSAVED project, so a crash or a hang never loses more than the last
    // committed step. Cleared on a clean exit; a survivor drives the restore
    // prompt. Snapshots immediately on each new committed step, else throttled.
    double m_lastRecoveryWrite = 0.0;    // wall-clock secs of last recovery write
    int    m_lastRecoveryStep = -2;      // history currentStep at last write
    // Debounce inputs for the recovery writer: when the newest change landed
    // (markDirty stamps non-history changes; the writer itself stamps history
    // step movement) — the snapshot fires once ~5 s AFTER this settles.
    double m_lastChangeSeenAt = 0.0;
    int    m_lastSeenStepForRecovery = -2;
    double m_pendingChangeSince = 0.0;   // oldest unsnapshotted change (burst backstop)
    bool   m_pendingProjectRecovery = false;
    void writeProjectRecoveryIfDue();    // per-frame crash-recovery snapshot
    void renderProjectRecoveryPrompt();  // startup "restore unsaved project?" modal
    void restoreProjectRecoveryNow();    // load the recovery snapshot

    // Hovered sketch region (for highlight in viewport)
    int m_hoveredSketchId = -1;
    int m_hoveredRegionIndex = -1;

    // Numeric dimension input shown while placing a sketch shape
    char m_sketchDimBuf[32] = "";
    // Line/Circle inline dimension, as a VALUE rather than the buffer above.
    // The buffer form meant an InputText, i.e. the OS keyboard on a tablet;
    // a value drives materializr::inputNumber and so gets the number pad,
    // matching the Rectangle W/H fields beside it.
    float m_sketchDimValue = 0.0f;
    bool m_sketchDimWasShown = false; // tracks placing transitions to grab keyboard focus

    // Dimension-label click-to-edit. m_dimEditingId is the constraint being
    // edited (-1 = none); the popup near the label shows the current value
    // and Enter commits / re-solves. m_dimEditingClickedThisFrame tells the
    // sketch click handler to swallow the click that opened the popup so a
    // bare point isn't also placed underneath.
    int  m_dimEditingId = -1;
    char m_dimEditingBuf[32] = "";
    bool m_dimEditingFocus = false;
    bool m_dimEditingClickedThisFrame = false;
    // Set by applyPendingDimension() (Dimension tool commit) to defer
    // ImGui::OpenPopup("##DimEdit") to the viewport's own ImGui window scope
    // next frame — OpenPopup only works when called from the window that
    // owns the popup's ID stack, which the app-level commit path isn't in.
    bool m_dimOpenEditRequested = false;
    // Set by the ##DimEdit popup block (Application_Viewport.cpp) the frame
    // an Escape press is seen while the popup is up — BEFORE ImGui closes
    // it and the block clears m_dimEditingId. renderViewport() runs before
    // handleShortcuts() each frame, so by the time the global Escape chain
    // asks "is a dimension popup open", m_dimEditingId is already -1; this
    // flag is how the chain learns the popup just consumed this Escape
    // press instead of falling through to the Dimension-mode / sketch-exit
    // steps. Consumed (cleared) by the chain on the SAME press it gates;
    // also cleared on every sketch enter/exit reset to avoid staleness.
    bool m_dimPopupConsumedEsc = false;
    // Same-frame signal: the ##DimEdit popup was open when this frame's left
    // click landed, so the click belongs to the popup (dismiss/interaction) —
    // the Dimension tool's click routing must not treat it as a fresh pick.
    bool m_dimPopupSwallowClick = false;

    // Sketch grid step in mm. This is the BASE the user chose (a display
    // number: "1" means one of whatever unit is showing). It is what persists.
    float m_sketchGridStep = 1.0f;
    // The base scaled by whole decades to suit the CURRENT zoom, recomputed
    // every frame in renderViewport's drawGrid (which both branches call, so
    // it is never stale) — see viewport/GridScale.h. Equal to the base outside
    // sketch mode and whenever the base already suits the zoom.
    //
    // WHICH STEP A SITE WANTS:
    //   anything snapping a point ON THE SKETCH PLANE, drawing the sketch grid,
    //   or LABELLING the step for the user -> this one, so the lines drawn, the
    //   points reachable and the number displayed can never disagree;
    //   the Settings presets, persistence and the unit carry-over -> the base,
    //   which is what the user actually chose;
    //   world-space gizmo/plane snapping outside sketch mode -> the base (the
    //   world grid is not zoom-scaled; the two are equal there anyway).
    //
    // renderViewport is the SINGLE WRITER of SketchTool's snap step, since only
    // it knows the zoom. Anywhere else that changes the base (the toolbar, a
    // unit switch) updates the base and lets the next frame follow; calling
    // SketchTool::setGridStep from those sites clobbers the scaled lattice and
    // leaves the cursor snapping somewhere the grid is not drawn.
    float m_effectiveGridStepMm = 1.0f;
    // World-aligned anchor used as the sketch grid origin and the camera
    // target. Computed at sketch entry from the face centre snapped to the
    // nearest grid intersection projected onto the sketch plane. Preserved
    // through orbit-exit so the new perspective view pivots around the same
    // point the user was sketching on.
    glm::vec3 m_sketchSnappedAnchor{0.0f};

    // Snap-to-grid for gizmo translate (shares the grid step with the sketch grid).
    bool m_snapToGrid = true;
    // Previous-frame hover state for the corner snap widget. The widget is
    // hit-tested with raw IsMouseHoveringRect (no ImGui item) so it can't
    // claim input via the usual IsItemActive path; instead the viewport
    // input handlers check this flag to skip picker/sketch-tool clicks when
    // the mouse is over the widget. Same pattern m_viewCube uses with
    // wasHovered().
    bool m_snapWidgetHovered = false;

    // Configurable camera mouse bindings (ImGuiMouseButton values: 0=Left,1=Right,
    // 2=Middle). Zoom is always the scroll wheel. Edited in File > Settings.
#if defined(MZ_MOBILE)
    int m_orbitButton = 0; // Left (trackpad default; rebindable in Settings)
    int m_panButton = 0;   // Left
#else
    int m_orbitButton = 2; // Middle
    int m_panButton = 1;   // Right
#endif
    // Touch multi-select toggle: the finger stand-in for holding Ctrl. While on,
    // the viewport selection code runs as if Ctrl were held (taps add/toggle
    // instead of replacing). Driven by the on-screen button in the viewport.
    bool m_multiSelectToggle = false;
    // Touch "Move" navigation lock: one-finger drag orbits, taps don't draw or
    // select — so panning/zooming (esp. in a sketch) can't start a drawing.
    bool m_moveModeToggle = false;
    // Touch press-drag-release: a drawing-tool press is pending; its point is
    // placed on release (the drag previews the radius/bulge/segment first).
    bool m_sketchPressActive = false;
    // For the drag tools (circle/rectangle) the centre/first corner is dropped on
    // press so the whole shape is one press-drag-release gesture; this flags that
    // the release must complete the shape (and only if the finger actually moved).
    bool m_sketchDragCenterPlaced = false;
    // History step count captured at the start of a drawing-tool press, so that
    // if a two-finger pan/zoom takes the press over we can tell whether the
    // press already pushed a step (e.g. Line's start vertex) and roll it back.
    int m_sketchPressStepBefore = 0;
    // Interactive mirror line manipulated by a sketch-style move/rotate gizmo.
    // Reuses the SketchGizmoHandle vocabulary (MoveX/MoveY/MoveFree/Rotate).
    SketchGizmoHandle m_mirrorGizmoHandle = SketchGizmoHandle::None;
    glm::vec2 m_mirrorGizmoStartAnchor{0.0f}; // mirror anchor at drag start
    glm::vec2 m_mirrorGizmoGrab{0.0f};        // sketch-space cursor at drag start
    float m_mirrorGizmoStartAngle = 0.0f;     // mirror angle at drag start
    float m_sketchDownX = 0.0f, m_sketchDownY = 0.0f; // press pos (px) for drag slop
    // ImGui drops IsItemHovered() mid-drag once the window-move grab takes the
    // ActiveId, which freezes the live sketch preview. Latch the viewport input
    // block alive while a left press-drag that began over the viewport is in
    // flight so onMouseMove keeps following the finger. Cleared on button-up.
    bool m_viewportInputLatch = false;
    // Touch: a menu-bar toggle that force-raises the system soft keyboard (some
    // Android builds don't reliably auto-raise it on field focus). OR'd with
    // io.WantTextInput; typed text still flows into whatever field is focused.
    bool m_softKeyboardForced = false;
    bool m_showSettings = false;
    int m_settingsOrbitButton = 2; // staged value in the Settings dialog
    int m_settingsPanButton = 1;

    // Touch mode (large UI + touch gestures) staged value for the Settings
    // dialog. The live state lives in the materializr::touchMode() global; this
    // mirrors the saved setting and is written back on save. Default tracks the
    // platform (see AppSettings::touchMode); applyAppSettings keeps it in sync.
#if defined(MZ_MOBILE)
    bool m_touchMode = true;
#else
    bool m_touchMode = false;
#endif
    // Desktop UI scale preference (Linux HiDPI; Settings → Appearance). Staged
    // value for the Settings dialog + persistence; applied at startup via
    // Window::setUiScaleOverride (a change takes effect on restart). 1.0 = Low.
    // --ui-scale / --hidpi command-line override (0 = none). Wins over the
    // saved setting for this launch — an escape hatch when the UI is too small
    // to read to change it in Settings.
    float m_cliUiScale = 0.0f;

    // Interface layout (see UiLayout in io/Settings.h and src/app/layout/).
    // Live-switchable: read every frame by run()/renderViewport(); persisted
    // on save. The helpers below are the preferred spelling at call sites.
    UiLayout m_uiLayout = UiLayout::Classic;
    // UI language index, mirroring materializr::Lang. -1 = never chosen, which
    // is what makes the setup wizard open with the language question.
    int m_language = -1;
    // Mirrors Settings::displayUnit. Change it ONLY through
    // applyDisplayUnitChange, which also drops any active text edit so a field
    // cannot commit in a different unit from the one it was showing.
    int m_displayUnit = 0;
    bool classicLayout() const { return m_uiLayout == UiLayout::Classic; }
    bool modernLayout()  const { return m_uiLayout == UiLayout::Modern;  }
    bool imTouchLayout() const { return m_uiLayout == UiLayout::ImTouch; }
    // im-touch only: the transparent model tree on the right edge.
    bool m_imTouchTree = true;
    // im-touch only: browser-tree group expansion (session-local; boots
    // fully expanded like Fusion's browser). No other layout reads these.
    bool m_imTouchTreeOpenBodies = true;
    bool m_imTouchTreeOpenSketches = true;
    bool m_imTouchTreeOpenConstruction = true;
    // im-touch Items overlay: hovered this frame (feeds the long-press gate in
    // Application_Viewport so press-and-hold synthesizes a right-click over the
    // tree, opening a row's context menu just like the classic/modern panels).
    bool m_imTouchTreeHovered = false;
    // Tab strip (classic in-viewport bar / modern pills): hovered this frame.
    // Same purpose — both already call BeginPopupContextItem for Save / Save As
    // / Close, but on touch that menu was UNREACHABLE: the long-press gate arms
    // only over the canvas and the Items panel, so a press-and-hold on a tab
    // never became the right-click those popups wait for.
    bool m_tabBarHovered = false;
    // im-touch rename: a namespaced key (body=id, sketch=1000000+id,
    // folder=2000000+id, plane=4000000+id, axis=5000000+id) whose name is being
    // edited in the rename modal; -1 = idle. The buffer holds the edited text.
    int  m_imTouchRenameKey = -1;
    bool m_imTouchRenameOpen = false;   // raise the modal next frame
    bool m_imTouchRenameFocus = false;  // grab the keyboard on first modal frame
    char m_imTouchRenameBuf[128] = {};
    // im-touch new-folder modal (reached from a body's Move-to-folder menu or
    // the Bodies header). The pending bodies drop into the folder on create.
    bool m_imTouchNewFolderOpen = false;
    bool m_imTouchNewFolderFocus = false;
    char m_imTouchNewFolderName[128] = {};
    std::vector<int> m_imTouchNewFolderBodies;
    // im-touch only: the Fusion-style history timeline along the bottom edge.
    bool m_imTouchTimeline = true;
    // im-touch timeline: step whose properties popup is open (-1 = none).
    // Mirrored into m_historyPanel's editing step so the viewport's orange
    // edited-element highlight follows. Session-local.
    int m_imTouchHistoryEdit = -1;
    // Center rect the modern/im-touch layouts leave for the viewport window
    // this frame (screen coords, points). Written by renderModernLayout() /
    // renderImTouchLayout(), read by renderViewport() to pin the undocked
    // "Viewport" window.
    float m_touchVpX = 0.0f, m_touchVpY = 0.0f, m_touchVpW = 0.0f, m_touchVpH = 0.0f;
    // Active tab of the touch shell's right panel (0 = Items,
    // 1 = History & Properties). Persisted.
    int m_touchRightTab = 0;
    // Modern-layout right panel: the Properties footer sizes to its content
    // (AutoResizeY, no scrollbar) rather than a fixed slab — it grows upward
    // from the bottom as a selection needs more room, and History absorbs the
    // rest and scrolls. The History split above it reserves last frame's
    // measured footer height, so a selection change settles in one frame.
    float m_propsFooterH = 0.0f;
    // Right-panel width in logical px (× uiScale at use); dragged via the
    // panel's left-edge splitter or edge tab, persisted, clamped at both ends.
    float m_touchRightW = 300.0f;
    // Tool-rail width, same convention (edge-tab drag, persisted).
    float m_touchRailW = 92.0f;
    // Edge-tab drag state (tap vs drag disambiguation).
    bool m_railTabDragged = false, m_rightTabDragged = false;

    // Autosave: once the project has been saved at least once (has a path on
    // disk), periodically re-save dirty changes. Toggled in File > Settings.
    bool m_autosaveEnabled = false;
    float m_autosaveIntervalSec = 120.0f;
    double m_lastAutosaveTime = 0.0;

    // Invert the cube-drag → orbit direction (Settings).
    bool m_invertCubeDrag = false;

    // Double-click window (s), applied to ImGuiIO::MouseDoubleClickTime. Higher
    // suits trackpads (slower double-taps). Persisted; default = ImGui's 0.30.
    float m_doubleClickTime = 0.30f;
    // Seconds a fillet may spend proving it terminates before being refused.
    // Mirrors Settings::filletProbeSeconds; pushed into FilletProbe on apply.
    float m_filletProbeSeconds = 2.5f;

    // Dimension label being dragged to a new spot, -1 when none. A press on a
    // label starts a drag rather than opening its edit popup; the popup opens
    // on RELEASE, and only if the pointer never really moved. Without this the
    // label is unmovable — every attempt to reposition it fires the editor.
    int       m_dimDragId = -1;
    // Label position minus cursor position at the moment of the press, in
    // sketch mm, so the tag keeps its grab point instead of snapping its centre
    // to the cursor.
    glm::vec2 m_dimDragGrab{0.0f};
    // Whether this press has travelled far enough to count as a drag. Below the
    // threshold it stays a click, so a slightly shaky press still edits.
    bool      m_dimDragMoved = false;

    // Rendering preferences (File > Settings → Rendering). Persisted.
    float m_lightAmbient = 0.40f;   // base illumination; higher = softer shadows
    bool  m_lightHeadlight = false; // key light tracks the camera
    bool  m_lightFill = true;       // soft opposing fill light
    int   m_msaaSamples = 4;        // viewport anti-aliasing: 0=off, 2, 4, 8
    int   m_meshQuality = 1;        // tessellation density: 0=Low..3=Ultra
    float m_selectionLineWidth = 3.0f; // px width of highlighted edges/body outlines
    float m_sketchLineWidth = 2.5f;    // px width of sketch geometry over the grid
    float m_sketchGridOpacity = 0.55f; // opacity of the sketch-plane grid (0..1)
    float m_sketchGridThickness = 1.0f; // grid line-width multiplier (0.1..2)
    bool  m_smallScreenWarned = false; // persisted: user ticked "don't show again"
    bool  m_smallScreenAck = false;    // dismissed for this run only
    bool  m_leftPanelHidden = false;   // persisted: Tools column collapsed
    bool  m_rightPanelHidden = false;  // persisted: Items/History/Properties column collapsed
    // "Viewport" window screen rect, captured each frame in renderViewport, used
    // to anchor the touch collapse handles at the panel/viewport boundaries.
    float m_viewportWinX = 0, m_viewportWinY = 0, m_viewportWinW = 0, m_viewportWinH = 0;
    // One-time notice on phone-sized screens (the UI is built for tablets+).
    void renderSmallScreenWarning();
    void renderPanelCollapseHandles();   // touch-mode edge tabs to hide/show each side
    void renderPluginMenuItems(const char* menuName);  // plugin MenuContributions for a menu

    // Touch tooltip-timeout state (see beginFrame): blank a parked pointer after
    // 15 s so a stuck tooltip clears.
    float  m_tipLastMouseX = -1e9f;
    float  m_tipLastMouseY = -1e9f;
    double m_tipStationarySince = 0.0;
    bool  m_showToolbarTooltips = true; // hover description on each toolbar button
    bool  m_showFps = true;             // small FPS readout (im-touch layout, top-centre)
    // Per-panel visibility (Settings > Panels), persisted. Default all on. These
    // gate each docked panel's render so it can be hidden to free screen space
    // and brought back from Settings — independent of the left/right column
    // collapse. (The viewport is never toggled — no multi-viewport yet.)
    bool  m_showTools        = true;
    bool  m_showInteractions = true;
    bool  m_showHistory      = true;
    bool  m_showItems        = true;
    bool  m_showProperties   = true;
    // Touch-mode camera sensitivity (Settings > Touch; persisted; 1.0 = default).
    float m_touchOrbitSens = 1.0f;
    float m_touchPanSens   = 1.0f;
    float m_touchZoomSens  = 1.0f;
    // Toggle for the sketch toolbar's live Full/Reduced/Off inference cycle
    // button. Off hides the button so users who set the level once in
    // Settings can declutter the sketch toolbar.
    bool  m_showInferenceToolbarToggle = true;
    // STL import (persisted). m_stlImportAccuracy pre-fills the import dialog's
    // fidelity slider; m_meshShowWireframe gates the facet wireframe of imported
    // mesh bodies (live — toggling it re-runs the mesh-body edge rebuild).
    float m_stlImportAccuracy = 0.5f;
    bool  m_meshShowWireframe = true;
    // Apply m_light*/m_msaaSamples/m_selectionLineWidth to the renderer + viewport.
    void applyRenderingSettings();
    // Map m_meshQuality to OCCT tessellation parameters.
    void meshQualityParams(float& deflection, float& angularDeflection) const;

    bool m_renderersReady = false;
    // Full-rebuild signal: clear all meshes and re-tessellate every visible
    // body. Necessary on theme/mesh-quality changes, project load, and the
    // first frame. Most edits should prefer markBodyDirty() so a 145-body
    // project doesn't pay full re-tessellation on every push/pull frame.
    bool m_meshesDirty = true;
    // Per-body partial rebuild signal. rebuildMeshes() walks this set and
    // updates only those bodies' meshes via setBodyMesh / removeBody. Cleared
    // after each rebuild pass.
    std::set<int> m_dirtyBodyIds;
    int m_hoveredBodyId = -1;

    // Gizmo drag state for history commit
    bool m_gizmoDragging = false;
    int m_gizmoDragBodyId = -1;            // primary (for Rotate/Scale + readouts)
    TopoDS_Shape m_gizmoDragOriginalShape; // primary's original
    // For multi-body Move/Rotate/Scale: all selected bodies' originals captured
    // at drag start. Cached pivot avoids recomputing 65 bboxes every frame for
    // a single rotation around the selection centroid.
    std::vector<std::pair<int, TopoDS_Shape>> m_gizmoDragOriginals;
    glm::vec3 m_gizmoSharedPivot{0.0f};
    // World-Y of the lowest face of the drag selection at drag start. The
    // translate readout reports the body's BOTTOM (= what the user sees
    // sitting on the grid) on the user-Z axis rather than the bbox centre,
    // so a cylinder resting on Z=0 reads `Z 0.00` instead of `Z 10.00`.
    // Sketch-only drags get the pivot's Y here (no bbox → no offset).
    float m_gizmoSharedBottomY{0.0f};
    // Primary body's bbox captured ONCE at drag start (the originals never
    // change during a drag). The Scale branch needs its diagonal every
    // frame; recomputing BRepBndLib::Add per drag frame was 50-150 ms on a
    // complex body — a large slice of the "moving one part lags" report.
    glm::vec3 m_gizmoDragBBoxMin{0.0f};
    glm::vec3 m_gizmoDragBBoxMax{0.0f};

    // GPU-only gizmo drag preview (same pattern as the Revolve live preview):
    // during the drag the document is NOT touched — the accumulated transform
    // is pushed as a model matrix onto the dragged bodies' shape+edge mesh
    // slots, so a drag frame costs two uniform updates instead of a BRep
    // transform + updateBody + re-tessellation + edge re-discretization of
    // the dragged body. The real (parametric, undoable) transform is applied
    // exactly once on release; Esc just resets the matrices.
    void gizmoPreviewApply(const glm::mat4& m);
    void gizmoPreviewReset() { gizmoPreviewApply(glm::mat4(1.0f)); }

    // Standalone-sketch gizmo drag — set when the gizmo is shown on a Sketch
    // selection (no body in the selection, not in sketch-edit, perspective
    // view). m_sketchGizmoDragSketches holds {sketchId, planeBefore} for
    // every dragged sketch; on release a SketchTransformOp per sketch is
    // pushed to history. Distinct from the body-attached sketch path: those
    // ride along through TransformOp's m_previousSketchPlanes machinery.
    std::vector<std::pair<int, gp_Pln>> m_sketchGizmoDragSketches;

    // Construction-plane gizmo drag — same shape as the sketch list above,
    // but writes back via Document::setPlane instead of Sketch::setPlane.
    // Used by both the in-popup placement gizmo and (after the popup
    // commits) any post-selection drag on a Plane in the document.
    std::vector<std::pair<int, gp_Pln>> m_planeGizmoDrag;

    // Construction-plane gizmo arming — mirrors m_sketchGizmoArmed. Selection
    // alone (clicking a plane in the viewport or items panel) gets you a
    // highlight only; pressing W/E or clicking Move/Rotate in the Plane
    // tools panel arms the gizmo for the currently selected plane. Cleared
    // when the active plane in the selection changes. During the original
    // Construction Plane popup placement (m_planeOpActive) we treat the
    // gizmo as implicitly armed so the user can manipulate the preview
    // straight away.
    bool m_planeGizmoArmed = false;
    int  m_planeGizmoArmedFor = -1;

    // Construction-axis gizmo drag + arming — same shape as the plane
    // version. Axes have no Rotate semantics (an infinite line has no
    // meaningful "rotate the line around itself") so only Translate
    // writes through. The drag list stores {axisId, origin-before-drag,
    // direction-before-drag} so the live drag can rebase off a stable
    // pose each frame.
    struct AxisDragEntry { int id; gp_Pnt origin; gp_Dir direction; };
    std::vector<AxisDragEntry> m_axisGizmoDrag;
    bool m_axisGizmoArmed = false;
    int  m_axisGizmoArmedFor = -1;

    // Per-body gizmo-center cache. Without this, the body-selected branch in
    // renderViewport calls BRepBndLib::Add(shape, bbox) every frame to place
    // the Move/Rotate gizmo on the body centroid — and on a complex part
    // (1.5m airplane skeleton: many trimmed B-spline surfaces per body) that
    // bbox walk is 50–150 ms each, dropping the idle frame rate to 6 FPS the
    // moment one or two bodies are selected. Key: the body's TShape pointer
    // PLUS the shape's location — a location-only transform (multi-body move
    // commit) keeps the TShape while moving the body, and a TShape-only key
    // left the gizmo sitting at the pre-move centroid. Topology rebuilds
    // (push/pull, fillet, single-body transform via copy=true) still miss on
    // the pointer — exactly when we'd want a fresh centroid anyway.
    struct GizmoCenterCacheEntry {
        const void* tsh = nullptr;
        TopLoc_Location loc;
        glm::vec3 center{0.0f};
    };
    std::map<int, GizmoCenterCacheEntry> m_gizmoCenterCache;

    // Sketches do NOT show the gizmo automatically on selection — that lets
    // the Tools toolbar surface its Move / Rotate / Loft / Edit options
    // cleanly without a gizmo dropped on top. The user clicks Move or Rotate
    // to "arm" the gizmo for the current sketch; selection-change clears it.
    // (Bodies still get the gizmo on selection as before.)
    bool m_sketchGizmoArmed = false;
    int  m_sketchGizmoArmedFor = -1; // sketch id armed for; cleared when sel changes

    // Multi-body Rotate type-in panel. When the Rotate gizmo is active and 2+
    // bodies are selected, this panel offers per-axis sliders + numeric input
    // so the user can apply an exact rotation in a single commit, bypassing the
    // per-frame lag of the live gizmo path on large selections.
    float m_multiRotate[3] = {0.0f, 0.0f, 0.0f};
    // The Close button hides the panel; it auto-reopens the next time the
    // conditions (Rotate gizmo + multi-body) are freshly satisfied, so the
    // user can dismiss it without losing access to it.
    bool m_multiTransformPanelOpen = true;
    bool m_multiTransformConditionsMet = false;
    // Accumulated delta from drag start (translate only). Used so snap-to-grid
    // can snap the absolute position rather than each per-frame increment.
    glm::vec3 m_gizmoTotalDelta{0.0f};
    // The live drag's preview transform, mirrored from gizmoPreviewApply().
    // The drag moves bodies by a GPU model matrix and never writes the
    // document, so anything else drawn in world space — the gizmo itself, the
    // selection outline — has no way to know the body moved. This is that
    // channel. Identity whenever no drag is running (gizmoPreviewReset()).
    glm::mat4 m_gizmoPreviewXf{1.0f};
    // Accumulated rotation (deg, about m_gizmoRotAxis) from drag start, for soft
    // 45° snapping; and accumulated per-axis scale (raw drag deltas → factors).
    float m_gizmoTotalAngle = 0.0f;
    glm::vec3 m_gizmoRotAxis{0.0f, 1.0f, 0.0f};
    glm::vec3 m_gizmoScaleAccum{0.0f}; // accumulated per-axis drag distance
    glm::vec3 m_gizmoTotalScale{1.0f, 1.0f, 1.0f}; // derived per-axis factors

    // Mirror: single button opens a popup; "across a face" arms face-pick mode.
    bool m_showMirrorPopup = false;
    bool m_mirrorPickFace = false;
    int m_mirrorBodyId = -1;

    // Scale side panel (shown in Scale gizmo mode): X/Y/Z percentages + uniform.
    float m_scalePct[3] = {100.0f, 100.0f, 100.0f};
    bool m_scaleUniform = true;
    // Scale popup unit mode. Percent is the multi-body-safe default; mm only
    // makes sense when exactly one body is selected (we can show its
    // bbox-derived target dims). renderScalePanel forces Percent whenever
    // the selection isn't a single body.
    enum class ScaleUnitMode { Percent, Millimeter };
    ScaleUnitMode m_scaleUnitMode = ScaleUnitMode::Percent;
    // mm-mode text buffer + focus state per user axis (X, Y, Z in Z-up
    // convention). Re-seeded from the body's current bbox each frame the
    // field isn't focused so the displayed value tracks external edits
    // (undo/redo, other panels) without overwriting the user's in-flight
    // typing.
    struct ScaleMmEdit {
        char buf[24] = "0";
        bool focused = false;
        int bodyId = -1;
        double initialExtent = 0;
    };
    ScaleMmEdit m_scaleMmEdit[3];

    // Interactive fillet/chamfer — the tool lives in EdgeOpController now.
    // Application keeps thin delegates plus the two accessors the shared
    // dimension-arrow renderer needs.
    using EdgeOpType = materializr::EdgeOpKind;
    materializr::EdgeOpController m_edgeCtl;
    void beginInteractiveEdgeOp(EdgeOpType type) {
        cancelAllInteractivePreviews();
        m_edgeCtl.beginEdgeOp(iopContext(), type);
        m_meshesDirty = true;
    }
    // Re-edit the FilletOp or ChamferOp at the given history index. Pulls the
    // existing radius/distance + edges + body id from the op, snapshots its
    // pre-state for live preview, and reuses the same drag handle + popup UI
    // as creation. Triggered by clicking a face the op produced.
    void beginInteractiveEdgeOpEdit(int historyIndex) {
        cancelAllInteractivePreviews();
        m_edgeCtl.beginEdgeOpEdit(iopContext(), historyIndex, m_edgeOpPickedBodyId);
        m_meshesDirty = true;
    }
    void updateInteractiveEdgeOp() { m_edgeCtl.updateEdgeOp(iopContext()); }
    void commitInteractiveEdgeOp() {
        m_edgeCtl.commit(iopContext());
        m_meshesDirty = true;
    }
    void cancelInteractiveEdgeOp() {
        m_edgeCtl.cancel(iopContext());
        m_meshesDirty = true;
    }
    // The body whose fillet/chamfer FACE was clicked to start an edit; handed
    // to the controller at begin so it can spot a baked (uneditable) feature.
    int m_edgeOpPickedBodyId = -1;

    // Refuse a modelling op whose selection includes an imported mesh, and say
    // why. See core/MeshGuard.h — an import is a REFERENCE body: sketch on it
    // and snap to it, but nothing rewrites its topology. Returns true when the
    // caller should stop.
    bool refuseMeshSelection(const char* opName);

    // Start a hole move driven by an EDGE selection. The picked rim edges
    // choose the verb (see MoveHoleOp::classifyRimEdges). Returns false when
    // the selection isn't one hole's rim, so the caller falls through.
    bool beginMoveHoleFromEdges() {
        cancelAllInteractivePreviews();
        return m_moveFaceCtl.beginMoveHoleFromEdges(iopContext());
    }

    // Resize-cylindrical (edit a closed cylindrical/conical face's diameter,
    // or a single circular edge of one) ====================================
    // Triggered by picking a closed cylindrical face (edits BOTH end edges
    // together → stays a cylinder) or a single circular edge (edits ONE end
    // → turns cylinder into a cone, makes funnels). Internally the commit
    // path always builds a CONE primitive at the two end radii — for the
    // face-edit case they're equal.
    // (state now lives in ResizeCylindricalController — m_resizeCylCtl)

    // ─── Thread (helical screw thread on a cylindrical face) ───────────────
    // beginThread copies the geometry the cylindrical-face detector left in
    // the m_resizeCyl* fields; the popup collects pitch/depth/handedness and
    // Apply pushes a ThreadOp (no live preview — the helical sweep + boolean
    // is too heavy to run per-frame).
    bool   m_threadActive = false;
    int    m_threadBodyId = -1;
    bool   m_threadIsHole = false;
    // Topological name of the picked cylinder face, minted at beginThread so
    // the committed ThreadOp follows an upstream edit (see ThreadOp::setter).
    materializr::topo::Ref m_threadFaceRef;
    double m_threadAxis[9] = {0, 0, 0, 0, 0, 1, 1, 0, 0}; // loc, dir, xdir
    double m_threadRadius = 5.0;
    double m_threadLength = 10.0;
    float  m_threadPitch  = 1.0f;
    float  m_threadDepth  = 0.6f;
    bool   m_threadRightHanded = true;
    int    m_threadProfile = 0;      // ThreadProfile enum (0 = Standard V)
    float  m_threadClearance = 0.0f; // radial fit gap for printed threads (mm)
    int    m_threadStarts = 1;       // interleaved helix count (bottle caps: 3-4)
    float  m_threadGrooveWidth = 0.0f; // explicit cut width (mm); 0 = from pitch
    char   m_threadPitchBuf[32] = "1.0";
    char   m_threadDepthBuf[32] = "0.6";
    // Apply runs the helical sweep + boolean on a worker thread (it takes
    // seconds) behind a modal, so the window keeps pumping events instead of
    // going "not responding". The future carries the cut result; the main
    // thread polls it each frame and pushes the op when ready.
    std::future<TopoDS_Shape> m_threadFuture;
    bool   m_threadComputing = false;
    // Async thread RE-CUT (cascade/editStep recompute path — distinct from the
    // popup's initial Apply worker above). ThreadOp::execute hands the heavy
    // re-cut here via the hook installed in the constructor; the body stays at
    // its pre-thread state until the worker's result lands (pollThreadRecuts,
    // once per frame). See ThreadOp::setAsyncRecutHook.
    struct PendingThreadRecut {
        ThreadOp* op = nullptr;      // history-owned; re-validated on landing
        int bodyId = -1;
        TopoDS_Shape launchedFrom;   // doc body at launch — stale-guard
        std::future<TopoDS_Shape> fut;
        int attempts = 1;            // relaunch-on-stale counter (cap 3)
        std::shared_ptr<std::atomic<bool>> cancel; // per-job worker token
    };
    std::vector<PendingThreadRecut> m_threadRecuts;
    void installThreadRecutHook();
    // Launch (or relaunch) the worker for this op against the CURRENT body.
    bool launchThreadRecut(ThreadOp& op, int attempts);
    void pollThreadRecuts();   // per-frame: apply/relaunch/discard results
    void flushThreadRecuts();  // block until drained (save path)
    // Cancel button on the re-cut modal: signal every worker, suspend the
    // affected Thread steps (body is sitting pre-thread), abandon futures.
    void cancelThreadRecuts();
    // Cancel token for the initial-Apply worker (m_threadFuture).
    std::shared_ptr<std::atomic<bool>> m_threadApplyCancel;
    // Abandoned std::async futures (their destructor BLOCKS): parked here and
    // reaped by pollThreadRecuts once the (cancelled) worker actually exits.
    std::vector<std::future<TopoDS_Shape>> m_threadZombies;

    // Section View — render-only clipping of the scene by a plane so the
    // user can inspect interiors (thread profiles, wall thickness) without
    // destructive booleans. Plane source is a construction plane or a world
    // plane; offset slides it along its normal; flip swaps which half is
    // hidden. ShapeRenderer discards clipped fragments; SectionView overlays
    // the true B-rep intersection curves on the cut.
    bool   m_sectionEnabled    = false;
    int    m_sectionPlaneId    = -1;  // construction plane id; -1 = world
    int    m_sectionWorldPlane = 0;   // 0=XY 1=XZ 2=YZ (when planeId < 0).
                                      // XY (vertical, normal Z) — the old
                                      // XZ/ground default clipped everything
                                      // above the floor.
    float  m_sectionOffset     = 0.0f;
    bool   m_sectionFlip       = false;
    bool   m_sectionDirty      = true; // recompute overlay curves next frame
    bool   m_sectionPending    = false;   // overlay recompute waiting for rest
    uint32_t m_sectionRestMs   = 0;       // last plane change (debounce clock)
    // Async overlay compute (one recompute on a threaded body took 100s).
    std::future<SectionView::Result> m_sectionFut;
    std::shared_ptr<std::atomic<bool>> m_sectionCancel;
    gp_Pln sectionBasePlane() const;  // flip applied, offset NOT applied
    void   renderSectionPanel();      // floating controls while enabled

    void beginThread(const materializr::CylindricalPick& pick);        // copies detector output, opens the popup
    std::unique_ptr<ThreadOp> makeThreadOpFromState() const;
    void commitThread();       // kicks the compute onto a worker thread
    void cancelThread();
    void renderThreadPanel();  // ImGui popup contents

    // Detect whether the currently-picked face is on a recognised resizable
    // body (solid cylinder / tube). Populates the relevant m_resizeCyl* fields
    // and returns true if so. Called per frame to drive the toolbar button.
    // Returns what it found rather than leaving it in members — see
    // CylindricalPick.h for why that mattered.
    materializr::CylindricalPick detectCylindricalResizeCandidate() const;

    // Interactive face ops (Shell / Taper / Scale Face) live in
    // controllers now — see InteractiveOpController.h. Each owns its own
    // state, lifecycle, and panel; the registry below drives suppression,
    // the Esc chain, and panel rendering generically.
    ShellController m_shellCtl;
    TaperController m_taperCtl;
    ScaleFaceController m_scaleFaceCtl;
    ProjectSketchController m_projectSketchCtl;
    DefeatureController m_defeatureCtl;
    ResizeCylindricalController m_resizeCylCtl;
    ExtrudeController m_extrudeCtl;
    PushPullController m_ppCtl;
    SplitController m_splitCtl;
    // m_moveFaceCtl is declared up with its delegates; it joined this array
    // once its lifecycle overrides landed, so every generic loop — Esc/Enter
    // chains, single-flight, suppression, input/overlay/gizmo dispatch —
    // covers Move Face without a special case.
    std::array<InteractiveOpController*, 11> m_iops{
        &m_shellCtl, &m_taperCtl, &m_scaleFaceCtl, &m_projectSketchCtl,
        &m_defeatureCtl, &m_resizeCylCtl, &m_moveFaceCtl, &m_extrudeCtl,
        &m_ppCtl, &m_edgeCtl, &m_splitCtl};
    IopContext iopContext();
    bool anyIopActive() const {
        for (auto* c : m_iops) if (c->active()) return true;
        return false;
    }
    // A controller has a viewport handle latched — camera orbit and face
    // picking stand off. Read off ScaleFace's dragAxis() directly before,
    // which only held while exactly one controller had a gizmo.
    bool anyIopDraggingHandle() const {
        for (auto* c : m_iops) if (c->draggingHandle()) return true;
        return false;
    }
    // A controller owns the viewport's left-drag (it draws handles there).
    bool anyIopWantsViewportInput() const {
        for (auto* c : m_iops)
            if (c->active() && c->wantsViewportInput()) return true;
        return false;
    }
    // Single-flight: starting one interactive op cancels any other live
    // preview — controller or legacy (push/pull, extrude, pattern, resize,
    // thread). Two concurrent previews on the same body snapshot each
    // other's PREVIEW state — cancelling the first then restores the
    // second's contaminated snapshot, leaving phantom geometry with no
    // history step (Steve's "cancelled the projection and it still stuck").
    void beginIop(InteractiveOpController& ctl);
    void cancelActiveIops(); // controller half, callable from legacy begins
    bool anyInteractivePreviewActive() const; // controllers + legacy previews
    void cancelAllInteractivePreviews();      // both halves; saves call this

    // im-touch: while an action (interactive preview) is live, its Confirm/
    // Cancel are hosted as corner FABs — the sketch Finish/Discard spot —
    // instead of buttons inside each op panel (which hide themselves while
    // this is true). One action at a time (single-flight), so the dispatch
    // below is unambiguous.
    bool imTouchActionCorner() const;
    void confirmActiveAction();  // corner ✓ — commit whichever action is live
    void cancelActiveAction();   // corner ✗ — cancel it

    // im-touch, touch input: a circle or rectangle drawn by press-drag-
    // release is HELD as a preview on lift instead of committing — a bubble
    // near the shape offers exact-value fields plus ✗/✓ (renderViewport's
    // confirm-bubble block). The pending pos is the lift point in sketch
    // coords — ✓ commits through it when nothing was typed. Drawing the next
    // shape auto-commits the held one (press handler consumes this state).
    bool m_sketchShapeConfirmPending = false;
    glm::vec2 m_sketchShapePendingPos{0.0f};
    char m_sketchShapeDimBuf[32] = {};   // circle: typed diameter
    // Rectangle: staged Width/Height, seeded from the dragged size at lift;
    // each is edited by its own amountField well in the bubble.
    float m_sketchShapeDimW = 0.0f;
    float m_sketchShapeDimH = 0.0f;
    // Preview endpoints FROZEN at lift (getPreviewStart/End at that moment).
    // The bubble anchors and its commit math use these, not the live
    // preview — stray hover/motion events that twitch the held preview must
    // not move the input box under the user's finger.
    glm::vec2 m_sketchShapeAnchorPs{0.0f};
    glm::vec2 m_sketchShapeAnchorPe{0.0f};

    // im-touch: anchor for the live action's distance well (extrude /
    // push-pull), LATCHED in world space when the action starts — the well
    // must not chase the growing arrow (the sketch bubbles' rule); the
    // dimension overlay re-projects the latched point each frame so it
    // still tracks camera pan/zoom. Valid = projected on-screen this frame;
    // the well falls back to its fixed spot otherwise.
    bool      m_actionAnchorLatched = false;
    glm::vec3 m_actionAnchorW{0.0f};
    bool  m_actionAnchorValid = false;
    float m_actionAnchorX = 0.0f;
    float m_actionAnchorY = 0.0f;

    // Last hover pick, reused by cursor-zoom so wheel ticks never ray-cast
    // the document themselves (see Application_Viewport zoom handler).
    bool m_zoomFocusHit = false;
    glm::vec3 m_zoomFocusPoint{0.0f};
    int m_zoomFocusFrame = -1;

    // Pan depth-anchor gesture tracking (see the anchoredPan lambda in the
    // camera-drag handler): the anchor is captured once per pan gesture —
    // desktop gestures live for as long as a camera button stays held,
    // touch gestures for as long as two-finger pan events keep arriving.
    bool m_panAnchorHeld = false;   // desktop: a camera button hold owns the anchor
    int m_lastTouchPanFrame = -1000; // touch: last frame a two-finger pan applied
    // Orbit re-anchors its pivot onto the geometry at the VIEW CENTRE at the
    // start of each orbit gesture, so it spins around the object instead of a
    // point that drifted behind it (cursor-zoom leaves the target off the
    // surface → orbit swings the model sideways, reading as pan+rotate). Held
    // for the gesture's lifetime; the centre pick is on the view axis, so
    // moving the target along it doesn't shift the image — only the pivot.
    bool m_orbitAnchorHeld = false;

    // Click-cycling state: first click at a spot picks the visible FACE,
    // a second click at the same spot cycles to the sketch region covered
    // by / behind that face — resolves the face-vs-region ambiguity when
    // bodies sit on both sides of their source sketch plane.
    glm::vec2 m_pickCyclePos{-1000.0f, -1000.0f};
    double m_pickCycleTick = 0.0; // ImGui time of the last pick at m_pickCyclePos
    // Touch: after a double-tap escalates to the body, ImGui's queued 2nd-tap
    // click can land a few frames LATER and revert to the face. Ignore face-select
    // clicks until this time (set ~0.5s out on escalation; bounded so a genuine
    // later tap isn't swallowed).
    double m_suppressFaceClickUntil = 0.0;
    int m_pickCycleLast = -1; // -1 none, 0 face, 1 region

    // Resolve the pull direction + neutral-plane point from the current
    // axis choice, the picked faces, and the body's bounds.
    bool resolveTaperFrame(glm::vec3& dirOut, glm::vec3& neutralOut) const;

    // Interactive Pattern (Linear / Radial). Same live-preview-via-history idiom
    // as push/pull and resize: each parameter change replays an updated PatternOp,
    // commit leaves the op in history at the user's values, cancel undoes it.
    enum class PatternKind { Linear, Radial };
    bool m_patternActive = false;
    PatternKind m_patternKind = PatternKind::Linear;
    int m_patternBodyId = -1;
    int m_patternAxisIdx = 0; // 0=X, 1=Y, 2=Z (used when m_patternAxisId < 0)
    int m_patternAxisId = -1; // selected construction axis id; -1 = world axis
    int m_patternCount = 3;
    float m_patternDistance = 5.0f; // linear: spacing in mm along chosen axis
    float m_patternAngle = 360.0f;   // radial: total sweep in degrees
    float m_patternOriginX = 0.0f, m_patternOriginY = 0.0f, m_patternOriginZ = 0.0f;
    bool m_patternPickingOrigin = false; // viewport is in axis-origin-pick mode
    // The live preview: ONE PatternOp toggled against the document, recorded
    // on History only at commit. Replaced a real history step pushed and
    // undone every frame — see LiveOpPreview for what that cost.
    materializr::LiveOpPreview m_patternPreview;
    bool m_patternInputFocus = true;
    char m_patternCountBuf[16] = "3";
    char m_patternDistanceBuf[32] = "5.0";
    char m_patternAngleBuf[32] = "360.0";

    void beginPattern(PatternKind kind);
    void updatePattern();      // (re-)push a preview PatternOp from current state
    void commitPattern();      // leave preview as the final op + clean up state
    void cancelPattern();      // undo preview if any + clean up state
    void renderPatternPanel(); // ImGui popup contents

    // Interactive Loft popup — N profile sections snapshotted from the
    // selected sketches (in click order) at begin time, plus Solid/Shell +
    // Smooth/Ruled toggles, all driving a live preview pushed onto history
    // (same pattern as Linear/Radial Pattern). LoftOp itself has always been
    // N-capable; this layer feeds it a whole stack of ribs. Each section has
    // its own Flip toggle — reversing a wire's vertex order re-pairs it
    // against its neighbours, the usual fix for the "apex pinch / twist"
    // output when start vertices don't line up — and the panel can reorder
    // sections, since ThruSections skins them in the order given.
    struct LoftSection {
        int sketchId = -1;                // identity for the panel label
        TopoDS_Wire outer;
        // Inner (hole) wires, so a ring section lofts into a tube.
        std::vector<TopoDS_Wire> holes;
        bool reverse = false;             // flip vertex order when feeding LoftOp
    };
    bool m_loftActive = false;
    std::vector<LoftSection> m_loftSections;
    // Bridge candidate: set when every loft section is a face of one body.
    int m_loftBridgeBodyId = -1;
    std::vector<TopoDS_Shape> m_loftBridgeFaces;
    bool m_loftSolid = true;
    bool m_loftRuled = false;
    // ONE LoftOp (or GuidedLoftOp in rails mode) toggled against the document;
    // History sees it only at commit. See LiveOpPreview.
    materializr::LiveOpPreview m_loftPreview;
    // Guided ("rails") mode: selected sketches WITHOUT a closed region are
    // treated as open rail curves when exactly one closed base profile is also
    // selected — beginLoft auto-detects and the panel switches to the rails
    // variant driving a GuidedLoftOp instead of a section LoftOp.
    bool m_loftRailsMode = false;
    gp_Pln m_loftBasePlane;
    struct LoftRail {
        int sketchId = -1;
        TopoDS_Wire wire;
    };
    std::vector<LoftRail> m_loftRails;

    void beginLoft();          // reads selection, snapshots wires, pushes initial preview
    void updateLoft();         // re-push preview with current params
    void commitLoft();
    void cancelLoft();
    void renderLoftPanel();

    // ── Boundary Fill (silhouette intersection) ──
    // N closed sketches, each treated as a silhouette: every profile is
    // extruded through the others' extent and the prisms are intersected
    // (visual hull). Separate feature from Loft by design — no section
    // ordering, no rails, no formula sensitivity.
    struct BFillProfile {
        int sketchId = -1;
        TopoDS_Wire outer;
        std::vector<TopoDS_Wire> holes;
        gp_Pln plane;
    };
    bool m_bfillActive = false;
    std::vector<BFillProfile> m_bfillProfiles;
    // ONE BoundaryFillOp toggled against the document; History sees it only
    // at commit. See LiveOpPreview.
    materializr::LiveOpPreview m_bfillPreview;

    void beginBoundaryFill();
    void updateBoundaryFill();
    void commitBoundaryFill();
    void cancelBoundaryFill();
    void renderBoundaryFillPanel();

    // ── Reference image (photo underlay hosted on a construction plane) ──
    // Import: file dialog → decode/validate → addPlane + setRefImage; the
    // photo then moves/rotates via the normal plane gizmo and is sketch-on-able
    // like any plane. The panel (shown while an image-hosting plane is
    // selected) drives opacity / physical width / the ruler-calibration popup.
    void beginRefImageImport();
    void renderRefImagePanel();
    void renderRefImageControls(int planeId);          // opacity / size / Calibrate
    void renderRefImageCalibrationPopup(int planeId);  // the two-click ruler popup
    void attachRefImageToPlane(int planeId);
    bool loadRefImageFile(const std::string& path, RefImageEntry& out,
                          std::string& baseName);
    // Calibration popup state: which plane's image is being calibrated
    // (-1 = closed), the ImGui preview texture (panel-owned, one at a time),
    // and up to two picked points in IMAGE-PIXEL coordinates.
    int          m_refImgCalibPlane = -1;
    unsigned int m_refImgPreviewTex = 0;
    int          m_refImgPreviewPlane = -1;
    int          m_refImgPreviewW = 0, m_refImgPreviewH = 0;
    int          m_refImgPickCount = 0;
    float        m_refImgPickPx[2][2] = {{0, 0}, {0, 0}};
    char         m_refImgDistBuf[32] = "10";
    // Calibrate preview navigation: scroll-to-zoom (cursor-anchored) +
    // right-drag pan, so ruler ticks on a big phone photo are pickable.
    float        m_refImgCalibZoom = 1.0f;
    float        m_refImgCalibPan[2] = {0, 0};

    // Interactive Construction Plane popup. ConstructionPlanePlugin fires
    // requestInteractiveOp("ConstructionPlane"); Application reads the
    // current selection (a planar face enables Parallel-to-Face mode) and
    // opens a small popup with XY / XZ / YZ / Parallel-to-Face + an offset
    // slider, live-previewed via ConstructionPlaneOp on history. Same Apply
    // / Cancel idiom as Loft and Pattern.
    bool m_planeOpActive = false;
    int  m_planeOpKindIdx = 0;   // 0=XY, 1=XZ, 2=YZ, 3=ParallelToFace
    double m_planeOpOffset = 0.0;
    gp_Pln m_planeOpBaseFace;     // host face's plane when Parallel-to-Face is available
    bool m_planeOpHaveFace = false;
    // The preview ConstructionPlaneOp — rebuilt per frame (parameters vary by
    // kind) but never pushed onto History until commit. See LiveOpPreview.
    materializr::LiveOpPreview m_planeOpPreview;
    char m_planeOpOffsetBuf[32] = "0.0";
    // Typeable "rotate by N° around X/Y/Z" applied to the current preview
    // plane via Document::setPlane. Keeps the offset slider + base
    // orientation untouched (we transform the live plane on top), and the
    // user can stack multiple rotations by re-clicking Apply.
    float m_planeOpRotDeg = 0.0f;
    char  m_planeOpRotBuf[32] = "0.0";
    // One per axis: a compound tilt is a single intent, not three sequential
    // applies through a radio button. The values are ABSOLUTE -- the plane's
    // total rotation from its chosen alignment -- so the fields keep reading
    // what you set, and pressing Enter twice cannot silently double it.
    char  m_planeOpRotBufX[32] = "0.0";
    char  m_planeOpRotBufY[32] = "0.0";
    char  m_planeOpRotBufZ[32] = "0.0";
    // Applied after EVERY preview rebuild, so changing the alignment or
    // nudging the offset no longer throws the rotation away.
    float m_planeOpRotX = 0.0f, m_planeOpRotY = 0.0f, m_planeOpRotZ = 0.0f;
    void applyPlaneOpRotation();
    int   m_planeOpRotAxisIdx = 0; // 0=X, 1=Z (user up), 2=Y

    // Selection-derived inputs for the kind-index 4/5/6 creation modes,
    // captured once at beginConstructionPlane. Each reduces to a plane with
    // normal N through point P (computed by computeDerivedPlaneNP), fed to
    // the op as ParallelToFace-style basePlane + point.
    //   4 = Midplane           — needs two planar planes/faces
    //   5 = Normal to axis/edge — needs an axis or straight edge (+ point)
    //   6 = Tangent to cylinder — needs a cylindrical face (+ a side ref)
    bool   m_planeOpHaveTwoPlanes = false;
    gp_Pln m_planeOpPlaneA, m_planeOpPlaneB;
    bool   m_planeOpHaveAxis = false;
    gp_Ax1 m_planeOpAxis;
    gp_Pnt m_planeOpAxisPoint;
    bool   m_planeOpHaveCylinder = false;
    gp_Ax1 m_planeOpCylAxis;
    double m_planeOpCylRadius = 0.0;
    gp_Dir m_planeOpCylRefDir{1.0, 0.0, 0.0};
    // Resolve the captured inputs for a derived kind index into a base
    // (normal, through-point) pair, pre-offset. Returns false if the needed
    // selection isn't present.
    bool computeDerivedPlaneNP(int kindIdx, gp_Dir& outNormal, gp_Pnt& outPoint) const;

    // Reference image being placed WITH a construction plane. The preview
    // plane is destroyed and rebuilt on every alignment/offset change
    // (LiveOpPreview::hold drops the previous instance), so the image cannot
    // live on it -- it is held here and re-attached after each preview apply.
    // That is what lets the photo be visible, scaled and calibrated while the
    // plane is still being positioned, instead of only after Apply.
    bool m_planeOpWantRefImage = false;
    bool m_planeOpHasPendingImage = false;
    RefImageEntry m_planeOpPendingImage;
    void pickPlaneOpRefImage();
    void reattachPlaneOpRefImage();
    void beginConstructionPlane();
    // Open the plane popup forced to a specific kind index (4=Midplane,
    // 5=Normal-to-Axis, 6=Tangent), used by the Properties-panel contextual
    // "Create …" buttons.
    void beginConstructionPlaneMode(int kindIdx);
    void updateConstructionPlane();
    void commitConstructionPlane();
    void cancelConstructionPlane();
    void renderConstructionPlanePanel();

    // Interactive Construction Axis popup — direct parallel to the plane
    // popup above. Plugin fires requestInteractiveOp("ConstructionAxis");
    // we open a small popup with World-X / Y / Z radios + an origin point
    // input. (Two-point and face-normal modes are listed but their
    // viewport-pick UX is deferred — typing the origin coords is enough
    // for the v0.6.x line.) Live preview via ConstructionAxisOp on
    // history, same Apply / Cancel idiom as the plane popup.
    bool m_axisOpActive = false;
    int  m_axisOpKindIdx = 0;     // 0=WorldX,1=userY,2=userZ,3=Cyl,4=Edge,5=2Pts,6=FaceNormal,7=2Planes
    double m_axisOpOrigin[3] = {0.0, 0.0, 0.0}; // world coords
    char m_axisOpOriginBuf[3][24] = {"0.0", "0.0", "0.0"};
    // Same arrangement as the plane popup's preview. See LiveOpPreview.
    materializr::LiveOpPreview m_axisOpPreview;

    // Selection-derived inputs for kind indices 3–7, captured at
    // beginConstructionAxis. Each reduces to an (origin, direction) the host
    // computes (computeDerivedAxisOD) and feeds via setOrigin/setDirection.
    bool   m_axisOpHaveCylinder = false;  gp_Ax1 m_axisOpCylAxis;
    bool   m_axisOpHaveEdge = false;      gp_Ax1 m_axisOpEdgeAxis;
    bool   m_axisOpHaveTwoVerts = false;  gp_Pnt m_axisOpV1, m_axisOpV2;
    bool   m_axisOpHaveFaceNormal = false; gp_Pnt m_axisOpFacePt; gp_Dir m_axisOpFaceNormal;
    bool   m_axisOpHaveTwoPlanes = false; gp_Pln m_axisOpPlaneA, m_axisOpPlaneB;
    bool computeDerivedAxisOD(int kindIdx, gp_Pnt& outOrigin, gp_Dir& outDir) const;

    void beginConstructionAxis();
    // Open the axis popup forced to a kind index (used by the Tools-panel
    // "Add Axis…" buttons): 3=Cylinder, 4=Edge, 5=Two points, 6=Face normal,
    // 7=Two-plane intersection.
    void beginConstructionAxisMode(int kindIdx);
    void updateConstructionAxis();
    void commitConstructionAxis();
    void cancelConstructionAxis();
    void renderConstructionAxisPanel();

    // Primitive creation popup. Plugin fires requestInteractiveOp("Primitive*")
    // and Application opens a small panel with the parameters appropriate for
    // the chosen kind (extents for Box, radius/height for Cylinder/Cone/etc.
    // /Origin for all of them). Confirm pushes a PrimitiveOp onto history;
    // Cancel just closes the popup. No live preview yet — the geometry's
    // cheap to recompute on commit and a stale preview body would have to be
    // undone on every keystroke. (Steve: "primitives popup parameters; live-
    // preview / fancier UI after 1.0".)
    bool   m_primitivePopupActive = false;
    int    m_primitivePopupKind   = 0; // 0=Box,1=Cyl,2=Sphere,3=Cone,4=Torus
    double m_primitivePopupExtents[3]  = {10.0, 10.0, 10.0}; // box X/Y/Z
    double m_primitivePopupRadius      = 5.0;                 // cyl/sphere/cone bottom/torus major
    double m_primitivePopupHeight      = 10.0;                // cyl/cone
    double m_primitivePopupTopRadius   = 0.0;                 // cone tip
    double m_primitivePopupMinorRadius = 2.0;                 // torus tube
    double m_primitivePopupOrigin[3]   = {0.0, 0.0, 0.0};     // world origin
    void beginPrimitivePopup(int kindIdx);
    void commitPrimitivePopup();
    void cancelPrimitivePopup();
    void renderPrimitivePopup();

    // STL import options dialog. Opened from the Import > STL menu (the plugin
    // routes through requestInteractiveOp("StlImport")). Collects a file path
    // (via Browse), a fidelity/accuracy slider, and a wireframe toggle, then
    // runs StlIO::import on commit. Mirrors the primitive-popup begin/render/
    // commit/cancel pattern.
    bool   m_stlDialogActive = false;
    std::string m_stlDialogPath;
    float  m_stlDialogAccuracy = 0.5f;
    bool   m_stlDialogWireframe = true;
    void beginStlImportDialog();
    void commitStlImport();
    void cancelStlImport();
    void renderStlImportDialog();

    // Unfold / Flatten — "lay it flat" for laser/CNC/templates. beginUnfoldDialog
    // runs the planar-net unfold on the selected body and opens a 2D Flat-Pattern
    // dialog (cut + fold lines), with a material dropdown driving fold handling
    // and SVG export. See modeling/Unfold.h.
    bool m_unfoldDialogActive = false;
    int  m_unfoldBodyId = -1;
    std::unique_ptr<materializr::FlatPattern> m_unfoldPattern;
    materializr::Rigidity m_unfoldRigidity = materializr::Rigidity::SemiRigid;
    float m_unfoldThicknessMm = 5.0f;
    std::vector<TopoDS_Face> m_unfoldSourceFaces; // kept so the bevel slider can re-run
    float m_unfoldMaxBevelDeg = 10.0f;            // angular tolerance: max bevel per score line
    bool  m_unfoldConformal = false;              // LSCM unwrap (one stretchy piece) vs developable pieces
    bool  m_unfoldPageA4 = false;                 // PDF export page size: A4 vs US Letter
    float m_unfoldRotationDeg = 0.0f;             // viewer/export rotation of the whole flat pattern
    int   m_unfoldExportFmt = 0;                  // export format: 0 = SVG (no page grid), 1 = PDF (tiled)
    int   m_unfoldRegDensity = 2;                 // PDF alignment-mark density: 0 None,1 Sparse,2 Normal,3 Dense
    void beginUnfoldDialog();
    void recomputeUnfold();
    void renderUnfoldDialog();

    // Revolve popup. Opens when the user clicks Revolve in the body Tools
    // panel; takes a sketch profile + an axis (canonical world axis or a
    // Construction Axis from the document) + angle + mode. Apply pushes a
    // RevolveOp. Pre-fills sketch / axis / target body from the current
    // selection so the common "select profile, select axis, click Revolve"
    // flow lands ready-to-Apply.
    bool m_revolveActive = false;
    // What the revolve does. 0 = "Rotate Body": apply a TransformOp::Rotate
    // around the picked axis to every selected body (multi-body supported,
    // no sketch needed, no new geometry created). 1 = "Sweep Sketch": full
    // RevolveOp that sweeps a sketch profile around the axis into a new /
    // boolean'd body (single-body target).
    int  m_revolveWhatIdx  = 0;
    int  m_revolveSketchId = -1;
    int  m_revolveAxisId   = -1;          // -1 = use canonical world axis below
    int  m_revolveWorldAxisIdx = 2;       // 0=X, 1=Y(user)=worldZ, 2=Z(user)=worldY
    int  m_revolveBodyId   = -1;          // primary body — first one in the selection
    std::vector<int> m_revolveBodyIds;    // all selected bodies (>=1); Rotate Body iterates this
    int  m_revolveModeIdx  = 0;           // 0=NewBody 1=Union 2=Cut 3=Intersect (Sweep mode)
    float m_revolveAngle   = 360.0f;
    char  m_revolveAngleBuf[24] = "360.0";
    // Live-preview state for Rotate Body mode. Snapshot of the body's
    // original shape taken at popup open; each angle change applies a fresh
    // transform from the snapshot (not from the current state, which would
    // accumulate). Apply restores the snapshot before pushing the real
    // TransformOp so the history step computes its own previousShape
    // correctly. Cancel restores and aborts.
    bool         m_revolveLiveActive = false;
    int          m_revolveOrigBodyId = -1;
    TopoDS_Shape m_revolveOrigShape;
    float        m_revolveLastAppliedAngle = 0.0f;

    // Arc-drag interaction state. The yellow arced arrow in the viewport
    // becomes clickable when the popup is open in Rotate Body mode:
    // press over the arc to grab it, drag tangentially to spin the body
    // around the axis. We track per-frame cursor-angle deltas (rather than
    // a single offset) so the drag is robust to the atan2 wrap-around when
    // the cursor crosses the ±π boundary. m_revolveArcWasHovered persists
    // for one frame so the picker / selection code can skip clicks that
    // landed on the arc.
    bool  m_revolveArcDragging = false;
    bool  m_revolveArcWasHovered = false;
    float m_revolveArcDragAngleAccum = 0.0f;   // accumulated cursor delta (rad)
    float m_revolveArcDragLastCursorAng = 0.0f;
    float m_revolveArcDragStartBodyAng = 0.0f; // m_revolveAngle when drag began

    // Captures the current selection (sketch + axis + bodies) and opens
    // the Revolve popup. Called from both the Revolve plugin's toolbar
    // button (via requestInteractiveOp dispatch) and any future Command
    // Palette entry. Keeping it as a single helper means the "what to do
    // when revolve is requested" logic stays in one place.
    void beginRevolve();
    void renderRevolvePopup();
    void applyRevolve();
    // Cancel / commit helpers — share the restore-original logic.
    void revolveLiveBegin();
    void revolveLiveApply(float angle);
    void revolveLiveRestore();

    // Interactive "Rotate Plane About Axis" popup. Triggered from the Items
    // panel plane context menu (m_itemsPanel->setRotatePlaneCallback). Tilts /
    // hinges an existing construction plane about a chosen line by a typed
    // angle. The line can be the plane's own U / V axis (tilt in place), a
    // construction axis, a selected straight edge, or a selected cylindrical
    // face's centreline — each resolved to a gp_Ax1 at open time (transient,
    // nothing persisted). Matches the plane gizmo's model: writes straight
    // through Document::setPlane with no history op (plane transforms are
    // intentionally outside undo — see Application_Viewport.cpp's planeOnly
    // branch). Live preview re-bases from m_rotPlaneOriginal each change;
    // Apply leaves the current pose, Cancel restores the snapshot.
    // ── Lay Flat on Plane (viewport right-click on a planar face) ──────────
    // Rotates the body so the picked face sits flush on a chosen plane (world
    // or construction), with an offset along the plane's normal and an
    // optional exact position for the face's centre. Live preview against a
    // snapshot; Apply commits as ordinary Transform history steps (rotate +
    // translate), so undo/reload need nothing new.
    bool m_alignActive = false;
    int  m_alignBodyId = -1;
    // Sketch mode: exactly one of bodyId / sketchId is >= 0. A sketch aligns
    // its PLANE onto the target (drawing side up; Flip turns it over) and
    // commits as ONE SketchTransformOp instead of the body's rotate+move.
    int  m_alignSketchId = -1;
    gp_Pln m_alignSketchPlaneBefore;
    TopoDS_Shape m_alignFace;
    TopoDS_Shape m_alignSnapshot;
    int   m_alignPlaneIdx = 0;            // 0..2 world, then construction planes
    float m_alignOffset = 0.0f;
    bool  m_alignFlip = false;
    bool  m_alignSetPos = false;
    float m_alignU = 0.0f, m_alignV = 0.0f;
    char  m_alignOffsetBuf[32] = "0.00";
    char  m_alignUBuf[32] = "0.00";
    char  m_alignVBuf[32] = "0.00";
    void beginAlignFaceToPlane(int bodyId, const TopoDS_Shape& face);
    void beginAlignSketchToPlane(int sketchId);
    void renderAlignFacePopup();
    bool computeAlignTransform(gp_Trsf& rotOut, gp_Trsf& moveOut,
                               gp_Pnt& centerOut, bool& needRot, bool& needMove);
    void applyAlignPreview();
    void cancelAlignFace();

    bool   m_rotPlaneActive = false;
    int    m_rotPlaneId = -1;             // target plane id
    gp_Pln m_rotPlaneOriginal;            // snapshot for preview re-base + cancel
    float  m_rotPlaneAngle = 0.0f;        // degrees
    char   m_rotPlaneAngleBuf[24] = "0.0";
    int    m_rotPlaneHingeIdx = 0;        // index into the parallel vectors below
    std::vector<gp_Ax1>      m_rotPlaneHinges;       // resolved hinge per combo entry
    std::vector<std::string> m_rotPlaneHingeLabels;  // combo display labels

    void beginRotatePlaneAboutAxis(int planeId);
    void renderRotatePlaneAboutAxisPopup();
    void applyRotatePlanePreview();       // setPlane(original rotated about hinge by angle)
    void cancelRotatePlaneAboutAxis();    // restore snapshot + close

    // Sketch Move type-in panel: when the Move gizmo is active on a single
    // selected standalone sketch (the sketch-as-construction-plane workflow),
    // this small popup offers X/Y/Z inputs for an exact translation. Apply
    // pushes a single SketchTransformOp with the typed offset; same auto-
    // reopen-when-conditions-met pattern as the multi-transform rotate panel.
    float m_sketchMove[3] = {0.0f, 0.0f, 0.0f};
    // Text-buffer mirrors of m_sketchMove for the three input fields. We keep
    // a buffer + atof flow (rather than ImGui::InputFloat) because InputFloat
    // doesn't commit until Enter / focus-out, which the user routinely missed
    // before clicking Apply.
    char m_sketchMoveBuf[3][32] = { "0", "0", "0" };
    bool m_sketchMovePanelOpen = true;
    bool m_sketchMoveConditionsMet = false;

    void renderSketchMovePanel();
    void applySketchMove();

    // Snap-grid corner widget (next to the ViewCube). Shows the current step
    // (0.1 / 1 / 10 mm) and gets a solid-blue border when snap is on. Click
    // opens a small popup with the snap toggle + step radios. Changes save
    // immediately so the choice persists across launches.
    void renderSnapWidget();
    // The snap settings popup body (checkbox + step radios), shared by the
    // viewport corner widget and the im-touch top cluster's Snap button.
    // Caller does OpenPopup("SnapSettings") in its own window context.
    void renderSnapSettingsPopup();

    // Sketch-mode Linear / Radial patterns. Simpler than body patterns: the
    // sketch is on a fixed 2D plane so there's no axis radio. Linear copies
    // along the sketch's +X axis by `m_sketchPatternDistance` per step;
    // radial rotates around the user-supplied (x, y) origin in sketch coords
    // for a total sweep of `m_sketchPatternAngle` degrees. The popup is a
    // small modal — no live preview. On apply we run an inline geometry copy
    // similar to SketchCopy / Mirror and push a single SketchEditOp.
    bool m_sketchPatternActive = false;
    PatternKind m_sketchPatternKind = PatternKind::Linear;
    int  m_sketchPatternCount = 3;
    float m_sketchPatternDistance = 5.0f;
    float m_sketchPatternAngle = 360.0f;
    float m_sketchPatternOriginX = 0.0f;
    float m_sketchPatternOriginY = 0.0f;
    bool m_sketchPatternFocusInput = false;
    char m_sketchPatternCountBuf[16]    = "3";
    char m_sketchPatternDistanceBuf[32] = "5.0";
    char m_sketchPatternAngleBuf[32]    = "360.0";
    char m_sketchPatternOXBuf[32] = "0.0";
    char m_sketchPatternOYBuf[32] = "0.0";
    // True while the user is clicking in the sketch viewport to set the
    // radial origin. The next sketch-mode click is captured by the pattern
    // popup instead of going through SketchTool.
    bool m_sketchPatternPickingOrigin = false;

    // Snapshot of the sketch at popup-open. Every parameter change replays
    // from this snapshot so the preview reflects only the current values,
    // not an accumulation of previous previews. On Apply we diff against
    // this snapshot to push the SketchEditOp; on Cancel we restore it.
    std::shared_ptr<materializr::Sketch> m_sketchPatternBefore;
    // Involved geometry captured at popup-open. We re-use the same IDs each
    // preview frame since the snapshot is restored each time (new IDs from
    // earlier preview copies wouldn't survive the restore).
    std::set<int> m_sketchPatternPts;
    std::set<int> m_sketchPatternLines;
    bool          m_sketchPatternSelectAll = false; // include all circles + arcs

    void beginSketchPattern(PatternKind kind);
    void updateSketchPattern();   // re-apply preview from m_sketchPatternBefore
    void commitSketchPattern();   // leave preview in place + push SketchEditOp
    void cancelSketchPattern();   // restore m_sketchPatternBefore + clear state
    void renderSketchPatternPopup();

    // User-facing axis convention follows 3D-printer / Z-up: X = side-to-side,
    // Y = forward-back, Z = up. Materializr's world stays Y-up internally, so
    // the user's axis index translates to a world direction via this helper
    // (user X → world X, user Y → world Z, user Z → world Y). Also returns
    // which world-axis index (0/1/2 = world X/Y/Z) the user index resolves to,
    // useful for coordinate-component access like `pos[worldIdx] = ...`.
    static glm::vec3 userAxisToWorldVec(int userIdx);
    static int       userAxisToWorldIdx(int userIdx);

    // Interactive extrude state now lives in ExtrudeController (declared
    // with the other iops); it is the first user of the LiveOp preview model.
    // Right-click face context menu state
    int m_contextMenuBodyId = -1;
    TopoDS_Shape m_contextMenuFace;
    int m_contextMenuPlaneId = -1; // >=0 → the pending context menu is for a plane
    int m_contextMenuSketchId = -1; // >=0 → the pending context menu is for a sketch
    bool m_contextMenuPending = false;

    // Project file + dirty tracking
    std::string m_currentProjectPath;          // empty until first save/load
    // Human name of the current project ("Materializr mug.materializr"). On
    // Android m_currentProjectPath can be a content:// document URI (so
    // quick-save can overwrite the picked file in place); this carries the
    // readable name for the title bar and for seeding Save As.
    std::string m_currentProjectName;
    std::vector<AppSettings::RecentProject> m_recentProjects; // Open Recent (persisted)
    int m_savedAtHistoryStep = -1;             // history index when last saved/loaded
    bool m_unsavedNonHistoryChanges = false;   // for mutations outside History (imports, etc.)

    // Close-with-unsaved-changes prompt. The prompt is shared between two
    // close intents: exiting the app, or just closing the current project
    // (File → Close Project). The post-save action picks which happens after
    // a successful Save.
    bool m_showSavePrompt = false;
    bool m_confirmedClose = false;
    bool m_closeAfterSave = false;             // set when user picked Save in the prompt

    // Loft (plugin) prompt: LoftPlugin sets this via PluginContext::request
    // InteractiveOp("LoftPickSecond") when the user clicks Loft with only one
    // sketch selected. We render a modal popup nudging them to Ctrl-click a
    // second sketch. Latched + cleared by the popup.
    bool m_loftPickHintPending = false;
    bool m_loftPickHintVisible = false;  // non-modal banner, dismissed on 2-sketch select or X
    enum class PostSaveAction { None, CloseProject, OpenProject };
    PostSaveAction m_postSaveAction = PostSaveAction::None;
    // When opening a project (dialog or Open Recent) with unsaved changes, the
    // actual open is deferred here and run after the save prompt resolves.
    std::function<void()> m_pendingOpenAction;

    // Settings option: re-open the most recent project on launch (only if it
    // wasn't explicitly closed before quit). The "last open project path" lives
    // in AppSettings::lastProjectPath and is mirrored from m_currentProjectPath
    // on save/load and cleared on closeProject().
    bool m_autoOpenLastProject = false;
    bool m_checkForUpdatesOnLaunch = true;
    // Beta channel opt-in: update checks also consider GitHub pre-releases.
    bool m_includePrereleases = false;
    // Supporter state: silences the every-launch support prompt (see
    // AppSettings::supporter for how it gets set).
    bool m_supporter = false;

    // Set by the --safe-mode CLI flag. When true, loadAppSettings stomps
    // rendering, autosave, and auto-open-last-project back to safe defaults
    // and persists them.
    bool m_safeMode = false;
};

} // namespace materializr
