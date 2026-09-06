#include "ui/UiTheme.h"
#include "gl_common.h"
#include <SDL.h>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <limits>
#include <map>
#include <set>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#include <pmmintrin.h>
#define MZR_HAS_SSE 1
#endif

namespace {
// OCCT's boolean/intersection math assumes the default FPU mode (round-to-
// nearest, denormals kept). GL drivers flip the SSE flush-to-zero / denormals-
// are-zero flags during rendering, after which an OCCT boolean that ran clean
// can silently degenerate. Call this after any GL work that precedes OCCT (the
// deferred-op progress frames) to put the FPU back the way OCCT expects.
inline void resetFpuForOcct() {
#ifdef MZR_HAS_SSE
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_OFF);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_OFF);
#endif
}
} // namespace

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "app/Application.h"
#include "i18n.h"
#include "app/Window.h"
#include "ui_scale.h"
#include "touch_mode.h"
#include "viewport/Viewport.h"
#include "viewport/Grid.h"
#include "viewport/ShapeRenderer.h"
#include "viewport/SketchRenderer.h"
#include "viewport/ViewCube.h"
#include "viewport/Picker.h"
#include "viewport/Gizmo.h"
#include "viewport/SelectionHighlight.h"
#include "viewport/BoxSelect.h"
#include "viewport/SectionView.h"
#include "viewport/EdgeRenderer.h"
#include "viewport/BackgroundRenderer.h"
#include "core/Document.h"
#include "core/History.h"
#include "core/SelectionManager.h"
#include "ui/Toolbar.h"
#include "ui/TouchIcons.h"
#include "ui/TouchTheme.h"
#include "ui/HistoryPanel.h"
#include "ui/ItemsPanel.h"
#include "ui/StatusBar.h"
#include "ui/ThemeManager.h"
#include "ui/PropertiesPanel.h"
#include "ui/AboutDialog.h"
#include "ui/WelcomeScreen.h"
#include "ui/LandingPage.h"
#include "app/ProjectSession.h"
#include "ui_layout_bridge.h"
#include <fstream>
#include "ios_storekit.h"
#include "ui/ShortcutsPanel.h"
#include "ui/HelpPanel.h"
#include "ui/MeasureTool.h"
#include "ui/UpdateChecker.h"
#include "modeling/Sketch.h"
#include "modeling/ThreadOp.h"
#include "modeling/CopyOp.h"
#include "modeling/SketchSolver.h"
#include "modeling/SketchTool.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/MergeFacesOp.h"
#include "modeling/SketchPlaneAxis.h"
#include "core/MeshGuard.h"
#include "modeling/ReplayOp.h"
#include "modeling/OperationFactory.h"
#include "modeling/PushPullOp.h"
#include "modeling/CombineSketchesOp.h"
#include "modeling/DuplicateSketchOp.h"
#include "modeling/TransformOp.h"
#include "core/Units.h"
#include "core/LengthEdit.h"
#include "modeling/FilletProbe.h"
#include "modeling/MirrorOp.h"
#include "modeling/FilletOp.h"
#include "modeling/ChamferOp.h"
#include "modeling/ShellOp.h"
#include "modeling/DeleteOp.h"
#include "modeling/SketchEditOp.h"
#include "modeling/MoveHoleOp.h"
#include "modeling/SketchTransformOp.h"
#include "modeling/ResizeCylindricalOp.h"
#include "io/StepIO.h"
#include "io/StlExport.h"
#include "io/SvgExport.h"
#include "io/DxfExport.h"
#include "io/FileDialogs.h"
#include "modeling/SvgImport.h"
#include "modeling/AirfoilImport.h"
#include "io/ProjectIO.h"
#include "io/SketchRecovery.h"
#include "io/ProjectRecovery.h"
#include "io/Settings.h"
#include "mobile_files.h" // mobileLastDocUri/Name + mobileOpenUri (Open Recent via persisted refs)
#include "ios_platform.h" // iosInBackground (inline false off-iOS)
#include "core/EventBus.h"
#include "core/Events.h"
#include "core/Verbose.h"
#include "core/UiKeepAlive.h"
#include "core/ThrowTrace.h"
#include "core/NumParse.h"
#include "plugin/PluginContext.h"
#include "plugin/PluginRegistry.h"

namespace materializr { namespace force_link { void linkAll(); } }

#include <imgui.h>
#include <imgui_internal.h> // dock-node tab-bar policy (per-node LocalFlags)
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>

#include "app/Window.h"
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <gp_Ax3.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <Geom_Plane.hxx>
#include <GeomLib_IsPlanarSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_ToroidalSurface.hxx>
#include <GeomAbs_CurveType.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <cstring>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_Triangle.hxx>
#include <TopLoc_Location.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <TopExp_Explorer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopTools_MapOfShape.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Mat.hxx>
#include <gp_XYZ.hxx>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <BRep_Builder.hxx>
#include <stdexcept>
#include <cstdio>
#ifdef _WIN32
#include <windows.h> // GetModuleFileNameA for exe dir (font path lookup)
#elif defined(__APPLE__)
#include <mach-o/dyld.h> // _NSGetExecutablePath for exe dir (no /proc on macOS)
#else
#include <unistd.h>  // readlink for resolving /proc/self/exe → exe dir (font path lookup)
#endif

namespace materializr {

namespace {
// Tapping the tool that is ALREADY active puts it down — back to Select/Move —
// instead of re-arming the same tool. Every sketch tool button used to call
// setMode() unconditionally, so a drawing tool could only be left by explicitly
// reaching for Select; on a tablet, where the reflex is to tap the lit button
// again, that reads as the tool being stuck (#71).
//
// setMode() already cancels any in-progress shape and clears the placement
// state, so a second tap on a half-drawn shape abandons it — the same outcome
// as Escape, which is what that reflex expects.
void toggleSketchMode(SketchTool* tool, SketchToolMode mode) {
    if (!tool) return;
    tool->setMode(tool->getMode() == mode ? SketchToolMode::Select : mode);
}
} // namespace

Application::Application(bool safeMode, float uiScaleOverride)
    : m_safeMode(safeMode), m_cliUiScale(uiScaleOverride > 0.0f ? uiScaleOverride : 0.0f) {
    // The CLI scale has to reach the constructor: the INITIAL window size is
    // scaled by it, and setUiScaleOverride() below runs too late for that.
    m_window = std::make_unique<Window>(1600, 900, "Materializr", m_cliUiScale);
    m_viewport = std::make_unique<Viewport>();
    m_grid = std::make_unique<Grid>();
    m_shapeRenderer = std::make_unique<ShapeRenderer>();
    m_sketchRenderer = std::make_unique<SketchRenderer>();
    m_edgeRenderer = std::make_unique<EdgeRenderer>();
    // PlaneRenderer ownership moved to ConstructionPlanePlugin (registered
    // as a plugin render pass). Application no longer touches it directly.
    m_backgroundRenderer = std::make_unique<BackgroundRenderer>();
    // First project session ("tab"). The session owns document/history/
    // selection; m_document & co are mirrors into it (see Application.h).
    m_sessions.push_back(std::make_unique<ProjectSession>());
    m_activeSession = 0;
    m_document = m_sessions[0]->document.get();
    m_history = m_sessions[0]->history.get();
    m_selection = m_sessions[0]->selection.get();
    m_eventBus = std::make_unique<EventBus>();

    // Cascade: when a SketchEditOp commits via a user-driven path that
    // identifies the sketch (Properties → Constraints panel today), re-run
    // every enabled ExtrudeOp downstream of that sketch so its body follows.
    m_eventBus->subscribe<SketchEditedEvent>(
        [this](const SketchEditedEvent& e) { cascadeFromSketchEdit(e.sketchId); });

    // Transient status/error messages from non-UI code (plugins, ops).
    m_eventBus->subscribe<materializr::ToastEvent>(
        [this](const materializr::ToastEvent& e) { showToast(e.text, e.seconds); });

    // Document body lifecycle → renderer slot lifecycle. Without this, a
    // PushPullOp::undo (firing on every preview frame during a drag) deletes
    // the body from Document but the renderer keeps drawing its stale mesh
    // — the "banding" effect of N overlapping preview prisms accumulating
    // during a drag. We drop the slot immediately rather than waiting for
    // someone to put the id in m_dirtyBodyIds (nothing does, today).
    m_eventBus->subscribe<materializr::BodyRemovedEvent>(
        [this](const materializr::BodyRemovedEvent& e) {
            if (e.bodyId < 0) return;
            if (m_shapeRenderer) m_shapeRenderer->removeBody(e.bodyId);
            if (m_edgeRenderer)  m_edgeRenderer->removeBody(e.bodyId);
            // Also clear any pending dirty entry — the body is gone, no
            // point asking the partial rebuild to revisit it.
            m_dirtyBodyIds.erase(e.bodyId);
        });

    // Construction-plane lifecycle is handled by ConstructionPlanePlugin —
    // it owns the PlaneRenderer, subscribes to PlaneAdded/Removed/Changed
    // events, and registers a render pass. Application is hands-off.

    // NOTE: we explicitly do NOT cascade off generic HistoryStepEvents. That
    // event also fires for in-flight push/pull preview undos (every drag
    // frame), and those undo-landings on a SketchEditOp would otherwise
    // re-cascade every frame — piling up duplicate bodies. The cascade is
    // driven solely by the explicit SketchEditedEvent above, published by
    // PropertiesPanel (live constraint editor) and HistoryPanel's Apply
    // Changes button. Other history mutators stay out of it.
    m_pluginContext = std::make_unique<PluginContext>();

    m_toolbar = std::make_unique<Toolbar>();
    m_historyPanel = std::make_unique<HistoryPanel>();
    m_itemsPanel = std::make_unique<ItemsPanel>();

    m_sketchTool = std::make_unique<SketchTool>();
    m_viewCube = std::make_unique<ViewCube>();
    m_picker = std::make_unique<Picker>();
    m_gizmo = std::make_unique<Gizmo>();
    m_selectionHighlight = std::make_unique<SelectionHighlight>();
    m_boxSelect = std::make_unique<BoxSelect>();
    m_sectionView = std::make_unique<SectionView>();
    m_statusBar = std::make_unique<StatusBar>();
    m_themeManager = std::make_unique<ThemeManager>();
    m_propertiesPanel = std::make_unique<PropertiesPanel>();
    m_aboutDialog = std::make_unique<AboutDialog>();
    m_welcomeScreen = std::make_unique<WelcomeScreen>();
    m_landingPage = std::make_unique<LandingPage>();
#if defined(MZ_IOS)
    // StoreKit observer must attach at launch (Apple requirement) so a
    // Supporter purchase interrupted in a previous run is redelivered; the
    // main loop consumes the entitlement next to the Welcome screen render.
    iosStoreInit();
#endif
    m_shortcutsPanel = std::make_unique<ShortcutsPanel>();
    m_helpPanel = std::make_unique<HelpPanel>();
    m_measureTool = std::make_unique<MeasureTool>();

    // Wire up references. Everything that consumes the ACTIVE SESSION's
    // document/history/selection is wired in wireDocumentConsumers() (called
    // below, and again on every tab switch); only session-independent wiring
    // stays inline here.
    m_toolbar->setPluginContext(m_pluginContext.get());
    // Touch mode gates the UI scale and the whole input/UX model, and it must be
    // known before fonts and widget sizes are baked below. Resolve it from the
    // saved settings up front (loadAppSettings() re-reads everything once the
    // ImGui context exists); falls back to the platform default if there's no
    // settings file yet.
    {
        AppSettings early = SettingsIO::load(SettingsIO::defaultPath());
        materializr::setTouchMode(early.touchMode);
        // Desktop UI scale must be known before the font atlas is baked in
        // initImGui() below — it's applied at the window level so uiScale()
        // (which fonts + style read) returns it.
        //
        // Linux HiDPI is DETECTED now, not asked (Window::linuxAutoUiScale);
        // the old Low/High setting and its first-run picker are gone. Only
        // --ui-scale / --hidpi still overrides, as the escape hatch for a
        // display whose DPI is reported wrongly.
        if (m_window) {
            if (m_cliUiScale > 0.0f) m_window->setUiScaleOverride(m_cliUiScale);
            // Cursor size rides the same answer, and must be set before
            // initImGui() creates ImGui's system cursors.
            m_window->applyCursorScale();
        }
    }
    // Scale the Tools-panel button heights to match the HiDPI font (touch mode),
    // otherwise 30px buttons under a 2x font overlap. 1.0 in desktop mode.
    if (m_window) {
        m_toolbar->setUiScale(m_window->uiScale());
        // Global scale for fixed-size dialogs/popups (uiSz/uiW), so their
        // hard-coded pixel widths grow with the font instead of clipping.
        materializr::setUiScale(m_window->uiScale());
    }
    // The recut hook reads m_document dynamically ([this] capture), so it is
    // tab-safe installed once; the per-History callbacks live in
    // wireDocumentConsumers().
    installThreadRecutHook();
    m_itemsPanel->setDirtyCallback([this]() { markDirty(); });
    m_itemsPanel->setExportStlCallback([this](int bodyId) { exportBodyAsStl(bodyId); });
    // The Export submenu's format list comes straight from the registry, so a
    // new export plugin appears there without touching ItemsPanel.
    m_itemsPanel->setExportFormatsProvider([]() {
        std::vector<std::string> names;
        for (const auto& f : PluginRegistry::instance().ioFormats())
            if (f.canExport && f.exportDocFn) names.push_back(f.name);
        return names;
    });
    m_itemsPanel->setExportBodiesCallback(
        [this](const std::vector<int>& ids, const std::string& fmt) {
            exportBodiesAs(ids, fmt);
        });
    m_itemsPanel->setExportToProjectCallback(
        [this](const std::vector<int>& ids) { exportBodiesToNewProject(ids); });
    m_itemsPanel->setEditSketchCallback([this](int sketchId) { editSketch(sketchId); });
    m_itemsPanel->setExportSketchSvgCallback([this](int sketchId) { exportSketchAsSvg(sketchId); });
    m_itemsPanel->setExportSketchDxfCallback([this](int sketchId) { exportSketchAsDxf(sketchId); });
    m_itemsPanel->setDuplicateSketchCallback([this](int sketchId) { duplicateSketch(sketchId); });
    m_itemsPanel->setCombineSketchesCallback(
        [this](const std::vector<int>& ids) { combineSketches(ids); });
    m_itemsPanel->setRotatePlaneCallback([this](int planeId) { beginRotatePlaneAboutAxis(planeId); });
    m_propertiesPanel->setRotatePlaneCallback([this](int planeId) { beginRotatePlaneAboutAxis(planeId); });
    m_propertiesPanel->setAttachRefImageCallback(
        [this](int planeId) { attachRefImageToPlane(planeId); });
    m_propertiesPanel->setDirtyCallback([this]() { markDirty(); });
    m_propertiesPanel->setLinkInfoCallback(
        [this](bool isBody, int id) { return linkHintFor(isBody, id); });
    m_propertiesPanel->setRelinkCallback(
        [this](bool isBody, int id) { relinkSketch(isBody, id); });
    // Element-size edits from the Properties panel while sketching: snapshot +
    // re-solve inside recordSketchMutation (so it's one undoable SketchEditOp),
    // then cascade to any body built from the sketch.
    m_propertiesPanel->setSketchMutateCallback(
        [this](const std::function<void()>& mut) {
            recordSketchMutation([&]() {
                mut();
                if (m_activeSketch) {
                    SketchSolver solver;
                    solver.solve(*m_activeSketch);
                }
            });
            if (m_eventBus && m_activeSketchId >= 0)
                m_eventBus->publish(SketchEditedEvent{m_activeSketchId});
            m_meshesDirty = true;
            markDirty();
        });
    // If no system file-dialog helper exists, Open/Save/Export would otherwise
    // do nothing at all — surface that instead of failing silently.
    FileDialogs::setUnavailableNotifier([this]() {
        showToast("No file-dialog program found \xE2\x80\x94 install 'zenity' "
                  "(GNOME) or 'kdialog' (KDE) to Open / Save / Export.", 8.0);
    });

    initImGui();
    renderSplashFrame("Loading settings & last project");
    loadAppSettings(); // restore persisted preferences before the theme is applied
    renderSplashFrame("Preparing renderers");
    m_themeManager->apply();
    initRenderers();
    renderSplashFrame("Almost there");
    setupCommands();

    // Session-dependent wiring: panels, event-bus binds, plugin context.
    wireDocumentConsumers();
    materializr::force_link::linkAll();
    PluginRegistry::instance().initAll(*m_pluginContext);
}

void Application::wireDocumentConsumers() {
    // Everything that caches a Document/History/SelectionManager pointer.
    // Re-run in full on every adoptSession — a consumer missing from this
    // list keeps operating on the PREVIOUS tab's project, which is the
    // defining bug class of the tabs design. Guards allow the ctor to call
    // this before every panel exists.
    if (m_sectionView) m_sectionView->setDocument(m_document);
    if (m_measureTool) {
        m_measureTool->setDocument(m_document);
        m_measureTool->setSelectionManager(m_selection);
    }
    if (m_toolbar) {
        m_toolbar->setSelectionManager(m_selection);
        m_toolbar->setHistory(m_history);
    }
    if (m_historyPanel) {
        m_historyPanel->setHistory(m_history);
        m_historyPanel->setDocument(m_document);
        m_historyPanel->setEventBus(m_eventBus.get());
    }
    if (m_itemsPanel) {
        m_itemsPanel->setDocument(m_document);
        m_itemsPanel->setSelectionManager(m_selection);
        m_itemsPanel->setHistory(m_history);
    }
    if (m_statusBar) {
        m_statusBar->setDocument(m_document);
        m_statusBar->setSelectionManager(m_selection);
    }
    if (m_propertiesPanel) {
        m_propertiesPanel->setHistory(m_history);
        m_propertiesPanel->setDocument(m_document);
        m_propertiesPanel->setSelectionManager(m_selection);
        m_propertiesPanel->setEventBus(m_eventBus.get());
    }
    // Core services of THIS session bind to the app-wide event bus, and the
    // per-History callbacks are re-applied (they are instance state, not
    // type state — a fresh session's History has none).
    m_document->setEventBus(m_eventBus.get());
    m_history->setEventBus(m_eventBus.get());
    m_selection->setEventBus(m_eventBus.get());
    m_history->setThreadsLastDeclineCallback([this]{ showThreadsLastToast(); });
    if (m_pluginContext && m_viewport)
        m_pluginContext->_bind(m_document, m_history, m_selection,
                               m_eventBus.get(), &m_viewport->getCamera(),
                               &m_meshesDirty, &m_inSketchMode);
    // Tell everything that caches DOCUMENT-DERIVED state to rebuild. The
    // setters above only reach consumers Application knows by name; plugins
    // own their render caches in file-local statics this function cannot
    // see, and no Plane/Axis/Body event fires on a tab switch (nothing about
    // either document changed — the ACTIVE one did). Publishing here means a
    // future plugin gets the invalidation for free by subscribing.
    if (m_eventBus) m_eventBus->publish(ActiveDocumentChangedEvent{});
}

void Application::stashActiveSessionState() {
    // The mirrors themselves (m_document & co) need no stashing — they
    // already point into the active session; only the working copies do.
    if (m_activeSession >= m_sessions.size()) return;
    ProjectSession& out = *m_sessions[m_activeSession];
    out.projectPath = m_currentProjectPath;
    out.projectName = m_currentProjectName;
    out.savedAtHistoryStep = m_savedAtHistoryStep;
    out.unsavedNonHistoryChanges = m_unsavedNonHistoryChanges;
    if (m_viewport) out.camera = m_viewport->getCamera();
}

void Application::applySessionState(size_t idx) {
    m_tabSelectionSync = true;   // tab bars re-assert the visual selection
    m_activeSession = idx;
    ProjectSession& in = *m_sessions[idx];
    m_document = in.document.get();
    m_history = in.history.get();
    m_selection = in.selection.get();
    m_currentProjectPath = in.projectPath;
    m_currentProjectName = in.projectName;
    m_savedAtHistoryStep = in.savedAtHistoryStep;
    m_unsavedNonHistoryChanges = in.unsavedNonHistoryChanges;
    if (m_viewport) {
        m_viewport->getCamera() = in.camera;
        // The copied camera carries the aspect of wherever it was stashed
        // (or a fresh session's default) — squished/stretched rendering
        // until a real resize without this (Steve's "taller and skinnier
        // mug", 2026-07-28).
        m_viewport->syncCameraAspect();
    }
    wireDocumentConsumers();
}

void Application::adoptSession(size_t idx) {
    if (idx >= m_sessions.size()) return;
    // Snapshot the outgoing project's recovery file while its state is still
    // live in the mirrors — inactive tabs don't tick the debounced writer.
    if (idx != m_activeSession) writeSessionRecoveryNow();
    stashActiveSessionState();
    applySessionState(idx);
}

int Application::nextFreeRecoveryIndex() const {
    // Smallest index no open session uses. Recycling keeps every snapshot
    // inside the startup scan's namespace — the bound that makes recovery
    // files impossible to orphan by de-linking (Steve's concern, 2026-07-28).
    // Past that many simultaneous tabs the last index is shared, best-effort.
    for (int i = 0; i < materializr::kMaxSessionsPerSlot; ++i) {
        bool used = false;
        for (const auto& s : m_sessions)
            if (s->recoveryIndex == i) { used = true; break; }
        if (!used) return i;
    }
    return materializr::kMaxSessionsPerSlot - 1;
}

bool Application::activeSessionIsScratch() const {
    // An untouched empty workspace — the tab a fresh launch or a "+" click
    // leaves you in. Anything else (a named project, geometry, sketches, or
    // history) counts as occupied, so opening a project from the home screen
    // gets its own tab rather than replacing what's there.
    if (!m_currentProjectPath.empty()) return false;
    if (!m_document) return true;
    if (!m_document->getAllBodyIds().empty()) return false;
    if (!m_document->getAllSketchIds().empty()) return false;
    return !m_history || m_history->stepCount() == 0;
}

size_t Application::sessionForProjectRef(const std::string& ref) const {
    if (ref.empty()) return m_sessions.size();
    for (size_t i = 0; i < m_sessions.size(); ++i) {
        const std::string& p = (i == m_activeSession) ? m_currentProjectPath
                                                      : m_sessions[i]->projectPath;
        if (!p.empty() && p == ref) return i;
    }
    return m_sessions.size();
}

bool Application::focusExistingProject(const std::string& ref) {
    const size_t idx = sessionForProjectRef(ref);
    if (idx >= m_sessions.size()) return false;
    if (idx == m_activeSession) {          // already looking at it
        if (m_landingPage) m_landingPage->setVisible(false);
        return true;
    }
    // The caller may have made a blank tab to load into before it knew the
    // project was already open; take it back out rather than leaving a stray
    // "Untitled". Checked BEFORE the switch, while it is still active.
    const bool dropScratch = activeSessionIsScratch();
    const size_t scratchIdx = m_activeSession;
    if (!switchToSession(idx)) return false;   // refused (mid-sketch) — it toasted
    if (dropScratch && m_sessions.size() > 1) closeSession(scratchIdx);
    if (m_landingPage) m_landingPage->setVisible(false);
    showToast("That project is already open \xE2\x80\x94 switched to its tab.");
    return true;
}

size_t Application::createSession() {
    auto s = std::make_unique<ProjectSession>();
    s->recoveryIndex = nextFreeRecoveryIndex();
    m_sessions.push_back(std::move(s));
    return m_sessions.size() - 1;
}

bool Application::switchToSession(size_t idx) {
    if (idx >= m_sessions.size()) return false;
    if (idx == m_activeSession) return true;
    // Mid-gesture state doesn't survive a document swap. A half-drawn sketch
    // is the user's call to resolve — refuse loudly rather than silently
    // committing or dropping it. A thread re-cut owns its body until it
    // lands; blocking on it here would freeze the switch for seconds.
    if (m_inSketchMode) {
        showToast("Finish or cancel the sketch before switching tabs.");
        return false;
    }
    if (!m_threadRecuts.empty()) {
        showToast("Wait for the thread re-cut to finish before switching tabs.");
        return false;
    }
    cancelAllInteractivePreviews();
    // The section cut is view state aimed at the OUTGOING project's geometry;
    // carried across it would carve the wrong model. Off on every switch.
    m_sectionEnabled = false;
    m_sectionDirty = true;

    adoptSession(idx);

    // Shelve the outgoing tab's GPU meshes on EVERY platform (uniform by
    // design — an invisible tab holds no GPU memory) and queue the incoming
    // tab's full rebuild for the next frame. Same discipline as project load.
    if (m_shapeRenderer) m_shapeRenderer->clear();
    if (m_edgeRenderer) m_edgeRenderer->clear();
    if (m_selectionHighlight) m_selectionHighlight->clearCaches();
    if (m_sketchRenderer) m_sketchRenderer->clearCache();
    m_dirtyBodyIds.clear();
    m_meshesDirty = true;
    return true;
}

void Application::closeSession(size_t idx) {
    if (idx >= m_sessions.size()) return;
    // The closing tab's snapshot is no longer unfinished work, and its
    // recovery index returns to the pool by virtue of the session vanishing.
    materializr::clearProjectRecovery(m_sessions[idx]->recoveryIndex);
    const bool wasActive = (idx == m_activeSession);
    m_sessions.erase(m_sessions.begin() + static_cast<long>(idx));
    bool closedLast = false;
    if (m_sessions.empty()) {
        // Always at least one tab: a fresh empty workspace takes its place.
        m_sessions.push_back(std::make_unique<ProjectSession>());
        closedLast = true;
    }
    if (wasActive) {
        // No stash — the outgoing session is gone. Apply a neighbor and run
        // the same GPU-shelving discipline as a normal switch.
        cancelAllInteractivePreviews();
        m_sectionEnabled = false;
        m_sectionDirty = true;
        applySessionState(std::min(idx, m_sessions.size() - 1));
        if (m_shapeRenderer) m_shapeRenderer->clear();
        if (m_edgeRenderer) m_edgeRenderer->clear();
        if (m_selectionHighlight) m_selectionHighlight->clearCaches();
        if (m_sketchRenderer) m_sketchRenderer->clearCache();
        m_dirtyBodyIds.clear();
        m_meshesDirty = true;
        // An intentional close — clean, or dirty-and-discarded through the
        // prompt — with no other project open lands on the HOME PAGE, not in
        // a bare untitled workspace (Steve, 2026-07-28). With other tabs
        // still open, the neighbor takes over instead, browser-style.
        if (closedLast) showLandingPage(/*fromStartup=*/true);
    } else if (m_activeSession > idx) {
        // The vector shifted under the active index; the session object (and
        // the mirrors into it) are untouched.
        --m_activeSession;
    }
}

Application::~Application() {
    PluginRegistry::instance().shutdownAll();
    m_backgroundRenderer.reset();
    m_edgeRenderer.reset();
    m_sketchRenderer.reset();
    m_shapeRenderer.reset();
    m_grid.reset();
    m_viewport.reset();
    shutdownImGui();
}

static const char* s_defaultLayout = R"([Window][WindowOverViewport_11111111]
Pos=0,19
Size=1600,881
Collapsed=0

[Window][Debug##Default]
Pos=60,60
Size=400,400
Collapsed=0

[Window][Viewport]
Pos=177,19
Size=1122,881
Collapsed=0
DockId=0x00000001,0

[Window][Tools]
Pos=0,19
Size=175,881
Collapsed=0
DockId=0x00000003,0

[Window][Interactions]
Pos=1301,19
Size=299,175
Collapsed=0
DockId=0x00000007,0

[Window][Items]
Pos=1301,197
Size=299,339
Collapsed=0
DockId=0x00000008,0

[Window][History]
Pos=1301,538
Size=299,362
Collapsed=0
DockId=0x00000006,1

[Window][Properties]
Pos=1301,538
Size=299,362
Collapsed=0
DockId=0x00000006,0

[Docking][Data]
DockSpace       ID=0x08BD597D Window=0x1BBC0F80 Pos=0,19 Size=1600,881 Split=X
  DockNode      ID=0x00000003 Parent=0x08BD597D SizeRef=175,900 Selected=0x18A5FDB9
  DockNode      ID=0x00000004 Parent=0x08BD597D SizeRef=1423,900 Split=X
    DockNode    ID=0x00000001 Parent=0x00000004 SizeRef=1122,900 CentralNode=1 Selected=0xC450F867
    DockNode    ID=0x00000002 Parent=0x00000004 SizeRef=299,900 Split=Y Selected=0x933ECD57
      DockNode  ID=0x00000005 Parent=0x00000002 SizeRef=148,528 Split=Y Selected=0x933ECD57
        DockNode ID=0x00000007 Parent=0x00000005 SizeRef=148,175 HiddenTabBar=1
        DockNode ID=0x00000008 Parent=0x00000005 SizeRef=148,348 Selected=0x933ECD57
      DockNode  ID=0x00000006 Parent=0x00000002 SizeRef=148,370 Selected=0x8C72BEA8
)";

// Build the default dock layout, scaling the side-panel WIDTHS by the desktop
// UI scale so on HiDPI (issue #26) the Tools column and right panels stay wide
// enough for the enlarged text instead of truncating it. Only the horizontal
// splits grow (the central viewport shrinks to compensate); heights and the
// 1600×881 canvas are unchanged. At scale ≤ 1 this returns s_defaultLayout
// verbatim. Panel growth is capped at 2× so an extreme --ui-scale can't squash
// the viewport. Note: only applied when writing a FRESH layout (no imgui.ini) —
// an existing saved layout keeps its widths.
static std::string defaultLayoutScaled(float scale) {
    if (scale <= 1.01f) return s_defaultLayout;
    const float ps = scale > 2.0f ? 2.0f : scale;   // cap panel growth
    const int toolsW  = static_cast<int>(std::lround(175.0f * ps));
    const int rightW  = static_cast<int>(std::lround(299.0f * ps));
    const int central = 1600 - toolsW - rightW;      // viewport (≥652 at 2×)
    const int restW   = 1600 - toolsW;               // central + right column
    const int rightX  = 1600 - rightW;               // right column start x
    const int vpX     = toolsW + 2;                  // viewport start x
    char buf[2200];
    std::snprintf(buf, sizeof(buf),
"[Window][WindowOverViewport_11111111]\nPos=0,19\nSize=1600,881\nCollapsed=0\n\n"
"[Window][Debug##Default]\nPos=60,60\nSize=400,400\nCollapsed=0\n\n"
"[Window][Viewport]\nPos=%d,19\nSize=%d,881\nCollapsed=0\nDockId=0x00000001,0\n\n"
"[Window][Tools]\nPos=0,19\nSize=%d,881\nCollapsed=0\nDockId=0x00000003,0\n\n"
"[Window][Interactions]\nPos=%d,19\nSize=%d,175\nCollapsed=0\nDockId=0x00000007,0\n\n"
"[Window][Items]\nPos=%d,197\nSize=%d,339\nCollapsed=0\nDockId=0x00000008,0\n\n"
"[Window][History]\nPos=%d,538\nSize=%d,362\nCollapsed=0\nDockId=0x00000006,1\n\n"
"[Window][Properties]\nPos=%d,538\nSize=%d,362\nCollapsed=0\nDockId=0x00000006,0\n\n"
"[Docking][Data]\n"
"DockSpace       ID=0x08BD597D Window=0x1BBC0F80 Pos=0,19 Size=1600,881 Split=X\n"
"  DockNode      ID=0x00000003 Parent=0x08BD597D SizeRef=%d,900 Selected=0x18A5FDB9\n"
"  DockNode      ID=0x00000004 Parent=0x08BD597D SizeRef=%d,900 Split=X\n"
"    DockNode    ID=0x00000001 Parent=0x00000004 SizeRef=%d,900 CentralNode=1 Selected=0xC450F867\n"
"    DockNode    ID=0x00000002 Parent=0x00000004 SizeRef=%d,900 Split=Y Selected=0x933ECD57\n"
"      DockNode  ID=0x00000005 Parent=0x00000002 SizeRef=148,528 Split=Y Selected=0x933ECD57\n"
"        DockNode ID=0x00000007 Parent=0x00000005 SizeRef=148,175 HiddenTabBar=1\n"
"        DockNode ID=0x00000008 Parent=0x00000005 SizeRef=148,348 Selected=0x933ECD57\n"
"      DockNode  ID=0x00000006 Parent=0x00000002 SizeRef=148,370 Selected=0x8C72BEA8\n",
        vpX, central, toolsW, rightX, rightW, rightX, rightW, rightX, rightW,
        rightX, rightW, toolsW, restW, central, rightW);
    return std::string(buf);
}

void Application::resetLayout() {
    // Restore the default panel arrangement live (no restart) — the recovery
    // for a panel dragged off-screen or a docking mess, and it re-applies the
    // DPI-scaled widths so a scale change takes proper effect too. Loading a
    // full ini string into ImGui rebuilds every window's dock assignment; we
    // also flush it to disk so it survives a crash.
    std::string layout = defaultLayoutScaled(m_window ? m_window->uiScale() : 1.0f);
    ImGui::LoadIniSettingsFromMemory(layout.c_str(), layout.size());
    if (const char* p = ImGui::GetIO().IniFilename)
        ImGui::SaveIniSettingsToDisk(p);
}

// Where to read/write imgui.ini. On Linux we keep the relative "imgui.ini" path
// (the AppImage runs from a user-writable cwd, which is the existing behaviour
// the user prefers). On Windows the exe usually launches from Program Files,
// which is read-only without admin — the write would silently fail and ImGui
// would fall back to its tiny "everything stacked at (0,0)" defaults. Anchor
// the file under %APPDATA% there so it's both writable and per-user.
static std::string s_imguiIniPath;
static const char* computeImguiIniPath() {
#ifdef _WIN32
    std::string base;
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata) {
        base = std::string(appdata) + "\\materializr";
    } else if (const char* up = std::getenv("USERPROFILE"); up && *up) {
        base = std::string(up) + "\\materializr";
    } else {
        s_imguiIniPath = "imgui.ini";
        return s_imguiIniPath.c_str();
    }
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    s_imguiIniPath = base + "\\imgui.ini";
#elif defined(MZ_IOS)
    // cwd is the read-only .app bundle (iosInitRuntime chdirs there for the
    // asset lookups) — a relative path would silently never save the layout.
    // Anchor next to the settings ($HOME -> <container>/Library, see
    // ios_platform.mm).
    if (const char* home = std::getenv("HOME"); home && *home) {
        std::string base = std::string(home) + "/.config/materializr";
        std::error_code ec;
        std::filesystem::create_directories(base, ec);
        s_imguiIniPath = base + "/imgui.ini";
    } else {
        s_imguiIniPath = "imgui.ini";
    }
#else
    s_imguiIniPath = "imgui.ini";
#endif
    return s_imguiIniPath.c_str();
}

// Resolve a TTF shipped in assets/fonts against the layouts we run from:
//   1. <exe>/../share/materializr/fonts/  (AppImage)
//   2. <exe>/../Resources/assets/fonts/   (macOS .app bundle)
//   3. <exe>/../assets/fonts/             (dev: binary in build/)
//   4. <exe>/assets/fonts/                (Windows portable zip: assets next to exe)
//   5. <cwd>/assets/fonts/                (dev: launched from repo root)
// Returns "" when the font isn't found anywhere — callers degrade gracefully.
std::string Application::resolveBundledFont(const std::string& fname) const {
    char exePath[4096];
    std::string exeDir;
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(nullptr, exePath, sizeof(exePath) - 1);
    if (n > 0) {
        exePath[n] = '\0';
        std::string p(exePath);
        auto slash = p.find_last_of("\\/");
        if (slash != std::string::npos) exeDir = p.substr(0, slash);
    }
#elif defined(__APPLE__)
    // No /proc on macOS — ask dyld for the executable path. The buffer is sized
    // generously; _NSGetExecutablePath fills it and NUL-terminates on success.
    uint32_t n = sizeof(exePath);
    if (_NSGetExecutablePath(exePath, &n) == 0) {
        std::string p(exePath);
        auto slash = p.find_last_of('/');
        if (slash != std::string::npos) exeDir = p.substr(0, slash);
    }
#else
    ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (n > 0) {
        exePath[n] = '\0';
        std::string p(exePath);
        auto slash = p.find_last_of('/');
        if (slash != std::string::npos) exeDir = p.substr(0, slash);
    }
#endif
    const std::string candidates[] = {
        exeDir + "/../share/materializr/fonts/" + fname,
        exeDir + "/../Resources/assets/fonts/" + fname,
        exeDir + "/../assets/fonts/" + fname,
        exeDir + "/assets/fonts/" + fname,
        "assets/fonts/" + fname,
    };
    for (const auto& path : candidates) {
        if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
            std::fclose(f);
            return path;
        }
    }
    return std::string();
}

void Application::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // Write the default layout when imgui.ini is missing, or migrate a layout
    // saved before the Interactions panel existed (detected by its window name)
    // so the panel actually appears docked above Items rather than not at all.
    const char* iniPath = computeImguiIniPath();
    {
        bool needDefault = true;
        if (std::FILE* f = std::fopen(iniPath, "r")) {
            std::string ini;
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) ini.append(buf, n);
            std::fclose(f);
            if (ini.find("[Window][Interactions]") != std::string::npos) needDefault = false;
        }
        if (needDefault) {
            if (std::FILE* f = std::fopen(iniPath, "w")) {
                // Scale the side-panel widths to the desktop UI scale so HiDPI
                // panels aren't too narrow for the enlarged text (issue #26).
                std::string layout = defaultLayoutScaled(
                    m_window ? m_window->uiScale() : 1.0f);
                std::fputs(layout.c_str(), f);
                std::fclose(f);
            }
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    // Point ImGui at the chosen ini path BEFORE the first NewFrame so it loads
    // the default layout we just wrote. The string is owned by s_imguiIniPath
    // and stays alive for the program's lifetime.
    io.IniFilename = iniPath;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Ctrl+Tab belongs to the PROJECT TABS (see handleShortcuts). ImGui binds
    // the same chord to its window-cycling overlay by default, so both fired
    // on one press: the project switched AND the dock-window ring came up.
    // Claim the key by clearing ImGui's binding rather than leaving two
    // handlers racing — panel focus is a click away, project tabs are not.
    if (ImGuiContext* gc = ImGui::GetCurrentContext()) {
        gc->ConfigNavWindowingKeyNext = ImGuiKey_None;
        gc->ConfigNavWindowingKeyPrev = ImGuiKey_None;
    }
    // loadAppSettings() ran before the ImGui context existed, so its
    // applyAppSettings couldn't reach io yet — push the loaded double-click
    // window now that there's a context.
    io.MouseDoubleClickTime = m_doubleClickTime;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.08f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.14f, 0.18f, 1.0f);

    // HiDPI / touch scaling. 1.0 on desktop (no change); on a tablet this scales
    // all padding/spacing/widget sizes so buttons are finger-sized. The font is
    // loaded at the matching size below so text stays crisp (not just upscaled).
    const float uiScale = m_window ? m_window->uiScale() : 1.0f;
    if (uiScale != 1.0f) style.ScaleAllSizes(uiScale);  // scales padding/spacing/scrollbar/grab
    if (materializr::touchMode()) {
        // Touch has no hover; a tooltip can only appear while a finger is held on
        // a widget. Drop the stationary gate (a fingertip is already stationary)
        // and shorten the delays so a brief press-and-hold reveals it.
        style.HoverStationaryDelay = 0.0f;
        style.HoverDelayShort = 0.15f;
        style.HoverDelayNormal = 0.30f;
        // Fatten resize hit targets for fingers. The dock splitter the panels
        // resize against is only a couple px wide (even ×uiScale), so grabbing
        // it on a touchscreen is a near-miss; widen it and add a general touch
        // hit-padding so window borders / grips are reachable too. (Kept modest
        // — TouchExtraPadding grows ALL reactive boxes, so overgrowing it makes
        // overlapping widgets fight for the touch.)
        style.DockingSeparatorSize = 12.0f;
        style.TouchExtraPadding = ImVec2(8.0f, 8.0f);
        // By default ImGui lets a window be dragged from any empty spot in its
        // body, not just the title bar. On a touchscreen that means a natural
        // drag-to-scroll over a panel's empty space sets g.MovingWindow and the
        // whole window slides around instead of scrolling (very visible in
        // Settings). Restrict moves to the title bar so body drags fall through
        // to the drag-to-scroll latch in Window.cpp.
        io.ConfigWindowsMoveFromTitleBarOnly = true;
    }

    // Swap ImGui's default ProggyClean for JetBrains Mono — slashed zero,
    // distinct 0/8/B/6, designed for engineering UIs. resolveBundledFont() tries
    // the AppImage, macOS .app Resources/, dev-build, and cwd layouts (see its
    // candidate list). Falls through to the bundled default if the TTF isn't
    // present, so a font miss never bricks the UI.
    {
        std::string path = resolveBundledFont("JetBrainsMono-Regular.ttf");
        ImFont* fnt = nullptr;
        if (!path.empty()) {
            // No explicit glyph range: ImGui 1.92 loads glyphs ON DEMAND and
            // marks ImFontConfig::GlyphRanges as legacy. That is why the em
            // dash and ellipsis already used in English render fine even though
            // they sit above the old 0x00FF default -- and why the translated
            // accents need nothing here either.
            fnt = io.Fonts->AddFontFromFileTTF(path.c_str(), 15.0f * uiScale);
            if (fnt) std::fprintf(stderr, "Loaded font: %s\n", path.c_str());
        }
        // If nothing loaded, ImGui will lazily fall back to its baked-in default.

        // Merge the Iconoir glyphs (PUA E000..) into the text font so any
        // string can inline an ICON_IC_* / MZ_ICON_* macro (im-touch shell
        // chrome; later the tool catalogue). Merging only works onto an
        // already-added font, so skip when JetBrains Mono itself is missing —
        // icons then render as '?' rather than bricking the atlas.
        std::string icons = resolveBundledFont(FONT_ICON_FILE_NAME_IC);
        if (fnt && !icons.empty()) {
            static const ImWchar kIconRange[] = { ICON_MIN_IC, ICON_MAX_IC, 0 };
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.GlyphOffset.y = 2.0f * uiScale; // optical baseline alignment
            if (io.Fonts->AddFontFromFileTTF(icons.c_str(), 15.0f * uiScale,
                                             &cfg, kIconRange))
                std::fprintf(stderr, "Loaded font: %s (icon merge)\n", icons.c_str());
        }
    }

    ImGui_ImplSDL2_InitForOpenGL(m_window->handle(), m_window->glContext());
#if defined(MZ_GLES)
    ImGui_ImplOpenGL3_Init("#version 300 es");
#else
    ImGui_ImplOpenGL3_Init("#version 330");
#endif
}

void Application::shutdownImGui() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void Application::initRenderers() {
    if (!m_grid->initialize()) {
        std::fprintf(stderr, "Failed to initialize grid renderer\n");
        return;
    }
    if (!m_shapeRenderer->initialize()) {
        std::fprintf(stderr, "Failed to initialize shape renderer\n");
        return;
    }
    if (!m_sketchRenderer->initialize()) {
        std::fprintf(stderr, "Failed to initialize sketch renderer\n");
        return;
    }

    if (!m_backgroundRenderer->initialize()) {
        std::fprintf(stderr, "Failed to initialize background renderer\n");
    }
    if (!m_selectionHighlight->initialize()) {
        std::fprintf(stderr, "Failed to initialize selection highlight\n");
    }
    if (!m_boxSelect->initialize()) {
        std::fprintf(stderr, "Failed to initialize box select\n");
    }
    if (!m_gizmo->initialize()) {
        std::fprintf(stderr, "Failed to initialize gizmo\n");
    }
    if (!m_edgeRenderer->initialize()) {
        std::fprintf(stderr, "Failed to initialize edge renderer\n");
    }
    if (!m_sectionView->initialize()) {
        std::fprintf(stderr, "Failed to initialize section view\n");
    }
    // Plugin-provided render passes (e.g. ConstructionPlanePlugin's plane
    // renderer). Each pass declares its own initialize() callback — run them
    // on the GL thread now, before the first frame, so the plugin can compile
    // shaders / allocate GL resources.
    for (auto& pass : materializr::PluginRegistry::instance().renderPasses()) {
        if (pass.initialize && !pass.initialize()) {
            std::fprintf(stderr, "Failed to initialize render pass: %s\n",
                         pass.name.c_str());
        }
    }

    // Create a demo box so there's something to see (a 20 mm cube) — but only
    // on a truly empty launch. If loadAppSettings already auto-opened the
    // user's last project, the document is populated and dropping the demo
    // box in on top would surprise them.
    if (m_document->getAllBodyIds().empty()) {
        TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape();
        m_document->addBody(box, "Demo Box");
        m_meshesDirty = true;
    }

    // Frame whatever the document holds — the demo box on a fresh launch, or
    // the auto-opened project's bodies — so nothing is clipped by the default
    // camera distance.
    try {
        Bnd_Box bbox;
        for (int id : m_document->getAllBodyIds()) {
            try { BRepBndLib::Add(m_document->getBody(id), bbox); } catch (...) {}
        }
        if (!bbox.IsVoid()) {
            double x0, y0, z0, x1, y1, z1;
            bbox.Get(x0, y0, z0, x1, y1, z1);
            m_viewport->getCamera().zoomToFit(
                glm::vec3(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(z0)),
                glm::vec3(static_cast<float>(x1), static_cast<float>(y1), static_cast<float>(z1)));
        }
    } catch (...) {}

    m_renderersReady = true;
}

void Application::setupCommands() {
    // Commands are now registered by plugins via PluginRegistry.
}

void Application::showThreadsLastToast() {
    // Fallback only: the normal path REFLOWS the op beneath the Thread step
    // (History::pushOperation) — this fires when that reflow can't land.
    m_toastText = "Couldn't reorder this change beneath the Thread step. "
                  "Delete the Thread step, make the change, then re-thread.";
    m_toastExpiry = ImGui::GetTime() + 5.0;
}

void Application::showToast(const std::string& text, double seconds) {
    m_toastText = text;
    m_toastExpiry = ImGui::GetTime() + seconds;
}

void Application::renderTransientToast() {
    if (m_toastText.empty()) return;
    if (ImGui::GetTime() > m_toastExpiry) { m_toastText.clear(); return; }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    // 80px clears classic's menu bar and modern's tab strip. im-touch floats
    // taller chrome over the viewport and scales it by uiScale, so on a tablet
    // the fixed offset is not guaranteed to clear it — take whichever is lower.
    const float y = std::max(vp->WorkPos.y + 80.0f,
                             materializr::viewportTopChromeBottom() + 12.0f);
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, y),
        ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.35f, 0.18f, 0.10f, 1.0f));
    if (ImGui::Begin("##toast", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoInputs)) {
        ImGui::PushTextWrapPos(420.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.6f, 1.0f), "%s",
                           m_toastText.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

// Renderer-only slot for a controller's ghost preview (Push/Pull's tinted tool
// volume). Negative and far outside any real body id — there is no Document
// body behind it, and nothing else may claim it.
static constexpr int kGhostPreviewId = -7777;

materializr::IopContext Application::iopContext() {
    return materializr::IopContext{
        *m_document, *m_history, *m_selection,
        [this] { m_meshesDirty = true; },
        [this](float f, const char* l) { return renderProgressFrame(f, l); },
        [this](std::function<void()> t) { m_deferredHeavyTask = std::move(t); },
        // im-touch hosts the Confirm/Cancel as corner FABs — the scaffold
        // then skips its in-panel buttons (Enter/Esc still work).
        // NOTE: aggregate init, so this list must stay in DECLARATION order —
        // cornerCommitUi sits before toast/refuseMesh in IopContext.
        imTouchLayout() && !m_inSketchMode,
        [this](const char* m) { showToast(m); },
        [this](const char* op) { return refuseMeshSelection(op); },
        m_snapToGrid, m_sketchGridStep,
        materializr::IopPanelPlace{uiScale(), imTouchLayout(),
                                   m_actionAnchorValid, m_actionAnchorX,
                                   m_actionAnchorY},
        [this](int sid) { ensureSketchSourceFace(sid); },
        [this](const TopoDS_Face& f, const gp_Pln& p) {
            return findBodyUnderRegion(f, p);
        },
        [this](int bid) { markBodyDirty(bid); },
        [this](int bid) {
            return m_shapeRenderer && m_shapeRenderer->findSlotByBody(bid) >= 0;
        },
        [this](const TopoDS_Shape& tool, bool cut) {
            if (!m_shapeRenderer) return;
            int slot = m_shapeRenderer->setBodyMesh(kGhostPreviewId, tool);
            if (slot < 0) return;
            m_shapeRenderer->setSubtractPreview(slot, cut);
            m_shapeRenderer->setColor(slot, glm::vec3(0.55f, 0.75f, 1.0f));
        },
        [this] { if (m_shapeRenderer) m_shapeRenderer->removeBody(kGhostPreviewId); },
        [this](int bid) {
            for (const auto& [sid, bodies] : sketchBodyLinks())
                if (bodies.count(bid)) return sid;
            return -1;
        }};
}

// Seed the placement rotation (shared by the Text and SVG tools) so the
// artwork reads upright in the CURRENT view — some sketch planes have
// their 2D axes pointing away from the camera's right/up, and unrotated
// placements came out sideways or upside-down. Projects the camera's
// right vector into sketch space and snaps to the nearest 90°.
void Application::seedUprightPlacementAngle() {
    if (!m_activeSketch || !m_viewport || !m_sketchTool) return;
    const gp_Ax3& ax = m_activeSketch->getPlane().Position();
    glm::vec3 xd(ax.XDirection().X(), ax.XDirection().Y(),
                 ax.XDirection().Z());
    glm::vec3 yd(ax.YDirection().X(), ax.YDirection().Y(),
                 ax.YDirection().Z());
    const Camera& cam = m_viewport->getCamera();
    glm::vec3 fwd = glm::normalize(cam.getTarget() - cam.getPosition());
    glm::vec3 right = glm::normalize(glm::cross(fwd, cam.getUp()));
    glm::vec2 d(glm::dot(right, xd), glm::dot(right, yd));
    if (glm::length(d) > 1e-4f) {
        float aDeg = glm::degrees(std::atan2(d.y, d.x));
        m_sketchTool->setTextAngle(
            static_cast<int>(std::round(aDeg / 90.0f)) * 90);
    }
}

void Application::cancelActiveIops() {
    auto ctx = iopContext();
    for (auto* c : m_iops)
        if (c->active()) c->cancel(ctx);
}

bool Application::anyInteractivePreviewActive() const {
    return anyIopActive() || m_extrudeCtl.active() || m_ppCtl.active() ||
           m_patternActive || m_threadActive;
}

void Application::cancelAllInteractivePreviews() {
    cancelActiveIops();
    // Legacy history-replay previews write the document every frame; a
    // controller preview running beside one corrupts both restore paths.
    if (m_extrudeCtl.active()) cancelInteractiveExtrude();
    if (m_ppCtl.active()) cancelPushPull();
    if (m_patternActive) cancelPattern();
    if (m_threadActive) cancelThread();
    // Fillet / chamfer preview — was missing from this list, so switching
    // tools mid-fillet left the previewed body stuck (the new op then
    // snapshotted it as its "pre-state" and Cancel restored the preview,
    // not the original). (Steve: "switching tools, the action that was
    // never committed gets a weird half-cancel I can't undo".)
    if (m_edgeCtl.active()) cancelInteractiveEdgeOp();
}

// im-touch corner-hosted action commit UI — see Application.h. The EdgeOp
// preview isn't in anyInteractivePreviewActive(), so it's listed explicitly
// here (same set cancelAllInteractivePreviews covers).
bool Application::imTouchActionCorner() const {
    return imTouchLayout() && !m_inSketchMode &&
           (anyInteractivePreviewActive() || m_edgeCtl.active());
}

void Application::confirmActiveAction() {
    if (m_extrudeCtl.active())       { commitInteractiveExtrude(); return; }
    if (m_ppCtl.active())  { commitPushPull(); return; }
    if (m_patternActive)   { commitPattern(); return; }
    if (m_threadActive) {
        // Same guard as the thread panel's Apply: refuse absurd turn counts
        // (the compute would run for minutes) instead of bypassing it.
        if (m_threadLength / std::max(0.1f, m_threadPitch) <= 300.0)
            commitThread();
        return;
    }
    if (m_edgeCtl.active())    { commitInteractiveEdgeOp(); return; }
    auto ctx = iopContext();
    for (auto* c : m_iops)
        if (c->active()) { c->commit(ctx); return; }
}

void Application::cancelActiveAction() {
    if (m_extrudeCtl.active())       { cancelInteractiveExtrude(); return; }
    if (m_ppCtl.active())  { cancelPushPull(); return; }
    if (m_patternActive)   { cancelPattern(); return; }
    if (m_threadActive)    { cancelThread(); return; }
    if (m_edgeCtl.active())    { cancelInteractiveEdgeOp(); return; }
    auto ctx = iopContext();
    for (auto* c : m_iops)
        if (c->active()) { c->cancel(ctx); return; }
}

void Application::beginIop(materializr::InteractiveOpController& ctl) {
    cancelAllInteractivePreviews();
    ctl.begin(iopContext());
}

void Application::beginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    // Touch tooltip timeout. A finger lift leaves io.MousePos parked on the last
    // tapped widget, so its tooltip hangs forever (no hover-out on touch). If the
    // pointer hasn't moved for 15 s with no button down, blank MousePos for this
    // frame so nothing is hovered and the tip clears; the next touch restores it.
    if (materializr::touchMode()) {
        ImGuiIO& io = ImGui::GetIO();
        bool buttonDown = io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2];
        bool moved = std::abs(io.MousePos.x - m_tipLastMouseX) > 0.5f ||
                     std::abs(io.MousePos.y - m_tipLastMouseY) > 0.5f;
        if (moved || buttonDown) {
            m_tipLastMouseX = io.MousePos.x;
            m_tipLastMouseY = io.MousePos.y;
            m_tipStationarySince = ImGui::GetTime();
        } else if (ImGui::GetTime() - m_tipStationarySince > 15.0) {
            io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX); // ImGui "no mouse" sentinel
        }
    }
    ImGui::NewFrame();
#if defined(MZ_IOS)
    // Shrink the root work rect by the device safe areas (status bar, rounded
    // corners, home indicator) so the menu bar / dockspace / status bar all
    // clear the system UI. WorkOffset* are re-derived inside NewFrame every
    // frame, so this must re-apply here, before any window is positioned.
    {
        float t = 0, l = 0, b = 0, r = 0;
        materializr::iosSafeAreaInsets(t, l, b, r);
        if (t > 0 || l > 0 || b > 0 || r > 0) {
            // Claim the safe areas from the BUILD inset accumulator, the same
            // way BeginViewportSideBar claims the menu bar's strip: the main
            // menu bar positions itself from GetBuildWorkRect() this frame,
            // and WorkPos/WorkSize (dockspace, status bar) pick it up next
            // frame — ImGui's normal one-frame work-rect latency.
            ImGuiViewportP* vp = static_cast<ImGuiViewportP*>(ImGui::GetMainViewport());
            vp->BuildWorkInsetMin.x += l;
            vp->BuildWorkInsetMin.y += t;
            vp->BuildWorkInsetMax.x += r;  // insets are positive on all four sides
            vp->BuildWorkInsetMax.y += b;
        }
    }
#endif
}

void Application::endFrame() {
    // Long-press feedback ring: a circle that fills as a stationary one-finger
    // press approaches the context-menu threshold, so the gesture is discoverable
    // and the user knows when to lift. Drawn over everything via the foreground
    // list; vanishes the moment the press moves (it became a drag) or lifts.
    if (materializr::touchMode() && m_window) {
        float hx = 0.0f, hy = 0.0f;
        float hp = m_window->holdProgress(hx, hy);
        if (hp > 0.0f) {
            float s = m_window->uiScale();
            float r = 16.0f * s;
            auto* dl = ImGui::GetForegroundDrawList();
            dl->AddCircle(ImVec2(hx, hy), r, IM_COL32(255, 255, 255, 60), 0, 2.0f * s);
            const float a0 = -1.5707963f;                 // start at 12 o'clock
            dl->PathArcTo(ImVec2(hx, hy), r, a0, a0 + hp * 6.2831853f, 48);
            dl->PathStroke(IM_COL32(120, 180, 255, 235), 0, 3.0f * s);
            if (hp >= 1.0f)
                dl->AddCircleFilled(ImVec2(hx, hy), 4.0f * s, IM_COL32(120, 180, 255, 235));
        }
    }
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    // Raise/dismiss the soft keyboard to match the focused text field. The
    // retap pulse: a tap this frame with a field STILL focused afterwards
    // (a defocusing tap would have dropped WantTextInput by now) re-raises a
    // keyboard the OS dismissed behind the latch's back — see updateTextInput.
    if (m_window) {
        ImGuiIO& kio = ImGui::GetIO();
        const bool want = kio.WantTextInput || m_softKeyboardForced;
        m_window->updateTextInput(want,
                                  kio.MouseClicked[0] && kio.WantTextInput);
    }
}

// Fold the stretch since the previous pump into the heavy task's worst gap.
void Application::noteHeavyPumpGap() {
    const auto now = std::chrono::steady_clock::now();
    const int gap = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastHeavyPump).count());
    if (gap > m_heavyWorstGapMs) m_heavyWorstGapMs = gap;
    m_lastHeavyPump = now;
}

void Application::renderSplashFrame(const char* status) {
    // One self-contained frame shown while startup blocks (auto-opening a
    // big project takes ~10 s on slower machines — this used to be a blank
    // window). Polls events so the WM doesn't flag us unresponsive.
    if (!m_window) return;

    auto drawOnce = [&]() {
        m_window->pollEvents();
        int fbw = 0, fbh = 0;
        m_window->framebufferSize(fbw, fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.075f, 0.082f, 0.11f, 1.0f); // matches the app background
        glClear(GL_COLOR_BUFFER_BIT);

        beginFrame();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::Begin("##splash", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings);

        const char* title = "M A T E R I A L I Z R";
        char ver[48];
        std::snprintf(ver, sizeof(ver), "version %s", MATERIALIZR_VERSION);

        ImGui::SetWindowFontScale(2.2f);
        ImVec2 ts = ImGui::CalcTextSize(title);
        ImGui::SetCursorPos(ImVec2((vp->WorkSize.x - ts.x) * 0.5f,
                                   vp->WorkSize.y * 0.40f));
        ImGui::TextColored(materializr::accentText(), "%s", title);
        ImGui::SetWindowFontScale(1.0f);

        ImVec2 vs = ImGui::CalcTextSize(ver);
        ImGui::SetCursorPos(ImVec2((vp->WorkSize.x - vs.x) * 0.5f,
                                   vp->WorkSize.y * 0.40f + ts.y * 2.2f + 8.0f));
        ImGui::TextDisabled("%s", ver);

        // Status line with a marching-dots heartbeat.
        int dots = static_cast<int>(ImGui::GetTime() * 3.0) % 4;
        char line[128];
        std::snprintf(line, sizeof(line), "%s%.*s", status, dots, "...");
        ImVec2 ls = ImGui::CalcTextSize(line);
        ImGui::SetCursorPos(ImVec2((vp->WorkSize.x - ls.x) * 0.5f,
                                   vp->WorkSize.y * 0.40f + ts.y * 2.2f + 40.0f));
        ImGui::Text("%s", line);

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        endFrame();
        m_window->swapBuffers();
    };

    // Render the FIRST splash frame to both buffers in the double-buffered swap
    // chain. The back buffer is undefined until the first swap; if the WM
    // presents the window in that gap you get an intermittent black flash before
    // the text. Priming both removes it. Later calls already have defined
    // content behind them, so a single pass is enough.
    drawOnce();
    if (!m_splashPrimed) {
        drawOnce();
        m_splashPrimed = true;
    }
}

void Application::drawIndeterminateBar() {
    // A segment sweeping left → right (marquee), not a bouncing fill. Used for
    // work with no readable progress (the projection boolean, a thread sweep).
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float fullW = std::max(ImGui::GetContentRegionAvail().x, 260.0f);
    float barH = ImGui::GetFrameHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + fullW, p0.y + barH),
                      ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
    float t = static_cast<float>(ImGui::GetTime());
    float segW = fullW * 0.28f;
    float x = std::fmod(t * fullW * 0.7f, fullW + segW) - segW; // wraps L→R
    float xa = std::max(0.0f, x);
    float xb = std::min(fullW, x + segW);
    if (xb > xa)
        dl->AddRectFilled(ImVec2(p0.x + xa, p0.y), ImVec2(p0.x + xb, p0.y + barH),
                          ImGui::GetColorU32(ImGuiCol_PlotHistogram), 3.0f);
    ImGui::Dummy(ImVec2(fullW, barH));
}

bool Application::renderProgressFrame(float fraction, const char* label) {
    // Called from inside a long op's execute() via the progress reporter. Must
    // run BETWEEN main frames (the op is deferred to m_deferredHeavyTask), so a
    // fresh ImGui frame here is safe. fraction==0 marks a new op → reset the
    // cancel latch so a prior cancel doesn't carry over. (fraction<0 is the
    // indeterminate spinner and must NOT reset it.)
    if (fraction == 0.0f) m_progressCancelled = false;
    if (m_progressCancelled || !m_window) return m_progressCancelled;

    m_window->pollEvents();
    int fbw = 0, fbh = 0;
    m_window->framebufferSize(fbw, fbh);   // SDL backend (upstream used glfw here)
    glViewport(0, 0, fbw, fbh);
    glClearColor(0.075f, 0.082f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    beginFrame();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float boxW = 440.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - boxW) * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.42f));
    ImGui::SetNextWindowSize(ImVec2(boxW, 0));
    ImGui::Begin("##progress", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Working\xE2\x80\xA6"));
    ImGui::Spacing();
    if (label && label[0]) ImGui::TextWrapped("%s", label);
    ImGui::Spacing();
    if (fraction < 0.0f) {
        drawIndeterminateBar();
    } else {
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(fraction * 100.0f + 0.5f));
        ImGui::ProgressBar(fraction, ImVec2(-1, 0), pct);
    }
    ImGui::Spacing();
    if (ImGui::Button(materializr::tr("Cancel"), materializr::uiSz(110, 0)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_progressCancelled = true;
    }
    ImGui::End();
    endFrame();
    m_window->swapBuffers();
    // GL just ran — restore the FPU mode before control returns to the OCCT op.
    resetFpuForOcct();
    return m_progressCancelled;
}

// renderDockspace() and the shared menu-item lists live in
// src/app/layout/LayoutCommon.cpp; the per-layout chrome is under
// src/app/layout/{classic,modern,imtouch}/.

// The sketch id a just-undone/redone step edited, or -1. Covers BOTH step
// kinds that mutate a sketch: SketchTransformOp (stores its id) and
// SketchEditOp (stores the live sketch pointer — matched back to its id).
// Needed so undo/redo OUTSIDE sketch mode re-cascades the driven body; the
// step's own undo only reverts sketch geometry, the cascade did the body.
int Application::sketchIdEditedBy(const Operation* op) const {
    if (!op || !m_document) return -1;
    if (auto* st = dynamic_cast<const materializr::SketchTransformOp*>(op))
        return st->getSketchId();
    if (auto* se = dynamic_cast<const materializr::SketchEditOp*>(op)) {
        auto target = se->getTarget();
        if (target)
            for (int sid : m_document->getAllSketchIds())
                if (m_document->getSketch(sid) == target) return sid;
    }
    return -1;
}

void Application::undoWithCascade() {
    const Operation* undone = m_history->getStep(m_history->currentStep());
    m_history->undo(*m_document);
    // Keep a sketch-driven body in sync after undoing a sketch edit (the
    // SketchEditOp undo only reverts geometry; the cascade did the body).
    // Mirrors the keyboard Ctrl+Z path.
    int cascaded = -1;
    if (m_inSketchMode && m_activeSketch && m_activeSketchId >= 0) {
        cascadeFromSketchEdit(m_activeSketchId);
        cascaded = m_activeSketchId;
    }
    if (int sid = sketchIdEditedBy(undone); sid >= 0 && sid != cascaded)
        cascadeFromSketchEdit(sid);
    m_meshesDirty = true;
}

void Application::redoWithCascade() {
    m_history->redo(*m_document);
    const Operation* redone = m_history->getStep(m_history->currentStep());
    int cascaded = -1;
    if (m_inSketchMode && m_activeSketch && m_activeSketchId >= 0) {
        cascadeFromSketchEdit(m_activeSketchId);
        cascaded = m_activeSketchId;
    }
    if (int sid = sketchIdEditedBy(redone); sid >= 0 && sid != cascaded)
        cascadeFromSketchEdit(sid);
    m_meshesDirty = true;
}

void Application::renderSmallScreenWarning() {
    if (m_smallScreenWarned || m_smallScreenAck) return;
    // Same turn-taking as the sketch-recovery prompt: don't fight the Welcome
    // modal for the popup stack (mutual close-reopen churn); show right after.
    if (m_welcomeScreen && m_welcomeScreen->isVisible()) return;
    ImGuiIO& io = ImGui::GetIO();
    // Effective UI canvas in logical points (HiDPI / touch scale is already baked
    // into DisplaySize). The reference tablet sits around 893x558 and is roomy;
    // phones land well under, especially in height. Tunable constants.
    // NB: `small` is a macro in <rpcndr.h> (#define small char), reachable via
    // <windows.h> — renamed to avoid the MSVC collision (see the `far` note in
    // Unfold.cpp; OCCT 7.9.3 leaks these Windows macros where 8.0 doesn't).
    const bool tiny = io.DisplaySize.x < 640.0f || io.DisplaySize.y < 470.0f;
    if (!tiny) return;

    if (!ImGui::IsPopupOpen("Small screen")) ImGui::OpenPopup("Small screen");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Small screen", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(uiW(440));
        ImGui::TextWrapped("%s", materializr::tr("Materializr is designed for tablets and larger displays. On a small screen the panels and toolbars are cramped and some controls may be hard to reach — a tablet or larger is strongly recommended."));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        static bool dontShow = false;
        ImGui::Checkbox(materializr::tr("Don't show this again"), &dontShow);
        ImGui::Spacing();
        if (ImGui::Button(materializr::tr("OK"), uiSz(140, 0))) {
            m_smallScreenAck = true;                 // gone for this run
            if (dontShow) { m_smallScreenWarned = true; saveAppSettings(); }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Application::loadAppSettings() {
    AppSettings s = SettingsIO::load(SettingsIO::defaultPath());
    applyAppSettings(s);

    // CLI --safe-mode: stomp anything that could a) crash a driver on a hot
    // restart, b) hang the launch by reopening a large project before the
    // user can intervene, or c) reach for the network during recovery.
    // Persist the safe values so the next normal launch is also recovered
    // without further action.
    if (m_safeMode) {
        m_lightAmbient            = 0.40f;
        m_lightHeadlight          = false;
        m_lightFill               = true;
        m_msaaSamples             = 0;   // disable multisample buffers entirely
        m_meshQuality             = 0;   // Low — coarsest tessellation
        m_autosaveEnabled         = false;
        m_autoOpenLastProject     = false;
        m_checkForUpdatesOnLaunch = false;
        std::fprintf(stdout,
                     "[safe-mode] Rendering reset to safe defaults "
                     "(MSAA off, mesh quality Low); autosave, "
                     "auto-open-last-project, and update-check disabled.\n");
        saveAppSettings();
    }

    applyRenderingSettings();
    m_meshesDirty = true; // re-tessellate at the loaded quality

    // Auto-open the previously-open project. Suppressed by --safe-mode (the
    // toggle was forced off above), and only reached otherwise if the project
    // wasn't closed via File → Close Project before quit (closeProject clears
    // the path in settings).
    if (m_autoOpenLastProject && !s.lastProjectPath.empty()) {
        // Defer to the first main-loop iteration so the load runs in the
        // between-frames slot where a loading bar can pump (and the window is
        // already up) — otherwise the synchronous load froze startup with the
        // OS flagging "not responding".
        std::string p = s.lastProjectPath;
        m_deferredHeavyTask = [this, p]() {
#if defined(__ANDROID__)
            if (p.rfind("content:", 0) == 0) {
                // A persisted document URI (quick-save identity). Resolve it
                // to a readable temp through the persisted grant, load that,
                // then restore the URI as the live identity.
                std::string tmp = materializr::mobileOpenUri(p);
                if (tmp.empty()) {
                    showToast("Couldn't reopen the last project - access may "
                              "have been revoked.");
                    return;
                }
                const bool viaFallback = materializr::mobileLastOpenWasFallback();
                loadProjectWithProgress(tmp);
                if (!m_currentProjectPath.empty()) {   // load succeeded
                    // Backup copy (original gone/disowned): leave the project
                    // unlinked so a save picks a destination instead of
                    // truncating a document we never read. See openRecentProject.
                    m_currentProjectPath = viaFallback ? std::string() : p;
                    std::string nm = materializr::mobileLastDocName();
                    if (!nm.empty()) m_currentProjectName = nm;
                    if (viaFallback)
                        showToast("Opened a local backup - the original is "
                                  "gone. Save to keep it.");
                }
                return;
            }
#endif
            loadProjectWithProgress(p);
        };
    }

    // Auto check for updates: hit the GitHub releases API and, if a newer
    // tag is available, pre-populate the update popup so it pops on the
    // first frame. UpdateChecker has a 5-second connect / 10-second total
    // timeout, so the worst case here is a few seconds of startup delay on
    // a broken network. Suppressed by --safe-mode.
    if (m_checkForUpdatesOnLaunch && !m_safeMode) {
        // Run on a worker thread — the synchronous version blocked startup for
        // up to its 10 s network timeout ("not responding"). The main loop
        // polls m_updateCheckFuture each frame and pops the popup when it's in.
        m_updateCheckFuture = std::async(std::launch::async, [pre = m_includePrereleases]() {
            auto t0 = std::chrono::steady_clock::now();
            auto r = UpdateChecker::check("materializr-cad", "materializr", pre);
            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            std::fprintf(stderr, "[update-check] %lld ms (ok=%d)\n",
                         static_cast<long long>(dt), r.ok ? 1 : 0);
            return r;
        });
    }

    // Plugins (the Getting Started tour's layout picker) read and switch the
    // interface layout through this bridge; the switch is live and persists.
    materializr::bindUiLayoutBridge(
        [this]() { return static_cast<int>(m_uiLayout); },
        [this](int idx) {
            m_uiLayout = static_cast<UiLayout>(idx);
            saveAppSettings();
        });
    materializr::bindLanguageBridge(
        [this]() { return m_language; },
        [this](int idx) {
            m_language = idx;
            saveAppSettings();
        });
    // Welcome screen: every launch until the user becomes a Supporter.
    // Suppressed by --safe-mode — no asking for coffee while the user is
    // recovering from a crash — and on the VERY FIRST launch, where the
    // Getting Started tour (layout picker first) owns the moment; the welcome
    // starts recurring from the second launch. Same marker the tutorial
    // plugin writes.
    const bool firstRun = [] {
        char* base = SDL_GetPrefPath("Materializr", "Materializr");
        if (!base) return false;
        std::string p = std::string(base) + "tutorial_seen";
        SDL_free(base);
        std::ifstream f(p);
        return !f.good();
    }();
    if (!m_supporter && !m_safeMode && !firstRun)
        m_welcomeScreen->setVisible(true);

    // Landing page vs. session restore. "Open last project on launch" now means
    // RESTORE MY TABS: with it on, every project that was open when you quit
    // comes back in its own tab and the home screen is skipped entirely (Steve,
    // 2026-07-28 — the setting had become dead weight once the landing page
    // started superseding the old single-project auto-open, and a tab-aware
    // resume is what it should have meant all along). With it off, the landing
    // page is the start screen. Safe mode gets neither: recovery owns the
    // moment, and the toggle was forced off above. Startup modals are popups
    // and render above the page, so the dialog turn-taking above is unaffected.
    if (!m_safeMode) {
        std::vector<std::string> restore;
        if (m_autoOpenLastProject) {
            for (const auto& p : s.sessionPaths) {
                if (p.empty()) continue;
                // Skip a project already queued: one project, one tab. Settings
                // written before focusExistingProject existed can hold the same
                // path twice (re-opening an open project used to make a second
                // tab), and restoring both faithfully reproduced the duplicate
                // on every launch thereafter.
                if (std::find(restore.begin(), restore.end(), p) != restore.end())
                    continue;
                restore.push_back(p);
            }
            // Settings written by a build that predates tabs have no
            // sessionPaths — fall back to the single last project.
            if (restore.empty() && !s.lastProjectPath.empty())
                restore.push_back(s.lastProjectPath);
        }
        if (restore.empty()) {
            m_deferredHeavyTask = nullptr;  // nothing to resume; home screen
            showLandingPage(/*fromStartup=*/true);
        } else {
            // Replaces the single-project auto-open queued above; same deferred
            // slot, so the loads run between frames with the window already up
            // and the loading bar able to pump.
            size_t active = static_cast<size_t>(s.sessionActive);
            m_deferredHeavyTask = [this, restore, active]() {
                restoreSessionTabs(restore, active);
            };
        }
    }
}

// Reopen a previous session's tabs. The FIRST project loads into the startup
// session (there is always exactly one, empty); each later one gets a new tab.
// A project that no longer exists is skipped with a toast rather than aborting
// the whole restore — a moved file should cost you that tab, not the session.
void Application::restoreSessionTabs(const std::vector<std::string>& paths,
                                     size_t activeIndex) {
    if (paths.empty()) return;
    std::vector<size_t> opened;
    int failed = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i > 0) {
            const size_t idx = createSession();
            if (!switchToSession(idx)) { closeSession(idx); ++failed; continue; }
        }
        bool ok = false;
#if defined(MZ_MOBILE)
        // A persisted SAF document URI, not a filesystem path — resolve it
        // through the grant first, then restore the URI as the live identity
        // so quick-save still writes back to the real document. A backup copy
        // (original gone) leaves the tab unlinked, same rule as Open Recent.
        if (paths[i].rfind("content:", 0) == 0) {
            const std::string tmp = materializr::mobileOpenUri(paths[i]);
            const bool viaFallback = materializr::mobileLastOpenWasFallback();
            if (!tmp.empty()) {
                loadProjectWithProgress(tmp);
                ok = !m_currentProjectPath.empty();
                if (ok) {
                    m_currentProjectPath = viaFallback ? std::string() : paths[i];
                    std::string nm = materializr::mobileLastDocName();
                    if (!nm.empty()) m_currentProjectName = nm;
                }
            }
        } else
#endif
        {
            loadProjectWithProgress(paths[i]);
            // loadProject* leaves m_currentProjectPath empty on failure —
            // that's the only signal it reports.
            ok = !m_currentProjectPath.empty();
        }
        if (!ok) {
            ++failed;
            if (i > 0) closeSession(m_activeSession);
            continue;
        }
        opened.push_back(m_activeSession);
    }
    // Restore focus to whichever tab was in front, by position among the tabs
    // that actually came back. Indices stay valid through the loop above: it
    // only ever appends, and the only closes are of the tab it just made.
    if (!opened.empty()) {
        size_t want = activeIndex < opened.size() ? activeIndex : 0;
        switchToSession(opened[want]);
        // The startup session is pre-existing and empty; if its project was
        // the one that went missing, it would otherwise linger as a stray
        // "Untitled" tab standing in for a file that no longer exists. Focus
        // moved first, so this close only shifts the active index.
        if (std::find(opened.begin(), opened.end(), size_t{0}) == opened.end() &&
            m_sessions.size() > 1)
            closeSession(0);
    }
    if (failed > 0)
        showToast(failed == 1 ? "1 project from your last session is missing."
                              : "Some projects from your last session are "
                                "missing.");
    // Everything failed: don't strand the user in an empty tab.
    if (opened.empty()) showLandingPage(/*fromStartup=*/false);
    // The per-project loads above each persisted settings MID-restore, so the
    // file still describes the half-built tab list (including any tab that
    // turned out to be missing). Rewrite it against the finished state.
    saveAppSettings();
}

void Application::applyRenderingSettings() {
    LightingParams lp;
    lp.ambient = m_lightAmbient;
    lp.headlight = m_lightHeadlight;
    lp.fill = m_lightFill;
    m_shapeRenderer->setLighting(lp);
    m_viewport->setSamples(m_msaaSamples);
    if (m_selectionHighlight) m_selectionHighlight->setLineWidth(m_selectionLineWidth);
    if (m_sketchRenderer) m_sketchRenderer->setLineWidth(m_sketchLineWidth);
}

void Application::meshQualityParams(float& deflection, float& angularDeflection) const {
    // Absolute linear deflection (mm) and angular deflection (radians). Lower
    // values produce denser, smoother meshes.
    switch (m_meshQuality) {
        case 0: deflection = 0.50f; angularDeflection = 0.50f; break; // Low
        case 2: deflection = 0.03f; angularDeflection = 0.15f; break; // High
        case 3: deflection = 0.01f; angularDeflection = 0.10f; break; // Ultra
        case 1:
        default: deflection = 0.10f; angularDeflection = 0.30f; break; // Medium
    }
}

AppSettings Application::currentSettings() const {
    AppSettings s;
    s.theme = (m_themeManager->getTheme() == Theme::Light) ? 1 : 0;
    s.touchMode = m_touchMode;
    s.uiLayout = m_uiLayout;
    s.language = m_language;
    s.displayUnit = m_displayUnit;
    s.imTouchTree = m_imTouchTree;
    s.imTouchTimeline = m_imTouchTimeline;
    s.touchRightTab = m_touchRightTab;
    s.touchRightW = m_touchRightW;
    s.touchRailW = m_touchRailW;
    s.orbitButton = m_orbitButton;
    s.panButton = m_panButton;
    s.levelOrbit = m_viewport->getCamera().isLevelOrbit();
    s.mouseSensitivity = m_viewport->getCamera().getMouseSensitivity();
    s.autosaveEnabled = m_autosaveEnabled;
    s.autosaveIntervalSec = static_cast<int>(m_autosaveIntervalSec);
    s.invertCubeDrag = m_invertCubeDrag;
    s.doubleClickTimeSec = m_doubleClickTime;
    s.filletProbeSeconds = m_filletProbeSeconds;
    s.lightAmbient = m_lightAmbient;
    s.lightHeadlight = m_lightHeadlight;
    s.lightFill = m_lightFill;
    s.msaaSamples = m_msaaSamples;
    s.meshQuality = m_meshQuality;
    s.selectionLineWidth = m_selectionLineWidth;
    s.sketchLineWidth = m_sketchLineWidth;
    s.sketchGridOpacity = m_sketchGridOpacity;
    s.sketchGridThickness = m_sketchGridThickness;
    s.smallScreenWarned = m_smallScreenWarned;
    s.leftPanelHidden = m_leftPanelHidden;
    s.rightPanelHidden = m_rightPanelHidden;
    s.showTools = m_showTools;
    s.showInteractions = m_showInteractions;
    s.showHistory = m_showHistory;
    s.showItems = m_showItems;
    s.showProperties = m_showProperties;
    s.touchOrbitSens = m_touchOrbitSens;
    s.touchPanSens = m_touchPanSens;
    s.touchZoomSens = m_touchZoomSens;
    s.showToolbarTooltips = m_showToolbarTooltips;
    s.showFps = m_showFps;
    s.autoOpenLastProject = m_autoOpenLastProject;
    s.recentProjects = m_recentProjects;
    s.lastProjectPath = m_currentProjectPath; // empty after closeProject()
    // Every open tab, in tab order, so "restore my session" can bring them all
    // back. The ACTIVE tab's path lives in m_currentProjectPath (the session's
    // own copy is only refreshed when it's stashed on the way out), so read it
    // from there; unsaved tabs contribute "" and are skipped on restore.
    s.sessionPaths.clear();
    for (size_t i = 0; i < m_sessions.size(); ++i)
        s.sessionPaths.push_back(i == m_activeSession ? m_currentProjectPath
                                                      : m_sessions[i]->projectPath);
    s.sessionActive = static_cast<int>(m_activeSession);
    s.lastFileDir = materializr::FileDialogs::getLastDir();
    s.checkForUpdatesOnLaunch = m_checkForUpdatesOnLaunch;
    s.includePrereleases = m_includePrereleases;
    s.supporter = m_supporter;
    s.snapToGrid = m_snapToGrid;
    // Stored as the DISPLAY NUMBER, not millimetres. The step is chosen from
    // presets labelled 0.1 / 0.5 / 1 / 10, and "1" means one of whatever unit
    // is showing. Saved as millimetres, picking "1" under feet wrote 304.8 —
    // or, from before the presets converted, wrote 1 and reloaded as a 1 mm
    // grid inside a 40 ft view: 12192 lines, which the renderer fades to
    // nothing, so the grid simply vanished. What persists is this BASE; the
    // step the sketch actually draws and snaps to is the base scaled by zoom
    // (Application.h m_effectiveGridStepMm), and only the base round-trips.
    s.sketchGridStep = static_cast<float>(materializr::toDisplay(m_sketchGridStep));
    // Mirror the live sketch-tool inference level back into the saved settings
    // so cycling the toolbar Full→Reduced→Off button persists across launches.
    s.inferenceLevel = m_sketchTool
        ? static_cast<int>(m_sketchTool->getInferenceLevel()) : 0;
    s.showInferenceToolbarToggle = m_showInferenceToolbarToggle;
    s.angleSnapDeg = m_sketchTool ? m_sketchTool->getAngleSnapDeg() : 15;
    s.stlImportAccuracy = m_stlImportAccuracy;
    s.meshShowWireframe = m_meshShowWireframe;
    return s;
}

// Push a settings struct onto the live members. Preferences only — session
// state (lastProjectPath) and one-shot startup actions (auto-open, update
// check) are deliberately not handled here; loadAppSettings/importSettings
// layer those on top as appropriate. Camera buttons land on both the active
// and the Settings-dialog "staged" copies so an import takes effect at once.
void Application::applyAppSettings(const AppSettings& s) {
    m_themeManager->setTheme(s.theme == 1 ? Theme::Light : Theme::Dark);
    // Touch mode drives the UI scale and input/UX model. The scale + fonts are
    // baked at startup (resolved early in the ctor), so a change here fully
    // applies on the next launch; keeping the global in sync means everything
    // reads a consistent value within the run.
    materializr::setTouchMode(s.touchMode);
    m_touchMode = s.touchMode;   // staged value for the Settings dialog
    m_uiLayout = s.uiLayout;     // interface layout — live, no restart needed
    // UI language — also live. -1 means the user has never chosen, which the
    // setup wizard turns into its opening question; until then, English.
    m_language = s.language;
    setLanguage((s.language > 0 && s.language < languageCount())
                    ? static_cast<Lang>(s.language)
                    : Lang::English);
    applyDisplayUnitChange(s.displayUnit);
    m_imTouchTree = s.imTouchTree;
    m_imTouchTimeline = s.imTouchTimeline;
    m_showFps = s.showFps;
    m_touchRightTab = (s.touchRightTab == 1) ? 1 : 0;
    m_touchRightW = s.touchRightW;
    if (m_touchRightW < 200.0f) m_touchRightW = 200.0f;
    if (m_touchRightW > 520.0f) m_touchRightW = 520.0f;
    m_touchRailW = s.touchRailW;
    if (m_touchRailW < 64.0f)  m_touchRailW = 64.0f;
    if (m_touchRailW > 208.0f) m_touchRailW = 208.0f;   // 2-column rail range
    // Camera button bindings are honoured on every platform. Android defaults to
    // trackpad mode (AppSettings sets orbit/pan = Left there) so one-finger touch
    // orbits out of the box, but an attached mouse/trackpad can be rebound via the
    // Settings dialog and the choice persists — touch pan/zoom stays on two-finger
    // gestures regardless, and sketch-mode drawing still overrides orbit.
    m_orbitButton = s.orbitButton;
    m_panButton = s.panButton;
    m_settingsOrbitButton = s.orbitButton;
    m_settingsPanButton = s.panButton;
    m_viewport->getCamera().setLevelOrbit(s.levelOrbit);
    m_viewport->getCamera().setMouseSensitivity(s.mouseSensitivity);
    m_autosaveEnabled = s.autosaveEnabled;
    m_autosaveIntervalSec = static_cast<float>(s.autosaveIntervalSec);
    m_invertCubeDrag = s.invertCubeDrag;
    m_doubleClickTime = s.doubleClickTimeSec;
    if (ImGui::GetCurrentContext())
        ImGui::GetIO().MouseDoubleClickTime = m_doubleClickTime;
    m_filletProbeSeconds = s.filletProbeSeconds;
    // The only bound on an uninterruptible OCCT blend, so it must reach the
    // probe on every apply — not just at construction.
    materializr::fillet::setProbeBudget(m_filletProbeSeconds);
    m_lightAmbient = s.lightAmbient;
    m_lightHeadlight = s.lightHeadlight;
    m_lightFill = s.lightFill;
    m_msaaSamples = s.msaaSamples;
    m_meshQuality = s.meshQuality;
    m_selectionLineWidth = s.selectionLineWidth;
    m_sketchLineWidth = s.sketchLineWidth;
    m_sketchGridOpacity = s.sketchGridOpacity;
    m_sketchGridThickness = s.sketchGridThickness;
    m_smallScreenWarned = s.smallScreenWarned;
    m_leftPanelHidden = s.leftPanelHidden;
    m_rightPanelHidden = s.rightPanelHidden;
    m_showTools = s.showTools;
    m_showInteractions = s.showInteractions;
    m_showHistory = s.showHistory;
    m_showItems = s.showItems;
    m_showProperties = s.showProperties;
    m_touchOrbitSens = s.touchOrbitSens;
    m_touchPanSens = s.touchPanSens;
    m_touchZoomSens = s.touchZoomSens;
    m_showToolbarTooltips = s.showToolbarTooltips;
    m_autoOpenLastProject = s.autoOpenLastProject;
    m_recentProjects = s.recentProjects;
    m_checkForUpdatesOnLaunch = s.checkForUpdatesOnLaunch;
    m_includePrereleases = s.includePrereleases;
    m_supporter = s.supporter;
    m_snapToGrid = s.snapToGrid;
    // Display number -> millimetres. Safe here because the display unit is
    // applied above (applyDisplayUnitChange) before this runs.
    m_sketchGridStep = static_cast<float>(materializr::toMm(s.sketchGridStep));
    m_showInferenceToolbarToggle = s.showInferenceToolbarToggle;
    m_stlImportAccuracy = s.stlImportAccuracy;
    m_meshShowWireframe = s.meshShowWireframe;
    materializr::FileDialogs::setLastDir(s.lastFileDir);
    if (m_sketchTool) {
        using IL = SketchTool::InferenceLevel;
        IL lvl = (s.inferenceLevel == 1) ? IL::Reduced
               : (s.inferenceLevel == 2) ? IL::Off
               : (s.inferenceLevel == 3) ? IL::Max
                                         : IL::Full;
        m_sketchTool->setInferenceLevel(lvl);
        m_sketchTool->setAngleSnapDeg(s.angleSnapDeg);
    }
    // Mirror onto the toolbar so the in-sketch grid controls show the loaded
    // values right away rather than waiting for the first frame's sync.
    if (m_toolbar) {
        m_toolbar->setSnapToGrid(s.snapToGrid);
        m_toolbar->setGridStep(m_sketchGridStep);   // millimetres, not the stored number
        m_toolbar->setShowInferenceToggle(s.showInferenceToolbarToggle);
    }
}

void Application::saveAppSettings() {
    SettingsIO::save(SettingsIO::defaultPath(), currentSettings());
}

void Application::exportSettings() {
    FileDialogs::saveFile("Export Settings", "materializr-settings.json",
        {{"JSON Files", "*.json"}},
        [this](const std::string& path) {
            if (path.empty()) return;
            if (SettingsIO::exportJson(path, currentSettings()))
                std::fprintf(stdout, "Exported settings to %s\n", path.c_str());
            else
                std::fprintf(stderr, "Failed to export settings to %s\n", path.c_str());
        });
}

void Application::importSettings() {
    FileDialogs::openFile("Import Settings",
        {{"JSON Files", "*.json"}},
        [this](const std::string& path) {
            if (path.empty()) return;
            bool ok = false;
            AppSettings s = SettingsIO::importJson(path, &ok);
            if (!ok) {
                std::fprintf(stderr, "Failed to import settings from %s\n", path.c_str());
                return;
            }
            // Apply the imported preferences live, then persist them to the
            // regular settings file so they survive the next launch. Theme is
            // applied explicitly since applyAppSettings only stages it.
            applyAppSettings(s);
            m_themeManager->apply();
            m_orbitButton = m_settingsOrbitButton; // commit staged camera buttons
            m_panButton = m_settingsPanButton;
            applyRenderingSettings();
            m_meshesDirty = true; // re-tessellate at the imported quality
            saveAppSettings();
            std::fprintf(stdout, "Imported settings from %s\n", path.c_str());
        });
}



void Application::handleToolAction(int action) {
    ToolAction a = static_cast<ToolAction>(action);
    switch (a) {
        case ToolAction::StartSketch: enterSketchMode(); break;
        // Each plane is built so that alignCameraToActiveSketch (camera at +normal,
        // up = plane YDirection) reproduces the matching ViewCube view exactly, in
        // this Y-up world: XY = Top, XZ = Front, YZ = Right. gp_Ax3(origin, normal,
        // xDir); YDirection (the camera up) = normal × xDir.
        // For the explicit base-plane buttons we prime the camera with the
        // canonical Top / Front / Right "up" first — without it, alignCamera
        // ToActiveSketch's continuity-preservation logic snaps the up vector
        // to whichever in-plane axis happened to project from the previous
        // view (e.g. world +X), making XY / XZ visually indistinguishable
        // with an empty viewport. Setting the canonical up here makes each
        // plane land on its CAD-traditional orientation.
        case ToolAction::StartSketchXY: // Top: camera +Y, screen-up = world +Z (user +Y)
            if (m_viewport) m_viewport->getCamera().setUp({0.0f, 0.0f, 1.0f});
            enterSketchOnPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0), gp_Dir(1, 0, 0)))); break;
        case ToolAction::StartSketchXZ: // Front: camera +Z, screen-up = world +Y (user +Z)
            if (m_viewport) m_viewport->getCamera().setUp({0.0f, 1.0f, 0.0f});
            enterSketchOnPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)))); break;
        case ToolAction::StartSketchYZ: // Right: camera +X, screen-up = world +Y (user +Z)
            if (m_viewport) m_viewport->getCamera().setUp({0.0f, 1.0f, 0.0f});
            enterSketchOnPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0), gp_Dir(0, 0, -1)))); break;
        case ToolAction::SketchOnFace: {
            const auto& sel = m_selection->getSelection();
            for (const auto& entry : sel) {
                if (entry.type == SelectionType::Face && !entry.shape.IsNull()) {
                    enterSketchOnFace(TopoDS::Face(entry.shape), entry.bodyId);
                    break;
                }
                // Sketch on a construction plane: same enter-sketch path the
                // XY/XZ/YZ start-sketch toolbar actions use, just with the
                // plane's stored gp_Pln rather than a canonical one.
                if (entry.type == SelectionType::Plane && entry.planeId >= 0) {
                    const auto* p = m_document->getPlane(entry.planeId);
                    if (p) enterSketchOnPlane(p->plane);
                    break;
                }
            }
            break;
        }
        case ToolAction::ExitSketchDiscard: {
            // Rewind history to where the user entered this sketch, then
            // leave sketch mode. Drops every line / circle / arc / etc. the
            // user drew, plus any in-progress placement state.
            if (m_inSketchMode && m_history && m_sketchTool) {
                m_sketchTool->onCancel(); // clear m_isPlacing etc.
                while (m_history->currentStep() > m_sketchEntryHistoryStep &&
                       m_history->canUndo()) {
                    m_history->undo(*m_document);
                }
                m_sketchEntryHistoryStep = -1;
                m_meshesDirty = true;
                // After undo'ing everything we did since entry, also remove
                // the sketch from the document if it ended up empty.
                if (m_activeSketch && m_activeSketch->elementCount() == 0 &&
                    m_activeSketchId >= 0) {
                    m_document->removeSketch(m_activeSketchId);
                }
            }
            // Use the same exit path the close-without-saving flow uses —
            // leaves the camera where it is and clears sketch state.
            if (m_inSketchMode) exitSketchMode();
            break;
        }
        case ToolAction::FinishSketch: {
            if (m_inSketchMode) {
                recordSketchMutation([&]{ m_sketchTool->onConfirm(); });
                exitSketchMode();
            }
            break;
        }
        case ToolAction::EditSketch: {
            const auto& sel = m_selection->getSelection();
            // Accept a selected whole sketch OR a selected sketch region (edit the
            // region's parent sketch) so "Edit Sketch" works from the region tools.
            for (const auto& entry : sel) {
                if ((entry.type == SelectionType::Sketch ||
                     entry.type == SelectionType::SketchRegion) && entry.sketchId >= 0) {
                    editSketch(entry.sketchId);
                    break;
                }
            }
            break;
        }
        case ToolAction::ExtrudeSketch: {
            // "Extrude From" — always creates a new body (Push/Pull is the
            // modify-in-place tool). Priority: selected region(s) > whole
            // sketch > face silhouette. The region branch is what makes a
            // single letter of a text sketch (or the circle inside a
            // rectangle) extrudable on its own — clicking a region selects
            // it, so the explicit pick must win over the whole profile.
            const auto& sel = m_selection->getSelection();
            bool started = false;
            {
                // Collect every selected region of ONE sketch (Ctrl+click
                // several letters → one combined extrude).
                int regionSketch = -1;
                std::vector<int> regionIdxs;
                for (const auto& entry : sel) {
                    if (entry.type != SelectionType::SketchRegion ||
                        entry.sketchId < 0)
                        continue;
                    if (regionSketch < 0) regionSketch = entry.sketchId;
                    if (entry.sketchId == regionSketch)
                        regionIdxs.push_back(entry.subShapeIndex);
                }
                if (regionSketch >= 0 && !regionIdxs.empty()) {
                    auto sketch = m_document->getSketch(regionSketch);
                    if (sketch) {
                        auto regions = sketch->buildRegions();
                        std::vector<TopoDS_Face> profileFaces;
                        for (int idx : regionIdxs) {
                            if (idx < 0 ||
                                idx >= static_cast<int>(regions.size()))
                                continue;
                            if (!regions[idx].face.IsNull())
                                profileFaces.push_back(regions[idx].face);
                        }
                        if (profileFaces.size() == 1) {
                            beginInteractiveExtrude(profileFaces.front(),
                                                    ExtrudeMode::NewBody,
                                                    /*targetBody=*/-1,
                                                    regionSketch);
                            started = true;
                        } else if (profileFaces.size() > 1) {
                            TopoDS_Compound comp;
                            BRep_Builder bb;
                            bb.MakeCompound(comp);
                            for (const auto& f : profileFaces)
                                bb.Add(comp, f);
                            beginInteractiveExtrude(comp,
                                                    ExtrudeMode::NewBody,
                                                    /*targetBody=*/-1,
                                                    regionSketch);
                            started = true;
                        }
                    }
                }
            }
            if (!started) {
                for (const auto& entry : sel) {
                    if (entry.type == SelectionType::Sketch &&
                        entry.sketchId >= 0) {
                        extrudeSketchById(entry.sketchId, ExtrudeMode::NewBody);
                        started = true;
                        break;
                    }
                }
            }
            if (!started) {
                for (const auto& entry : sel) {
                    if (entry.type == SelectionType::Face && !entry.shape.IsNull()) {
                        beginInteractiveExtrude(entry.shape,
                                                ExtrudeMode::NewBody,
                                                /*targetBody=*/-1);
                        break;
                    }
                }
            }
            break;
        }

        case ToolAction::SubtractSketch: {
            // Clicking a sketch in the viewport selects a region, so handle that
            // first; fall back to a whole-sketch selection (from the Items panel).
            const auto& sel = m_selection->getSelection();
            bool started = false;
            for (const auto& entry : sel) {
                if (entry.type == SelectionType::SketchRegion && entry.sketchId >= 0) {
                    subtractSketchRegion(entry.sketchId, entry.subShapeIndex);
                    started = true;
                    break;
                }
            }
            if (!started) {
                for (const auto& entry : sel) {
                    if (entry.type == SelectionType::Sketch && entry.sketchId >= 0) {
                        extrudeSketchById(entry.sketchId, ExtrudeMode::Subtract);
                        break;
                    }
                }
            }
            break;
        }
        case ToolAction::PushPull: {
            beginPushPull();
            break;
        }
        case ToolAction::MoveFace: {
            beginMoveFace();
            break;
        }
        case ToolAction::LookAtSketch: {
            alignCameraToActiveSketch();
            break;
        }
        case ToolAction::SelectSketch:
            if (m_inSketchMode) m_sketchTool->setMode(SketchToolMode::Select);
            break;
        case ToolAction::Line:
            if (m_inSketchMode) toggleSketchMode(m_sketchTool.get(), SketchToolMode::Line);
            break;
        case ToolAction::Circle:
            if (m_inSketchMode) toggleSketchMode(m_sketchTool.get(), SketchToolMode::Circle);
            break;
        case ToolAction::Rectangle:
            if (m_inSketchMode) toggleSketchMode(m_sketchTool.get(), SketchToolMode::Rectangle);
            break;
        case ToolAction::Arc:
            if (m_inSketchMode) toggleSketchMode(m_sketchTool.get(), SketchToolMode::Arc);
            break;
        case ToolAction::Spline:
            if (m_inSketchMode) toggleSketchMode(m_sketchTool.get(), SketchToolMode::Spline);
            break;
        case ToolAction::Polygon:
            if (m_inSketchMode) {
                // Side count comes from the toolbar's Polygon popout.
                m_sketchTool->setPolygonSides(m_toolbar->getRequestedPolygonSides());
                toggleSketchMode(m_sketchTool.get(), SketchToolMode::Polygon);
            }
            break;
        case ToolAction::Trim:
            if (m_inSketchMode) toggleSketchMode(m_sketchTool.get(), SketchToolMode::Trim);
            break;
        case ToolAction::SketchOffset:
            if (m_inSketchMode) toggleSketchMode(m_sketchTool.get(), SketchToolMode::Offset);
            break;
        case ToolAction::SketchDimension:
            if (m_inSketchMode) toggleSketchMode(m_sketchTool.get(), SketchToolMode::Dimension);
            break;
        case ToolAction::SketchText:
            if (m_inSketchMode) {
                // First activation: default to the UI font (always bundled).
                if (m_sketchTool->getTextFontPath().empty()) {
                    m_sketchTool->setTextFontPath(
                        resolveBundledFont("JetBrainsMono-Regular.ttf"));
                }
                seedUprightPlacementAngle();
                m_sketchTool->setMode(SketchToolMode::Text);
            }
            break;

        case ToolAction::SketchAirfoil:
            if (m_inSketchMode) {
                // Filter by CONTENT, not extension: .dat is one of the most
                // generic extensions there is, and airfoil files also ship as
                // .txt or .air. The parser is what decides -- it refuses
                // anything that is not chord-normalised, with a reason.
                materializr::FileDialogs::openFile(
                    "Import Airfoil Section",
                    {{"Airfoil coordinates", "*.dat *.txt *.air *.DAT *.TXT"}},
                    [this](const std::string& path) {
                        if (path.empty() || !m_sketchTool) return;
                        materializr::AirfoilProfile prof;
                        std::string err;
                        if (!materializr::AirfoilImport::load(path, prof, &err)) {
                            showToast(std::string("Not an airfoil file: ") + err);
                            return;
                        }
                        // A published section is typically 60-200 points per
                        // surface; that many spline control points is slow to
                        // solve and to walk for regions, and buys nothing at
                        // model scale. The panel can raise it.
                        materializr::AirfoilImport::simplify(prof, 40);
                        m_airfoilPointBudget = 40;
                        m_airfoilSource = path;
                        m_sketchTool->setAirfoil(std::move(prof));
                        seedUprightPlacementAngle();
                        m_sketchTool->setMode(SketchToolMode::Airfoil);
                    });
            }
            break;

        case ToolAction::SketchSvg:
            if (m_inSketchMode) {
                materializr::FileDialogs::openFile(
                    "Import SVG", {{"SVG Files", "*.svg *.SVG"}},
                    [this](const std::string& path) {
                        if (path.empty() || !m_sketchTool) return;
                        materializr::SvgPaths svg;
                        if (!materializr::SvgImport::load(path, svg)) return;
                        m_sketchTool->setSvgPaths(std::move(svg));
                        seedUprightPlacementAngle();
                        m_sketchTool->setMode(SketchToolMode::Svg);
                    });
            }
            break;

        // --- Sketch constraints (opt-in only; nothing autoConstrains) ---
        case ToolAction::SketchConstrainCoincident:
            applySketchConstraint(ConstraintType::Coincident); break;
        case ToolAction::SketchConstrainHorizontal:
            applySketchConstraint(ConstraintType::Horizontal); break;
        case ToolAction::SketchConstrainVertical:
            applySketchConstraint(ConstraintType::Vertical); break;
        case ToolAction::SketchConstrainParallel:
            applySketchConstraint(ConstraintType::Parallel); break;
        case ToolAction::SketchConstrainPerpendicular:
            applySketchConstraint(ConstraintType::Perpendicular); break;
        case ToolAction::SketchConstrainEqual:
            applySketchConstraint(ConstraintType::Equal); break;
        case ToolAction::SketchConstrainFixed:
            applySketchConstraint(ConstraintType::Fixed); break;
        case ToolAction::SketchDimDistance:
            applySketchConstraint(ConstraintType::Distance); break;
        case ToolAction::SketchDimAngle:
            applySketchConstraint(ConstraintType::Angle); break;
        case ToolAction::SketchDimRadius:
            applySketchConstraint(ConstraintType::Radius); break;
        case ToolAction::SketchConstrainTangent:
            applySketchConstraint(ConstraintType::Tangent); break;
        case ToolAction::SketchConstrainConcentric:
            applySketchConstraint(ConstraintType::Concentric); break;

        // --- Sketch element transforms (operate on the Select-mode selection) ---
        // Rotate is handled by the sketch gizmo's ring handle (see Application_
        // Viewport.cpp), not as a toolbar action.
        case ToolAction::SketchMirror: {
            // Interactive mirror: arm a draggable/rotatable mirror line with a
            // live preview; the dialog's "Mirror" button commits (see
            // Application_Dialogs.cpp + Application_Viewport.cpp).
            if (!m_inSketchMode || !m_activeSketch || !m_sketchTool) break;
            if (!m_sketchTool->beginMirror())
                std::fprintf(stdout, "Mirror: nothing to mirror\n");
            break;
        }
        case ToolAction::SketchCopy: {
            if (!m_inSketchMode || !m_activeSketch || !m_sketchTool) break;

            // Operate on the current sketch-element selection, or — if nothing
            // is selected (the common case when the user hasn't switched into
            // Select mode yet) — on the whole sketch.
            std::set<int> involved;
            std::set<int> selLines;
            if (m_sketchTool->hasElementSelection()) {
                involved.insert(m_sketchTool->getSelectedPoints().begin(),
                                m_sketchTool->getSelectedPoints().end());
                selLines = m_sketchTool->getSelectedLines();
                for (int lid : selLines) {
                    for (const auto& l : m_activeSketch->getLines()) {
                        if (l.id == lid) {
                            involved.insert(l.startPointId);
                            involved.insert(l.endPointId);
                            break;
                        }
                    }
                }
            } else {
                // No selection → entire sketch.
                for (const auto& p : m_activeSketch->getPoints()) involved.insert(p.id);
                for (const auto& l : m_activeSketch->getLines())  selLines.insert(l.id);
            }
            if (involved.empty()) break;

            // Copy: duplicate the points + lines, OFFSET by a couple grid steps
            // so the copy is visibly distinct from the original (landing them
            // exactly on top read as "nothing happened"), then SELECT the copies
            // and switch to Select mode so the move gizmo arms over them.
            float gstep = m_sketchTool->getGridStep();
            float off = (gstep > 0.0f) ? gstep * 2.0f : 5.0f;
            glm::vec2 copyOffset(off, off);
            auto before = std::make_shared<Sketch>(*m_activeSketch);
            std::unordered_map<int, int> remap;
            for (int oldId : involved) {
                auto* p = m_activeSketch->getPoint(oldId);
                if (!p) continue;
                remap[oldId] = m_activeSketch->addPoint(p->pos + copyOffset);
            }
            std::set<int> newLineIds;
            for (int lid : selLines) {
                for (const auto& l : m_activeSketch->getLines()) {
                    if (l.id != lid) continue;
                    auto sIt = remap.find(l.startPointId);
                    auto eIt = remap.find(l.endPointId);
                    if (sIt != remap.end() && eIt != remap.end()) {
                        int newLine = m_activeSketch->addLine(sIt->second, eIt->second);
                        newLineIds.insert(newLine);
                    }
                    break;
                }
            }
            // Record + select the duplicates. setMode FIRST, then select, so the
            // mode switch can't clear the selection — that leaves the copies
            // selected in Select mode, which auto-shows the move gizmo over them
            // so the user can drag them straight off the originals.
            std::set<int> newPointIds;
            for (auto& kv : remap) newPointIds.insert(kv.second);
            m_sketchTool->setMode(SketchToolMode::Select);
            m_sketchTool->setSelection(newPointIds, newLineIds);

            auto after = std::make_shared<Sketch>(*m_activeSketch);
            if (before->getPoints().size() != after->getPoints().size() ||
                before->getLines().size()  != after->getLines().size()) {
                auto op = std::make_unique<SketchEditOp>(m_activeSketch, before, after);
                m_history->pushExecuted(std::move(op));
            }
            break;
        }

        case ToolAction::SketchLinearPattern:
            beginSketchPattern(PatternKind::Linear);
            break;
        case ToolAction::SketchRadialPattern:
            beginSketchPattern(PatternKind::Radial);
            break;
        case ToolAction::SketchCycleInference:
            if (m_sketchTool) {
                using IL = SketchTool::InferenceLevel;
                IL cur = m_sketchTool->getInferenceLevel();
                // Cycle strongest -> weakest, wrapping: Max -> Full -> Reduced -> Off -> Max.
                IL next = cur == IL::Max     ? IL::Full
                        : cur == IL::Full    ? IL::Reduced
                        : cur == IL::Reduced ? IL::Off
                                             : IL::Max;
                m_sketchTool->setInferenceLevel(next);
                // Persist immediately. The settings combo saves on change, but
                // this toolbar button didn't — so a level picked here was lost on
                // restart (Android kills the process on swipe-away, so there's no
                // exit-save to fall back on). inferenceLevel is a saved setting.
                saveAppSettings();
            }
            break;
        case ToolAction::SketchToggleDrawOrigin:
            if (m_sketchTool) {
                using RM = SketchTool::RectMode;
                using CM = SketchTool::CircleMode;
                if (m_sketchTool->getMode() == SketchToolMode::Rectangle)
                    m_sketchTool->setRectMode(
                        m_sketchTool->getRectMode() == RM::Corner ? RM::Center : RM::Corner);
                else if (m_sketchTool->getMode() == SketchToolMode::Circle)
                    m_sketchTool->setCircleMode(
                        m_sketchTool->getCircleMode() == CM::Center ? CM::TwoPoint : CM::Center);
            }
            break;

        case ToolAction::ResetCamera: m_viewport->getCamera().reset(); break;
        case ToolAction::Measure:
            if (m_measureTool) {
                // Activating the tool drops the user at the mode picker. The
                // panel renders three mode buttons (Object / Edge / Point-to-
                // Point) and waits for them to pick one.
                m_measureTool->setMode(MeasureMode::PickMode);
            }
            break;

        case ToolAction::Move: {
            // A selected face turns Move into Move Face — same verb to the user,
            // the selection picks body-vs-face. Trigger whenever a face is in the
            // selection (even if a hole edge / wall is the *primary* pick — those
            // refine hole behavior), so the edge doesn't route us to body-move.
            bool moveFaceSel = false;
            for (const auto& e : m_selection->getSelection())
                if (e.type == SelectionType::Face && !e.shape.IsNull()) { moveFaceSel = true; break; }
            if (moveFaceSel) {
                beginMoveFace();
                break;
            }
            // Edges that form one hole's rim: the selection picks the verb
            // (tilt / reshape / slide). Same button, as with faces.
            if (beginMoveHoleFromEdges()) break;
            // Bodies / standalone sketches / construction planes all get the
            // Move gizmo — the viewport gizmo-visibility block handles whichever
            // selection type is active. SketchRegion picks count as the parent
            // sketch. For sketches the click "arms" the gizmo for the current
            // sketch id; selection-change clears the arm so the next sketch
            // selection again shows just the toolbar options.
            const bool isPlane =
                m_selection->primaryType() == SelectionType::Plane;
            const bool isAxis  =
                m_selection->primaryType() == SelectionType::Axis;
            if (!m_selection->hasSelectedBodies() &&
                !m_selection->hasSelectedSketches() &&
                !m_selection->hasSelectedSketchRegions() &&
                !isPlane && !isAxis) break;
            m_gizmo->setMode(GizmoMode::Translate);
            m_selection->setNavigationOnly(false);
            for (const auto& e : m_selection->getSelection()) {
                if ((e.type == SelectionType::Sketch ||
                     e.type == SelectionType::SketchRegion) && e.sketchId >= 0) {
                    m_sketchGizmoArmed = true;
                    m_sketchGizmoArmedFor = e.sketchId;
                    break;
                }
                if (e.type == SelectionType::Plane && e.planeId >= 0) {
                    m_planeGizmoArmed = true;
                    m_planeGizmoArmedFor = e.planeId;
                    break;
                }
                if (e.type == SelectionType::Axis && e.axisId >= 0) {
                    m_axisGizmoArmed = true;
                    m_axisGizmoArmedFor = e.axisId;
                    break;
                }
            }
            break;
        }
        case ToolAction::Rotate: {
            // A selected face turns Rotate into a face TILT (the loft engine
            // with a rotation about the face centre — same mechanic as Move).
            {
                bool faceSel = false;
                for (const auto& e : m_selection->getSelection())
                    if (e.type == SelectionType::Face && !e.shape.IsNull()) { faceSel = true; break; }
                if (faceSel) { beginMoveFace(FaceXform::Rotate); break; }
            }
            // Axis doesn't get Rotate — an infinite line has no meaningful
            // rotation handle. Rotate is body / sketch / plane only.
            const bool isPlane =
                m_selection->primaryType() == SelectionType::Plane;
            if (!m_selection->hasSelectedBodies() &&
                !m_selection->hasSelectedSketches() &&
                !m_selection->hasSelectedSketchRegions() &&
                !isPlane) break;
            m_gizmo->setMode(GizmoMode::Rotate);
            m_selection->setNavigationOnly(false);
            for (const auto& e : m_selection->getSelection()) {
                if ((e.type == SelectionType::Sketch ||
                     e.type == SelectionType::SketchRegion) && e.sketchId >= 0) {
                    m_sketchGizmoArmed = true;
                    m_sketchGizmoArmedFor = e.sketchId;
                    break;
                }
                if (e.type == SelectionType::Plane && e.planeId >= 0) {
                    m_planeGizmoArmed = true;
                    m_planeGizmoArmedFor = e.planeId;
                    break;
                }
            }
            break;
        }
        case ToolAction::Scale: {
            // A selected face routes to Scale Face — the ONE face-scale tool.
            // This used to run MoveFaceOp::Scale as a separate thing, on the
            // theory that scaling the face and re-sloping the walls toward a
            // scaled copy were different operations. Measured, they are not:
            // a 20mm box top scaled to 50% gives the identical 4666.667
            // frustum either way, because MoveFaceOp::Scale IS ScaleFaceOp
            // with the blend length at the full depth — which is already what
            // ScaleFaceController defaults to. The face rails no longer offer
            // a Scale button at all; classic's shared Transform row still
            // shows one, and this is what keeps it honest.
            //
            // MoveFaceOp::Kind::Scale stays in the op layer: saved projects
            // recorded it and must replay and edit exactly as before.
            {
                bool faceSel = false;
                for (const auto& e : m_selection->getSelection())
                    if (e.type == SelectionType::Face && !e.shape.IsNull()) { faceSel = true; break; }
                if (faceSel) { beginIop(m_scaleFaceCtl); break; }
            }
            // Scale-on-sketch is a no-op (the plane is 2D-infinite), so we
            // keep this body-only.
            if (!m_selection->hasSelectedBodies()) break;
            m_gizmo->setMode(GizmoMode::Scale);
            m_selection->setNavigationOnly(false);
            break;
        }
        case ToolAction::Split: {
            beginIop(m_splitCtl);
            break;
        }

        case ToolAction::Mirror: {
            const auto& sel = m_selection->getSelection();
            if (!sel.empty() && sel[0].bodyId >= 0) {
                m_mirrorBodyId = sel[0].bodyId;
                m_mirrorPickFace = false;
                m_showMirrorPopup = true;
            }
            break;
        }

        case ToolAction::Revolve:
            // (kept in the enum for stability with older bindings; the
            //  Toolbar entry was removed in the RevolvePlugin refactor.
            //  Dispatch is handled by the requestInteractiveOp path now.)
            beginRevolve();
            break;

        case ToolAction::Fillet: {
            if (m_selection->selectedEdgeCount() >= 1)
                beginInteractiveEdgeOp(EdgeOpType::Fillet);
            break;
        }

        case ToolAction::Chamfer: {
            if (m_selection->selectedEdgeCount() >= 1)
                beginInteractiveEdgeOp(EdgeOpType::Chamfer);
            break;
        }

        case ToolAction::EditDiameter: {
            beginIop(m_resizeCylCtl);   // resolves its own pick from the selection
            break;
        }
        case ToolAction::Thread: {
            // Same detector as Edit Diameter, and now the same VALUE — Thread
            // used to read the resize state's members as its input.
            const auto pick = detectCylindricalResizeCandidate();
            if (pick.ok) beginThread(pick);
            break;
        }

        case ToolAction::Shell: {
            beginIop(m_shellCtl);
            break;
        }

        case ToolAction::Taper: {
            beginIop(m_taperCtl);
            break;
        }

        case ToolAction::ScaleFace: {
            beginIop(m_scaleFaceCtl);
            break;
        }

        case ToolAction::ProjectSketch: {
            beginIop(m_projectSketchCtl);
            break;
        }

        case ToolAction::RemoveFace: {
            beginIop(m_defeatureCtl);
            break;
        }

        case ToolAction::MergeFaces: {
            // The repair half of #81, for geometry that was ALREADY split when
            // it arrived (imported STEP) or was edited before the ops started
            // preventing new seams.
            //
            // Picked faces beat picked bodies. A face selection is the user
            // saying "these two are one face", which bounds the merge to those
            // faces and lets the op try a much looser tolerance than is ever
            // safe body-wide — the seams left on a real imported part are
            // near-coplanar, not exactly coplanar. With no faces picked it is
            // the conservative whole-body pass.
            if (refuseMeshSelection("Merge Faces")) break;
            int merged = 0, attempted = 0;
            if (m_selection->selectedFaceCount() >= 2) {
                std::map<int, std::vector<TopoDS_Shape>> byBody;
                for (const auto& e : m_selection->getSelection())
                    if (e.type == SelectionType::Face && !e.shape.IsNull() &&
                        e.bodyId >= 0)
                        byBody[e.bodyId].push_back(e.shape);
                MergeFacesOp::Refusal why = MergeFacesOp::Refusal::None;
                for (auto& [id, faces] : byBody) {
                    if (faces.size() < 2) continue;   // one face alone can't merge
                    ++attempted;
                    auto op = std::make_unique<MergeFacesOp>();
                    op->setBody(id);
                    op->setFaces(faces);
                    MergeFacesOp::resetLastRefusal();
                    if (m_history->pushOperation(std::move(op), *m_document)) ++merged;
                    // pushOperation took the op by value and has destroyed it;
                    // the reason is parked on the class. Read it now.
                    else if (why == MergeFacesOp::Refusal::None)
                        why = MergeFacesOp::lastRefusal();
                }
                if (merged == 0) {
                    // Say which failure it was. The old text named a tolerance
                    // problem for every refusal, including the two that no
                    // tolerance can fix, which sends people hunting for a
                    // setting instead of looking at the faces they picked.
                    const char* msg = nullptr;
                    switch (why) {
                        case MergeFacesOp::Refusal::OppositeNormals:
                            msg = "Those faces point in opposite directions \xE2\x80\x94 they "
                                  "lie in the same plane, but the material is on opposite "
                                  "sides, so they are different surfaces of the part rather "
                                  "than two halves of one. Merging can't join them, and "
                                  "wouldn't add material to either.";
                            break;
                        case MergeFacesOp::Refusal::NotAdjacent:
                            msg = "Those faces don't touch \xE2\x80\x94 merging dissolves the "
                                  "edge between two faces, and there isn't one. If a small "
                                  "step separates them, level it first, then merge.";
                            break;
                        case MergeFacesOp::Refusal::Unsafe:
                            msg = "That merge would have moved material, so it was refused "
                                  "\xE2\x80\x94 the faces are one surface, but joining them "
                                  "would reshape the part rather than tidy it.";
                            break;
                        case MergeFacesOp::Refusal::FacesNotFound:
                            msg = "Couldn't find those faces on the body any more \xE2\x80\x94 "
                                  "they may already have been merged.";
                            break;
                        default: break;   // NotSameSurface and friends: the tolerance text below is right
                    }
                    showToast(attempted == 0
                        ? "Pick two or more faces on the SAME body to merge them."
                        : (msg ? msg
                               : "Couldn't merge those \xE2\x80\x94 they aren't close enough "
                                 "to one surface, or the merge wouldn't hold together."));
                }
            } else {
                const std::vector<int> bodies = materializr::selectedBodyIds(*m_selection);
                if (bodies.empty()) break;
                for (int id : bodies) {
                    auto op = std::make_unique<MergeFacesOp>();
                    op->setBody(id);
                    if (m_history->pushOperation(std::move(op), *m_document)) ++merged;
                }
                // The whole-body pass only takes exactly-coplanar faces. Point
                // the user at the face-picking route rather than implying the
                // part is as merged as it can get.
                if (merged == 0)
                    showToast("Nothing exactly coplanar left to merge \xE2\x80\x94 pick "
                              "the faces either side of a seam and try again.");
            }
            // The picked faces are gone — they were replaced by the face they
            // merged into. Holding on to them would leave the highlight drawing
            // shapes the body no longer has, and hand the next op dead
            // references.
            if (merged > 0) m_selection->clear();
            m_meshesDirty = true;
            break;
        }

        case ToolAction::Unfold: {
            beginUnfoldDialog();
            break;
        }

        case ToolAction::EditFilletChamfer: {
            // Find the FilletOp / ChamferOp in history that owns the picked face,
            // then re-open it for editing with the existing radius / distance.
            TopoDS_Shape pickedFace;
            int pickedBodyId = -1;
            for (const auto& e : m_selection->getSelection()) {
                if (e.type == SelectionType::Face && !e.shape.IsNull()) {
                    pickedFace = e.shape; pickedBodyId = e.bodyId; break;
                }
            }
            if (pickedFace.IsNull()) break;
            // Remember which body's face was clicked — the edit path uses it to
            // detect a baked feature (clicked body doesn't change after edit).
            m_edgeOpPickedBodyId = pickedBodyId;
            // Edit the op that BEST owns the picked face — highest
            // ownsFaceScore (exact IsSame beats the geometric fallback), latest
            // on ties. The old first-match loop opened an earlier fuzzy
            // over-matching fillet instead of the actual chamfer under the
            // cursor (#49); the Toolbar's button uses the same rule.
            const auto& ops = m_history->operations();
            int bestI = -1, bestScore = 0;
            for (int i = 0; i < static_cast<int>(ops.size()); ++i) {
                const auto& op = ops[i];
                if (!op || !op->isEnabled()) continue;
                if (op->kind() != Operation::Kind::Fillet &&
                    op->kind() != Operation::Kind::Chamfer) continue;
                int sc = op->ownsFaceScore(pickedFace);
                if (sc > 0 && sc >= bestScore) { bestScore = sc; bestI = i; }
            }
            if (bestI >= 0) beginInteractiveEdgeOpEdit(bestI);
            break;
        }

        default: break;
    }
}

void Application::handleShortcuts() {
    // A threaded thread-cut compute is in flight (its modal popup is up). Suppress
    // shortcuts until it resolves: undo/redo/delete would mutate the document — and
    // the worker's target body — out from under the in-flight op, whose result is
    // pushed on the main thread when the future lands.
    if (m_threadComputing) return;

    ImGuiIO& io = ImGui::GetIO();

    // Undo/Redo — poll the hardware Ctrl state directly so it works even when
    // ImGui has text input focus. Always false on Android (no modifier keys).
    bool ctrlHeld = Window::isCtrlDown();
    if (ctrlHeld && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (!m_edgeCtl.active() && !m_extrudeCtl.active() && !m_ppCtl.active()) {
            // Mid-placement Ctrl+Z cancels the IN-PROGRESS shape first (the
            // editor convention — and Steve's muscle memory); the next
            // Ctrl+Z then undoes committed elements as usual.
            if (m_inSketchMode && m_sketchTool && m_sketchTool->isPlacing()) {
                m_sketchTool->onCancel();
            } else if (m_history->canUndo() &&
                       // In sketch mode, NEVER undo past the sketch's own edits
                       // into the host body — rolling the body back while the
                       // sketch is live (and rendering against it) crashed.
                       (!m_inSketchMode ||
                        m_history->currentStep() > m_sketchEntryHistoryStep)) {
                const Operation* undone =
                    m_history->getStep(m_history->currentStep());
                m_history->undo(*m_document);
                // A sketch-mutating step (SketchTransformOp / SketchEditOp)
                // updated its body via the cascade; re-cascade so the body
                // follows the reverted sketch. (No-op for detached sketches —
                // the guard in cascade returns early. In sketch mode the
                // active-sketch branch below cascades instead.)
                if (int sid = sketchIdEditedBy(undone);
                    sid >= 0 && !(m_inSketchMode && sid == m_activeSketchId))
                    cascadeFromSketchEdit(sid);
                // In sketch mode, the host face is the anchor for the whole
                // sketch session — clearing the selection would drop its blue
                // highlight even though the sketch is still active. Skip the
                // body-selection reset (sketch-element selection inside the
                // SketchTool is unaffected by m_selection).
                if (!m_inSketchMode) {
                    m_selection->clear();
                    m_hoveredBodyId = -1;
                } else if (m_activeSketch) {
                    // Undoing a line restores the state to just its first-click
                    // anchor (added before the line's history step) — a stray
                    // point. Sweep any such orphan so undo leaves no dangling
                    // vertex.
                    m_activeSketch->pruneOrphanPoints();
                    // A sketch edit's body update was applied through the cascade
                    // (editStep) — the SketchEditOp's own undo only reverts the
                    // sketch geometry, not the body. Re-cascade so the body follows
                    // the now-reverted sketch instead of staying at its last shape.
                    if (m_activeSketchId >= 0) cascadeFromSketchEdit(m_activeSketchId);
                }
                // The undo can remove/renumber the very entities the
                // Dimension tool has picked or is mid-pick on — stale ids
                // referencing geometry that may no longer exist. Drop them
                // rather than let the next click resolve against ghosts.
                if (m_inSketchMode && m_sketchTool &&
                    m_sketchTool->getMode() == SketchToolMode::Dimension) {
                    m_sketchTool->clearDimState();
                }
                m_meshesDirty = true;
            }
        }
    }
    if (ctrlHeld && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        if (!m_edgeCtl.active() && !m_extrudeCtl.active() && !m_ppCtl.active()) {
            if (m_history->canRedo()) {
                m_history->redo(*m_document);
                const Operation* redone =
                    m_history->getStep(m_history->currentStep());
                if (!m_inSketchMode) {
                    m_selection->clear();
                    m_hoveredBodyId = -1;
                } else if (m_activeSketch && m_activeSketchId >= 0) {
                    // Mirror of the undo path: re-sync the body to the
                    // re-applied sketch edit.
                    cascadeFromSketchEdit(m_activeSketchId);
                }
                if (int sid = sketchIdEditedBy(redone);
                    sid >= 0 && !(m_inSketchMode && sid == m_activeSketchId))
                    cascadeFromSketchEdit(sid);
                // Same stale-pick hazard as the undo path above.
                if (m_inSketchMode && m_sketchTool &&
                    m_sketchTool->getMode() == SketchToolMode::Dimension) {
                    m_sketchTool->clearDimState();
                }
                m_meshesDirty = true;
            }
        }
    }
    // Ctrl+A: context-aware select-all. Skipped when ImGui has text-input focus
    // so the standard "select all text" behaviour in input fields still works.
    if (ctrlHeld && ImGui::IsKeyPressed(ImGuiKey_A, false) && !io.WantTextInput) {
        if (m_inSketchMode && m_activeSketch && m_sketchTool) {
            // In a sketch: hand off to the sketch tool's selectAll (also wired
            // to the double-click shortcut).
            m_sketchTool->setMode(SketchToolMode::Select);
            m_sketchTool->selectAll();
        } else if (m_selection && m_selection->hasSelection() &&
                   (m_selection->primaryType() == SelectionType::Edge ||
                    m_selection->primaryType() == SelectionType::Face)) {
            // Extend an edge/face selection to every edge/face on the same
            // body (or bodies) that already contributes to the selection.
            SelectionType targetType = m_selection->primaryType();
            std::set<int> bodyIds;
            for (const auto& entry : m_selection->getSelection()) {
                if (entry.type == targetType && entry.bodyId >= 0) {
                    bodyIds.insert(entry.bodyId);
                }
            }
            for (int bodyId : bodyIds) {
                try {
                    const TopoDS_Shape& shape = m_document->getBody(bodyId);
                    TopAbs_ShapeEnum kind = (targetType == SelectionType::Edge)
                                                ? TopAbs_EDGE : TopAbs_FACE;
                    int idx = 0;
                    for (TopExp_Explorer it(shape, kind); it.More(); it.Next(), ++idx) {
                        SelectionEntry e;
                        e.type = targetType;
                        e.bodyId = bodyId;
                        // subShapeIndex isn't strictly needed for findEntry —
                        // it falls back to IsSame(shape) — but populating it
                        // for faces matches what the picker does.
                        if (targetType == SelectionType::Face) e.subShapeIndex = idx;
                        e.shape = it.Current();
                        m_selection->addToSelection(e);
                    }
                } catch (...) {}
            }
        } else if (m_document) {
            // No useful sub-shape context: select every visible body.
            m_selection->clear();
            for (int bodyId : m_document->getAllBodyIds()) {
                if (!m_document->isBodyVisible(bodyId)) continue;
                SelectionEntry e;
                e.type = SelectionType::Body;
                e.bodyId = bodyId;
                try { e.shape = m_document->getBody(bodyId); } catch (...) {}
                m_selection->addToSelection(e);
            }
        }
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_I)) {
        importStepFile();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_E)) {
        exportStepFile();
    }
    // Ctrl+S = SAVE, matching what the File menu has always advertised next to
    // "Save Project". It used to call saveProject() — the Save-As picker — so
    // the shortcut popped a file dialog for a project that already had a file,
    // while the menu item it was printed beside saved in place (Steve,
    // 2026-07-28). saveProjectQuick falls back to the picker on its own when
    // the project has never been saved.
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        saveProjectQuick();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
        loadProject();
    }
    // Ctrl+Tab cycles the open tabs (Shift reverses). No-op with one tab, and
    // inert while the landing page owns the screen — switching the session
    // behind a full-screen page changes nothing visible but leaves the next
    // click acting on a tab the user can't see (Steve, 2026-07-28).
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Tab) && m_sessions.size() > 1 &&
        !landingPageUp()) {
        const size_t n = m_sessions.size();
        switchToSession(io.KeyShift ? (m_activeSession + n - 1) % n
                                    : (m_activeSession + 1) % n);
    }
    // Plain D — Dimension tool in sketch mode (Onshape-style). Ctrl+D stays
    // Duplicate (handled below); text-input focus swallows the key.
    if (m_inSketchMode && m_sketchTool && !io.KeyCtrl && !io.WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_D, false)) {
        m_sketchTool->setMode(SketchToolMode::Dimension);
    }
    // Ctrl+D — Duplicate in place. Branches on selection type:
    //   Body   → CopyOp (full history support, undoable via Ctrl+Z)
    //   Axis   → Document::addAxis with the source's origin/direction
    //   Plane  → Document::addPlane with the source's gp_Pln
    //   Sketch → deep-clone (points + lines + circles + arcs); constraints
    //            skipped for now (id remapping is non-trivial)
    //   Face   → no-op; duplicating a face has no clean semantic.
    // After the duplicate lands the SELECTION is replaced with the new
    // entity so the user immediately operates on the fresh copy. Skipped
    // when a text field has focus so it doesn't fire while typing.
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) &&
        !ImGui::IsAnyItemActive() && m_selection) {
        const auto& sel = m_selection->getSelection();
        if (!sel.empty()) {
            const auto& first = sel[0];
            if (first.type == SelectionType::Body && first.bodyId >= 0) {
                int srcFolder = m_document->getBodyFolder(first.bodyId);
                auto op = std::make_unique<CopyOp>();
                op->setSourceBodyId(first.bodyId);
                op->setOffset(0.0, 0.0, 0.0);
                CopyOp* opPtr = op.get();
                if (m_history->pushOperation(std::move(op), *m_document)) {
                    int newId = opPtr->getCreatedBodyId();
                    if (newId >= 0) {
                        if (srcFolder >= 0)
                            m_document->setBodyFolder(newId, srcFolder);
                        SelectionEntry e;
                        e.type = SelectionType::Body;
                        e.bodyId = newId;
                        try { e.shape = m_document->getBody(newId); } catch (...) {}
                        m_selection->select(e);
                    }
                    m_meshesDirty = true;
                }
            } else if (first.type == SelectionType::Axis && first.axisId >= 0) {
                if (const auto* a = m_document->getAxis(first.axisId)) {
                    int newId = m_document->addAxis(a->origin, a->direction,
                                                     a->name + " copy");
                    if (newId >= 0) {
                        SelectionEntry e;
                        e.type = SelectionType::Axis;
                        e.axisId = newId;
                        m_selection->select(e);
                    }
                    markDirty();
                }
            } else if (first.type == SelectionType::Plane && first.planeId >= 0) {
                if (const auto* p = m_document->getPlane(first.planeId)) {
                    int newId = m_document->addPlane(p->plane,
                                                      p->name + " copy");
                    if (newId >= 0) {
                        SelectionEntry e;
                        e.type = SelectionType::Plane;
                        e.planeId = newId;
                        m_selection->select(e);
                    }
                    markDirty();
                }
            } else if ((first.type == SelectionType::Sketch ||
                        first.type == SelectionType::SketchRegion) &&
                       first.sketchId >= 0) {
                auto src = m_document->getSketch(first.sketchId);
                if (src) {
                    auto dst = std::make_shared<materializr::Sketch>();
                    dst->setPlane(src->getPlane());
                    dst->setSourceBody(src->getSourceBody());
                    if (!src->getSourceFace().IsNull())
                        dst->setSourceFace(src->getSourceFace());
                    // Re-add points first, build an id remap so derived
                    // elements (lines / circles / arcs) can reference the
                    // new point ids. Constraints carry point/line ids
                    // too — skipped for now; deferred until id remapping
                    // for constraints lands.
                    std::map<int, int> pmap;
                    for (const auto& p : src->getPoints())
                        pmap[p.id] = dst->addPoint(p.pos);
                    for (const auto& l : src->getLines()) {
                        auto a = pmap.find(l.startPointId);
                        auto b = pmap.find(l.endPointId);
                        if (a != pmap.end() && b != pmap.end())
                            dst->addLine(a->second, b->second);
                    }
                    for (const auto& c : src->getCircles()) {
                        auto cp = pmap.find(c.centerPointId);
                        if (cp != pmap.end())
                            dst->addCircle(cp->second, c.radius);
                    }
                    for (const auto& arc : src->getArcs()) {
                        auto cp = pmap.find(arc.centerPointId);
                        auto sp = pmap.find(arc.startPointId);
                        auto ep = pmap.find(arc.endPointId);
                        if (cp != pmap.end() && sp != pmap.end() && ep != pmap.end())
                            dst->addArc(cp->second, sp->second, ep->second, arc.radius);
                    }
                    int newId = m_document->addSketch(dst,
                                  m_document->getSketchName(first.sketchId) + " copy");
                    if (newId >= 0) {
                        SelectionEntry e;
                        e.type = SelectionType::Sketch;
                        e.sketchId = newId;
                        m_selection->select(e);
                    }
                    markDirty();
                    m_meshesDirty = true;
                }
            }
            // Face / Edge / etc.: no-op intentionally — duplicating a
            // face / edge has no clean standalone interpretation.
        }
    }
    // Backspace during spline placement removes the last control point —
    // the natural "oops, one back" while clicking out a curve.
    if (m_inSketchMode && m_sketchTool &&
        m_sketchTool->getMode() == SketchToolMode::Spline &&
        !m_sketchTool->splinePointsInProgress().empty() &&
        !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        recordSketchMutation([&] { m_sketchTool->removeLastSplinePoint(); });
    }
    // Backspace while the Text / SVG / Airfoil tool is active removes the
    // WHOLE last stamp — re-place a misjudged logo or section without leaving
    // the tool. (The panels all advertise Backspace, so every stamp mode has
    // to honour it.)
    if (m_inSketchMode && m_sketchTool &&
        (m_sketchTool->getMode() == SketchToolMode::Text ||
         m_sketchTool->getMode() == SketchToolMode::Svg ||
         m_sketchTool->getMode() == SketchToolMode::Airfoil) &&
        m_sketchTool->hasLastStamp() &&
        !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        recordSketchMutation([&] { m_sketchTool->undoLastStamp(); });
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (m_sketchGizmoHandle != SketchGizmoHandle::None) {
            // Revert each involved point to its drag-start position and exit
            // the gizmo drag (or popup adjust) without pushing a history op.
            if (m_activeSketch) {
                for (auto& [id, orig] : m_sketchGizmoOriginals)
                    m_activeSketch->movePoint(id, orig);
            }
            m_sketchGizmoHandle = SketchGizmoHandle::None;
            m_sketchGizmoBefore.reset();
            m_sketchGizmoOriginals.clear();
            m_sketchGizmoRotateAdjusting = false;
        } else if (m_mirrorPickFace) {
            m_mirrorPickFace = false; // cancel "mirror across a face" mode
        } else if (m_gizmoDragging) {
            // Cancel the drag. The live preview is GPU-only (model matrices on
            // the mesh slots — the document never moved), so reverting the
            // bodies is just resetting the matrices: no doc write, no remesh.
            // Sketch planes / construction planes / axes WERE live-written
            // during the drag, so restore those from their captured
            // before-poses (the old cancel missed them — and missed every
            // body but the primary in a multi-drag).
            gizmoPreviewReset();
            try {
                for (auto& [sid, plnBefore] : m_sketchGizmoDragSketches) {
                    auto sk = m_document->getSketch(sid);
                    if (sk) sk->setPlane(plnBefore);
                }
                for (auto& [pid, plnBefore] : m_planeGizmoDrag)
                    m_document->setPlane(pid, plnBefore);
                for (auto& a : m_axisGizmoDrag)
                    m_document->setAxis(a.id, a.origin, a.direction);
            } catch (...) {}
            m_gizmo->cancelDrag();
            m_gizmoDragging = false;
            m_gizmoDragOriginalShape.Nullify();
            m_gizmoDragBodyId = -1;
            m_gizmoTotalDelta = glm::vec3(0.0f);
            m_gizmoDragOriginals.clear();
            m_sketchGizmoDragSketches.clear();
            m_planeGizmoDrag.clear();
            m_axisGizmoDrag.clear();
        } else if (anyIopActive()) {   // push/pull included — it's in m_iops now
            for (auto* c : m_iops)
                if (c->active()) { c->cancel(iopContext()); break; }
        } else if (false) {
        } else if (false) {   // edge ops ride the generic m_iops chain now
        } else if (m_extrudeCtl.active()) {
            cancelInteractiveExtrude();
        } else if (m_inSketchMode) {
            // Two-step Escape inside sketch mode:
            //   1st press while a shape placement is in progress → cancel
            //      just that placement (Line mid-stroke, Circle awaiting
            //      its radius click, Polygon awaiting its second click,
            //      Spline mid-stream, etc.). The sketch stays active so
            //      the user can resume drawing.
            //   2nd press (or 1st press when nothing is in progress) →
            //      exit sketch mode entirely (same as Finish Sketch but
            //      without an explicit click).
            // Dimension mode is picking entities, not "placing" a shape —
            // isPlacing() never goes true for it, so without this branch
            // Escape here always fell straight through to exitSketchMode()
            // and SketchTool::onCancel's Dimension branch (clear picks /
            // fall back to Select) was unreachable from the keyboard. Route
            // Dimension explicitly first; onCancel() implements both halves
            // itself (mid-pick -> clear picks, stay in Dimension; idle ->
            // Select mode), so a SECOND Escape lands here again with the
            // mode now Select, isPlacing() still false, and exits the sketch
            // as usual.
            //
            // The ##DimEdit value-entry popup (Application_Viewport.cpp)
            // ALSO wants Escape (to dismiss itself and keep the measured
            // value, staying in Dimension/idle-picking, per spec). It runs
            // during renderViewport(), which happens before this shortcut
            // handler each frame, and it clears m_dimEditingId as part of
            // closing — so by the time we get here a naive check can no
            // longer tell the popup was even open. m_dimPopupConsumedEsc is
            // set by that render pass on any Escape seen while the popup was
            // up; consume it here and do NOTHING else, so this press closes
            // ONLY the popup. The mode-level step above happens on the NEXT
            // Escape, once the popup is actually gone.
            if (m_dimPopupConsumedEsc) {
                m_dimPopupConsumedEsc = false;
            } else if (m_sketchTool && m_sketchTool->getMode() == SketchToolMode::Dimension) {
                m_sketchTool->onCancel();
            } else if (m_sketchTool && m_sketchTool->isPlacing()) {
                m_sketchTool->onCancel();
            } else {
                exitSketchMode();
            }
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && m_edgeCtl.active()) {
        m_edgeCtl.confirmFromKey(iopContext());
        m_meshesDirty = true;
    }
    // Extrude has no scaffold panel (which is where the other iops catch
    // Enter), so its Enter-to-confirm lives here — same as Move Face's.
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && m_extrudeCtl.active()) {
        m_extrudeCtl.confirmFromKey(iopContext());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && m_ppCtl.active()) {
        m_ppCtl.confirmFromKey(iopContext());
        m_meshesDirty = true;
    }
    // Move Face has no scaffold panel (which is where the other iops catch
    // Enter), so its Enter-to-confirm lives here.
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && m_moveFaceCtl.active()) {
        commitMoveFace();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
        m_viewport->getCamera().reset();
    }
    // F9 = collapse/restore BOTH docked side columns (max-viewport toggle).
    // Guarded against text fields so it doesn't fire mid-typing.
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F9)) {
        bool hide = !(m_leftPanelHidden && m_rightPanelHidden);
        m_leftPanelHidden = m_rightPanelHidden = hide;
        saveAppSettings();
    }
    // F = Frame: zoom-fit to the current selection, or to visible bodies if
    // nothing's selected. The whole point is the user can hide everything
    // they don't care about, hit F, and have the camera snap onto the
    // remaining part — no more pan-zoom-tilt dance to reach a small off-
    // origin object. Suppressed in sketch mode + while a text field has
    // focus so it doesn't fire while typing constraint values.
    if (!m_inSketchMode && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_F)) {
        frameSelection();
    }
    // Delete: while in sketch mode, restrict to sketch-element deletion so the
    // host body (which stays selected to keep its face highlighted, per the
    // Ctrl+Z fix) doesn't get nuked under the user's nose. Outside sketch mode
    // Delete still removes selected bodies / sketches through history.
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        if (m_inSketchMode && m_activeSketch && m_sketchTool) {
            // Delete the sketch-element selection only — the host body (which
            // stays selected to keep its face highlighted) must not get nuked.
            deleteSelectedSketchElements();
        } else if (m_selection->hasSelection()) {
            const auto& sel = m_selection->getSelection();
            std::vector<int> bodiesToDelete;
            std::vector<int> sketchesToDelete;
            std::vector<int> planesToDelete;
            std::vector<int> axesToDelete;
            for (const auto& entry : sel) {
                // A selected sketch (or sketch region) deletes the whole
                // sketch; a body/face/edge selection deletes its body;
                // planes and axes delete directly (same as the Items
                // panel's right-click — the Delete key used to silently
                // ignore them).
                if (entry.type == SelectionType::Sketch || entry.type == SelectionType::SketchRegion) {
                    if (entry.sketchId >= 0) {
                        bool already = false;
                        for (int s : sketchesToDelete) { if (s == entry.sketchId) { already = true; break; } }
                        if (!already) sketchesToDelete.push_back(entry.sketchId);
                    }
                } else if (entry.type == SelectionType::Plane) {
                    if (entry.planeId >= 0) planesToDelete.push_back(entry.planeId);
                } else if (entry.type == SelectionType::Axis) {
                    if (entry.axisId >= 0) axesToDelete.push_back(entry.axisId);
                } else if (entry.bodyId >= 0) {
                    bool already = false;
                    for (int b : bodiesToDelete) { if (b == entry.bodyId) { already = true; break; } }
                    if (!already) bodiesToDelete.push_back(entry.bodyId);
                }
            }
            for (int pid : planesToDelete) { m_document->removePlane(pid); markDirty(); }
            for (int aid : axesToDelete)   { m_document->removeAxis(aid);  markDirty(); }
            if (!planesToDelete.empty() || !axesToDelete.empty())
                m_selection->clear();
            for (int bodyId : bodiesToDelete) {
                auto op = std::make_unique<DeleteOp>();
                op->setBodyId(bodyId);
                m_history->pushOperation(std::move(op), *m_document);
            }
            for (int sketchId : sketchesToDelete) {
                m_document->removeSketch(sketchId);
                markDirty();
            }
            m_selection->clear();
            m_hoveredBodyId = -1;
            m_meshesDirty = true;
        }
    }
    // Gizmo mode switching. WantTextInput is true while an InputText (rename
    // field, dimension input, etc.) has focus — letting W/E/R fire there both
    // switches gizmo mode AND inserts the character, which is rude.
    if (!m_inSketchMode && !io.KeyCtrl && !io.WantTextInput) {
        bool changed = false;
        if (ImGui::IsKeyPressed(ImGuiKey_W)) {
            m_gizmo->setMode(GizmoMode::Translate); changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_E)) {
            m_gizmo->setMode(GizmoMode::Rotate); changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R)) {
            m_gizmo->setMode(GizmoMode::Scale); changed = true;
        }
        // Explicit ask for a gizmo: drop any navigation-only suppression.
        if (changed) m_selection->setNavigationOnly(false);
    }
}

void Application::rebuildMeshes() {
    float deflection, angularDeflection;
    meshQualityParams(deflection, angularDeflection);

    // Diagnostic: a full rebuild that takes seconds on the MAIN thread is a
    // freeze — say so, with the trigger state.
    const uint32_t rmStart = m_meshesDirty ? SDL_GetTicks() : 0;
    const bool rmWasFull = m_meshesDirty;

    if (m_meshesDirty) {
        // Full rebuild — clear everything and re-tessellate every visible
        // body. Used on project load, mesh-quality change, theme switch.
        m_shapeRenderer->clear();
        m_edgeRenderer->clear();
        auto ids = m_document->getAllBodyIds();
        int meshN = static_cast<int>(ids.size()), meshI = 0;
        for (int id : ids) {
            // During a load (deferred slot), pump a per-body progress frame so
            // tessellating a heavy model keeps the window responsive.
            if (m_pumpMeshProgress) {
                renderProgressFrame(meshN > 0 ? float(meshI) / float(meshN) : -1.0f,
                                    "Preparing view\xE2\x80\xA6");
            }
            ++meshI;
            if (id < 0) continue;        // defensive: skip bad ids
            if (!m_document->isBodyVisible(id)) continue;
            TopoDS_Shape shape;
            try { shape = m_document->getBody(id); } catch (...) { continue; }
            int idx = m_shapeRenderer->setBodyMesh(id, shape, deflection,
                                                   angularDeflection);
            if (idx >= 0) {
                m_shapeRenderer->setColor(idx, m_document->getBodyColor(id));
                if (m_extrudeCtl.active() &&
                    m_extrudeCtl.mode() == ExtrudeMode::Subtract &&
                    id == m_extrudeCtl.previewBodyId()) {
                    m_shapeRenderer->setSubtractPreview(idx, true);
                }
            }
            // Imported meshes have a facet edge per triangle; only draw that
            // wireframe when the user wants it (clean shaded body otherwise).
            if (m_document->isBodyMesh(id) && !m_meshShowWireframe)
                m_edgeRenderer->removeBody(id);
            else
                m_edgeRenderer->setBodyEdges(id, shape, deflection);
        }
        m_dirtyBodyIds.clear();
        if (rmWasFull) {
            const uint32_t took = SDL_GetTicks() - rmStart;
            if (took > 500)
                std::fprintf(stderr, "[Perf] full mesh rebuild took %.2fs "
                                     "on the main thread\n", took / 1000.0);
        }
        return;
    }

    // Partial rebuild — only the bodies in m_dirtyBodyIds need new meshes.
    // The other (potentially 100+) bodies are left untouched, which is the
    // whole point of this path: interactive ops like push/pull stay smooth
    // on a complex project.
    if (m_dirtyBodyIds.empty()) return;

    // Copy out: setBodyMesh may mutate the renderer's internal slots; safer
    // to iterate a snapshot.
    std::vector<int> ids(m_dirtyBodyIds.begin(), m_dirtyBodyIds.end());
    m_dirtyBodyIds.clear();
    for (int id : ids) {
        bool exists = false;
        try { (void)m_document->getBody(id); exists = true; } catch (...) {}
        if (!exists || !m_document->isBodyVisible(id)) {
            m_shapeRenderer->removeBody(id);
            m_edgeRenderer->removeBody(id);
            continue;
        }
        const TopoDS_Shape& shape = m_document->getBody(id);
        int idx = m_shapeRenderer->setBodyMesh(id, shape, deflection,
                                               angularDeflection);
        if (idx >= 0) {
            m_shapeRenderer->setColor(idx, m_document->getBodyColor(id));
            if (m_extrudeCtl.active() &&
                m_extrudeCtl.mode() == ExtrudeMode::Subtract &&
                id == m_extrudeCtl.previewBodyId()) {
                m_shapeRenderer->setSubtractPreview(idx, true);
            }
        }
        // See note above: skip the facet wireframe for imported meshes unless on.
        if (m_document->isBodyMesh(id) && !m_meshShowWireframe)
            m_edgeRenderer->removeBody(id);
        else
            m_edgeRenderer->setBodyEdges(id, shape, deflection);
    }
}

void Application::handleViewCubeAction(int action) {
    ViewCubeAction a = static_cast<ViewCubeAction>(action);
    Camera& cam = m_viewport->getCamera();

    // Incremental rotations: 90° around the camera's current axes (FreeCAD-like).
    constexpr float kRot = static_cast<float>(M_PI * 0.5);
    switch (a) {
        case ViewCubeAction::RotateLeft:  cam.rotateAroundTarget(-kRot, 0.0f); return;
        case ViewCubeAction::RotateRight: cam.rotateAroundTarget( kRot, 0.0f); return;
        case ViewCubeAction::RotateUp:    cam.rotateAroundTarget(0.0f, -kRot); return;
        case ViewCubeAction::RotateDown:  cam.rotateAroundTarget(0.0f,  kRot); return;

        // Roll: rotate the camera's "up" vector around the view direction by
        // 90°. Doesn't change camera position / target, so a snapped ortho
        // view stays snapped — just spins in place.
        case ViewCubeAction::Home:
            // Default 3/4 isometric view (FrontTopRight). Camera offset along
            // (+1,+1,+1) so all three labelled faces (Front, Top, Right) are
            // visible, with world +Y up.
            handleViewCubeAction(static_cast<int>(ViewCubeAction::FrontTopRight));
            return;

        case ViewCubeAction::RollLeft:
        case ViewCubeAction::RollRight: {
            glm::vec3 viewDir = glm::normalize(cam.getTarget() - cam.getPosition());
            float ang = (a == ViewCubeAction::RollLeft) ? +kRot : -kRot;
            float ca = std::cos(ang), sa = std::sin(ang);
            // Rodrigues' rotation of m_up around viewDir.
            glm::vec3 u = cam.getUp();
            glm::vec3 rotated = u * ca + glm::cross(viewDir, u) * sa
                              + viewDir * glm::dot(viewDir, u) * (1.0f - ca);
            cam.setUp(glm::normalize(rotated));
            return;
        }
        default: break;
    }

    // Compute model bbox to centre and size the view.
    Bnd_Box bbox;
    for (int id : m_document->getAllBodyIds()) {
        if (!m_document->isBodyVisible(id)) continue;
        try { BRepBndLib::Add(m_document->getBody(id), bbox); } catch (...) {}
    }
    glm::vec3 cmin(-1.0f), cmax(1.0f);
    if (!bbox.IsVoid()) {
        double x0,y0,z0,x1,y1,z1; bbox.Get(x0,y0,z0,x1,y1,z1);
        cmin = glm::vec3(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(z0));
        cmax = glm::vec3(static_cast<float>(x1), static_cast<float>(y1), static_cast<float>(z1));
    }
    // An in-progress sketch isn't in the Document yet, so the body loop above
    // can't see it. Frame it too — otherwise a ViewCube click during the very
    // first sketch snaps to the tiny default cube instead of the drawing.
    if (m_activeSketch) {
        glm::vec3 smin, smax;
        if (m_activeSketch->getWorldBounds(smin, smax)) {
            if (bbox.IsVoid()) { cmin = smin; cmax = smax; }
            else { cmin = glm::min(cmin, smin); cmax = glm::max(cmax, smax); }
        }
    }
    glm::vec3 center = (cmin + cmax) * 0.5f;
    float radius = glm::length(cmax - cmin) * 0.5f;
    if (radius < 1.0f) radius = 1.0f;

    // Direction the camera looks FROM (model→camera) and its up vector. World is
    // Y-up, so Top/Bottom views need a non-Y up vector to be defined.
    glm::vec3 dir(0.0f), up(0.0f, 1.0f, 0.0f);
    switch (a) {
        case ViewCubeAction::Front:  dir = { 0, 0, 1}; up = {0, 1, 0}; break;
        case ViewCubeAction::Back:   dir = { 0, 0,-1}; up = {0, 1, 0}; break;
        case ViewCubeAction::Right:  dir = { 1, 0, 0}; up = {0, 1, 0}; break;
        case ViewCubeAction::Left:   dir = {-1, 0, 0}; up = {0, 1, 0}; break;
        // Top + Bottom: the "up" direction is computed from the CURRENT
        // camera's horizontal forward so the snap respects turntable
        // orientation — i.e. whatever was "ahead of you" in the orbiting
        // view ends up at the top of the screen when you look straight
        // down. Without this the view always snaps to the same up
        // direction regardless of where you'd yawed to, which feels
        // jarring after a turntable spin.
        case ViewCubeAction::Top:
        case ViewCubeAction::Bottom: {
            dir = (a == ViewCubeAction::Top) ? glm::vec3(0, 1, 0)
                                             : glm::vec3(0,-1, 0);
            glm::vec3 fwd = cam.getTarget() - cam.getPosition();
            glm::vec3 horiz(fwd.x, 0.0f, fwd.z);
            if (glm::length(horiz) < 1e-3f) horiz = glm::vec3(0, 0, -1);
            up = glm::normalize(horiz);
            break;
        }
        case ViewCubeAction::FrontTopRight:    dir = { 1, 1, 1}; break;
        case ViewCubeAction::FrontTopLeft:     dir = {-1, 1, 1}; break;
        case ViewCubeAction::BackTopRight:     dir = { 1, 1,-1}; break;
        case ViewCubeAction::BackTopLeft:      dir = {-1, 1,-1}; break;
        case ViewCubeAction::FrontBottomRight: dir = { 1,-1, 1}; break;
        case ViewCubeAction::FrontBottomLeft:  dir = {-1,-1, 1}; break;
        case ViewCubeAction::BackBottomRight:  dir = { 1,-1,-1}; break;
        case ViewCubeAction::BackBottomLeft:   dir = {-1,-1,-1}; break;
        // Edge (two-face) views: look down the seam of two faces (one zero
        // component) so both are visible. up stays world +Y — never parallel to
        // an edge dir since each has a non-zero horizontal component.
        case ViewCubeAction::TopFront:     dir = { 0, 1, 1}; break;
        case ViewCubeAction::TopBack:      dir = { 0, 1,-1}; break;
        case ViewCubeAction::TopLeft:      dir = {-1, 1, 0}; break;
        case ViewCubeAction::TopRight:     dir = { 1, 1, 0}; break;
        case ViewCubeAction::BottomFront:  dir = { 0,-1, 1}; break;
        case ViewCubeAction::BottomBack:   dir = { 0,-1,-1}; break;
        case ViewCubeAction::BottomLeft:   dir = {-1,-1, 0}; break;
        case ViewCubeAction::BottomRight:  dir = { 1,-1, 0}; break;
        case ViewCubeAction::FrontLeft:    dir = {-1, 0, 1}; break;
        case ViewCubeAction::FrontRight:   dir = { 1, 0, 1}; break;
        case ViewCubeAction::BackLeft:     dir = {-1, 0,-1}; break;
        case ViewCubeAction::BackRight:    dir = { 1, 0,-1}; break;
        default: return;
    }
    dir = glm::normalize(dir);

    cam.setOrthographic(false);
    cam.setTarget(center);
    cam.setPosition(center + dir * (radius * 3.0f)); // approx; zoomToFit refines
    cam.setUp(up);
    cam.zoomToFit(cmin, cmax);
}

std::string Application::projectDisplayName() const {
    if (!m_currentProjectName.empty()) return m_currentProjectName;
    if (m_currentProjectPath.empty()) return "New project";
    std::string pn = m_currentProjectPath;
    auto slash = pn.find_last_of("/\\");
    if (slash != std::string::npos) pn = pn.substr(slash + 1);
    return pn;
}

// A save taken while a sketch is being drawn must not lose that sketch.
//
// m_activeSketch is a live shared_ptr the Document does not know about until
// sketch mode ENDS (the registration in exitSketchMode). Saving straight from
// sketch mode therefore serialised SKETCH_COUNT 0 — the geometry was simply
// absent from the file — and the accompanying SketchEditOp step serialised with
// no params, because SketchEditOp::serializeWithDocument resolves its target via
// Document::findSketchId, which cannot resolve an unregistered pointer. The
// result reloaded as a frozen ReplayOp that reproduces nothing. Drawing a circle
// and pressing Ctrl+S was enough to lose it.
//
// Registering here rather than force-finishing the sketch is deliberate: a save
// should never change what the user is editing, and they stay in sketch mode.
// exitSketchMode stays correct afterwards — its add is guarded on
// m_activeSketchId < 0, so it will not add the same sketch twice, and its
// "existing sketch emptied during this edit" branch still removes it.
void Application::flushActiveSketchToDocument() {
    if (!m_document || !m_activeSketch) return;
    if (m_activeSketchId >= 0) return;                // already in the Document
    if (m_activeSketch->elementCount() == 0) return;  // nothing worth keeping
    m_activeSketchId = m_document->addSketch(m_activeSketch);
    markDirty();
}

// Call ONLY after ProjectIO::save() reports success. The crash-recovery draft
// is a promise "if we crash, restore this" — clearing it here, not inside
// flushActiveSketchToDocument(), means a failed/interrupted disk write still
// leaves the draft in place to recover from. Clearing it right after the
// in-memory Document registration (the original shape of this fix) deleted
// the sole recovery copy before the sketch was durably on disk at all.
void Application::acknowledgeSketchDraftCommitted() {
    if (!m_inSketchMode || !m_activeSketch || m_activeSketchId < 0) return;
    materializr::clearSketchDraft();
    m_lastDraftElemCount = -1;
}

void Application::saveProject() {
    // Seed the picker with the CURRENT project's name (a resave keeps its
    // name instead of silently reverting to "project.materializr").
    std::string suggest = m_currentProjectName;
    if (suggest.empty() && !m_currentProjectPath.empty() &&
        m_currentProjectPath.rfind("content:", 0) != 0) {
        std::filesystem::path p(m_currentProjectPath);
        suggest = p.filename().string();
    }
    // .mzr is the preferred extension; the long-form .materializr stays fully
    // interchangeable (the loader identifies projects by header, not name), so
    // a resave keeps whichever extension the file already has.
    if (suggest.empty()) suggest = "project.mzr";
    else if (suggest.find(".mzr") == std::string::npos &&
             suggest.find(".materializr") == std::string::npos)
        suggest += ".mzr";
    FileDialogs::saveFile("Save Project", suggest,
        {{"Materializr Project", "*.mzr *.materializr"}, {"All Files", "*"}},
        [this](const std::string& chosenPath) {
            if (chosenPath.empty()) return;
            // Only commit the in-progress sketch once the user has actually
            // confirmed a destination — flushing before the picker opened
            // meant merely opening-then-cancelling Save As permanently
            // registered the sketch and flipped the dirty flag with no file
            // ever written.
            flushActiveSketchToDocument();
            std::string path = chosenPath;
#if !defined(MZ_MOBILE)
            // Keep a project extension. The file is gzip-compressed, so
            // without one the OS shows it as a generic "compressed archive"
            // and the open filter can't find it. Either spelling is accepted;
            // a bare name gets the preferred .mzr. (On Android the SAF
            // picker, not this path, names the file.)
            {
                const auto ext = std::filesystem::path(path).extension();
                if (ext != ".mzr" && ext != ".materializr")
                    path += ".mzr";
            }
#endif
            ProjectHistory hist = captureProjectHistory();
            std::vector<uint8_t> thumb;
            captureProjectThumbnailPNG(thumb);
            auto result = ProjectIO::save(path, *m_document, &hist,
                                          thumb.empty() ? nullptr : &thumb);
            if (result.success) {
#if !defined(MZ_MOBILE)
                // On mobile (Android SAF / iOS export sheet) `path` here is
                // only a temp cache file — FileDialogs::poll() commits it to
                // the user's actual chosen document in a SEPARATE step
                // AFTER this callback returns (mobileCommitSave(), whose
                // result isn't even threaded back here). Clearing the
                // recovery draft on this "success" would delete the only
                // recovery copy before the real destination write has even
                // been attempted. Desktop has no such gap: `path` IS the
                // final destination and this write is durable.
                acknowledgeSketchDraftCommitted();
#endif
                m_currentProjectPath = path;
                m_currentProjectName =
                    std::filesystem::path(path).filename().string();
#if defined(__ANDROID__)
                // Adopt the picked DOCUMENT (content:// URI with a persisted
                // write grant) as the project identity, not the throwaway
                // cache temp `path` points at. Quick-save then overwrites the
                // real file in place; before this it silently wrote to the
                // dead temp and the user's document never saw another byte.
                {
                    std::string uri = materializr::mobileLastDocUri();
                    if (!uri.empty()) {
                        m_currentProjectPath = uri;
                        std::string nm = materializr::mobileLastDocName();
                        if (!nm.empty()) m_currentProjectName = nm;
                    }
                }
#endif
                markSaved();
                saveAppSettings(); // persist lastProjectPath for auto-open
                // Save As also lands in Open Recent (persistable ref on Android).
                {
                    std::string ref, name;
#if defined(MZ_MOBILE)
                    ref  = materializr::mobileLastDocUri();
                    name = materializr::mobileLastDocName();
                    if (ref.empty()) ref = path;
#else
                    ref = path;
#endif
                    if (name.empty()) name = std::filesystem::path(path).filename().string();
                    addRecentProject(ref, name);
                    // Keyed by the SAME ref the recents list uses, so the
                    // landing tile finds it (matters on Android, where the
                    // identity is the content: URI, not the temp path).
                    cacheProjectThumbnail(ref, thumb);
                }
                std::fprintf(stdout, "Project saved to %s\n", path.c_str());
                if (m_closeAfterSave) {
                    if (m_postSaveAction == PostSaveAction::CloseProject) {
                        doCloseProject();
                        m_postSaveAction = PostSaveAction::None;
                    } else if (m_postSaveAction == PostSaveAction::OpenProject) {
                        auto act = std::move(m_pendingOpenAction);
                        m_pendingOpenAction = nullptr;
                        m_postSaveAction = PostSaveAction::None;
                        if (act) act();
                    } else {
                        m_confirmedClose = true;
                    }
                }
            } else {
                std::fprintf(stderr, "Save failed: %s\n", result.errorMessage.c_str());
            }
            m_closeAfterSave = false;
        });
}

void Application::saveProjectQuick() {
    flushActiveSketchToDocument();
    // An explicit save expresses "keep what's committed" — captureProjectHistory
    // cancels any live preview first, so a mid-preview save can't persist the
    // preview body and its phantom history step. (Historically that leaked and
    // crashed at least once.)
    if (m_currentProjectPath.empty()) {
        saveProject();
        return;
    }
    // Android: the project identity is a content:// document URI. Write to a
    // private temp, then commit into the document through the persisted write
    // grant — in-place overwrite, no picker, no "name (1)" copies.
    if (m_currentProjectPath.rfind("content:", 0) == 0) {
        const char* home = std::getenv("HOME");
        std::string tmp = std::string(home ? home : ".") + "/.mz_qsave.materializr";
        ProjectHistory hist = captureProjectHistory();
        std::vector<uint8_t> thumb;
        captureProjectThumbnailPNG(thumb);
        auto result = ProjectIO::save(tmp, *m_document, &hist,
                                      thumb.empty() ? nullptr : &thumb);
        if (result.success &&
            materializr::mobileCommitSaveToRef(m_currentProjectPath, tmp)) {
            acknowledgeSketchDraftCommitted();
            markSaved();
            saveAppSettings();
            cacheProjectThumbnail(m_currentProjectPath, thumb);
            showToast("Saved " + projectDisplayName());
        } else if (!result.success) {
            std::fprintf(stderr, "Save failed: %s\n", result.errorMessage.c_str());
            showToast("Save failed - see log");
        } else {
            // The grant was revoked or the file is gone: fall back to Save As
            // so the work still lands somewhere the user picks.
            showToast("Couldn't write the original file - choose where to save");
            saveProject();
        }
        std::remove(tmp.c_str());
        return;
    }
    ProjectHistory hist = captureProjectHistory();
    std::vector<uint8_t> thumb;
    captureProjectThumbnailPNG(thumb);
    auto result = ProjectIO::save(m_currentProjectPath, *m_document, &hist,
                                  thumb.empty() ? nullptr : &thumb);
    if (result.success) {
        acknowledgeSketchDraftCommitted();
        markSaved();
        saveAppSettings(); // persist lastProjectPath for auto-open
        cacheProjectThumbnail(m_currentProjectPath, thumb);
        std::fprintf(stdout, "Project saved to %s\n", m_currentProjectPath.c_str());
        if (m_closeAfterSave) {
            if (m_postSaveAction == PostSaveAction::CloseProject) {
                doCloseProject();
                m_postSaveAction = PostSaveAction::None;
            } else if (m_postSaveAction == PostSaveAction::OpenProject) {
                auto act = std::move(m_pendingOpenAction);
                m_pendingOpenAction = nullptr;
                m_postSaveAction = PostSaveAction::None;
                if (act) act();
            } else {
                m_confirmedClose = true;
            }
        }
    } else {
        std::fprintf(stderr, "Save failed: %s\n", result.errorMessage.c_str());
    }
    m_closeAfterSave = false;
}

ProjectHistory Application::captureProjectHistory(bool cancelPreviews) {
    // A snapshot taken while an async thread re-cut is in flight would bake
    // the UNTHREADED body under a Thread step. Drain first (rare, seconds).
    flushThreadRecuts();
    // A live preview writes the previewed geometry straight into the document
    // body every frame. Since we seed the snapshot from the current body
    // (below), an uncommitted preview would leak into the last committed step's
    // snapshot — the shell-preview-not-cancelled leak that produced a hollow,
    // un-re-shellable body with no shell op in the history. Cancel first so the
    // capture reflects only committed operations. (Recovery opts out to avoid
    // reverting the user's in-progress drag on a background autosave tick.)
    if (cancelPreviews) cancelAllInteractivePreviews();

    ProjectHistory h;
    int n = m_history->currentStep() + 1; // number of applied steps
    if (n <= 0) return h;                  // nothing to persist

    // Current full body set (id -> shape): the state after the last applied step.
    std::map<int, TopoDS_Shape> cur;
    for (int id : m_document->getAllBodyIds()) cur[id] = m_document->getBody(id);

    // Walk the steps backward, reading each op's stored before-shapes. This is
    // non-destructive (unlike undo()) and never recomputes geometry.
    std::vector<ProjectHistoryStep> steps(n);
    // Descriptions go INTO THE FILE (ProjectIO writes them as DESC), and
    // Operation::description() now formats lengths in the display unit. Saved
    // under inches, a step read "Extrude 2.000 in" forever after — on any
    // machine, whatever its unit setting — because a step that reloads as a
    // baked ReplayOp returns the stored string verbatim. Capture in
    // millimetres so what reaches disk is canonical and portable.
    const materializr::ScopedUnit canonicalForFile(materializr::LengthUnit::Mm);
    for (int i = n - 1; i >= 0; --i) {
        const Operation* op = m_history->getStep(i);
        if (!op) continue;
        steps[i].typeId = op->typeId();
        steps[i].name = op->name();
        steps[i].description = op->description();
        steps[i].enabled = op->isEnabled();
        // SketchEditOp's params blob needs the live document to look up the
        // sketch id its m_target belongs to. Other ops use the parameterless
        // serializeParams() — base Operation returns "" so they're a no-op.
        if (auto* sk = dynamic_cast<const materializr::SketchEditOp*>(op)) {
            steps[i].params = sk->serializeWithDocument(*m_document);
        } else {
            steps[i].params = op->serializeParams();
        }
        steps[i].timestampUnix = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::seconds>(
                op->timestamp().time_since_epoch()).count());
        if (!op->isEnabled()) continue; // a disabled step changed nothing

        OperationDiff d = op->captureDiff();
        for (const auto& [id, before] : d.modifiedBefore) {
            auto it = cur.find(id);
            if (it != cur.end()) steps[i].changed.push_back({id, it->second}); // after
            cur[id] = before;                                                  // step back
        }
        for (int id : d.created) {
            auto it = cur.find(id);
            if (it != cur.end()) steps[i].changed.push_back({id, it->second}); // after
            cur.erase(id);                                                     // didn't exist before
        }
        for (const auto& [id, before] : d.deletedBefore) {
            steps[i].deleted.push_back(id); // gone after this step
            cur[id] = before;               // existed before
        }
    }

    // `cur` is now the initial state (before step 0).
    h.present = true;
    for (const auto& [id, shape] : cur) h.initialState.push_back({id, shape});
    h.steps = std::move(steps);
    return h;
}

void Application::rebuildHistoryFromProject(const ProjectHistory& hist,
                                            const std::string& savedByVersion) {
    m_history->clear();
    if (!hist.present) return;

    // Health report: count steps that reload as baked (non-editable) ReplayOps.
    // A body-affecting baked step means geometry the user can see but can't edit
    // (e.g. frozen by an older save) — surfaced after the loop so the parametric
    // state of a project is visible up front instead of discovered mid-edit.
    int bakedBodySteps = 0;     // baked steps that change/delete a body
    int bakedSketchSteps = 0;   // baked sketch-only steps (benign)

    // Accumulate full states forward from the initial snapshot, giving each
    // reloaded step a ReplayOp that knows its complete before/after body set.
    std::map<int, TopoDS_Shape> running;
    for (const auto& [id, shape] : hist.initialState) running[id] = shape;

    auto toVec = [](const std::map<int, TopoDS_Shape>& m) {
        ReplayOp::BodyState v; v.reserve(m.size());
        for (const auto& [id, s] : m) v.push_back({id, s});
        return v;
    };

    // Replaying a step RE-RUNS its geometry: on a slower machine four extrude
    // booleans in one real project cost ~6 s each, 24 s of the 24.4 s load, all
    // inside a single main-loop iteration. Offer the keep-alive a step count
    // so the load shows honest progress instead of a frozen window; it decides
    // how often that is actually worth drawing. Within a step, the ops' own
    // OCCT progress callbacks keep it fed — see core/UiKeepAlive.h.
    const size_t totalSteps = hist.steps.size();
    size_t stepNo = 0;
    for (const auto& st : hist.steps) {
        ++stepNo;
        if (totalSteps > 1) {
            char lbl[96];
            std::snprintf(lbl, sizeof(lbl), "Rebuilding history \xE2\x80\x94 step %d of %d",
                          static_cast<int>(stepNo), static_cast<int>(totalSteps));
            m_heavyProgressLabel = lbl;
            m_heavyProgressFrac =
                static_cast<float>(stepNo - 1) / static_cast<float>(totalSteps);
            // Through the keep-alive, NOT straight to renderProgressFrame: it
            // is what rate-limits the drawing. A frame per step is 52 frames
            // whether or not the machine can afford them.
            materializr::uiKeepAlive();
        }
        ReplayOp::BodyState before = toVec(running);

        // While `running` still holds the pre-step state, derive what this
        // step did to the body set so a rehydrated real op can restore its
        // post-execution bookkeeping (Operation::rehydrateFromReload).
        Operation::ReloadState reload;
        for (const auto& [id, shape] : st.changed) {
            if (running.find(id) == running.end()) {
                reload.created.push_back(id);
                reload.createdAfter.push_back({id, shape});
            } else {
                reload.modifiedBefore.push_back({id, running[id]});
                reload.modifiedAfter.push_back({id, shape});
            }
        }
        for (int id : st.deleted) {
            auto it = running.find(id);
            if (it != running.end()) reload.deletedBefore.push_back({id, it->second});
        }

        for (const auto& [id, shape] : st.changed) running[id] = shape;
        for (int id : st.deleted) running.erase(id);
        ReplayOp::BodyState after = toVec(running);

        // First, try to reconstruct a real op type from typeId + params blob
        // — this is how reloaded steps stay editable (live-Properties panel
        // works on the sketch, but the History → click-step → Properties
        // path needs an actual SketchEditOp, not a ReplayOp). Falls through
        // to the generic ReplayOp path on any parse failure.
        std::unique_ptr<Operation> op;
        if (st.typeId == "sketchedit" && !st.params.empty()) {
            op = ProjectIO::rehydrateSketchEditOp(st.params, *m_document);
        }

        // Backward-compat: boolean/delete steps written before they serialised
        // params have an empty blob, so they'd reload as baked ReplayOps and
        // silently overwrite any edit made to an UPSTREAM step (e.g. a fillet
        // feeding a union). Synthesise a params blob from the step's body diff
        // — target = the modified body, tool/victim = the deleted body — plus
        // the boolean mode parsed from the saved description. New projects carry
        // real params and skip this path.
        std::string params = st.params;
        if (params.empty()) {
            char buf[320];
            if (st.typeId == "boolean" && reload.modifiedBefore.size() == 1 &&
                reload.deletedBefore.size() == 1) {
                int mode = st.description.find("Subtract")  != std::string::npos ? 1
                         : st.description.find("Intersect") != std::string::npos ? 2 : 0;
                std::snprintf(buf, sizeof(buf), "target=%d;tool=%d;mode=%d",
                              reload.modifiedBefore[0].first,
                              reload.deletedBefore[0].first, mode);
                params = buf;
            } else if (st.typeId == "delete" && reload.deletedBefore.size() == 1) {
                std::snprintf(buf, sizeof(buf), "body=%d",
                              reload.deletedBefore[0].first);
                params = buf;
            } else if (st.typeId == "transform" &&
                       reload.modifiedBefore.size() == 1 &&
                       reload.modifiedAfter.size() == 1) {
                // Recover the rigid transform from the before/after snapshots so
                // the step reloads as a real op that re-applies to the LIVE body
                // (instead of a baked ReplayOp that overwrites upstream edits).
                gp_Trsf t;
                if (TransformOp::rigidTrsfBetween(reload.modifiedBefore[0].second,
                                                  reload.modifiedAfter[0].second, t)) {
                    std::snprintf(buf, sizeof(buf),
                        "body=%d;raw=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
                        reload.modifiedBefore[0].first,
                        t.Value(1,1), t.Value(1,2), t.Value(1,3), t.Value(1,4),
                        t.Value(2,1), t.Value(2,2), t.Value(2,3), t.Value(2,4),
                        t.Value(3,1), t.Value(3,2), t.Value(3,3), t.Value(3,4));
                    params = buf;
                }
            }
        }

        // Generic factory path: build the real op from typeId, restore its
        // parameters, then its post-execution state. Only ops that opt into
        // rehydrateFromReload() (returning true) come back editable; everyone
        // else falls through to the baked ReplayOp below, unchanged.
        if (!op && !params.empty()) {
            auto candidate = OperationFactory::create(st.typeId);
            if (candidate && candidate->deserializeParams(params) &&
                candidate->rehydrateFromReload(reload, *m_document)) {
                op = std::move(candidate);
                // Per-step reload tracing is --verbose only (fires once per
                // history step on every project load); the one-line [Reload]
                // health summary below stays always-on for bug reports.
                if (materializr::isVerbose())
                    std::fprintf(stderr, "[Reload] step '%s' (%s): rehydrated as "
                                         "real op (created=%zu modified=%zu)\n",
                                 st.name.c_str(), st.typeId.c_str(),
                                 reload.created.size(), reload.modifiedBefore.size());
            }
        }
        if (!op) {
            const bool affectsBody = !st.changed.empty() || !st.deleted.empty();
            if (affectsBody) ++bakedBodySteps; else ++bakedSketchSteps;
            if (materializr::isVerbose())
                std::fprintf(stderr, "[Reload] step '%s' (%s): baked ReplayOp "
                                     "(params=%s, affectsBody=%d)\n",
                             st.name.c_str(), st.typeId.c_str(),
                             st.params.empty() ? "none" : "present", (int)affectsBody);
            op = std::make_unique<ReplayOp>(
                st.typeId, st.name, st.description,
                std::move(before), std::move(after));
            // Carry the saved blob into the ReplayOp so future loaders /
            // editors can still see it.
            if (!st.params.empty())
                static_cast<ReplayOp*>(op.get())->setStoredParams(st.params);
        }
        op->setEnabled(st.enabled);
        // Restore the original timestamp so the HistoryPanel's date grouping
        // is preserved across reload. Legacy projects (timestamp == 0) get
        // bumped to "yesterday" so they group under that header instead of
        // landing on today and mixing with new work.
        if (st.timestampUnix > 0) {
            op->setTimestamp(std::chrono::system_clock::time_point{
                std::chrono::seconds{st.timestampUnix}});
        } else {
            op->setTimestamp(std::chrono::system_clock::now() -
                             std::chrono::hours{24});
        }
        m_history->pushExecuted(std::move(op));
    }

    // After all ops are rehydrated, the document bodies reflect their final state
    // (potentially after downstream Transforms). Re-resolve each fillet/chamfer's
    // generated-face indices against the final body so ownsFace() matches the
    // face positions the user actually sees and can click.
    materializr::refreshAllEdgeOpFaces(*m_history, *m_document);

    // Retrofit generative anchors onto fillets/chamfers loaded from a project
    // that predates the feature (their saved params carry no anchor= key). Do
    // it now, while their edges are still valid against the loaded body — the
    // user's next sketch edit would otherwise break the rebind before anchors
    // could ever be captured. Source sketch is derived from the body links.
    if (m_history) {
        auto links = sketchBodyLinks();
        for (int i = 0; i < m_history->stepCount(); ++i) {
            Operation* op = const_cast<Operation*>(m_history->getStep(i));
            if (auto* f = dynamic_cast<FilletOp*>(op)) {
                if (f->getSourceSketch() < 0)
                    for (const auto& [sid, bodies] : links)
                        if (bodies.count(f->getBodyId())) { f->setSourceSketch(sid); break; }
                f->ensureAnchors(*m_document);
            } else if (auto* c = dynamic_cast<ChamferOp*>(op)) {
                if (c->getSourceSketch() < 0)
                    for (const auto& [sid, bodies] : links)
                        if (bodies.count(c->getBodyId())) { c->setSourceSketch(sid); break; }
                c->ensureAnchors(*m_document);
            }
        }
    }

    // Health report. Two sources of non-editable geometry:
    //  • baked body-affecting STEPS (an op that didn't round-trip), and
    //  • base bodies in the INITIAL STATE that are not touched by any history
    //    step — geometry with no construction history (truly imported or frozen).
    //
    // A body in initialState that IS later modified by a step (common when the
    // project was saved mid-undo, causing a push/pull to lose its created-body
    // tracking) is NOT truly frozen: the ops still drive it. Count only bodies
    // that no step's diff references at all.
    std::set<int> touchedByOps;
    for (const auto& st : hist.steps) {
        for (const auto& [id, shp] : st.changed) touchedByOps.insert(id);
    }
    int frozenBodies = 0;
    for (const auto& [id, shp] : hist.initialState) {
        if (!touchedByOps.count(id)) ++frozenBodies;
    }
    std::fprintf(stderr,
        "[Reload] health: %d steps, %d baked body-features, %d baked sketch-edits, "
        "%zu base bodies (%d truly frozen, %zu op-driven phantoms)\n",
        static_cast<int>(hist.steps.size()), bakedBodySteps, bakedSketchSteps,
        hist.initialState.size(), frozenBodies,
        hist.initialState.size() - static_cast<std::size_t>(frozenBodies));
    const int nonEditable = bakedBodySteps + frozenBodies;
    // Only warn about frozen geometry if the file predates version tagging
    // (no SAVED_BY line). A file that WAS saved by a versioned build is current
    // — phantom initialState bodies in it are save-tracking artifacts, not a
    // true format downgrade, so we must not call it "older format".
    if (nonEditable > 0 && savedByVersion.empty()) {
        const int n        = frozenBodies > 0 ? frozenBodies : bakedBodySteps;
        const char* what   = frozenBodies > 0 ? "body(ies)" : "feature(s)";
        std::string msg =
            "This project was saved in an older format: " + std::to_string(n) + " " +
            what + " are frozen and can't be edited by value. The shapes are intact "
            "\xE2\x80\x94 to change a baked round/chamfer, select its face and use "
            "Repair Geometry to restore the sharp edge, then redo it. New saves "
            "won't have this.";
        showToast(msg, 9.0);
    }
}

void Application::ensureSketchSourceFace(int sketchId) {
    auto sk = m_document->getSketch(sketchId);
    if (!sk) return;
    if (!sk->getSourceFace().IsNull()) return; // already set; nothing to do
    // A detached sketch is independent of its former host: don't rebind its
    // face (the geometric match below would fail anyway once moved, but a
    // detached-in-place sketch could still match and inherit the host's
    // hole wires into its regions).
    if (sk->isDetachedFromBody()) return;
    int bid = sk->getSourceBody();
    if (bid < 0) {
        // Severed link (a pick that failed body attribution saved
        // sourceBody=-1): try to re-adopt — the body owning a planar face
        // coplanar with the sketch plane is the host. Heals old files.
        bid = findBodyUnderRegionlessPlane(sk->getPlane());
        if (bid < 0) return;
        sk->setSourceBody(bid);
        std::fprintf(stderr, "[Sketch] re-adopted severed body link -> %d\n",
                     bid);
    }
    TopoDS_Shape body;
    try { body = m_document->getBody(bid); } catch (...) { return; }
    if (body.IsNull()) return;

    const gp_Pln& sketchPln = sk->getPlane();
    gp_Pnt sO = sketchPln.Location();
    gp_Dir sN = sketchPln.Axis().Direction();

    // Two passes — first prefer faces that have inner wires (i.e., faces with
    // holes) since those are usually what the user sketched on; second pass
    // accepts the first geometric match. Tolerances loose enough to survive
    // a save/load + history-replay round trip without being so loose that
    // unrelated parallel faces match.
    auto matchPass = [&](bool requireHoles) -> TopoDS_Face {
        TopoDS_Face hit;
        for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
            TopoDS_Face f = TopoDS::Face(ex.Current());
            Handle(Geom_Surface) surf = BRep_Tool::Surface(f);
            if (surf.IsNull()) continue;
            Handle(Geom_Plane) gpln = Handle(Geom_Plane)::DownCast(surf);
            if (gpln.IsNull()) continue;
            gp_Pln fPln = gpln->Pln();
            gp_Dir fN = fPln.Axis().Direction();
            // Normals parallel (either direction). Tolerance ~0.6° of slack.
            if (std::abs(sN.Dot(fN)) < 0.9999) continue;
            // Sketch origin should lie on the face's plane within 0.05 mm.
            gp_Pnt fO = fPln.Location();
            gp_Vec d(fO, sO);
            double dist = std::abs(d.Dot(gp_Vec(fN)));
            if (dist > 0.05) continue;
            if (requireHoles) {
                // Walk wires — need at least one beyond the outer wire to
                // qualify as a face-with-hole.
                TopoDS_Wire outer = BRepTools::OuterWire(f);
                int wireCount = 0;
                for (TopExp_Explorer we(f, TopAbs_WIRE); we.More(); we.Next()) {
                    ++wireCount;
                }
                if (wireCount < 2 || outer.IsNull()) continue;
            }
            hit = f;
            break;
        }
        return hit;
    };
    TopoDS_Face f = matchPass(/*requireHoles=*/true);
    if (f.IsNull()) f = matchPass(/*requireHoles=*/false);
    if (!f.IsNull()) sk->setSourceFace(f);
}

// The body owning a planar face coplanar with `pln` (normals parallel,
// origin on the face plane within 0.05 mm). Used to re-adopt a sketch whose
// body link was severed. First match wins — coplanar-face ambiguity across
// bodies is rare and any match beats a dead link.
int Application::findBodyUnderRegionlessPlane(const gp_Pln& pln) const {
    if (!m_document) return -1;
    const gp_Dir sN = pln.Axis().Direction();
    const gp_Pnt sO = pln.Location();
    for (int bid : m_document->getAllBodyIds()) {
        TopoDS_Shape body;
        try { body = m_document->getBody(bid); } catch (...) { continue; }
        if (body.IsNull()) continue;
        for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
            Handle(Geom_Plane) gpln = Handle(Geom_Plane)::DownCast(
                BRep_Tool::Surface(TopoDS::Face(ex.Current())));
            if (gpln.IsNull()) continue;
            const gp_Pln fPln = gpln->Pln();
            if (std::abs(sN.Dot(fPln.Axis().Direction())) < 0.9999) continue;
            gp_Vec d(fPln.Location(), sO);
            if (std::abs(d.Dot(gp_Vec(fPln.Axis().Direction()))) > 0.05)
                continue;
            return bid;
        }
    }
    return -1;
}

int Application::findBodyUnderRegion(const TopoDS_Face& region,
                                     const gp_Pln& plane) const {
    if (region.IsNull()) return -1;
    const gp_Dir sN = plane.Axis().Direction();
    const gp_Pnt sO = plane.Location();
    // 2D-in-plane extent of the region (its footprint), used to confirm the
    // sketch actually sits OVER the candidate face rather than merely sharing
    // its infinite plane. A bounding-box overlap is robust to the region being
    // centred on an existing hole (a centroid/point-in-face test would fail
    // there, since the hole's centre is empty space — which is exactly the
    // "fill the hole" case we must support).
    Bnd_Box regionBox;
    try { BRepBndLib::Add(region, regionBox); } catch (...) { return -1; }
    if (regionBox.IsVoid()) return -1;

    for (int bid : m_document->getAllBodyIds()) {
        if (!m_document->isBodyVisible(bid)) continue;
        TopoDS_Shape body;
        try { body = m_document->getBody(bid); } catch (...) { continue; }
        if (body.IsNull()) continue;
        for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
            TopoDS_Face bf = TopoDS::Face(ex.Current());
            Handle(Geom_Surface) surf = BRep_Tool::Surface(bf);
            Handle(Geom_Plane) gpln = Handle(Geom_Plane)::DownCast(surf);
            if (gpln.IsNull()) continue;
            const gp_Pln fPln = gpln->Pln();
            const gp_Dir fN = fPln.Axis().Direction();
            // Coplanar: normals parallel (either sense) and the sketch origin
            // on the face's plane. Same tolerances as ensureSketchSourceFace.
            if (std::abs(sN.Dot(fN)) < 0.9999) continue;
            gp_Vec d(fPln.Location(), sO);
            if (std::abs(d.Dot(gp_Vec(fN))) > 0.05) continue;
            // The region's footprint must overlap this face's footprint —
            // otherwise it's a coplanar sketch sitting off to the side of the
            // body, which should stay free-floating.
            Bnd_Box faceBox;
            try { BRepBndLib::Add(bf, faceBox); } catch (...) { continue; }
            if (faceBox.IsVoid() || faceBox.IsOut(regionBox)) continue;
            return bid;
        }
    }
    return -1;
}

bool Application::loadProjectAt(const std::string& path) {
    if (path.empty()) return false;
    m_document->clear();
    m_history->clear();
    m_selection->clear();
    // Every cached highlight tessellation belongs to the outgoing project's
    // shapes — drop them (the entries pin their TShapes alive; see
    // SelectionHighlight::clearCaches).
    if (m_selectionHighlight) m_selectionHighlight->clearCaches();
    // Same for the static-sketch GPU buffers (keyed on the outgoing
    // project's sketch pointers; signature validation makes staleness
    // harmless, but the GPU memory belongs to dead sketches).
    if (m_sketchRenderer) m_sketchRenderer->clearCache();
    ProjectHistory hist;
    auto result = ProjectIO::load(path, *m_document, &hist);
    if (!result.success) {
        std::fprintf(stderr, "Load failed: %s\n", result.errorMessage.c_str());
        return false;
    }
    rebuildHistoryFromProject(hist, result.savedByVersion);
    // A reopened project should sit at the history tip with no redo stack — a
    // phantom redo tail would, e.g., block autosave (which won't save below-tip).
    m_history->dropRedoTail();
    m_currentProjectPath = path;
    m_currentProjectName.clear();   // callers adopting a doc URI set it after
    markSaved();
    m_meshesDirty = true;
    // Home view = the ViewCube Home button: default isometric orientation AND
    // zoom-to-fit the loaded geometry (computed from the OCCT body bounds, which
    // are ready now). Plain Camera::reset() only sets a fixed (5,5,5) eye that
    // ignores the model's size/position, leaving it zoomed-in or off-screen.
    handleViewCubeAction(static_cast<int>(ViewCubeAction::FrontTopRight));

    // m_sourceFace (the TopoDS_Face the sketch was drawn on) isn't part of
    // the project file — only the plane and sourceBodyId are. Re-derive it
    // for every loaded sketch so Sketch::buildRegions can union the host
    // face's wires (holes, fillets) into the sketch profile.
    for (int sid : m_document->getAllSketchIds()) {
        ensureSketchSourceFace(sid);
    }

    std::fprintf(stdout, "Loaded %d bodies, %d history steps from %s\n",
                 result.bodiesLoaded, static_cast<int>(hist.steps.size()),
                 path.c_str());
    // (Plane re-sync happens automatically — ConstructionPlanePlugin's
    // PlaneAddedEvent subscriber flips its own dirty flag during the
    // history replay above.)
    // Persist as the last-open project so the next launch can auto-reopen it.
    saveAppSettings();
    // A project is on screen now — the landing page's job is done.
    if (m_landingPage) m_landingPage->setVisible(false);
    return true;
}

void Application::loadProjectWithProgress(const std::string& path) {
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::duration d) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    };
    // Show something immediately so the window isn't a frozen blank. Start the
    // latch clear: a Cancel/Escape left over from an earlier op would otherwise
    // make every progress frame below return without drawing OR pumping.
    m_progressCancelled = false;
    renderProgressFrame(-1.0f, "Loading project\xE2\x80\xA6");

    auto t0 = clock::now();
    bool ok = loadProjectAt(path);   // ProjectIO::load (BREP read) + history rebuild
    auto t1 = clock::now();
    if (ok) {
        // Tessellate up front HERE (between frames) so the per-body progress
        // frames are safe, instead of letting the first render frame block.
        m_pumpMeshProgress = true;
        rebuildMeshes();
        m_pumpMeshProgress = false;
        m_meshesDirty = false;
    }
    auto t2 = clock::now();
    std::fprintf(stderr, "[load-timing] parse+history=%lld ms  tessellate=%lld ms  total=%lld ms\n",
                 static_cast<long long>(ms(t1 - t0)),
                 static_cast<long long>(ms(t2 - t1)),
                 static_cast<long long>(ms(t2 - t0)));
}

void Application::addRecentProject(const std::string& ref, const std::string& name) {
    if (ref.empty()) return;
    constexpr size_t kMaxRecents = 10;
    // Drop any existing entry with the same ref, then push this to the front.
    for (auto it = m_recentProjects.begin(); it != m_recentProjects.end(); ) {
        if (it->ref == ref) it = m_recentProjects.erase(it);
        else                ++it;
    }
    AppSettings::RecentProject rp;
    rp.ref  = ref;
    rp.name = name.empty() ? ref : name;
    m_recentProjects.insert(m_recentProjects.begin(), rp);
    if (m_recentProjects.size() > kMaxRecents)
        m_recentProjects.resize(kMaxRecents);
    saveAppSettings();
}

void Application::removeRecentProject(const std::string& ref) {
    bool changed = false;
    for (auto it = m_recentProjects.begin(); it != m_recentProjects.end(); ) {
        if (it->ref == ref) { it = m_recentProjects.erase(it); changed = true; }
        else                ++it;
    }
    if (changed) saveAppSettings();
}

void Application::guardedOpen(std::function<void()> doOpen) {
    if (!isDirty()) { doOpen(); return; }
    // Unsaved changes: defer the open until the save prompt resolves so we never
    // silently discard work (this also closes the same gap on the Open dialog).
    m_pendingOpenAction = std::move(doOpen);
    m_postSaveAction = PostSaveAction::OpenProject;
    m_showSavePrompt = true;
}

void Application::openRecentProject(const AppSettings::RecentProject& r) {
    // Copy first: addRecentProject/removeRecentProject mutate m_recentProjects,
    // which may be the vector backing the reference `r`.
    const std::string ref  = r.ref;
    const std::string name = r.name;
    // One project, one tab. Every recent-open route lands here — the home
    // screen's tiles, the "+" dropdown, the File menu — so the guard sits here
    // rather than at each of them.
    if (focusExistingProject(ref)) return;
    guardedOpen([this, ref, name]() {
#if defined(MZ_MOBILE)
        // ref is a persisted SAF content:// URI — resolve to a temp file, no picker.
        std::string tmp = materializr::mobileOpenUri(ref);
        if (tmp.empty()) {
            showToast("Couldn't open \"" + name + "\" - access may have been revoked.");
            removeRecentProject(ref);
            return;
        }
        // A BACKUP copy, served because the document is gone or disowned —
        // real content, but not what that URI holds now. Keeping the URI as
        // the save target would let the next quick-save truncate a document
        // we never read (or recreate one the user deliberately deleted), so
        // the project comes back UNLINKED: saving prompts for a destination.
        const bool viaFallback = materializr::mobileLastOpenWasFallback();
        if (loadProjectAt(tmp)) {
            addRecentProject(ref, name);  // bump to front
            // The resolved temp is peekable even though the content: ref is
            // not — harvest the embedded thumbnail into the cache so this
            // project's landing tile fills in from the next show onward.
            {
                std::vector<uint8_t> png;
                if (ProjectIO::peekThumbnail(tmp, png))
                    cacheProjectThumbnail(ref, png);
            }
#if defined(__ANDROID__)
            if (viaFallback) {
                m_currentProjectPath.clear();   // Save → picker, not overwrite
                m_currentProjectName = name;
                showToast("Opened a local backup of \"" + name +
                          "\" - the original is gone. Save to keep it.");
            } else {
                // Track the DOCUMENT as the project identity so quick-save
                // writes back to the real file (loadProjectAt stored the
                // cache temp).
                m_currentProjectPath = ref;
                m_currentProjectName = name;
            }
            saveAppSettings();            // lastProjectPath -> the real ref
#endif
        }
        else { showToast("Failed to open \"" + name + "\"."); removeRecentProject(ref); }
#else
        if (loadProjectAt(ref)) addRecentProject(ref, name);  // bump to front
        else {
            showToast("Couldn't open \"" + name + "\" - the file may have moved or been deleted.");
            removeRecentProject(ref);
        }
#endif
    });
}

void Application::loadProject() {
    FileDialogs::openFile("Open Project",
        {{"Materializr Project", "*.mzr *.materializr"}, {"All Files", "*"}},
        [this](const std::string& path) {
            if (path.empty()) return;
            // Picking a file that is already open in another tab focuses it
            // instead of making a second one. Checked here, after the picker,
            // because that is the first moment the file is known.
            //
            // Compare on the same IDENTITY a tab stores. On mobile that is the
            // SAF content:// URI, not `path` — the picker hands back a cache
            // temp it copied the document into, and a fresh temp per open would
            // never match anything. The URI is already readable here: the
            // poll that produced `path` only fires once the Java side has
            // opened the document and recorded it.
            std::string ident = path;
#if defined(MZ_MOBILE)
            {
                const std::string uri = materializr::mobileLastDocUri();
                if (!uri.empty()) ident = uri;
            }
#endif
            if (focusExistingProject(ident)) return;
            // Guard unsaved changes (the picked path is captured for after the
            // save prompt resolves), then load + record in Open Recent.
            guardedOpen([this, path]() {
                if (!loadProjectAt(path)) return;
                // Record with a *persistable* ref: the SAF content:// URI on
                // Android (the `path` is a throwaway temp there), the real path
                // on desktop.
                std::string ref, name;
#if defined(MZ_MOBILE)
                ref  = materializr::mobileLastDocUri();
                name = materializr::mobileLastDocName();
                if (ref.empty()) ref = path; // fallback (non-persistable provider)
#if defined(__ANDROID__)
                // Same identity adoption as Open Recent: quick-save must reach
                // the picked document, not the cache temp it was read from.
                if (ref.rfind("content:", 0) == 0) {
                    m_currentProjectPath = ref;
                    m_currentProjectName =
                        name.empty()
                            ? std::filesystem::path(path).filename().string()
                            : name;
                    saveAppSettings();
                }
#endif
#else
                ref = path;
#endif
                if (name.empty()) name = std::filesystem::path(path).filename().string();
                addRecentProject(ref, name);
            });
        });
}

void Application::closeProject() {
    // If nothing to lose, close immediately. If autosave is on and the project
    // already has a path, autosave quietly before closing. Otherwise (dirty +
    // no autosave) route through the save-prompt with CloseProject intent.
    if (!isDirty()) { doCloseProject(); return; }
    // The quiet-autosave shortcut only applies at the history tip — saving in
    // an undone state would silently drop the redo tail from the file (only
    // applied steps persist). Below the tip, fall through to the explicit
    // prompt so losing those steps is the user's call, not autosave's.
    if (m_autosaveEnabled && !m_currentProjectPath.empty() &&
        !(m_history && m_history->canRedo())) {
        saveProjectQuick();
        doCloseProject();
        return;
    }
    m_postSaveAction = PostSaveAction::CloseProject;
    m_showSavePrompt = true;
}

void Application::doCloseProject() {
    m_document->clear();
    m_history->clear();
    m_selection->clear();
    m_currentProjectPath.clear();
    m_currentProjectName.clear();
    m_savedAtHistoryStep = -1;
    m_unsavedNonHistoryChanges = false;
    m_meshesDirty = true;
    // Home view (empty scene → sensible default at origin), same as ViewCube Home.
    handleViewCubeAction(static_cast<int>(ViewCubeAction::FrontTopRight));
    // Persist: lastProjectPath now empty → no auto-open on next launch.
    saveAppSettings();
}

bool Application::isDirty() const {
    return (m_history && m_history->currentStep() != m_savedAtHistoryStep)
        || m_unsavedNonHistoryChanges;
}

void Application::applyDisplayUnitChange(int unit) {
    // Clamp here too: Settings clamps on load, but the combo and any future
    // caller should not be able to hand the table an index it cannot serve.
    if (unit < 0 || unit >= materializr::kLengthUnitCount) unit = 0;
    // A text field mid-edit was showing the OLD unit; letting it commit after
    // the switch would interpret those digits in the NEW one. Drop the edit.
    // Guarded: settings are applied before the ImGui context exists.
    if (ImGui::GetCurrentContext()) ImGui::ClearActiveID();

    // ClearActiveID drops FOCUS, and the sketch dimension field only ever
    // grabs focus once per placement — m_sketchDimWasShown latches true on the
    // first frame and is cleared only on commit or on leaving placement. So
    // switching units mid-placement left that popup on screen with an input
    // nothing could type into, and no way to finish the shape by keyboard.
    // Whoever clears the active ID has to clear the latch that guards
    // re-focusing, or the two disagree about whether a field is live.
    //
    // The buffer goes too, and not just for tidiness: it still holds digits
    // meant as the OLD unit. Clearing the active ID stops THIS frame's commit,
    // but the characters survive, so clicking back into the field and pressing
    // Enter would commit them as the new unit — the exact misreading the
    // ClearActiveID above exists to prevent.
    m_sketchDimBuf[0] = '\0';
    m_sketchDimValue = 0.0f;
    m_sketchDimWasShown = false;   // re-seeds and re-grabs focus next frame

    // Carry the grid step's DISPLAYED NUMBER across, not its millimetres. The
    // step is the snap lattice and the visible grid, so leaving it at 1 mm
    // while the view frames 40 ft draws 12192 lines (the renderer fades them
    // to nothing: the grid vanishes) and offers a lattice 1/300th of a usable
    // one. Working "in feet" means a grid of one foot.
    //
    // Safe only because SketchTool::tolStep() now caps what POINTING
    // tolerances take from the step. Rescaling it while trim, pick, inference
    // and hover distances derived from it directly put the trim threshold at
    // 152 mm — see 0733a59, which reverted exactly that.
    const auto next = static_cast<materializr::LengthUnit>(unit);
    if (next != materializr::currentUnit() && m_sketchGridStep > 0.0f) {
        const double shownStep = materializr::toDisplay(m_sketchGridStep);
        materializr::setCurrentUnit(next);
        m_sketchGridStep = static_cast<float>(materializr::toMm(shownStep));
        if (m_toolbar)    m_toolbar->setGridStep(m_sketchGridStep);
        if (m_sketchTool) m_sketchTool->setGridStep(m_sketchGridStep);
    } else {
        materializr::setCurrentUnit(next);
    }
    m_displayUnit = unit;
}

void Application::markDirty() {
    m_unsavedNonHistoryChanges = true;
    // Recovery debounce: remember WHEN the newest change landed so the
    // crash-recovery writer can snapshot once things settle (see
    // writeProjectRecoveryIfDue), instead of re-gzipping on a timer.
    m_lastChangeSeenAt = SDL_GetTicks() / 1000.0;
}

void Application::markSaved() {
    m_savedAtHistoryStep = m_history ? m_history->currentStep() : -1;
    m_unsavedNonHistoryChanges = false;
}

void Application::requestClose() {
    if (m_confirmedClose) return;
    if (!isDirty()) { m_confirmedClose = true; return; }
    m_showSavePrompt = true;
    m_closeAfterSave = false;
    m_window->requestClose(false);
}

void Application::renderSavePrompt() {
    if (m_showSavePrompt) {
        ImGui::OpenPopup("Unsaved Changes");
        m_showSavePrompt = false; // OpenPopup latches; only call once per request
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const char* prompt;
        switch (m_postSaveAction) {
            case PostSaveAction::CloseProject:
                prompt = "You have unsaved changes. Save before closing the project?"; break;
            case PostSaveAction::OpenProject:
                prompt = "You have unsaved changes. Save before opening another project?"; break;
            default:
                prompt = "You have unsaved changes. Save before exiting?"; break;
        }
        ImGui::Text("%s", prompt);
        ImGui::Separator();
        if (ImGui::Button(materializr::tr("Save"), materializr::uiSz(100, 0))) {
            m_closeAfterSave = true;
            saveProjectQuick();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(materializr::tr("Don't Save"), materializr::uiSz(100, 0))) {
            if (m_postSaveAction == PostSaveAction::CloseProject) {
                doCloseProject();
                m_postSaveAction = PostSaveAction::None;
            } else if (m_postSaveAction == PostSaveAction::OpenProject) {
                auto act = std::move(m_pendingOpenAction);
                m_pendingOpenAction = nullptr;
                m_postSaveAction = PostSaveAction::None;
                if (act) act();
            } else {
                m_confirmedClose = true;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(materializr::tr("Cancel"), materializr::uiSz(100, 0))) {
            m_closeAfterSave = false;
            m_pendingOpenAction = nullptr;
            m_postSaveAction = PostSaveAction::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Application::importStepFile() {
    FileDialogs::openFile("Import STEP",
        {{"STEP Files", "*.step *.stp *.STEP *.STP"}},
        [this](const std::string& path) {
            if (path.empty()) return;
            auto result = StepIO::import(path, *m_document);
            if (result.success) {
                m_meshesDirty = true;
                markDirty();
                std::fprintf(stdout, "Imported %d bodies from %s\n", result.bodiesImported, path.c_str());
            } else {
                std::fprintf(stderr, "Import failed: %s\n", result.errorMessage.c_str());
            }
        });
}

void Application::exportStepFile() {
    FileDialogs::saveFile("Export STEP", "export.step",
        {{"STEP Files", "*.step *.stp"}},
        [this](std::string path) {
            if (path.empty()) return;
            // Keep a STEP extension — accept either .step or .stp; append
            // .step only when the typed name has neither.
            std::string ext = std::filesystem::path(path).extension().string();
            if (ext != ".step" && ext != ".stp") path += ".step";
            auto result = StepIO::exportFile(path, *m_document);
            if (result.success) std::fprintf(stdout, "Exported to %s\n", path.c_str());
            else std::fprintf(stderr, "Export failed: %s\n", result.errorMessage.c_str());
        });
}

void Application::exportBodyAsStl(int bodyId) {
    if (!m_document || bodyId < 0) return;
    // Build a safe default filename from the body's name. Strip / replace
    // characters that the OS would reject in a filename so the dialog
    // doesn't open with an invalid suggestion the user has to fix.
    std::string name = m_document->getBodyName(bodyId);
    if (name.empty()) name = "body-" + std::to_string(bodyId);
    for (char& ch : name) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|') ch = '_';
    }
    std::string defaultFile = name + ".stl";

    TopoDS_Shape shape;
    try { shape = m_document->getBody(bodyId); } catch (...) {}
    if (shape.IsNull()) {
        std::fprintf(stderr, "Export STL: body %d has no geometry\n", bodyId);
        return;
    }

#if defined(MZ_MOBILE)
    // Touch: offer Share (to Drive/email/3D apps) or Save-to-device. Both context
    // menus (viewport long-press + Items panel) route here.
    FileDialogs::mobileExportShareOrSave(defaultFile, "application/octet-stream",
        [shape](const std::string& path) {
            auto result = StlExport::exportShape(path, shape);
            if (result.success)
                std::fprintf(stdout, "Exported %d triangles to %s\n",
                             result.triangleCount, path.c_str());
            else
                std::fprintf(stderr, "STL export failed: %s\n", result.errorMessage.c_str());
            return result.success;
        });
#else
    FileDialogs::saveFile("Export Body to STL", defaultFile,
        {{"STL Files", "*.stl"}},
        [shape](std::string path) {
            if (path.empty()) return;
            // Keep the .stl extension — pfd/zenity don't force it, so a typed
            // name with no extension saved a valid but extensionless file
            // (mirrors the .materializr project-save enforcement).
            if (std::filesystem::path(path).extension() != ".stl")
                path += ".stl";
            auto result = StlExport::exportShape(path, shape);
            if (result.success) {
                std::fprintf(stdout, "Exported %d triangles to %s\n",
                             result.triangleCount, path.c_str());
            } else {
                std::fprintf(stderr, "STL export failed: %s\n",
                             result.errorMessage.c_str());
            }
        });
#endif
}

void Application::exportBodiesAs(const std::vector<int>& bodyIds,
                                 const std::string& formatName) {
    if (!m_document || bodyIds.empty()) return;
    // Find the format's document exporter in the registry. exportFn is no use
    // here: it runs its own file dialog whose callback reads ctx.document()
    // frames later, which would export the WHOLE project instead of the
    // chosen bodies.
    const IOFormatContribution* fmt = nullptr;
    for (const auto& f : PluginRegistry::instance().ioFormats()) {
        if (f.name == formatName && f.exportDocFn) { fmt = &f; break; }
    }
    if (!fmt) { showToast("Can't export to " + formatName + "."); return; }

    // A scratch document holding BAKED copies of the chosen bodies, at their
    // real positions — that's what makes a print-in-place assembly come out
    // as one file with the parts still where they belong. shared_ptr because
    // the dialog's callback runs frames later.
    auto scratch = std::make_shared<Document>();
    for (int id : bodyIds) {
        TopoDS_Shape s;
        try { s = m_document->getBody(id); } catch (...) {}
        if (s.IsNull()) continue;
        const int nid = scratch->addBody(s, m_document->getBodyName(id));
        scratch->setBodyColor(nid, m_document->getBodyColor(id));
    }
    if (scratch->getAllBodyIds().empty()) {
        showToast("Nothing to export — those bodies have no geometry.");
        return;
    }

    // Default filename: the body's name for one, the project's for a set.
    std::string base;
    if (bodyIds.size() == 1) base = m_document->getBodyName(bodyIds.front());
    if (base.empty()) base = m_currentProjectName;
    if (base.empty()) base = "export";
    if (const auto dot = base.rfind('.'); dot != std::string::npos && dot > 0)
        base = base.substr(0, dot);          // drop a project extension
    for (char& ch : base)
        if (std::strchr("/\\:*?\"<>|", ch)) ch = '_';
    const std::string ext = fmt->extensions.empty() ? "dat" : fmt->extensions.front();
    const std::string defaultFile = base + "." + ext;

    auto write = [scratch, fmt, ext](std::string path) {
        if (path.empty()) return false;
        if (std::filesystem::path(path).extension() != "." + ext) path += "." + ext;
        const bool ok = fmt->exportDocFn(*scratch, path);
        std::fprintf(ok ? stdout : stderr, "%s %s\n",
                     ok ? "Exported" : "Export FAILED:", path.c_str());
        return ok;
    };
    const std::string title = bodyIds.size() > 1
        ? "Export " + std::to_string(bodyIds.size()) + " Bodies"
        : "Export Body";
#if defined(MZ_MOBILE)
    FileDialogs::mobileExportShareOrSave(defaultFile, "application/octet-stream",
                                         [write](const std::string& p) { return write(p); });
#else
    std::string filter = "*." + ext;
    FileDialogs::saveFile(title, defaultFile,
                          {{formatName + " Files", filter}},
                          [write](std::string path) { (void)write(std::move(path)); });
#endif
}

void Application::exportSketchAsSvg(int sketchId) {
    if (!m_document || sketchId < 0) return;
    auto sketch = m_document->getSketch(sketchId);
    if (!sketch) return;

    // Safe default filename from the sketch name (strip characters the OS rejects).
    std::string name = m_document->getSketchName(sketchId);
    if (name.empty()) name = "sketch-" + std::to_string(sketchId);
    for (char& ch : name) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|') ch = '_';
    }
    std::string defaultFile = name + ".svg";

    // Capture the shared_ptr so the (async) dialog callback can't dangle.
    auto sk = sketch;

#if defined(MZ_MOBILE)
    FileDialogs::mobileExportShareOrSave(defaultFile, "image/svg+xml",
        [sk](const std::string& path) {
            auto result = materializr::SvgExport::exportSketch(path, *sk);
            if (result.success)
                std::fprintf(stdout, "Exported %d curve(s) to %s\n",
                             result.curveCount, path.c_str());
            else
                std::fprintf(stderr, "SVG export failed: %s\n", result.errorMessage.c_str());
            return result.success;
        });
#else
    FileDialogs::saveFile("Export Sketch to SVG", defaultFile,
        {{"SVG Files", "*.svg"}},
        [sk](std::string path) {
            if (path.empty()) return;
            // Keep the .svg extension — the picker doesn't force it.
            if (std::filesystem::path(path).extension() != ".svg") path += ".svg";
            auto result = materializr::SvgExport::exportSketch(path, *sk);
            if (result.success) {
                std::fprintf(stdout, "Exported %d curve(s) to %s\n",
                             result.curveCount, path.c_str());
            } else {
                std::fprintf(stderr, "SVG export failed: %s\n",
                             result.errorMessage.c_str());
            }
        });
#endif
}

void Application::exportSketchAsDxf(int sketchId) {
    // The SVG twin (above) for the CAM/laser-cutter world: R12 DXF entities
    // in millimeters. Same name derivation and dialog flow.
    if (!m_document || sketchId < 0) return;
    auto sketch = m_document->getSketch(sketchId);
    if (!sketch) return;

    std::string name = m_document->getSketchName(sketchId);
    if (name.empty()) name = "sketch-" + std::to_string(sketchId);
    for (char& ch : name) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|') ch = '_';
    }
    std::string defaultFile = name + ".dxf";
    auto sk = sketch; // keep alive across the async dialog callback

#if defined(MZ_MOBILE)
    FileDialogs::mobileExportShareOrSave(defaultFile, "application/dxf",
        [sk](const std::string& path) {
            auto result = materializr::DxfExport::exportSketch(path, *sk);
            if (result.success)
                std::fprintf(stdout, "Exported %d DXF entit%s to %s\n",
                             result.entityCount,
                             result.entityCount == 1 ? "y" : "ies", path.c_str());
            else
                std::fprintf(stderr, "DXF export failed: %s\n", result.errorMessage.c_str());
            return result.success;
        });
#else
    FileDialogs::saveFile("Export Sketch to DXF", defaultFile,
        {{"DXF Files", "*.dxf"}},
        [sk](std::string path) {
            if (path.empty()) return;
            if (std::filesystem::path(path).extension() != ".dxf") path += ".dxf";
            auto result = materializr::DxfExport::exportSketch(path, *sk);
            if (result.success) {
                std::fprintf(stdout, "Exported %d DXF entit%s to %s\n",
                             result.entityCount,
                             result.entityCount == 1 ? "y" : "ies", path.c_str());
            } else {
                std::fprintf(stderr, "DXF export failed: %s\n",
                             result.errorMessage.c_str());
            }
        });
#endif
}

void Application::combineSketches(const std::vector<int>& ids) {
    if (ids.size() < 2 || !m_document) return;
    auto target = m_document->getSketch(ids.front());
    if (!target) return;

    // Keep only the others that are COPLANAR with the target (parallel plane +
    // lying on it). Combining non-coplanar sketches has no meaning.
    const gp_Pln& tp = target->getPlane();
    gp_Vec tN(tp.Axis().Direction());
    gp_Pnt tO = tp.Location();
    std::vector<int> coplanar;
    for (size_t i = 1; i < ids.size(); ++i) {
        auto sk = m_document->getSketch(ids[i]);
        if (!sk) continue;
        const gp_Pln& sp = sk->getPlane();
        gp_Vec sN(sp.Axis().Direction());
        if (std::abs(sN.Dot(tN)) < 0.999) continue;
        gp_Vec d(sp.Location().X() - tO.X(), sp.Location().Y() - tO.Y(),
                 sp.Location().Z() - tO.Z());
        if (std::abs(d.Dot(tN)) > 0.05) continue;
        coplanar.push_back(ids[i]);
    }
    if (coplanar.empty()) {
        showToast("Combine needs sketches that share a plane.");
        return;
    }

    auto op = std::make_unique<CombineSketchesOp>();
    op->setTarget(ids.front(), *target);
    for (int oid : coplanar) {
        auto sk = m_document->getSketch(oid);
        if (sk) op->addOther(oid, *sk, m_document->getSketchName(oid),
                             m_document->isSketchVisible(oid));
    }
    if (m_history->pushOperation(std::move(op), *m_document)) {
        m_selection->clear();
        markDirty();
        m_meshesDirty = true;
        std::fprintf(stdout, "Combined %d sketch(es) into %d\n",
                     static_cast<int>(coplanar.size()), ids.front());
    }
}

void Application::duplicateSketch(int sketchId) {
    if (!m_document || !m_history) return;
    auto src = m_document->getSketch(sketchId);
    if (!src) return;

    // Independent deep copy: geometry, constraints and plane come along. The
    // deep copy also carries the source's body/face link, but
    // DuplicateSketchOp::execute severs it (issue #21) so the copy is a
    // standalone sketch with its own id — editing, push/pull or extrude never
    // touches the original or the body built from it, and instead makes a new one.
    auto copy = std::make_shared<Sketch>(*src);

    std::string base = m_document->getSketchName(sketchId);
    if (base.empty()) base = "Sketch";
    const std::string name = base + " copy";

    auto op = std::make_unique<DuplicateSketchOp>();
    op->setCopy(copy, sketchId, name);
    DuplicateSketchOp* raw = op.get();  // valid while History owns the op
    if (m_history->pushOperation(std::move(op), *m_document)) {
        markDirty();
        m_meshesDirty = true;
        std::fprintf(stdout, "Duplicated sketch %d -> %d\n",
                     sketchId, raw->newSketchId());
        showToast("Duplicated \"" + base + "\" \xE2\x80\x94 edit the copy freely "
                  "(e.g. resize holes); the original is untouched.");
    }
}

void Application::enterSketchMode() {
    // If a planar face is selected, route through enterSketchOnFace for consistency
    if (m_selection && m_selection->hasSelectedFaces()) {
        const auto& sel = m_selection->getSelection();
        for (const auto& entry : sel) {
            if (entry.type == SelectionType::Face && !entry.shape.IsNull()) {
                enterSketchOnFace(TopoDS::Face(entry.shape), entry.bodyId);
                return;
            }
        }
    }

    m_activeSketch = std::make_shared<Sketch>();
    m_sketchSolver = std::make_unique<SketchSolver>();
    m_activeSketchId = -1;

    m_sketchTool->setSketch(m_activeSketch.get());
    m_sketchTool->setSolver(m_sketchSolver.get());
    m_sketchSolver->setSketch(m_activeSketch.get());
    m_sketchTool->setMode(SketchToolMode::Line);
    m_inSketchMode = true;
    m_sketchEntryHistoryStep = m_history ? m_history->currentStep() : -1;
    if (m_history) m_history->setUndoFloor(m_sketchEntryHistoryStep);  // no undo past sketch entry
    m_toolbar->setSketchMode(true);
    // Fresh sketch session: drop any stale Dimension edit-popup state left
    // over from a previous session (e.g. Esc/undo left m_dimEditingId set
    // without the popup ever closing) so it can't reopen against a
    // constraint id from a different sketch.
    m_dimOpenEditRequested = false;
    m_dimEditingId = -1;
    m_dimPopupConsumedEsc = false;
    alignCameraToActiveSketch();
}

void Application::enterSketchOnPlane(const gp_Pln& plane) {
    // Start a fresh, freestanding sketch on a world base plane (no source face),
    // so the user can model from scratch with no existing body. Drawing tools,
    // the adjustable grid and the ortho camera all come from the shared sketch
    // path via alignCameraToActiveSketch() / renderSketchTools().
    m_activeSketch = std::make_shared<Sketch>();
    m_sketchSolver = std::make_unique<SketchSolver>();
    m_activeSketchId = -1;

    m_activeSketch->setPlane(plane);

    m_sketchTool->setSketch(m_activeSketch.get());
    m_sketchTool->setSolver(m_sketchSolver.get());
    m_sketchSolver->setSketch(m_activeSketch.get());
    m_sketchTool->setMode(SketchToolMode::Line);
    m_inSketchMode = true;
    m_sketchEntryHistoryStep = m_history ? m_history->currentStep() : -1;
    if (m_history) m_history->setUndoFloor(m_sketchEntryHistoryStep);  // no undo past sketch entry
    m_toolbar->setSketchMode(true);
    // Fresh sketch session: drop any stale Dimension edit-popup state left
    // over from a previous session (e.g. Esc/undo left m_dimEditingId set
    // without the popup ever closing) so it can't reopen against a
    // constraint id from a different sketch.
    m_dimOpenEditRequested = false;
    m_dimEditingId = -1;
    m_dimPopupConsumedEsc = false;
    alignCameraToActiveSketch();
}

bool Application::faceIsPlanar(const TopoDS_Face& face) const {
    Handle(Geom_Surface) s = BRep_Tool::Surface(face);
    if (s.IsNull()) return false;
    if (s->IsKind(STANDARD_TYPE(Geom_Plane))) return true;
    GeomLib_IsPlanarSurface tester(s, 1.0e-7);
    return tester.IsPlanar();
}

void Application::enterSketchOnFace(const TopoDS_Face& face, int sourceBodyId) {
    // A pick that failed body attribution (bodyId -1) severs the sketch-body
    // link for the sketch's whole life: sourceFace can't rebind after
    // reload, the centroid/centre snaps die, and Subtract loses its target
    // (Steve's cap sketch saved with sourceBody=-1 — every centre fix was
    // inert on it). Recover the link: the body CONTAINING the picked face
    // is the source.
    if (sourceBodyId < 0 && m_document) {
        for (int id : m_document->getAllBodyIds()) {
            TopoDS_Shape b;
            try { b = m_document->getBody(id); } catch (...) { continue; }
            if (b.IsNull()) continue;
            for (TopExp_Explorer fx(b, TopAbs_FACE); fx.More(); fx.Next()) {
                if (fx.Current().IsSame(face)) { sourceBodyId = id; break; }
            }
            if (sourceBodyId >= 0) break;
        }
        std::fprintf(stderr, "[Sketch] on-face pick had no body id — "
                             "recovered body=%d\n", sourceBodyId);
    }
    // Sketching needs a FLAT face. A curved face (cylinder / sphere / fillet)
    // has no single plane — we'd otherwise drop the sketch onto a tangent plane
    // at an arbitrary point on the curve, which isn't useful and a construction
    // plane (Add Plane…) covers properly. Refuse with guidance.
    //
    // Detect planarity GEOMETRICALLY, not by surface type: a face can be flat
    // while backed by a non-Geom_Plane surface — e.g. a slanted side face
    // produced by scaling a box's top into a frustum is a planar trapezoid on a
    // ruled/BSpline surface. A literal Geom_Plane type-check called those
    // "curved" by mistake. GeomLib_IsPlanarSurface accepts the flat ones (and
    // recovers their plane) while still rejecting genuinely warped faces.
    gp_Pln pln;
    {
        Handle(Geom_Surface) s = BRep_Tool::Surface(face);
        bool planar = false;
        if (!s.IsNull()) {
            if (s->IsKind(STANDARD_TYPE(Geom_Plane))) {
                pln = Handle(Geom_Plane)::DownCast(s)->Pln();
                planar = true;
            } else {
                GeomLib_IsPlanarSurface tester(s, 1.0e-7);
                if (tester.IsPlanar()) { pln = tester.Plan(); planar = true; }
            }
        }
        if (!planar) {
            showToast("Can't sketch on a curved face \xE2\x80\x94 use Add "
                      "Plane\xE2\x80\xA6 to place a construction plane.");
            return;
        }
    }

    // For an imported MESH face, the recovered plane is an arbitrary seed
    // triangle's plane (UnifySameDomain keeps a seed, it doesn't best-fit), which
    // at low import accuracy can be visibly tilted from the flat you picked. Best-
    // fit the plane to the face's actual triangulation — an area-weighted normal +
    // centroid — so the sketch lands on the region's true average plane. This is
    // what makes "sketch on a flat-ish face" reliable regardless of import accuracy.
    if (sourceBodyId >= 0 && m_document && m_document->isBodyMesh(sourceBodyId)) {
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull()) {
            BRepMesh_IncrementalMesh mesher(face, 0.1);
            mesher.Perform();
            tri = BRep_Tool::Triangulation(face, loc);
        }
        if (!tri.IsNull() && tri->NbTriangles() > 0) {
            const gp_Trsf& trsf = loc.Transformation();
            const bool hasX = !loc.IsIdentity();
            gp_Vec nSum(0.0, 0.0, 0.0);
            gp_XYZ cSum(0.0, 0.0, 0.0);
            double aSum = 0.0;
            for (int i = 1; i <= tri->NbTriangles(); ++i) {
                int a, b, c;
                tri->Triangle(i).Get(a, b, c);
                gp_Pnt p1 = tri->Node(a), p2 = tri->Node(b), p3 = tri->Node(c);
                if (hasX) { p1.Transform(trsf); p2.Transform(trsf); p3.Transform(trsf); }
                const gp_Vec cross = gp_Vec(p1, p2).Crossed(gp_Vec(p1, p3)); // 2·area·n̂
                const double area = cross.Magnitude();
                nSum += cross;
                cSum += (p1.XYZ() + p2.XYZ() + p3.XYZ()) * (area / 3.0);
                aSum += area;
            }
            if (nSum.Magnitude() > 1e-12 && aSum > 1e-12) {
                pln = gp_Pln(gp_Pnt(cSum / aSum), gp_Dir(nSum));
            }
        }
    }

    // Align the sketch plane's X axis to the face's own geometry. The plane
    // recovered from the surface uses the surface's intrinsic parametric X,
    // which a boolean or a loft can leave rotated any which way.
    //
    // The rule lives in modeling/SketchPlaneAxis.h so it can be tested — ctest
    // cannot see src/app, and this heuristic has now misfired twice: once on a
    // lofted cap (fixed by following the longest edge) and once on a symmetric
    // taper, whose two LONGEST edges are its diagonals, so the grid rotated to
    // a diagonal on a part built symmetric about a world axis.
    {
        const gp_Ax3& cur = pln.Position();
        const gp_Dir x = materializr::sketchPlaneXDirection(
            face, cur.Direction(), cur.XDirection());
        pln = gp_Pln(gp_Ax3(cur.Location(), cur.Direction(), x));
    }

    // Whether the plane origin below ends up on the face's true centre —
    // the refs builder then adds (0,0) as a snappable point so the centre
    // is directly clickable.
    bool faceCenterAnchored = false;

    // THREADED body cap: the Thread step in history knows the TRUE axis
    // (kept accurate through resize/move/cascade by the face-ref and
    // coaxial re-resolution), so anchor there DIRECTLY — it outranks any
    // geometric fitting. Fitting is unreliable on threaded caps: a cap cut
    // inside the groove span has NO crest arc on its boundary, and the
    // only exact circle left is the sweep's construction arc, centred at
    // the surface's parametric origin ~0.3 mm off-axis — the fitted anchor
    // adopted it and the true centre stopped snapping (2026-07-21
    // regression, Steve's second poke).
    {
        glm::vec2 c2;
        if (threadAxisCenter2d(sourceBodyId, pln, c2)) {
            const gp_Ax3& ax = pln.Position();
            const gp_Pnt& O = ax.Location();
            const gp_Dir Xd = ax.XDirection();
            const gp_Dir Yd = ax.YDirection();
            gp_Pnt p(O.X() + Xd.X() * c2.x + Yd.X() * c2.y,
                     O.Y() + Xd.Y() * c2.x + Yd.Y() * c2.y,
                     O.Z() + Xd.Z() * c2.x + Yd.Z() * c2.y);
            pln = gp_Pln(gp_Ax3(p, ax.Direction(), Xd));
            faceCenterAnchored = true;
        }
    }

    // Re-anchor the plane ORIGIN to the face's natural centre. A boolean-
    // generated plane's origin is arbitrary, and everything hangs off it:
    // typed coordinates, the snap-to-grid lattice, the drawn face grid. If
    // the face boundary is dominated by concentric circular edges (a rod
    // cap — including a threaded one, whose crest arcs survive the groove
    // runout — or an annulus), their shared centre is the body axis: put
    // (0,0) there. Faces without a dominant circle keep the surface origin.
    if (!faceCenterAnchored) {
        const gp_Ax3& ax = pln.Position();
        const gp_Dir n = ax.Direction();
        struct Cluster { gp_Pnt center; double sweep = 0.0; };
        std::vector<Cluster> clusters;
        for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
            BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
            if (c.GetType() != GeomAbs_Circle) continue;
            gp_Circ circ = c.Circle();
            // In-plane circles only: axis parallel to the sketch normal and
            // centre on the plane.
            if (std::abs(circ.Axis().Direction().Dot(n)) < 0.9999) continue;
            gp_Pnt cc = circ.Location();
            gp_Vec toC(ax.Location(), cc);
            if (std::abs(toC.Dot(gp_Vec(n))) > 1e-3) continue;
            double sweep = std::abs(c.LastParameter() - c.FirstParameter());
            bool merged = false;
            for (auto& cl : clusters) {
                if (cl.center.Distance(cc) < 1e-4) {
                    cl.sweep += sweep;
                    merged = true;
                    break;
                }
            }
            if (!merged) clusters.push_back({cc, sweep});
        }
        const Cluster* best = nullptr;
        for (const auto& cl : clusters)
            if (!best || cl.sweep > best->sweep) best = &cl;
        if (best && best->sweep >= M_PI) {
            pln = gp_Pln(gp_Ax3(best->center, n, ax.XDirection()));
            faceCenterAnchored = true;
        }

        // Swept / lofted caps defeat the analytic scan: every boundary edge
        // is a BSPLINE even when geometrically circular (probe_capface: a
        // swept-thread rod's top face is 4 BSplines with the plane origin
        // 0.64 mm off-axis). Fit a circle to the OUTER wire instead, then
        // TRIM to the outermost band — the crest arcs lie exactly on the
        // body radius while the groove runout dips inward, so the trimmed
        // refit converges on the true axis.
        if (!faceCenterAnchored) {
            const gp_Pnt O = ax.Location();
            const gp_Dir Xd = ax.XDirection();
            const gp_Dir Yd = ax.YDirection();
            std::vector<std::pair<double, double>> pts;
            try {
                TopoDS_Wire ow = BRepTools::OuterWire(face);
                if (!ow.IsNull()) {
                    for (TopExp_Explorer ex(ow, TopAbs_EDGE); ex.More();
                         ex.Next()) {
                        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
                        const int N = 40;
                        for (int i = 0; i <= N; ++i) {
                            double u = c.FirstParameter() +
                                       (c.LastParameter() -
                                        c.FirstParameter()) * i / N;
                            gp_Pnt p = c.Value(u);
                            gp_Vec v(O, p);
                            pts.emplace_back(v.Dot(gp_Vec(Xd)),
                                             v.Dot(gp_Vec(Yd)));
                        }
                    }
                }
            } catch (...) { pts.clear(); }
            // Algebraic (Kasa) circle fit: x²+y² = 2ax + 2by + c.
            auto fit = [](const std::vector<std::pair<double, double>>& q,
                          double& a, double& b, double& r) -> bool {
                if (q.size() < 8) return false;
                double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
                double sz = 0, sxz = 0, syz = 0;
                const double m = static_cast<double>(q.size());
                for (const auto& p : q) {
                    const double x = p.first, y = p.second;
                    const double z = x * x + y * y;
                    sx += x; sy += y; sxx += x * x; syy += y * y;
                    sxy += x * y; sz += z; sxz += x * z; syz += y * z;
                }
                // Solve [sxx sxy sx; sxy syy sy; sx sy m]·[A B C]ᵀ =
                // [sxz syz sz]ᵀ with A=2a, B=2b, C=r²−a²−b².
                const double d = sxx * (syy * m - sy * sy) -
                                 sxy * (sxy * m - sy * sx) +
                                 sx * (sxy * sy - syy * sx);
                if (std::abs(d) < 1e-12) return false;
                const double A = (sxz * (syy * m - sy * sy) -
                                  sxy * (syz * m - sy * sz) +
                                  sx * (syz * sy - syy * sz)) / d;
                const double B = (sxx * (syz * m - sz * sy) -
                                  sxz * (sxy * m - sy * sx) +
                                  sx * (sxy * sz - syz * sx)) / d;
                const double C = (sxx * (syy * sz - sy * syz) -
                                  sxy * (sxy * sz - sx * syz) +
                                  sxz * (sxy * sy - syy * sx)) / d;
                a = A * 0.5; b = B * 0.5;
                const double r2 = C + a * a + b * b;
                if (r2 <= 0.0) return false;
                r = std::sqrt(r2);
                return true;
            };
            // RANSAC over LOCAL sample triples. Least-squares-then-trim
            // converges to the wrong centre here (verified in simulation:
            // the one-sided groove dip biases the initial fit, and trimming
            // then keeps the wrong band, landing 0.6 mm off with a
            // plausible residual). But the boundary CONTAINS exact circular
            // arcs — the crest flat crosses the top plane as a true arc of
            // the body radius (and the root flat as a concentric one) — so
            // circles through nearby sample triples hit the axis exactly.
            // Best-inlier circle wins at a TIGHT tolerance (the samples are
            // exact BRep evaluations); Kasa-refit the inliers. Straight
            // edges produce near-infinite radii — capped against the
            // boundary size — and sloppy fits die on the tight RMS gate.
            const size_t np = pts.size();
            if (np >= 24) {
                double xLo = 1e300, xHi = -1e300, yLo = 1e300, yHi = -1e300;
                for (const auto& p : pts) {
                    xLo = std::min(xLo, p.first);  xHi = std::max(xHi, p.first);
                    yLo = std::min(yLo, p.second); yHi = std::max(yHi, p.second);
                }
                const double diag = std::hypot(xHi - xLo, yHi - yLo);
                const double tol = 1e-3;
                size_t bestInl = 0;
                double bx = 0, by = 0, br = 0;
                for (size_t k : {size_t(2), size_t(5), size_t(10)}) {
                    for (size_t i = 0; i < np; ++i) {
                        const auto& p1 = pts[i];
                        const auto& p2 = pts[(i + k) % np];
                        const auto& p3 = pts[(i + 2 * k) % np];
                        const double d =
                            2.0 * (p1.first * (p2.second - p3.second) +
                                   p2.first * (p3.second - p1.second) +
                                   p3.first * (p1.second - p2.second));
                        if (std::abs(d) < 1e-12) continue;
                        const double z1 = p1.first * p1.first +
                                          p1.second * p1.second;
                        const double z2 = p2.first * p2.first +
                                          p2.second * p2.second;
                        const double z3 = p3.first * p3.first +
                                          p3.second * p3.second;
                        const double ux = (z1 * (p2.second - p3.second) +
                                           z2 * (p3.second - p1.second) +
                                           z3 * (p1.second - p2.second)) / d;
                        const double uy = (z1 * (p3.first - p2.first) +
                                           z2 * (p1.first - p3.first) +
                                           z3 * (p2.first - p1.first)) / d;
                        const double r = std::hypot(p1.first - ux,
                                                    p1.second - uy);
                        if (r < 0.1 || r > 2.0 * diag) continue;
                        // Only ENCLOSING circles qualify — no boundary
                        // sample may lie outside. Steve's real cap carries
                        // TWO exact arcs: the crest arc on the true axis
                        // AND a sweep-construction arc (r 7.38, centred
                        // 0.64 mm off-axis at the surface origin) that WON
                        // by 44 inliers to 43, anchoring the grid off-
                        // kilter. The crest circle encloses every sample;
                        // the impostor leaves the whole crest band outside.
                        size_t inl = 0;
                        bool enclosing = true;
                        for (const auto& p : pts) {
                            const double d2 = std::hypot(p.first - ux,
                                                         p.second - uy);
                            if (std::abs(d2 - r) < tol) ++inl;
                            if (d2 > r + 0.02) { enclosing = false; break; }
                        }
                        if (!enclosing) continue;
                        if (inl > bestInl) {
                            bestInl = inl; bx = ux; by = uy; br = r;
                        }
                    }
                }
                // The winning circle must be a DOMINANT boundary feature,
                // not an incidental one — a rounded-rectangle face's corner
                // fillet is a perfect exact arc (12% of samples) whose
                // centre is NOT where anyone wants the origin. The threaded
                // cap's crest arc carries ~26% of samples; a plain circular
                // boundary carries ~all of them.
                if (bestInl >= 16 && bestInl * 5 >= np) {
                    std::vector<std::pair<double, double>> inliers;
                    for (const auto& p : pts)
                        if (std::abs(std::hypot(p.first - bx,
                                                p.second - by) - br) < tol)
                            inliers.push_back(p);
                    double fa, fb, fr;
                    if (fit(inliers, fa, fb, fr)) {
                        double rms = 0.0;
                        for (const auto& p : inliers) {
                            const double dr = std::hypot(p.first - fa,
                                                         p.second - fb) - fr;
                            rms += dr * dr;
                        }
                        rms = std::sqrt(rms / inliers.size());
                        if (rms <= 1e-3) {
                            gp_Pnt c3d(O.X() + Xd.X() * fa + Yd.X() * fb,
                                       O.Y() + Xd.Y() * fa + Yd.Y() * fb,
                                       O.Z() + Xd.Z() * fa + Yd.Z() * fb);
                            pln = gp_Pln(gp_Ax3(c3d, n, Xd));
                            faceCenterAnchored = true;
                        }
                    }
                }
            }
        }
    }

    m_activeSketch = std::make_shared<Sketch>();
    m_sketchSolver = std::make_unique<SketchSolver>();
    m_activeSketchId = -1;

    // Remember which body this face belongs to so a later Subtract (and other
    // body-relative ops) know what to cut from. Every face-sketch entry point
    // routes through here, so setting it here keeps the source body consistent.
    //
    // EXCEPT on a mesh body, which is a REFERENCE body: an imported STL is a
    // tessellation, not a modelled solid. You sketch on it to trace and snap
    // against real geometry — that still works, the face references gathered
    // below are what provide it — but it is not a modelling host. It has no
    // analytic topology to cut into, and it can never be re-derived, so a
    // parametric link to it is a promise we cannot keep.
    //
    // Binding to it made every body-relative op aim at the mesh: drawing a
    // profile on an STL face and extruding it pushed/pulled INTO the STL
    // instead of making a new body (Steve, 2026-08-03). Left free-floating, the
    // extrude takes the targetBody < 0 path and creates a new body, which is
    // what tracing a reference part is for.
    const bool meshHost = sourceBodyId >= 0 && m_document &&
                          m_document->isBodyMesh(sourceBodyId);
    m_activeSketch->setSourceBody(meshHost ? -1 : sourceBodyId);

    {
        // `pln` was computed above and already handles planar faces whose surface
        // isn't a literal Geom_Plane (scaled-frustum side faces, etc.).
        // Honour the face's topological orientation: a REVERSED face's outward
        // normal is opposite to its surface normal, so flip the sketch plane so
        // the camera lands on the visible side of the face.
        if (face.Orientation() == TopAbs_REVERSED) {
            gp_Ax3 ax = pln.Position();
            ax.ZReverse();
            pln = gp_Pln(ax);
        }
        // STEP-imported faces sometimes carry an orientation flag that doesn't
        // match the geometric outward direction, so the orientation check
        // alone isn't enough — we end up with a sketch plane pointing INTO
        // the body and push/pull goes the wrong way. Verify by probing the
        // body's solid classifier on BOTH sides of the face: one direction
        // should be OUT and the other IN. If the directions are reversed
        // (forward is IN, opposite is OUT), flip the plane. We probe both
        // sides at a generous offset (1 mm) so tessellation slack near the
        // surface doesn't confuse the classifier.
        if (sourceBodyId >= 0) {
            try {
                const TopoDS_Shape& body = m_document->getBody(sourceBodyId);
                if (!body.IsNull()) {
                    Bnd_Box bb;
                    BRepBndLib::Add(face, bb);
                    if (!bb.IsVoid()) {
                        double xmin, ymin, zmin, xmax, ymax, zmax;
                        bb.Get(xmin, ymin, zmin, xmax, ymax, zmax);
                        gp_Pnt c((xmin + xmax) * 0.5,
                                 (ymin + ymax) * 0.5,
                                 (zmin + zmax) * 0.5);
                        gp_Dir nd = pln.Position().Direction();
                        const double eps = 1.0; // mm
                        gp_Pnt fwd(c.X() + nd.X() * eps,
                                   c.Y() + nd.Y() * eps,
                                   c.Z() + nd.Z() * eps);
                        gp_Pnt back(c.X() - nd.X() * eps,
                                    c.Y() - nd.Y() * eps,
                                    c.Z() - nd.Z() * eps);
                        BRepClass3d_SolidClassifier fwdCls(body, fwd,  1e-6);
                        BRepClass3d_SolidClassifier backCls(body, back, 1e-6);
                        bool fwdIsIn  = (fwdCls.State()  == TopAbs_IN);
                        bool backIsIn = (backCls.State() == TopAbs_IN);
                        // Only flip if we have a clear "forward is inside,
                        // opposite is outside" disagreement. Mixed / ambiguous
                        // states (ON / UNKNOWN) leave the existing direction
                        // alone so we don't double-flip a face that was
                        // already correctly oriented by the topology check.
                        if (fwdIsIn && !backIsIn) {
                            gp_Ax3 ax = pln.Position();
                            ax.ZReverse();
                            pln = gp_Pln(ax);
                        }
                    }
                }
            } catch (...) {}
        }
        m_activeSketch->setPlane(pln);
        m_activeSketch->setSourceFace(face);
        // The plane origin IS the true centre when anchored — publish it as
        // the sketch's centre snap (outranks + suppresses the area
        // centroid; see SketchTool).
        if (faceCenterAnchored)
            m_activeSketch->setCenterPoint(glm::vec2(0.0f));
        std::fprintf(stderr, "[Sketch] on-face: body=%d anchored=%d "
                             "origin=(%.4f, %.4f, %.4f)\n",
                     sourceBodyId, faceCenterAnchored ? 1 : 0,
                     pln.Location().X(), pln.Location().Y(),
                     pln.Location().Z());

        // Walk the face's vertices and edges, project them onto the sketch
        // plane in 2D, and stash them on the Sketch as reference geometry.
        // The inference snap reads these so the cursor can land on the host
        // face's existing corners / edge midpoints / straight edges even
        // before any sketch elements are drawn.
        {
            Sketch::FaceReference refs;
            const gp_Ax3& ax3 = pln.Position();
            gp_Pnt O = ax3.Location();
            gp_Dir Xd = ax3.XDirection();
            gp_Dir Yd = ax3.YDirection();
            gp_Dir Nd = ax3.Direction();
            auto project = [&](const gp_Pnt& p) -> glm::vec2 {
                double dx = p.X() - O.X();
                double dy = p.Y() - O.Y();
                double dz = p.Z() - O.Z();
                double u = dx * Xd.X() + dy * Xd.Y() + dz * Xd.Z();
                double v = dx * Yd.X() + dy * Yd.Y() + dz * Yd.Z();
                return glm::vec2(static_cast<float>(u), static_cast<float>(v));
            };
            // Signed distance of a 3D point from the sketch plane (along normal).
            auto planeDist = [&](const gp_Pnt& p) -> double {
                return (p.X() - O.X()) * Nd.X() + (p.Y() - O.Y()) * Nd.Y() +
                       (p.Z() - O.Z()) * Nd.Z();
            };
            auto dedup = [](std::vector<glm::vec2>& v, glm::vec2 p) {
                for (const auto& q : v) {
                    if (glm::length(q - p) < 1e-4f) return;
                }
                v.push_back(p);
            };
            auto dedupLine = [](std::vector<std::pair<glm::vec2, glm::vec2>>& v,
                                glm::vec2 a, glm::vec2 b) {
                for (const auto& q : v) {
                    if ((glm::length(q.first - a) < 1e-4f && glm::length(q.second - b) < 1e-4f) ||
                        (glm::length(q.first - b) < 1e-4f && glm::length(q.second - a) < 1e-4f))
                        return;
                }
                v.emplace_back(a, b);
            };
            auto dedupCircle = [](std::vector<Sketch::FaceReference::Circle>& v,
                                  const Sketch::FaceReference::Circle& c) {
                for (const auto& q : v) {
                    if (glm::length(q.center - c.center) < 1e-4f &&
                        std::abs(q.radius - c.radius) < 1e-4f)
                        return;
                }
                v.push_back(c);
            };

            // Project one face's vertices and edges into the sketch plane and
            // append them to the reference set. Reused for the host face and
            // each neighbouring face so the cursor can snap to the body's
            // nearby corners / edges (Fusion-style projected geometry).
            auto processFace = [&](const TopoDS_Face& f3d) {
                // Vertices — the face's corner points. IN-PLANE ONLY: a
                // neighbouring face's out-of-plane vertex (e.g. where two
                // fillets meet ABOVE the sketch face) projected straight down
                // becomes an invisible magnet inside the sketch area — it
                // hover-charges and pulls clicks off the grid even though the
                // cursor never went near the real 3D edge (Steve's report).
                const double kPlaneTol = 1e-3;
                for (TopExp_Explorer ex(f3d, TopAbs_VERTEX); ex.More(); ex.Next()) {
                    gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(ex.Current()));
                    if (std::abs(planeDist(p)) > kPlaneTol) continue;
                    dedup(refs.points, project(p));
                }
                // Edges — straight edges become reference lines (+ midpoint);
                // in-plane circles become true circle refs for continuous
                // perimeter snapping; everything else is sampled to points.
                for (TopExp_Explorer ex(f3d, TopAbs_EDGE); ex.More(); ex.Next()) {
                    TopoDS_Edge edge = TopoDS::Edge(ex.Current());
                    BRepAdaptor_Curve curve(edge);
                    double f = curve.FirstParameter();
                    double l = curve.LastParameter();
                    gp_Pnt pStart, pEnd, pMid;
                    curve.D0(f, pStart);
                    curve.D0(l, pEnd);
                    curve.D0(0.5 * (f + l), pMid);
                    // IN-PLANE ONLY (see the vertex gate above): out-of-plane
                    // neighbour edges (walls, fillets rising off the face)
                    // projected down are phantom snap geometry, not something
                    // the user can see or aim at. Shared edges, the host
                    // perimeter and in-plane rims all pass untouched.
                    if (std::abs(planeDist(pStart)) > kPlaneTol ||
                        std::abs(planeDist(pEnd))   > kPlaneTol ||
                        std::abs(planeDist(pMid))   > kPlaneTol)
                        continue;
                    glm::vec2 a = project(pStart);
                    glm::vec2 b = project(pEnd);
                    if (curve.GetType() == GeomAbs_Line) {
                        dedupLine(refs.lines, a, b);
                        dedup(refs.points, 0.5f * (a + b)); // midpoint
                    } else if (curve.GetType() == GeomAbs_Circle) {
                        gp_Circ circ = curve.Circle();
                        dedup(refs.points, project(circ.Location()));
                        // A circle lies *in* the sketch plane when its axis is
                        // parallel to the plane normal and its centre sits on
                        // the plane. Only then does it project to a true circle
                        // we can snap to continuously; otherwise it foreshortens
                        // to an ellipse, so we fall back to sampled points.
                        bool inPlane =
                            std::abs(circ.Axis().Direction().Dot(Nd)) > 0.999 &&
                            std::abs(planeDist(circ.Location())) < 1e-4;
                        if (inPlane) {
                            Sketch::FaceReference::Circle fc;
                            fc.center = project(circ.Location());
                            fc.radius = static_cast<float>(circ.Radius());
                            const float TWO_PI = 2.0f * static_cast<float>(M_PI);
                            if (std::abs((l - f) - 2.0 * M_PI) < 1e-6) {
                                fc.startAngle = 0.0f;
                                fc.sweep = TWO_PI;
                            } else {
                                // Build the CCW span (in projected 2D) bounded by
                                // the edge's start/end and verified to pass
                                // through its midpoint param.
                                gp_Pnt pm;
                                curve.D0(0.5 * (f + l), pm);
                                glm::vec2 m2 = project(pm);
                                float a0 = std::atan2(a.y - fc.center.y, a.x - fc.center.x);
                                float a1 = std::atan2(b.y - fc.center.y, b.x - fc.center.x);
                                float am = std::atan2(m2.y - fc.center.y, m2.x - fc.center.x);
                                auto ccw = [&](float from, float to) {
                                    float d = to - from;
                                    while (d < 0.0f) d += TWO_PI;
                                    while (d >= TWO_PI) d -= TWO_PI;
                                    return d;
                                };
                                if (ccw(a0, am) <= ccw(a0, a1)) {
                                    fc.startAngle = a0;
                                    fc.sweep = ccw(a0, a1);
                                } else {
                                    fc.startAngle = a1;
                                    fc.sweep = ccw(a1, a0);
                                }
                            }
                            dedupCircle(refs.circles, fc);
                        } else {
                            const int samples = 8;
                            for (int i = 1; i < samples; ++i) {
                                double t = f + (l - f) * (double(i) / samples);
                                gp_Pnt p;
                                curve.D0(t, p);
                                dedup(refs.points, project(p));
                            }
                        }
                    } else if (curve.GetType() == GeomAbs_Ellipse) {
                        // Ellipse also has a centre; treat it as a snap target.
                        gp_Elips el = curve.Ellipse();
                        dedup(refs.points, project(el.Location()));
                        const int samples = 8;
                        for (int i = 1; i < samples; ++i) {
                            double t = f + (l - f) * (double(i) / samples);
                            gp_Pnt p;
                            curve.D0(t, p);
                            dedup(refs.points, project(p));
                        }
                    } else {
                        // Splines / hyperbolas / etc. — just sample perimeter
                        // points so something snappable exists along the curve.
                        const int samples = 8;
                        for (int i = 1; i < samples; ++i) {
                            double t = f + (l - f) * (double(i) / samples);
                            gp_Pnt p;
                            curve.D0(t, p);
                            dedup(refs.points, project(p));
                        }
                    }
                }
            };

            // The originating face.
            processFace(face);

            // The re-anchored plane origin IS the face's true centre (the
            // circular-boundary axis) — make it a snappable point so
            // "start the circle at the centre" is one click.
            if (faceCenterAnchored) dedup(refs.points, glm::vec2(0.0f));

            // Neighbouring faces: any face sharing one of the host face's edges.
            // Their projected geometry (side-wall edges, bordering faces) becomes
            // snappable too. One-time walk on sketch entry, so cost is fine.
            if (sourceBodyId >= 0) {
                try {
                    const TopoDS_Shape& body = m_document->getBody(sourceBodyId);
                    if (!body.IsNull()) {
                        TopTools_IndexedDataMapOfShapeListOfShape edgeFaceMap;
                        TopExp::MapShapesAndAncestors(body, TopAbs_EDGE,
                                                      TopAbs_FACE, edgeFaceMap);
                        TopTools_MapOfShape seenFaces;
                        for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
                            const TopoDS_Shape& he = ex.Current();
                            if (!edgeFaceMap.Contains(he)) continue;
                            // Range-based loop instead of
                            // TopTools_ListIteratorOfListOfShape — vcpkg OCCT
                            // drops that standalone iterator header on Windows.
                            for (const TopoDS_Shape& nf : edgeFaceMap.FindFromKey(he)) {
                                if (nf.IsSame(face)) continue;
                                if (!seenFaces.Add(nf)) continue; // already projected
                                processFace(TopoDS::Face(nf));
                            }
                        }
                    }
                } catch (...) {}
            }

            m_activeSketch->setFaceReferences(std::move(refs));
        }
    }

    m_sketchTool->setSketch(m_activeSketch.get());
    m_sketchTool->setSolver(m_sketchSolver.get());
    m_sketchSolver->setSketch(m_activeSketch.get());
    m_sketchTool->setMode(SketchToolMode::Line);
    m_inSketchMode = true;
    m_sketchEntryHistoryStep = m_history ? m_history->currentStep() : -1;
    if (m_history) m_history->setUndoFloor(m_sketchEntryHistoryStep);  // no undo past sketch entry
    m_toolbar->setSketchMode(true);
    // Fresh sketch session: drop any stale Dimension edit-popup state left
    // over from a previous session (e.g. Esc/undo left m_dimEditingId set
    // without the popup ever closing) so it can't reopen against a
    // constraint id from a different sketch.
    m_dimOpenEditRequested = false;
    m_dimEditingId = -1;
    m_dimPopupConsumedEsc = false;
    alignCameraToActiveSketch();
}

void Application::applySketchConstraint(ConstraintType type) {
    if (!m_inSketchMode || !m_activeSketch || !m_sketchTool || !m_sketchSolver) return;

    const auto& selPts = m_sketchTool->getSelectedPoints();
    const auto& selLns = m_sketchTool->getSelectedLines();

    auto pushConstraint = [&](ConstraintType t, int a, int b = -1,
                              double v = 0.0, double vy = 0.0) {
        Constraint c;
        c.id = 0; // sketch assigns
        c.type = t;
        c.entityA = a;
        c.entityB = b;
        c.value = v;
        c.valueY = vy;
        c.isSatisfied = false;
        m_activeSketch->addConstraint(c);
    };

    int added = 0;
    // Wrap the whole mutation in recordSketchMutation so the constraint add
    // (+ subsequent solver pass) becomes a single SketchEditOp on the history
    // stack. Ctrl+Z removes the constraint(s) just added. Description is
    // specialised by SketchEditOp::description() reading the constraint diff.
    recordSketchMutation([&]{
    switch (type) {
        case ConstraintType::Horizontal:
        case ConstraintType::Vertical: {
            // Apply to every selected line independently.
            for (int lid : selLns) {
                pushConstraint(type, lid);
                ++added;
            }
            break;
        }
        case ConstraintType::Coincident: {
            // Chain pairs: (p0,p1), (p0,p2), ... so any number of points fuse
            // to the same spot. (Pairwise from the first is cheaper than full
            // mesh and the solver converges to the same result.)
            std::vector<int> v(selPts.begin(), selPts.end());
            for (size_t i = 1; i < v.size(); ++i) {
                pushConstraint(ConstraintType::Coincident, v[0], v[i]);
                ++added;
            }
            break;
        }
        case ConstraintType::Parallel:
        case ConstraintType::Perpendicular: {
            // Each subsequent line gets bound to the first.
            std::vector<int> v(selLns.begin(), selLns.end());
            for (size_t i = 1; i < v.size(); ++i) {
                pushConstraint(type, v[0], v[i]);
                ++added;
            }
            break;
        }
        case ConstraintType::Equal: {
            // Equal binds LINES (equal length) or CIRCLES/ARCS (equal radius),
            // whichever the selection holds — each subsequent entity bound to
            // the first of its kind. A mixed selection constrains lines to
            // lines and curves to curves independently.
            std::vector<int> lns(selLns.begin(), selLns.end());
            for (size_t i = 1; i < lns.size(); ++i) {
                pushConstraint(ConstraintType::Equal, lns[0], lns[i]);
                ++added;
            }
            std::vector<int> curves;
            for (int id : m_sketchTool->getSelectedCircles()) curves.push_back(id);
            for (int id : m_sketchTool->getSelectedArcs())    curves.push_back(id);
            for (size_t i = 1; i < curves.size(); ++i) {
                pushConstraint(ConstraintType::Equal, curves[0], curves[i]);
                ++added;
            }
            break;
        }
        case ConstraintType::Fixed: {
            // Pin each selected point at its CURRENT position.
            for (int pid : selPts) {
                const SketchPoint* pp = m_activeSketch->getPoint(pid);
                if (!pp) continue;
                pushConstraint(ConstraintType::Fixed, pid, -1,
                               static_cast<double>(pp->pos.x),
                               static_cast<double>(pp->pos.y));
                ++added;
            }
            break;
        }
        case ConstraintType::Distance: {
            // Pairwise from the first selected point — initial value is the
            // geometry's current distance, so the constraint isn't immediately
            // destructive (it just locks the present distance in place).
            std::vector<int> v(selPts.begin(), selPts.end());
            for (size_t i = 1; i < v.size(); ++i) {
                const SketchPoint* p0 = m_activeSketch->getPoint(v[0]);
                const SketchPoint* pi = m_activeSketch->getPoint(v[i]);
                if (!p0 || !pi) continue;
                double dist = static_cast<double>(glm::length(p0->pos - pi->pos));
                pushConstraint(ConstraintType::Distance, v[0], v[i], dist);
                ++added;
            }
            break;
        }
        case ConstraintType::Angle: {
            // Each subsequent line bound to the first; initial value is the
            // signed angle the geometry currently makes.
            std::vector<int> v(selLns.begin(), selLns.end());
            if (v.size() < 2) break;
            const auto& lines = m_activeSketch->getLines();
            auto lineDir = [&](int id) {
                for (const auto& l : lines) {
                    if (l.id != id) continue;
                    const SketchPoint* s = m_activeSketch->getPoint(l.startPointId);
                    const SketchPoint* e = m_activeSketch->getPoint(l.endPointId);
                    if (s && e) return e->pos - s->pos;
                    break;
                }
                return glm::vec2(0.0f);
            };
            glm::vec2 dirA = lineDir(v[0]);
            if (glm::length(dirA) < 1e-6f) break;
            float angA = std::atan2(dirA.y, dirA.x);
            for (size_t i = 1; i < v.size(); ++i) {
                glm::vec2 dirB = lineDir(v[i]);
                if (glm::length(dirB) < 1e-6f) continue;
                float angB = std::atan2(dirB.y, dirB.x);
                double signedAngle = static_cast<double>(angB - angA);
                const double TWO_PI = 2.0 * M_PI;
                while (signedAngle >  M_PI) signedAngle -= TWO_PI;
                while (signedAngle < -M_PI) signedAngle += TWO_PI;
                pushConstraint(ConstraintType::Angle, v[0], v[i], signedAngle);
                ++added;
            }
            break;
        }
        case ConstraintType::Radius: {
            // Lock each selected circle / arc at its current radius. Adding
            // the constraint is non-destructive; user edits the value later.
            const auto& selC = m_sketchTool->getSelectedCircles();
            const auto& selA = m_sketchTool->getSelectedArcs();
            for (int cid : selC) {
                for (const auto& circ : m_activeSketch->getCircles()) {
                    if (circ.id == cid) {
                        pushConstraint(ConstraintType::Radius, cid, -1, circ.radius);
                        ++added;
                        break;
                    }
                }
            }
            for (int aid : selA) {
                for (const auto& arc : m_activeSketch->getArcs()) {
                    if (arc.id == aid) {
                        pushConstraint(ConstraintType::Radius, aid, -1, arc.radius);
                        ++added;
                        break;
                    }
                }
            }
            break;
        }
        case ConstraintType::Tangent: {
            // Tangent constraint takes one arc/circle (entityA) and one line
            // (entityB). Pair every selected curve with every selected line.
            const auto& selC = m_sketchTool->getSelectedCircles();
            const auto& selA = m_sketchTool->getSelectedArcs();
            for (int lid : selLns) {
                for (int cid : selC) {
                    pushConstraint(ConstraintType::Tangent, cid, lid);
                    ++added;
                }
                for (int aid : selA) {
                    pushConstraint(ConstraintType::Tangent, aid, lid);
                    ++added;
                }
            }
            break;
        }
        case ConstraintType::Concentric: {
            // Two circles / arcs share a centre. Build a flat list of selected
            // curves and pair the first with each subsequent.
            std::vector<int> curves;
            for (int cid : m_sketchTool->getSelectedCircles()) curves.push_back(cid);
            for (int aid : m_sketchTool->getSelectedArcs())    curves.push_back(aid);
            for (size_t i = 1; i < curves.size(); ++i) {
                pushConstraint(ConstraintType::Concentric, curves[0], curves[i]);
                ++added;
            }
            break;
        }
        default:
            break;
    }

    if (added > 0) {
        m_sketchSolver->setSketch(m_activeSketch.get());
        m_sketchSolver->solve(*m_activeSketch);
    }
    }); // end recordSketchMutation
    if (added > 0) markDirty();
}

void Application::applyPendingDimension() {
    if (!m_inSketchMode || !m_activeSketch || !m_sketchTool) return;
    // Value copy: clearDimState() below resets the tool's live m_dimPending
    // to defaults, so a reference here would read back type=Distance,
    // measured=0.0 by the time the prefill code runs.
    const PendingDimension pd = m_sketchTool->getPendingDimension();
    if (!pd.valid || !m_sketchTool->dimReadyToCommit()) return;

    // Label offset = placed position minus the auto anchor the renderer uses.
    // The renderer resolves anchor per type; store the raw placed position
    // relative to the dimension's geometric anchor (computed the same way the
    // label pass does — see dimensionAutoAnchor in Application_Viewport.cpp).
    glm::vec2 anchor = dimensionAutoAnchor(pd);
    glm::vec2 off = m_sketchTool->getDimLabelPos() - anchor;

    int editId = -1;
    // Popup prefill value: the new-add path prefills from the freshly
    // measured geometry (pd.measured). A dedup MATCH instead keeps whatever
    // value the constraint already carried (see the loop below) — so
    // re-picking an existing dimension just to move its label doesn't
    // clobber a value the user hand-typed earlier — EXCEPT a reversed-order
    // Angle match, which must renegotiate the value for correctness (see
    // the angleSwapped comment below); that path prefills from the fresh
    // pd.measured like a new add.
    double prefillValue = pd.measured;
    recordSketchMutation([&] {
        // Dedup: same type on the same (unordered) entity pair replaces the
        // label placement instead of stacking a duplicate constraint —
        // matches applyDimension's policy. Deliberately does NOT overwrite
        // c.value with pd.measured on a match: pd.measured is a live
        // re-measurement of the CURRENT geometry, which can differ from a
        // value the user already typed into the edit popup, and clobbering
        // it here would silently discard that typed value with no undo step
        // to recover it (bitwise-equal values skip recordSketchMutation's
        // history push). The one exception is angleSwapped below, where
        // keeping the old value would be actively wrong, not just imprecise.
        bool replaced = false;
        // Mirrored DistancePointLine pair-identity: a line-line parallel
        // pick resolves to (point = second line's start endpoint, line =
        // first line). Picking the SAME two lines in the opposite order
        // resolves to the mirror pair (point = first line's start endpoint,
        // line = second line) — a different (point,line) id pair driving
        // the geometrically same gap between the two lines. Recognise that
        // as the same dimension so re-picking in reversed order updates the
        // existing constraint instead of stacking a duplicate.
        auto isMirroredDPL = [&](const Constraint& c) -> bool {
            if (c.type != ConstraintType::DistancePointLine ||
                pd.type != ConstraintType::DistancePointLine) return false;
            const SketchLine* cLine = nullptr;   // line carrying c (c.entityB)
            const SketchLine* pdLine = nullptr;  // line carrying pd (pd.entityB)
            for (const auto& l : m_activeSketch->getLines()) {
                if (l.id == c.entityB) cLine = &l;
                if (l.id == pd.entityB) pdLine = &l;
            }
            if (!cLine || !pdLine) return false;
            bool pdPointOnCLine = (pd.entityA == cLine->startPointId ||
                                    pd.entityA == cLine->endPointId);
            bool cPointOnPdLine = (c.entityA == pdLine->startPointId ||
                                    c.entityA == pdLine->endPointId);
            if (!(pdPointOnCLine && cPointOnPdLine)) return false;
            // Endpoint cross-membership alone isn't enough: a triangle
            // altitude dimensioned from each of two non-parallel sides in
            // turn (P-on-L2 then P-on-L1, sharing no special relationship
            // beyond "each point happens to be an endpoint of the other
            // line") satisfies the membership check above but is NOT the
            // same physical gap — treating it as a match would silently
            // overwrite the first dimension's constraint with the second
            // pick's entity ids while keeping the first's stale value.
            // Mirrored derivation is only actually the same measurement
            // when the two lines are parallel (see resolveDimension's
            // line-line branch, which is the only place that produces this
            // point/line pairing shape in the first place).
            return SketchTool::linesParallelWithinDimTol(*m_activeSketch, cLine->id, pdLine->id);
        };
        for (auto& c : m_activeSketch->getMutableConstraints()) {
            if (c.type != pd.type) continue;
            bool same = (c.entityA == pd.entityA && c.entityB == pd.entityB);
            bool distSwapped = (c.type == ConstraintType::Distance &&
                                 c.entityA == pd.entityB && c.entityB == pd.entityA);
            bool angleSwapped = (c.type == ConstraintType::Angle &&
                                  c.entityA == pd.entityB && c.entityB == pd.entityA);
            // CircleGap is symmetric in its two circles — a reversed re-pick
            // of the same pair is the same dimension.
            bool gapSwapped = (c.type == ConstraintType::CircleGap &&
                                c.entityA == pd.entityB && c.entityB == pd.entityA);
            bool mirroredDPL = !same && !distSwapped && !angleSwapped && isMirroredDPL(c);
            if (same || distSwapped || angleSwapped || gapSwapped || mirroredDPL) {
                // Reversed-order matches (Angle swap, mirrored
                // DistancePointLine) adopt the NEW pick's entity order/ids
                // so the constraint stays consistent with how it was just
                // re-picked.
                if (distSwapped || angleSwapped || gapSwapped || mirroredDPL) {
                    c.entityA = pd.entityA;
                    c.entityB = pd.entityB;
                    // The recorded orientation describes the OLD ordering: a
                    // Distance hint runs entityA->entityB, and a mirrored
                    // DistancePointLine's side is measured against the old
                    // line's direction. Both are backwards once the entities
                    // swap, so clear them and let the solver re-record from
                    // the geometry as it now stands.
                    c.orientX = 0.0;
                    c.orientY = 0.0;
                }
                // Value: left untouched everywhere the stored number stays
                // geometrically correct under the (possibly new) entity
                // order — same-order matches, a swapped Distance/mirrored
                // DPL (both are order-independent magnitudes: a distance or
                // a perpendicular gap reads the same regardless of which
                // point/line ended up as entityA/entityB). Angle is NOT
                // order-independent: c.value is defined as "entityB's angle
                // relative to entityA's", so swapping which line is which
                // without renegotiating the number would have the solver
                // enforce the NEGATED relative angle against the wrong
                // reference line — a silent geometry flip, not just a label
                // move. pd.measured was freshly computed for the NEW order,
                // so it's the only value consistent with the swapped roles.
                if (angleSwapped) c.value = pd.measured;
                c.labelOffX = off.x;
                c.labelOffY = off.y;
                editId = c.id;
                prefillValue = c.value;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            Constraint c{};
            c.type = pd.type;
            c.entityA = pd.entityA;
            c.entityB = pd.entityB;
            c.value = pd.measured;
            c.labelOffX = off.x;
            c.labelOffY = off.y;
            // Dimensions are REFERENCE by default: placing one measures the
            // geometry without moving it. The solver skips it and it costs no
            // degree of freedom, so annotating a sketch can never drag it out
            // of shape or flip it to Over-constrained. The label's edit popup
            // has a "Driving" checkbox to promote it when the user actually
            // wants the number to control the geometry.
            //
            // Only here — Constraint::isDriving defaults to true, so the
            // right-click Add Constraint menu and every geometric constraint
            // are unaffected.
            c.isDriving = false;
            editId = m_activeSketch->addConstraint(c);
        }
        if (m_sketchSolver) {
            m_sketchSolver->setSketch(m_activeSketch.get());
            m_sketchSolver->solve(*m_activeSketch);
        }
    });
    m_sketchTool->clearDimState();

    // Open the existing edit popup, prefilled from prefillValue — the
    // freshly measured geometry for a new dimension, or the KEPT existing
    // value for a dedup match (see the comment above; a match never writes
    // pd.measured into c.value, so the popup must prefill from what's
    // actually stored, not from the re-measurement). Enter drives the
    // geometry, Esc keeps the prefilled value.
    if (editId >= 0) {
        m_dimEditingId = editId;
        // Seed through the SAME helper the click-to-edit path uses. These three
        // lines wrote raw millimetres: placing a dimension under any non-mm unit
        // showed the millimetre number in the popup while the label beside it
        // read the display unit — 29.70 in the field against "O 2.970 cm".
        const auto dimKind = pd.type == ConstraintType::Angle  ? materializr::DimKind::Angle
                           : pd.type == ConstraintType::Radius ? materializr::DimKind::Radius
                                                               : materializr::DimKind::Length;
        bool dimIsArc = false;
        if (dimKind == materializr::DimKind::Radius && m_activeSketch)
            for (const auto& c : m_activeSketch->getConstraints())
                if (c.id == editId) {
                    dimIsArc = materializr::constraintIsArcRadius(*m_activeSketch, c);
                    break;
                }
        materializr::seedDimensionText(m_dimEditingBuf, sizeof(m_dimEditingBuf),
                                       dimKind, dimIsArc, prefillValue);
        m_dimEditingFocus = true;
        m_dimOpenEditRequested = true; // viewport calls OpenPopup("##DimEdit") next frame
        // No m_meshesDirty here: for a new dimension pd.measured is the
        // geometry's CURRENT value (resolveDimension reads it off the live
        // picks) so this commit never moves anything for the solver to
        // re-tessellate — same as applySketchConstraint's Distance/Angle
        // path just above, which doesn't set it either. For a dedup match
        // the value is untouched entirely. The ##DimEdit popup's own commit
        // handler sets m_meshesDirty when a typed value actually changes
        // geometry.
        markDirty();
    }
}

void Application::recordSketchMutation(const std::function<void()>& mutator) {
    if (!m_activeSketch) { mutator(); return; }
    // Signature includes counts AND element IDs so that swaps (trim line→line,
    // trim circle→arc) register as a mutation even though counts may be equal.
    auto signature = [](const Sketch& s) {
        size_t h = 1469598103934665603ull;
        auto mix = [&](size_t v) { h = (h ^ v) * 1099511628211ull; };
        // Hash point positions and circle/arc radii too (quantised to 1e-4 mm)
        // so a pure move/resize — a line length, rectangle W×H, or arc sweep
        // edit that keeps every id and count fixed — still registers as a
        // mutation and gets its own undoable history step.
        auto mixPos = [&](glm::vec2 p) {
            mix(static_cast<size_t>(std::llround(p.x * 1e4)));
            mix(static_cast<size_t>(std::llround(p.y * 1e4)));
        };
        mix(s.getPoints().size());
        for (const auto& p : s.getPoints()) { mix(static_cast<size_t>(p.id)); mixPos(p.pos); }
        mix(s.getLines().size());
        for (const auto& l : s.getLines()) mix(static_cast<size_t>(l.id));
        mix(s.getCircles().size());
        for (const auto& c : s.getCircles()) {
            mix(static_cast<size_t>(c.id));
            mix(static_cast<size_t>(std::llround(c.radius * 1e4)));
        }
        mix(s.getArcs().size());
        for (const auto& a : s.getArcs()) {
            mix(static_cast<size_t>(a.id));
            mix(static_cast<size_t>(std::llround(a.radius * 1e4)));
        }
        mix(s.getSplines().size());
        for (const auto& sp : s.getSplines()) mix(static_cast<size_t>(sp.id));
        mix(s.getPolygons().size());
        for (const auto& p : s.getPolygons()) mix(static_cast<size_t>(p.id));
        // Constraints too — including their values so an edit (not just an
        // add / remove) registers as a mutation and pushes a history step.
        mix(s.getConstraints().size());
        for (const auto& c : s.getConstraints()) {
            mix(static_cast<size_t>(c.id));
            mix(static_cast<size_t>(c.type));
            mix(static_cast<size_t>(c.entityA));
            mix(static_cast<size_t>(c.entityB));
            size_t vb; std::memcpy(&vb, &c.value, sizeof(vb));
            mix(vb);
            std::memcpy(&vb, &c.valueY, sizeof(vb));
            mix(vb);
            // Label offsets too: a dedup-replace that only re-places a
            // label (value bitwise-equal) must still register as a
            // mutation, or the re-placement gets no undo step.
            std::memcpy(&vb, &c.labelOffX, sizeof(vb));
            mix(vb);
            std::memcpy(&vb, &c.labelOffY, sizeof(vb));
            mix(vb);
            // Driving/reference too — promoting a dimension changes no
            // number, so without this the toggle hashes identically and
            // recordSketchMutation skips the history push entirely.
            mix(static_cast<size_t>(c.isDriving ? 1 : 0));
        }
        return h;
    };
    size_t beforeSig = signature(*m_activeSketch);
    auto before = std::make_shared<Sketch>(*m_activeSketch);
    // Fold a deferred anchor snapshot (from the first click of a line chain) in
    // as the "before", so the anchor + first segment undo as ONE step. Drop it
    // if it belongs to a different sketch (stale).
    if (m_deferredSketchBefore) {
        if (m_deferredSketchOwner == m_activeSketch.get()) {
            before = m_deferredSketchBefore;
            beforeSig = m_deferredSketchBeforeSig;
        } else {
            m_deferredSketchBefore.reset();
            m_deferredSketchOwner = nullptr;
        }
    }
    mutator();
    // A line-chain anchor click (only the start point placed) shouldn't be its
    // own undo step: hold its before-snapshot and let the first segment absorb
    // it. Esc (onCancel removes the anchor) and Enter (leaves an orphan) fall
    // through the paths below and resolve correctly.
    if (m_sketchTool && m_sketchTool->isChainAnchorPending()) {
        if (!m_deferredSketchBefore) {
            m_deferredSketchBefore = before;
            m_deferredSketchBeforeSig = beforeSig;
            m_deferredSketchOwner = m_activeSketch.get();
        }
        return;
    }
    m_deferredSketchBefore.reset();
    m_deferredSketchOwner = nullptr;
    size_t afterSig = signature(*m_activeSketch);
    if (afterSig == beforeSig) return; // nothing structural changed → no history step
    auto after = std::make_shared<Sketch>(*m_activeSketch);
    auto op = std::make_unique<SketchEditOp>(m_activeSketch, std::move(before), std::move(after));
    // Stamp the sketch id now, while m_activeSketch is definitively in the
    // document — so a save later can serialise the snapshots even if the live
    // pointer has since been replaced (which silently froze delete steps).
    op->setSketchId(m_document->findSketchId(m_activeSketch.get()));
    m_history->pushExecuted(std::move(op));
}

void Application::sketchChainBack() {
    if (!m_inSketchMode || !m_sketchTool) return;
    SketchToolMode m = m_sketchTool->getMode();
    if (m == SketchToolMode::Line) {
        if (m_sketchTool->lineSegmentCount() < 1) return;
        // dropLineChainTail removes EXACTLY the tracked last segment; wrapping
        // it makes the removal one undoable step. (Don't prune here — backing
        // to the lone start vertex should KEEP it as the chain's live anchor.)
        recordSketchMutation([&]{ m_sketchTool->dropLineChainTail(); });
    } else if (m == SketchToolMode::Spline) {
        // Spline control points live in the tool until Confirm (no per-point
        // history step), so just pop the last one.
        m_sketchTool->removeLastSplinePoint();
    } else {
        return;
    }
    m_meshesDirty = true;
}

void Application::sketchChainCancel() {
    if (!m_inSketchMode || !m_sketchTool) return;
    if (m_sketchTool->getMode() == SketchToolMode::Line) {
        // Peel every segment of THIS chain in one undo step (each dropLineChainTail
        // removes the current tail; it returns false once only the start is left).
        recordSketchMutation([&]{ while (m_sketchTool->dropLineChainTail()) {} });
    }
    // Reset placement (drops the spline points / the lone start vertex). Any
    // now-disconnected start vertex is swept below.
    m_sketchTool->onCancel();
    if (m_activeSketch) m_activeSketch->pruneOrphanPoints();
    m_meshesDirty = true;
}

void Application::deleteSelectedSketchElements() {
    if (!m_inSketchMode || !m_activeSketch || !m_sketchTool) return;
    // Delete the SketchTool's element selection (points + lines) if any,
    // wrapped in recordSketchMutation so Ctrl+Z / Undo brings them back.
    const auto pts = m_sketchTool->getSelectedPoints();
    const auto lns = m_sketchTool->getSelectedLines();
    if (pts.empty() && lns.empty()) return;
    recordSketchMutation([&]{
        for (int lid : lns) m_activeSketch->removeElement(lid);
        for (int pid : pts) m_activeSketch->removeElement(pid);
        // Deleting a line leaves its two endpoints behind (they weren't in
        // the selection) — sweep up the now-unreferenced points so no orphan
        // vertices linger.
        m_activeSketch->pruneOrphanPoints();
    });
    m_sketchTool->clearElementSelection();
    markDirty();
}

void Application::frameSelection() {
    Bnd_Box bb;
    // Selection first.
    if (m_selection) {
        for (const auto& e : m_selection->getSelection()) {
            if (e.bodyId < 0) continue;
            try {
                const TopoDS_Shape& s = m_document->getBody(e.bodyId);
                if (!s.IsNull()) BRepBndLib::AddOptimal(s, bb,
                                                        Standard_False, Standard_False);
            } catch (...) {}
        }
    }
    // Fall back to all visible bodies.
    if (bb.IsVoid()) {
        for (int id : m_document->getAllBodyIds()) {
            if (!m_document->isBodyVisible(id)) continue;
            try {
                const TopoDS_Shape& s = m_document->getBody(id);
                if (!s.IsNull()) BRepBndLib::AddOptimal(s, bb,
                                                        Standard_False, Standard_False);
            } catch (...) {}
        }
    }
    if (!bb.IsVoid()) {
        Standard_Real x0,y0,z0,x1,y1,z1;
        bb.Get(x0,y0,z0,x1,y1,z1);
        m_viewport->getCamera().zoomToFit(
            glm::vec3((float)x0,(float)y0,(float)z0),
            glm::vec3((float)x1,(float)y1,(float)z1));
    }
}

bool Application::threadAxisCenter2d(int bodyId, const gp_Pln& pln,
                                     glm::vec2& out) const {
    if (!m_history) return false;
    // bodyId < 0 = "any threaded body": a sketch whose body link was severed
    // (saved with sourceBody=-1) still deserves its centre — among all
    // thread axes piercing the plane, the one closest to the plane origin
    // is the host (the sketch was created ON that face).
    const gp_Ax3& ax = pln.Position();
    const gp_Dir n = ax.Direction();
    bool found = false;
    float bestD = 1e30f;
    for (int i = 0; i <= m_history->currentStep(); ++i) {
        const Operation* s = m_history->getStep(i);
        if (!s || !s->isEnabled() || s->kind() != Operation::Kind::Thread) continue;
        const ThreadOp* th = dynamic_cast<const ThreadOp*>(s);
        if (!th) continue;
        if (bodyId >= 0 && th->getBodyId() != bodyId) continue;
        const gp_Ax2& tax = th->getAxis();
        if (std::abs(tax.Direction().Dot(n)) < 0.9999)
            continue; // the axis must pierce the plane squarely (caps)
        const double denom = gp_Vec(tax.Direction()).Dot(gp_Vec(n));
        if (std::abs(denom) < 1e-9) continue;
        const gp_Pnt& O = ax.Location();
        const gp_Pnt& A = tax.Location();
        const double t = gp_Vec(A, O).Dot(gp_Vec(n)) / denom;
        gp_Pnt p(A.X() + tax.Direction().X() * t,
                 A.Y() + tax.Direction().Y() * t,
                 A.Z() + tax.Direction().Z() * t);
        gp_Vec v(O, p);
        glm::vec2 c(static_cast<float>(v.Dot(gp_Vec(ax.XDirection()))),
                    static_cast<float>(v.Dot(gp_Vec(ax.YDirection()))));
        const float d = glm::length(c);
        if (!found || d < bestD) { found = true; bestD = d; out = c; }
        if (bodyId >= 0) break; // explicit body: first matching axis wins
    }
    return found;
}

void Application::editSketch(int sketchId) {
    auto sketch = m_document->getSketch(sketchId);
    if (!sketch) return;

    // For sketches loaded from a previous session, sourceFace isn't part of
    // the project file — re-bind it from the host body before the user
    // starts editing / using sketch regions.
    ensureSketchSourceFace(sketchId);

    m_activeSketch = sketch; // shared ownership - edits go straight to the stored sketch
    m_sketchSolver = std::make_unique<SketchSolver>();
    m_activeSketchId = sketchId;

    // Recompute the host body's TRUE-centre snap for this session. The
    // centre marker (and face references) aren't serialized, so a RE-EDITED
    // sketch otherwise only offers the area-centroid snap — off-axis on a
    // threaded cap, and immune to every fresh-sketch fix ("nothing changed
    // at all": Steve was re-editing an existing sketch). The stored plane
    // must NOT be re-anchored (geometry lives in it); the centre lands at
    // its true 2D coordinates instead of (0,0).
    sketch->clearCenterPoint();
    {
        glm::vec2 c2;
        // sourceBody may legitimately be -1 (severed link, e.g. a pick
        // that failed body attribution) — threadAxisCenter2d then matches
        // any thread axis piercing the plane.
        if (threadAxisCenter2d(sketch->getSourceBody(), sketch->getPlane(),
                               c2)) {
            sketch->setCenterPoint(c2);
            std::fprintf(stderr, "[Sketch] edit %d: body=%d true centre at "
                                 "(%.4f, %.4f)\n",
                         sketchId, sketch->getSourceBody(), c2.x, c2.y);
        } else {
            std::fprintf(stderr, "[Sketch] edit %d: body=%d no thread-axis "
                                 "centre (centroid snap stays)\n",
                         sketchId, sketch->getSourceBody());
        }
    }

    m_sketchTool->setSketch(m_activeSketch.get());
    m_sketchTool->setSolver(m_sketchSolver.get());
    m_sketchSolver->setSketch(m_activeSketch.get());
    // When re-entering an existing sketch the user is much more likely to want
    // to tweak it than draw fresh geometry; start in Select/Move mode so they
    // can immediately click & drag points/lines.
    m_sketchTool->setMode(SketchToolMode::Select);
    m_inSketchMode = true;
    m_sketchEntryHistoryStep = m_history ? m_history->currentStep() : -1;
    if (m_history) m_history->setUndoFloor(m_sketchEntryHistoryStep);  // no undo past sketch entry
    m_toolbar->setSketchMode(true);
    // Fresh sketch session: drop any stale Dimension edit-popup state left
    // over from a previous session (e.g. Esc/undo left m_dimEditingId set
    // without the popup ever closing) so it can't reopen against a
    // constraint id from a different sketch.
    m_dimOpenEditRequested = false;
    m_dimEditingId = -1;
    m_dimPopupConsumedEsc = false;
    m_selection->clear();
    alignCameraToActiveSketch();
}

void Application::extrudeSketchById(int sketchId, ExtrudeMode mode) {
    auto sketch = m_document->getSketch(sketchId);
    if (!sketch) return;
    // Even-odd island compound — multi-shape sketches (SVG, text) extrude
    // every island with its proper holes instead of feeding OCCT one face
    // with disjoint "holes" (which came out non-manifold).
    TopoDS_Shape profile = sketch->buildProfileShape();
    if (profile.IsNull()) {
        std::fprintf(stderr, "Sketch has no closed profile to extrude\n");
        return;
    }

    // A Subtract PREFERS the sketch's host body, but no longer requires one: a
    // free-floating sketch (construction plane, origin plane) or a detached one
    // cuts whichever body the swept profile actually runs into, resolved from
    // the tool volume at commit. This used to bail here with a stderr line, so
    // the Subtract button was a silent no-op on every unattached sketch.
    // Detached sketches deliberately pass -1: the former host is no longer the
    // preferred answer, only a candidate like any other.
    int targetBody = -1;
    if (mode == ExtrudeMode::Subtract && !sketch->isDetachedFromBody())
        targetBody = sketch->getSourceBody();
    beginInteractiveExtrude(profile, mode, targetBody, sketchId);
}

void Application::subtractSketchRegion(int sketchId, int regionIndex) {
    auto sketch = m_document->getSketch(sketchId);
    if (!sketch) return;

    // Preferred host only — see extrudeSketchById: with no attachment the cut
    // target comes from the swept volume at commit, not from the sketch's
    // provenance, and a detached sketch's former host gets no special claim.
    const int targetBody = sketch->isDetachedFromBody() ? -1
                                                       : sketch->getSourceBody();

    auto regions = sketch->buildRegions();
    if (regionIndex < 0 || regionIndex >= static_cast<int>(regions.size())) return;
    const TopoDS_Face& profile = regions[regionIndex].face;
    if (profile.IsNull()) {
        std::fprintf(stderr, "Sketch region has no profile face to subtract\n");
        return;
    }
    beginInteractiveExtrude(profile, ExtrudeMode::Subtract, targetBody, sketchId);
}

void Application::alignCameraToActiveSketch() {
    if (!m_activeSketch || !m_viewport) return;

    const gp_Pln& pln = m_activeSketch->getPlane();
    const gp_Ax3& ax = pln.Position();
    gp_Pnt o = ax.Location();
    gp_Dir n = ax.Direction();
    gp_Dir y = ax.YDirection();

    glm::vec3 planeOrigin(static_cast<float>(o.X()), static_cast<float>(o.Y()), static_cast<float>(o.Z()));
    glm::vec3 normal(static_cast<float>(n.X()), static_cast<float>(n.Y()), static_cast<float>(n.Z()));
    glm::vec3 up(static_cast<float>(y.X()), static_cast<float>(y.Y()), static_cast<float>(y.Z()));

    // Frame the drawn content: union the host face's bbox (when present, for
    // context) with the sketch geometry's own world bounds, then target that
    // union's centre and size the ortho box to its diagonal. Framing the face
    // alone left off-face or off-origin drawings shoved into a corner; framing
    // the plane origin (the no-source-face case) was worse — the origin sits
    // at a corner of the drawing, so the whole sketch landed in one quadrant.
    // Frame a sensible number of DISPLAY UNITS, not a fixed number of
    // millimetres. 40 mm is a reasonable first view in millimetres and an
    // absurd one in feet, where it makes the whole visible sketch 0.13 ft and
    // every number on screen a fraction. Reported from testing: a line most of
    // the way across the screen measured 0.24 ft.
    //
    // Only the FRAMING is unit-aware here. The BASE step is left alone: the
    // tolerance decoupling this comment used to defer has since happened —
    // SketchTool takes the zoom-scaled step for SNAPPING (setGridStep) and the
    // base for TOLERANCES (setToleranceStep) — but the base is still the value
    // the user chose, and framing has no business rewriting it.
    const float unitSpan = static_cast<float>(materializr::toMm(40.0));
    float orthoSize = std::max({20.0f, unitSpan, m_sketchGridStep * 40.0f});
    glm::vec3 lookAt = planeOrigin;
    {
        glm::vec3 bmin( std::numeric_limits<float>::max());
        glm::vec3 bmax(-std::numeric_limits<float>::max());
        bool haveBounds = false;
        if (!m_activeSketch->getSourceFace().IsNull()) {
            try {
                Bnd_Box bb;
                BRepBndLib::Add(m_activeSketch->getSourceFace(), bb);
                if (!bb.IsVoid()) {
                    double x0, y0, z0, x1, y1, z1;
                    bb.Get(x0, y0, z0, x1, y1, z1);
                    bmin = glm::min(bmin, glm::vec3((float)x0, (float)y0, (float)z0));
                    bmax = glm::max(bmax, glm::vec3((float)x1, (float)y1, (float)z1));
                    haveBounds = true;
                }
            } catch (...) {}
        }
        glm::vec3 smin, smax;
        if (m_activeSketch->getWorldBounds(smin, smax)) {
            bmin = glm::min(bmin, smin);
            bmax = glm::max(bmax, smax);
            haveBounds = true;
        }
        if (haveBounds) {
            float diag = 0.5f * glm::length(bmax - bmin);
            if (diag > 1e-3f) orthoSize = diag * 1.2f;
            lookAt = (bmin + bmax) * 0.5f;
        }
    }

    // Snap the look-at point onto the SNAP LATTICE, so the grid drawn in the
    // viewport passes through the positions clicks actually land on. The
    // rounding has to happen in the sketch plane's own (u,v) frame, because
    // that is the frame both halves are defined in: SketchTool::snap() rounds
    // sketch coordinates — measured from the PLANE ORIGIN along XDirection /
    // YDirection — to multiples of the step, and the grid shader lays its
    // lines at multiples of the step measured from this anchor.
    //
    // Rounding world XYZ and projecting onto the plane (what this did) is not
    // the same thing: the projection of a world lattice point is not a lattice
    // point in-plane, so the drawn grid ended up offset from the snap lattice
    // by (plane origin mod step) — an arbitrary fraction of a cell, since a
    // face's plane origin sits at whatever world coords the geometry put it.
    // Measured offsets were 10–50% of a cell. At a 1 mm grid that reads as
    // slightly-fat lines; at 0.1 mm it is most of a cell, i.e. "I can't draw a
    // line on the snap grid" (Steve, 2026-07-31).
    //
    // The anchor doubles as the camera target, and staying on the lattice
    // keeps that just as stable — it moves by at most half a cell.
    {
        gp_Pnt a = Sketch::latticeAnchor(
            pln, gp_Pnt(lookAt.x, lookAt.y, lookAt.z),
            static_cast<double>(std::max(m_sketchGridStep, 0.01f)));
        lookAt = glm::vec3(static_cast<float>(a.X()), static_cast<float>(a.Y()),
                           static_cast<float>(a.Z()));
    }
    m_sketchSnappedAnchor = lookAt;

    Camera& cam = m_viewport->getCamera();
    float standoff = std::max(orthoSize * 4.0f, 10.0f);

    // Pick an "up" direction that keeps the apparent orientation as close to
    // the user's previous view as possible.
    //
    // First try projecting the camera's current up onto the sketch plane;
    // works for vertical / tilted faces where the previous up has a useful
    // component in-plane. For HORIZONTAL faces (top / bottom), the camera's
    // up axis is parallel to the plane normal so the projection is zero —
    // fall back to projecting the camera's horizontal FORWARD direction
    // instead. That preserves turntable continuity: whichever way the user
    // was facing before clicking the face ends up at the top of the new
    // sketch view, instead of jumping to the face's arbitrary internal Y
    // direction (which causes the random 90° rotations on top / bottom).
    glm::vec3 chosenUp = up; // ultimate fallback: face's own Y
    glm::vec3 prevUp = cam.getUp();
    glm::vec3 projUp = prevUp - normal * glm::dot(prevUp, normal);
    if (glm::length(projUp) > 0.1f) {
        chosenUp = glm::normalize(projUp);
    } else {
        glm::vec3 fwd = cam.getTarget() - cam.getPosition();
        glm::vec3 projFwd = fwd - normal * glm::dot(fwd, normal);
        if (glm::length(projFwd) > 0.1f) {
            chosenUp = glm::normalize(projFwd);
        }
    }
    // Snap the chosen up to the nearest 90° of the face's natural axes so the
    // view always lands axis-aligned. Without this the up vector inherits any
    // arbitrary yaw the user had before clicking the face — the sketch comes
    // out "cocked" at whatever orbit angle they happened to be in.
    glm::vec3 faceY = up;
    glm::vec3 faceX = glm::cross(faceY, normal); // in-plane, perpendicular to faceY
    if (glm::length(faceX) > 1e-4f) faceX = glm::normalize(faceX);
    float dY = glm::dot(chosenUp, faceY);
    float dX = glm::dot(chosenUp, faceX);
    if (std::abs(dY) >= std::abs(dX)) {
        chosenUp = (dY >= 0.0f) ? faceY : -faceY;
    } else {
        chosenUp = (dX >= 0.0f) ? faceX : -faceX;
    }

    // Stand off on the face's OUTWARD side. The sketch plane's normal is
    // the underlying surface's — a REVERSED face stores it pointing INTO
    // the body, which used to fling the camera to the far side of the part
    // ("still flat with it, but opposite side" — Steve, on a narrow side
    // face). BRepGProp_Face::Normal applies the orientation flag.
    glm::vec3 standDir = normal;
    if (!m_activeSketch->getSourceFace().IsNull()) {
        try {
            BRepGProp_Face gpFace(TopoDS::Face(m_activeSketch->getSourceFace()));
            double u1, u2, v1, v2;
            gpFace.Bounds(u1, u2, v1, v2);
            gp_Pnt p;
            gp_Vec nv;
            gpFace.Normal(0.5 * (u1 + u2), 0.5 * (v1 + v2), p, nv);
            if (nv.Magnitude() > 1e-9) {
                glm::vec3 outward(static_cast<float>(nv.X()),
                                  static_cast<float>(nv.Y()),
                                  static_cast<float>(nv.Z()));
                if (glm::dot(outward, normal) < 0.0f) standDir = -normal;
            }
        } catch (...) {}
    }

    cam.setTarget(lookAt);
    cam.setPosition(lookAt + standDir * standoff);
    cam.setUp(chosenUp);
    cam.setOrthoSize(orthoSize);
    cam.setOrthographic(true);
}

TopoDS_Face Application::buildSketchProfileFace(const Sketch& sketch) const {
    auto wires = sketch.buildWires();
    if (wires.empty()) return TopoDS_Face();

    // Pick the wire with the largest 3D bbox diagonal as the outer; the rest are holes.
    // This produces a single face with holes so a prism over a "ring" sketch becomes a tube.
    int outerIdx = 0;
    double bestExtent = -1.0;
    std::vector<double> extents(wires.size(), 0.0);
    for (size_t i = 0; i < wires.size(); ++i) {
        Bnd_Box bb;
        BRepBndLib::Add(wires[i], bb);
        if (bb.IsVoid()) continue;
        double xmin, ymin, zmin, xmax, ymax, zmax;
        bb.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        double dx = xmax - xmin, dy = ymax - ymin, dz = zmax - zmin;
        double diag = dx*dx + dy*dy + dz*dz;
        extents[i] = diag;
        if (diag > bestExtent) {
            bestExtent = diag;
            outerIdx = static_cast<int>(i);
        }
    }

    BRepBuilderAPI_MakeFace faceMaker(sketch.getPlane(), wires[outerIdx]);
    if (!faceMaker.IsDone()) return TopoDS_Face();

    for (size_t i = 0; i < wires.size(); ++i) {
        if (static_cast<int>(i) == outerIdx) continue;
        // Reverse inner wire orientation so it acts as a hole
        TopoDS_Wire inner = TopoDS::Wire(wires[i].Reversed());
        faceMaker.Add(inner);
    }
    faceMaker.Build();
    if (!faceMaker.IsDone()) return TopoDS_Face();
    return faceMaker.Face();
}

glm::vec2 Application::screenToSketch(float sx, float sy, float vpW, float vpH) {
    Camera& cam = m_viewport->getCamera();
    glm::mat4 view = cam.getViewMatrix();
    glm::mat4 proj = cam.getProjectionMatrix();
    glm::mat4 invVP = glm::inverse(proj * view);

    // Normalize to [-1,1]
    float nx = (sx / vpW) * 2.0f - 1.0f;
    float ny = 1.0f - (sy / vpH) * 2.0f;

    // Unproject near and far points
    glm::vec4 nearPt = invVP * glm::vec4(nx, ny, -1.0f, 1.0f);
    glm::vec4 farPt = invVP * glm::vec4(nx, ny, 1.0f, 1.0f);
    nearPt /= nearPt.w;
    farPt /= farPt.w;

    glm::vec3 rayOrigin(nearPt);
    glm::vec3 rayDir = glm::normalize(glm::vec3(farPt) - glm::vec3(nearPt));

    // Intersect ray with sketch plane
    const gp_Pln& pln = m_activeSketch->getPlane();
    const gp_Ax3& ax = pln.Position();
    glm::vec3 planeOrigin(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
    glm::vec3 planeNormal(ax.Direction().X(), ax.Direction().Y(), ax.Direction().Z());
    glm::vec3 planeX(ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z());
    glm::vec3 planeY(ax.YDirection().X(), ax.YDirection().Y(), ax.YDirection().Z());

    float denom = glm::dot(rayDir, planeNormal);
    if (std::abs(denom) < 1e-8f) return glm::vec2(0);

    float t = glm::dot(planeOrigin - rayOrigin, planeNormal) / denom;
    glm::vec3 hitPoint = rayOrigin + rayDir * t;

    // Project hit point onto sketch plane's 2D coordinate system
    glm::vec3 local = hitPoint - planeOrigin;
    return glm::vec2(glm::dot(local, planeX), glm::dot(local, planeY));
}


void Application::exitSketchMode() {
    m_inSketchMode = false;
    if (m_history) m_history->clearUndoFloor();  // undo is unrestricted again
    m_toolbar->setSketchMode(false);
    m_sketchTool->setMode(SketchToolMode::None);
    m_sketchTool->setSketch(nullptr);
    m_sketchTool->setSolver(nullptr);
    // Drop any stale Dimension edit-popup state so it can't reopen (with a
    // now-dangling constraint id) on the next sketch session.
    m_dimOpenEditRequested = false;
    m_dimEditingId = -1;
    m_dimPopupConsumedEsc = false;

    // Persist the sketch into the document if it has any geometry. New sketches get added;
    // edits to existing sketches are already reflected via the shared_ptr.
    if (m_activeSketch && m_activeSketch->elementCount() > 0) {
        if (m_activeSketchId < 0) {
            m_activeSketchId = m_document->addSketch(m_activeSketch);
            markDirty();
            std::fprintf(stdout, "Sketch saved (id %d)\n", m_activeSketchId);
        }
    } else if (m_activeSketchId < 0) {
        std::fprintf(stdout, "Sketch discarded (empty)\n");
    } else if (m_activeSketch) {
        // An EXISTING sketch the user emptied during this edit: drop it from
        // the document rather than leaving a blank entry in the Items panel.
        m_document->removeSketch(m_activeSketchId);
        markDirty();
        std::fprintf(stdout, "Sketch %d removed (emptied)\n", m_activeSketchId);
    }

    m_activeSketch.reset();
    m_sketchSolver.reset();
    m_activeSketchId = -1;
    m_meshesDirty = true; // refresh sketch rendering set

    // The sketch is resolved (committed to the document or discarded), so the
    // crash-recovery draft is no longer "unfinished" — drop it. A draft only
    // survives to the next launch when the app exits WITHOUT reaching here.
    materializr::clearSketchDraft();
    m_lastDraftElemCount = -1;

    // Stay where the user is — don't yank them back to the pre-sketch camera.
    // Exiting sketch should feel like leaving ortho-snap mode: the area being
    // looked at remains framed, only the sketch grid disappears. Any orbit
    // they do drops ortho mode and returns to perspective with a level
    // horizon (handled in Camera::orbitLevel).
}

void Application::writeSketchDraftIfDue() {
    // Periodically persist the in-progress sketch so a crash / kill doesn't lose
    // it. Cheap (sketches are tiny); throttled to ~2 s and skipped when nothing
    // changed since the last write.
    if (!m_inSketchMode || !m_activeSketch) return;
    int elems = m_activeSketch->elementCount();
    if (elems == 0) return; // nothing worth recovering yet
    double now = ImGui::GetTime();
    if (elems == m_lastDraftElemCount && now - m_lastDraftWrite < 2.0) return;
    if (now - m_lastDraftWrite < 0.75) return; // hard floor against thrashing
    materializr::writeSketchDraft(*m_activeSketch,
                                  m_activeSketch->getSourceBody(),
                                  m_currentProjectPath);
    m_lastDraftWrite = now;
    m_lastDraftElemCount = elems;
}

void Application::renderSketchRecoveryPrompt() {
    if (!m_pendingSketchRecovery) return;
    // Wait for the Welcome screen: opening a second popup at the same stack
    // level while it is up makes the two close each other every frame (see
    // the Welcome render site in run()). Fires the frame Welcome closes.
    if (m_welcomeScreen && m_welcomeScreen->isVisible()) return;
    ImGui::OpenPopup("Recover Sketch?");
    // Pin centered EVERY frame (not Appearing): on Android the popup can
    // first appear on a frame where the surface size isn't final yet, and an
    // Appearing-anchored centre computed from that degenerate viewport
    // strands the dialog in the top-left corner for good.
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // NoSavedSettings: this window's geometry is meaningless across launches
    // (it's re-centred every frame above), so never let a degenerate Pos/Size
    // — e.g. from some future popup-stack collision like the update-popup one
    // this class of bug already caused once — get written to imgui.ini and
    // self-perpetuate as the window's starting size on the next launch.
    if (ImGui::BeginPopupModal("Recover Sketch?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted(
            materializr::tr("An unfinished sketch from your last session was found."));
        ImGui::TextDisabled("%s", materializr::tr("It wasn't committed before the app closed (a crash, or a restart)."));
        ImGui::Spacing();
        if (ImGui::Button(materializr::tr("Restore it"), materializr::uiSz(140, 0))) {
            restoreSketchDraftNow();
            m_pendingSketchRecovery = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(materializr::tr("Discard"), materializr::uiSz(140, 0))) {
            materializr::clearSketchDraftAt(materializr::sketchDraftRestorePath());
            m_pendingSketchRecovery = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Application::restoreSketchDraftNow() {
    Sketch draft;
    materializr::SketchDraftMeta meta;
    if (!materializr::readSketchDraft(draft, meta) || !meta.valid) {
        // Unreadable: drop the ORPHAN we were offered (not our own live draft
        // path, which after the per-instance split is a different file), so a
        // corrupt leftover isn't re-offered at every launch.
        materializr::clearSketchDraftAt(materializr::sketchDraftRestorePath());
        return;
    }
    // Put it back in the TAB it came from before restoring anything. Without
    // this the draft was grafted onto whatever session happened to be active,
    // which after project recovery is always tab 0 — so a sketch begun in an
    // untitled tab reappeared on top of an unrelated restored project (Steve,
    // 2026-09-04). sketchDraftTargetSession holds the whole decision.
    std::vector<std::string> paths;
    paths.reserve(m_sessions.size());
    for (size_t i = 0; i < m_sessions.size(); ++i)
        paths.push_back(i == m_activeSession ? m_currentProjectPath
                                             : m_sessions[i]->projectPath);
    const size_t want = materializr::sketchDraftTargetSession(
        meta.projectPath, paths, m_activeSession, activeSessionIsScratch());
    size_t target = want;
    bool madeTab = false;
    if (want >= m_sessions.size()) {
        target = createSession();            // its own tab, as it had been in
        madeTab = true;
    }
    if (target != m_activeSession && !switchToSession(target)) {
        // Refused (mid-sketch / thread re-cut). Can't happen at startup, but
        // keep the draft rather than restore it into the wrong project — it
        // will be offered again next launch.
        if (madeTab) closeSession(target);
        showToast("Couldn't reopen the unfinished sketch's tab; it's still saved.");
        return;
    }

    // Re-enter sketch mode on the draft's plane (empty sketch; sets the undo
    // boundary here), then graft the geometry in AS A RECORDED MUTATION so the
    // restore is one undoable history step. Without this the restored geometry
    // had no history behind it, so Ctrl+Z couldn't touch it (the per-stroke
    // history from before the crash isn't in the draft — only the final shape).
    // (Face sketches re-bind their host face at Finish via ensureSketchSourceFace;
    // here we just restore the drawing on its plane so no work is lost.)
    enterSketchOnPlane(draft.getPlane());
    recordSketchMutation([&]{
        *m_activeSketch = draft;             // copy geometry + ids + constraints
        m_activeSketch->setSourceBody(meta.sourceBodyId);
    });
    m_sketchSolver->setSketch(m_activeSketch.get());
    m_sketchTool->setSketch(m_activeSketch.get());
    alignCameraToActiveSketch();
    m_meshesDirty = true;
    m_lastDraftElemCount = -1; // force a fresh draft write going forward
    // Consumed. The orphan lives in the DEAD instance's slot; from here we
    // autosave into our own, so leaving it would re-offer this sketch at every
    // future launch.
    materializr::clearSketchDraftAt(materializr::sketchDraftRestorePath());
    std::fprintf(stdout, "[Recovery] restored in-progress sketch (%d elements)"
                         " into tab %zu%s\n",
                 m_activeSketch->elementCount(), m_activeSession,
                 madeTab ? " (new)" : "");
}

void Application::writeProjectRecoveryIfDue() {
    // Snapshot the whole project to the crash-recovery sidecar — including an
    // UNSAVED one, which the user-facing autosave can't touch (it needs a path).
    // Snapshots immediately when a new step commits (so a hang in the NEXT op's
    // preview loses nothing), else throttled for non-structural dirtiness.
    if (m_pendingProjectRecovery) return;      // don't clobber a snapshot we may restore
    if (!isDirty()) return;                    // nothing unsaved to protect
    // Same guards as autosave: never snapshot a half-baked live preview / sketch,
    // and never below the history tip (the file only persists applied steps, so a
    // below-tip save would silently drop the redo tail).
    if (m_history && m_history->canRedo()) return;
    if (anyInteractivePreviewActive() || m_inSketchMode || m_edgeCtl.active())
        return;
    const int bodies = m_document ? m_document->bodyCount() : 0;
    const int curStep = m_history ? m_history->currentStep() : -1;
    if (bodies == 0 && curStep < 0) return;    // empty new document: nothing to lose

    const double now = SDL_GetTicks() / 1000.0;

    // Debounce, not a metronome (Steve's call, #48): snapshot once ~5 s AFTER
    // the last committed change settles — "time to save a good copy in case
    // the NEXT change crashes the program" — then go quiet until something
    // changes again. The old scheme re-serialized + re-gzipped the whole
    // project every 5 s for as long as it sat dirty (a periodic multi-MB
    // CPU/disk hit forever, even backgrounded).
    if (curStep != m_lastSeenStepForRecovery) {
        m_lastSeenStepForRecovery = curStep;   // history moved = a change
        m_lastChangeSeenAt = now;              // (markDirty stamps the rest)
    }
    // Nothing new since the last snapshot → nothing to protect; stay quiet.
    if (m_lastRecoveryWrite >= m_lastChangeSeenAt) return;
    // First change of a new episode: remember when the oldest UNSNAPSHOTTED
    // change landed — the burst backstop below is measured from here, not
    // from the last write, or the first change after a long quiet spell
    // would trip it instantly instead of settling for 5 s.
    if (m_pendingChangeSince <= m_lastRecoveryWrite)
        m_pendingChangeSince = m_lastChangeSeenAt;
    if (now - m_lastChangeSeenAt < 5.0) {
        // Still inside the settle window. Backstop: during a long BURST of
        // rapid changes (each < 5 s apart) a pure debounce would never fire
        // and a crash mid-burst would lose the whole run — strictly worse
        // than the old metronome. Write anyway once the oldest pending
        // change has waited a minute.
        if (now - m_pendingChangeSince < 60.0) return;
    }

    // Don't cancel a live preview on a background recovery tick — that would
    // revert the user's in-progress drag. The recovery file may then capture a
    // preview, which is acceptable for crash recovery (best-effort snapshot).
    //
    // And don't BLOCK on an in-flight async thread re-cut: capture drains
    // recuts (flushThreadRecuts), which on a background tick reads as the
    // whole app going "not responding" for the length of a boolean thread
    // cut. Skip this tick — the body is mid-recompute anyway, and the next
    // tick lands right after the recut does.
    if (!m_threadRecuts.empty()) return;
    ProjectHistory hist = captureProjectHistory(/*cancelPreviews=*/false);
    if (materializr::writeProjectRecovery(*m_document, &hist, m_currentProjectPath,
                                          bodies, curStep + 1,
                                          currentSession().recoveryIndex)) {
        m_lastRecoveryWrite = now;
        m_lastRecoveryStep = curStep;
    }
}

void Application::writeSessionRecoveryNow() {
    // Forced (undebounced) snapshot of the ACTIVE session — called when a tab
    // is about to deactivate. An inactive session cannot change, so this one
    // write keeps its recovery file exact until it becomes active again;
    // combined with the debounced writer above, EVERY open project survives a
    // crash, not just the front tab.
    if (!isDirty() || !m_document) return;
    if (m_history && m_history->canRedo()) return;   // same below-tip guard
    if (!m_threadRecuts.empty()) return;
    ProjectHistory hist = captureProjectHistory(/*cancelPreviews=*/false);
    materializr::writeProjectRecovery(
        *m_document, &hist, m_currentProjectPath,
        m_document->bodyCount(),
        (m_history ? m_history->currentStep() : -1) + 1,
        currentSession().recoveryIndex);
}

void Application::renderProjectRecoveryPrompt() {
    if (!m_pendingProjectRecovery) return;
    // Wait for the Welcome screen — same popup-stack turn-taking as the
    // sketch-recovery prompt (see renderSketchRecoveryPrompt).
    if (m_welcomeScreen && m_welcomeScreen->isVisible()) return;
    ImGui::OpenPopup("Recover Project?");
    // Pinned centred every frame — see the Android note on the sketch prompt.
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // NoSavedSettings — see the identical note on renderSketchRecoveryPrompt.
    if (ImGui::BeginPopupModal("Recover Project?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoSavedSettings)) {
        materializr::ProjectRecoveryMeta meta;
        materializr::readProjectRecoveryMeta(meta);
        ImGui::TextUnformatted(
            materializr::tr("Unsaved work from your last session was recovered."));
        if (!meta.projectPath.empty())
            ImGui::TextDisabled(materializr::tr("Project: %s"), meta.projectPath.c_str());
        else
            ImGui::TextDisabled("%s", materializr::tr("An unsaved project (never written to a file)."));
        ImGui::TextDisabled(materializr::tr("%d bodies, %d history steps."),
                            meta.bodyCount, meta.stepCount);
        ImGui::TextDisabled("%s", materializr::tr("Materializr didn't close cleanly (a crash, hang, or restart)."));
        // One snapshot per tab the dead instance had open — the summary above
        // describes the newest; all of them come back, a tab each.
        const int nOrphans = materializr::projectRecoveryOrphanCount();
        if (nOrphans > 1)
            ImGui::TextDisabled(materializr::tr("%d projects in total — each reopens in its own tab."), nOrphans);
        ImGui::Spacing();
        if (ImGui::Button(nOrphans > 1 ? "Restore all" : "Restore it",
                          materializr::uiSz(140, 0))) {
            restoreProjectRecoveryNow();
            m_pendingProjectRecovery = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(materializr::tr("Discard"), materializr::uiSz(140, 0))) {
            // These are the dead session's orphaned snapshots — our own live
            // slot is separate and untouched. Discard means ALL of them, to
            // match the restore: leaving the rest to resurface on the next
            // launch after the user said no is just nagging.
            for (const auto& p : materializr::projectRecoveryOrphanPaths())
                materializr::clearProjectRecoveryAt(p);
            materializr::clearProjectRecoveryCandidate();
            m_pendingProjectRecovery = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Application::restoreProjectRecoveryNow() {
    // EVERY orphan comes back, one per tab — a crash with four tabs open used
    // to hand them back one launch at a time (Steve, 2026-07-28). The newest
    // (the prompt's candidate) goes first so it lands in the tab the user is
    // already looking at.
    std::vector<std::string> paths = materializr::projectRecoveryOrphanPaths();
    const std::string newest = materializr::projectRecoveryRestorePath();
    if (!newest.empty()) {
        auto it = std::find(paths.begin(), paths.end(), newest);
        if (it != paths.end()) std::rotate(paths.begin(), it, it + 1);
        else paths.insert(paths.begin(), newest);
    }
    if (paths.empty()) return;

    int restored = 0, failed = 0;
    size_t firstTab = m_activeSession;   // where the newest snapshot lands
    for (size_t i = 0; i < paths.size(); ++i) {
        const std::string& recPath = paths[i];
        materializr::ProjectRecoveryMeta meta;
        materializr::readProjectRecoveryMetaAt(recPath, meta);
        // Each snapshot after the first gets its own tab. A refused switch
        // can't happen here (nothing is mid-sketch at startup), but honour it
        // anyway rather than restoring into the wrong tab.
        if (restored > 0) {
            const size_t idx = createSession();
            if (!switchToSession(idx)) { closeSession(idx); ++failed; continue; }
        }
        // Load through the normal project loader (rebuilds bodies + editable
        // history). loadProjectAt sets m_currentProjectPath to the sidecar and
        // marks it saved — override both with the project's ORIGINAL identity
        // so the user can't overwrite the sidecar and unsaved work stays
        // unsaved/dirty.
        if (!loadProjectAt(recPath)) {
            std::fprintf(stderr, "[Recovery] failed to load snapshot %s\n",
                         recPath.c_str());
            materializr::clearProjectRecoveryAt(recPath);
            if (restored > 0) closeSession(m_activeSession);
            ++failed;
            continue;
        }
        // Consumed: drop the orphan so it isn't offered again next launch. The
        // restored state is re-snapshotted into OUR slot within seconds (the
        // markDirty below makes writeProjectRecoveryIfDue fire).
        materializr::clearProjectRecoveryAt(recPath);
        m_currentProjectPath = meta.projectPath; // "" if it was never saved
        markDirty();                             // unsaved since the snapshot
        m_lastRecoveryStep = -2;                 // force a fresh snapshot
        ++restored;
        std::fprintf(stdout, "[Recovery] restored project (%d bodies, %d steps)"
                             " into tab %zu\n",
                     meta.bodyCount, meta.stepCount, m_activeSession);
    }
    // Land on the tab the PROMPT described (the newest snapshot), not
    // whichever one happened to load last.
    if (restored > 1 && firstTab < m_sessions.size()) switchToSession(firstTab);
    materializr::clearProjectRecoveryCandidate();  // whatever is left of it
    saveAppSettings();                             // fix lastProjectPath off the sidecar
    if (restored > 1)
        showToast("Recovered " + std::to_string(restored) + " projects.");
    if (failed > 0)
        showToast(std::to_string(failed) + " recovered project(s) "
                  "couldn't be reopened.");
}

void Application::run() {
    // A draft surviving from a previous session means it ended mid-sketch
    // (crash / kill / quit while drawing) — offer to restore on first frame.
    m_pendingSketchRecovery = materializr::hasSketchDraft();
    // A whole-project recovery snapshot surviving means the last session ended
    // unexpectedly with unsaved work — offer to restore that too.
    m_pendingProjectRecovery = materializr::hasProjectRecovery();
    if (materializr::isVerbose())
        std::fprintf(stderr,
                     "[Recovery] startup scan: pending=%d orphans=%d path=%s\n",
                     m_pendingProjectRecovery ? 1 : 0,
                     materializr::projectRecoveryOrphanCount(),
                     materializr::projectRecoveryRestorePath().c_str());

    // Opt-in perf instrumentation (MZR_PERF=1): once a second, report how many
    // frames we actually RENDERED vs how many loop iterations ran, plus which
    // "active work" state forced rendering. Lets us see e.g. "in-sketch idle =
    // 60 rendered/s" (a wasteful continuous-render state) vs "true idle = ~0".
    const bool kPerf = std::getenv("MZR_PERF") != nullptr;
    uint32_t perfLastMs = SDL_GetTicks();
    int perfRendered = 0, perfIters = 0;
    // Startup render-grace: keep drawing for the first few seconds regardless of
    // reported window focus, so the UI always appears after the loading screen
    // even if the WM is slow to hand the new window focus. See foreground below.
    const uint32_t runStartMs = SDL_GetTicks();

    // FRAME-LEVEL EXCEPTION FIREWALL.
    //
    // main() wraps app.run() in a catch that returns 1. On desktop that reads
    // as a crash; on Android it is far worse and far more confusing: SDL_main
    // returning makes SDLActivity finish itself, so the window simply vanishes
    // with NO signal, NO tombstone and NO ANR — Android logs it as
    // "app-request". It looks exactly like a crash and is impossible to
    // diagnose from the outside. One real instance: tapping Apply Changes on a
    // recovery-restored project let a std::runtime_error("Body not found: 1")
    // out of a stale body lookup, and the app quietly exited with the user's
    // unsaved work.
    //
    // Document::getBody and friends throw on a missing id, and stale ids
    // outlive a replay in more places than can be audited once and trusted
    // (refreshAllEdgeOpFaces already carries a comment about this same throw
    // aborting the app on load). So: one escaped exception costs a FRAME, not
    // the session. The loop re-enters and the user gets a toast.
    //
    // The catch is not a licence to ignore these — it logs to stderr (logcat on
    // Android), which is how the caller gets found. Anything appearing here is
    // a bug to fix at its source.
    for (;;) {
      try {
        while (true) {
        // Main-loop stall watchdog: a gap of seconds between iterations IS
        // the "not responding" freeze — print it so the journal names the
        // stall instead of us guessing which subsystem blocked.
        {
            static uint32_t lastIterMs = 0;
            const uint32_t nowMs = SDL_GetTicks();
            if (lastIterMs != 0 && nowMs - lastIterMs > 1000) {
                // A heavy task deliberately owns the loop for as long as it
                // takes; that is not the freeze this watchdog hunts for, as
                // long as it kept answering the compositor. Say which one it
                // was, or the loading of a big project reads as the bug.
                if (m_heavyRanThisIter)
                    std::fprintf(stderr, "[Perf] heavy task held the loop %.2fs "
                                 "(%d UI pumps, %d frames, worst gap %.2fs)\n",
                                 (nowMs - lastIterMs) / 1000.0,
                                 m_heavyPumps, m_heavyDraws,
                                 m_heavyWorstGapMs / 1000.0);
                else
                    std::fprintf(stderr, "[Perf] main loop stalled %.2fs\n",
                                 (nowMs - lastIterMs) / 1000.0);
            }
            m_heavyRanThisIter = false;
            lastIterMs = nowMs;
        }
        // Apply/discard any landed async thread re-cuts before this frame.
        pollThreadRecuts();

        // True while any interactive tool or animation is in flight and needs
        // continuous rendering even with no user input.
        auto hasActiveWork = [&]() -> bool {
            // Always-on: self-completing work that needs frames to FINISH —
            // a pending heavy task to run, a toast that must tick down and clear
            // (regressed once as "toast never clears"), a modal popup, or an
            // extension tool that may animate on its own.
            if (m_deferredHeavyTask || m_showUpdatePopup || !m_toastText.empty())
                return true;
            if (!m_threadRecuts.empty()) return true; // async re-cut in flight
            if (PluginRegistry::instance().activeTool()) return true;
            // Interactive manipulation states (sketch + every live preview/op)
            // are INPUT-driven: they only need continuous frames while the user
            // is acting on them. Render for a short grace window after the last
            // input (m_interactiveGraceUntil, refreshed on any event below), then
            // idle — the preview stays on screen, frozen, and wakes instantly on
            // the next drag/keypress. The grace also covers the ~0.3s sketch
            // hover-dwell charge. Previously each of these pinned a flat 60fps
            // the whole time it was open (e.g. a push/pull left mid-edit) —
            // wasteful on the iGPU, a battery/thermal sink on mobile.
            bool interactive =
                m_inSketchMode || m_ppCtl.active() || m_gizmoDragging ||
                m_edgeCtl.active() || m_moveFaceCtl.active() ||
                m_revolveActive;
            if (!interactive)
                for (auto* c : m_iops) if (c && c->active()) { interactive = true; break; }
            if (interactive && SDL_GetTicks() / 1000.0 < m_interactiveGraceUntil)
                return true;
            return false;
        };

        ++perfIters;
        if (kPerf) {
            uint32_t nowMs = SDL_GetTicks();
            if (nowMs - perfLastMs >= 1000) {
                std::string st;
                if (m_inSketchMode)            st += "sketch ";
                if (m_ppCtl.active())          st += "pushpull ";
                if (m_gizmoDragging)           st += "gizmo ";
                if (m_edgeCtl.active())            st += "edgeop ";
                if (m_moveFaceCtl.active())       st += "moveface ";
                if (m_revolveActive)           st += "revolve ";
                if (m_deferredHeavyTask)       st += "heavy ";
                if (!m_toastText.empty())      st += "toast ";
                if (m_showUpdatePopup)         st += "update ";
                bool iop = false;
                for (auto* c : m_iops) if (c && c->active()) iop = true;
                if (iop)                       st += "iop ";
                if (PluginRegistry::instance().activeTool()) st += "tool ";
                if (st.empty())                st = "(idle)";
                std::fprintf(stderr,
                    "[perf] rendered=%d/s iters=%d/s wake=%d state=%s\n",
                    perfRendered, perfIters, m_wakeFrames, st.c_str());
                perfRendered = 0; perfIters = 0; perfLastMs = nowMs;
            }
        }

        // Suspend rendering entirely while backgrounded (not the focused window,
        // or minimized): a backgrounded GL app still composited at 60fps is what
        // makes the whole desktop's cursor lag on a shared GPU, and it's pure
        // waste on mobile. Autosave + deferred tasks below still run; FOCUS_GAINED
        // is a significant event so we repaint instantly on return.
        // Force continuous rendering for a short grace right after launch so the
        // splash→UI handoff ALWAYS completes on its own. At a fresh idle startup
        // there's no input event (m_wakeFrames == 0) and no active work, so the
        // idle-skip below would otherwise leave the first real UI frame undrawn
        // behind the loading screen until the user clicks or moves the mouse.
        // This is a hard override of BOTH the focus gate and the idle-skip — the
        // earlier "foreground for 3 s" only neutralised the focus term, leaving
        // the idle term to still skip (the splash-hang regression).
        const bool launchGrace = (SDL_GetTicks() - runStartMs < 3000u);
#if defined(MZ_IOS)
        // iOS: the OS pauses a backgrounded app too, but there is a window
        // around WILLENTERBACKGROUND where GL calls get the app terminated by
        // the watchdog. Hard-stop rendering while UIKit says we are backgrounded
        // (the 500 ms event-wait below keeps the loop cheap until suspension).
        const bool foreground = !materializr::iosInBackground();
#elif defined(MZ_MOBILE)
        // Android exemption: the OS already pauses the activity (and SDL the GL
        // surface) when backgrounded, so the gate buys nothing here — and
        // SDL_WINDOW_INPUT_FOCUS isn't set until the first touch, so gating on it
        // froze the startup splash→UI handoff until the user tapped the screen.
        const bool foreground = true;
#else
        // Desktop: respect window focus to suspend when backgrounded.
        const bool foreground = m_window->isForeground() || launchGrace;
#endif

        // Frame pacing. Active (recent input / live work): no wait — render at
        // vsync rate. Idle in the FOREGROUND: render at a low FLOOR rate
        // instead of stopping. The old hard 0 fps idle made every first tap
        // pay a wake-up round trip and left system-side effects (the iOS
        // soft-keyboard raise, whose request we can only issue at the end of
        // a rendered frame) stranded between bursts — on the tablet the
        // keyboard prompt felt seconds late no matter what counted as "active
        // work". A ~15 fps floor keeps the first tap, the keyboard, and every
        // overlay live at a quarter of the full-rate GPU cost; the wait still
        // returns EARLY on any event, so activity ramps to full rate with no
        // added latency. Backgrounded (desktop) still parks completely in
        // 500 ms waits. During the launch grace we never block — keep frames
        // flowing for the splash→UI handoff.
        constexpr int kIdleFloorMs = 66;   // idle frame interval ≈ 15 fps
        int waitMs = 0;
        if (!launchGrace && !foreground)
            waitMs = 500;
        else if (!launchGrace && m_wakeFrames == 0 && !hasActiveWork())
            waitMs = kIdleFloorMs;
        int eventLevel = m_window->pollEvents(waitMs);
        // Start of this iteration's frame budget — read by the frame-rate cap
        // at the bottom of the loop (measured after the event wait so the
        // idle floor's own sleep doesn't count against the budget).
        const Uint32 frameLoopStartMs = SDL_GetTicks();
        // Significant events (click, key, scroll, resize, focus): 5 frames.
        // Trivial events (mouse motion, expose): 25 frames — at 60 fps that is
        // ~416 ms, enough for ImGui's default 300 ms hover-tooltip delay to fire
        // AFTER the cursor stops moving. Without this extra tail, the tooltip
        // timer freezes the moment we stop rendering (ImGui time only advances
        // inside NewFrame). The idle timeout (eventLevel == 0) is not a trigger:
        // we skip rendering until a real event or active work wakes us.
        if (eventLevel >= 2)
            m_wakeFrames = 5;
        else if (eventLevel == 1)
            m_wakeFrames = std::max(m_wakeFrames, 25);

#if defined(MZ_MOBILE)
        // Multi-finger tap gestures (Android/iOS): two-finger tap = undo,
        // three-finger tap = redo. Same guards as the Edit menu; both flags
        // are consumed every frame so a blocked tap can't fire later.
        {
            const bool undoTap = m_window->consumeUndoTap();
            const bool redoTap = m_window->consumeRedoTap();
            if (!anyInteractivePreviewActive()) {
                if (undoTap && m_history->canUndo()) {
                    undoWithCascade();
                    m_wakeFrames = std::max(m_wakeFrames, 5);
                } else if (redoTap && m_history->canRedo()) {
                    redoWithCascade();
                    m_wakeFrames = std::max(m_wakeFrames, 5);
                }
            }
        }
#endif

        // Any input refreshes the interactive-state render grace (see
        // hasActiveWork): keep rendering for kGraceSec after the last event,
        // then drop to the idle floor rate. 1s comfortably covers the ~0.3s
        // sketch hover-dwell charge; rendering ramps back to full rate
        // instantly on the next event.
        if (eventLevel > 0) {
            constexpr double kGraceSec = 1.0;
            m_interactiveGraceUntil = SDL_GetTicks() / 1000.0 + kGraceSec;
        }

        // Last frame's GL (driver/ImGui render) can leave the SSE FPU in
        // flush-to-zero / denormals-are-zero mode, which makes OCCT geometry —
        // including SVG import tessellation and wire-building done mid-frame —
        // come out subtly different run to run (a different region degenerates
        // into an uncuttable sliver each re-import). Put the FPU back to OCCT's
        // expected mode at the top of EVERY frame so all geometry is stable.
        resetFpuForOcct();

        // Run a heavy op deferred from last frame's commit HERE, between frames,
        // so its progress reporter (renderProgressFrame) can pump its own frames
        // without nesting ImGui frames.
        if (m_deferredHeavyTask) {
            auto task = std::move(m_deferredHeavyTask);
            m_deferredHeavyTask = nullptr;
            // Arm the UI keep-alive for the duration of the task, and ONLY for
            // that duration. Between frames is the one place where repainting
            // from deep inside an op is safe (no ImGui frame is in flight), and
            // scoping it here is what lets the same ops call uiKeepAlive() from
            // an OCCT callback during a live drag preview and get a harmless
            // no-op. See core/UiKeepAlive.h.
            m_heavyProgressFrac = -1.0f;
            m_heavyProgressLabel.clear();
            m_heavyPumps = m_heavyDraws = m_heavyWorstGapMs = 0;
            m_nextHeavyDraw = m_lastHeavyPump = std::chrono::steady_clock::now();
            materializr::setUiKeepAlive([this]() {
                using clock = std::chrono::steady_clock;
                ++m_heavyPumps;
                noteHeavyPumpGap();
                // Pump FIRST and unconditionally. This is the part that keeps
                // the compositor's ping answered, it touches no GL, and it must
                // not be hostage to the draw below -- which is throttled, and
                // which renderProgressFrame skips entirely once the cancel
                // latch is set.
                if (m_window) m_window->pollEvents();

                const auto now = clock::now();
                if (now < m_nextHeavyDraw) return;
                renderProgressFrame(m_heavyProgressFrac,
                                    m_heavyProgressLabel.c_str());
                ++m_heavyDraws;
                // Back off from the draw's OWN cost, so a cheap frame animates
                // smoothly and an expensive one (see m_nextHeavyDraw) can never
                // eat more than a fifth of the wall clock.
                const auto after = clock::now();
                const auto cost = after - now;
                m_nextHeavyDraw = after + std::max(
                    std::chrono::duration_cast<clock::duration>(
                        std::chrono::milliseconds(200)), cost * 4);
            });
            task();
            materializr::setUiKeepAlive(nullptr);
            noteHeavyPumpGap();   // close the books on the tail of the task
            m_heavyRanThisIter = true;   // the watchdog reads this next time round
            m_wakeFrames = 5; // task finished — repaint the result
        }

        // Apply the launch-time update check once its worker finishes — never
        // block waiting for it.
        if (m_updateCheckFuture.valid() &&
            m_updateCheckFuture.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
            auto r = m_updateCheckFuture.get();
            if (r.ok && r.updateAvailable) {
                m_updateCurrent    = r.current;
                m_updateLatest     = r.latest;
                m_updateAvailable  = true;
                m_updateReleaseUrl = r.releasePageUrl;
                m_updateMessage    = "";
                m_updateChecked    = true;
                m_showUpdatePopup  = true;
                m_wakeFrames = 5;
            }
        }

        // Keep the in-progress sketch crash-recoverable.
        writeSketchDraftIfDue();
        // Keep the whole committed project crash/hang-recoverable (incl. unsaved).
        writeProjectRecoveryIfDue();

        // The save-prompt's Don't Save / post-save-success path sets this flag
        // directly. Check it every frame so we exit without requiring the user
        // to click the X a second time.
        if (m_confirmedClose) break;

        // Intercept window-close requests: if there are unsaved changes, show
        // the prompt and cancel the close until the user picks Save/Don't Save.
        if (m_window->shouldClose()) {
            requestClose();
            if (m_confirmedClose) break;
        }

        // Autosave — MUST run before the idle short-circuit below. A change
        // wakes only a brief render burst, then the loop idles and `continue`s
        // past everything down-stream; if autosave lived after the skip it
        // would essentially never fire for a model you edit and then leave
        // alone. The timer uses SDL_GetTicks (wall clock) rather than
        // ImGui::GetTime(), which is frozen while we're not rendering and so
        // would never let the interval elapse during idle.
        // Only for projects already on disk, only when there are pending
        // changes; the interval is measured from the last save.
        if (m_autosaveEnabled && !m_currentProjectPath.empty()) {
            double now = SDL_GetTicks() / 1000.0;
            if (isDirty()) {
                // Never autosave while the user is below the history tip
                // (mid undo-exploration): the file only persists APPLIED
                // steps, so saving now would silently truncate the redo
                // tail from the project. Resume once they redo back to the
                // tip or push a new op (which discards the tail anyway).
                if (m_history && m_history->canRedo()) {
                    // hold off — keep checking each interval
                } else if (anyInteractivePreviewActive() || m_inSketchMode ||
                           m_edgeCtl.active()) {
                    // hold off — an autosave must never cancel (or serialize) a
                    // live tool preview / an in-progress sketch out from under
                    // the user (a half-baked uncommitted-sketch state has
                    // crashed before). Resume once the tool / sketch closes.
                } else if (now - m_lastAutosaveTime >= m_autosaveIntervalSec) {
                    // Defensive: a serialization failure (OCCT throw, bad
                    // state) must never take the whole app down on a background
                    // autosave — log and skip, try again next interval.
                    try { saveProjectQuick(); }
                    catch (...) {
                        std::fprintf(stderr, "[Autosave] failed - skipped\n");
                    }
                    m_lastAutosaveTime = now;
                }
            } else {
                m_lastAutosaveTime = now;
            }
        } else {
            m_lastAutosaveTime = SDL_GetTicks() / 1000.0;
        }

        // Only a BACKGROUNDED window skips rendering now — foreground idle
        // renders at the floor rate above (see kIdleFloorMs; the old idle
        // skip is what made first-taps and the mobile keyboard feel dead).
        if (!launchGrace && !foreground) continue;
        if (m_wakeFrames > 0) m_wakeFrames--;
        ++perfRendered;   // passed the idle skip → this iteration renders a frame

        beginFrame();
        // With the modern/im-touch layouts on, the touch theme wraps the WHOLE
        // frame so every dialog / interactive-op popup matches the shell look
        // (rounded, padded, dark) instead of the classic desktop style. Latched
        // once per frame: the layout can flip mid-frame (Settings dropdown) and
        // the pop below must match this push, not the new value.
        const bool frameTouchTheme = !classicLayout();
        // Palette follows ThemeManager (View → Theme / Settings → Appearance)
        // so the modern/im-touch layouts have a live light mode like classic.
        touchui::setLightMode(m_themeManager &&
                              m_themeManager->getTheme() == Theme::Light);
        // im-touch wears crisp 4 px corners kit-wide; the modern layout keeps
        // the soft rounded look (each widget's own default).
        touchui::setCornerRadius(imTouchLayout() ? 4.0f : -1.0f);
        if (frameTouchTheme) touchui::pushChrome();
        // Always submit the dockspace host — under every layout. ImGui only
        // keeps a dock node alive while its DockSpace() is submitted each frame;
        // skipping it (the old else-only path) dropped the classic nodes, so on
        // switch-BACK the panels returned FLOATING at their last spot instead of
        // docked. The viewport then spanned the full width UNDER them, drawing
        // the ViewCube behind the panel. Kept alive here, the layout restores
        // exactly. In modern/im-touch the host sits behind the shell + pinned
        // viewport (NoBringToFrontOnFocus) and its panels aren't submitted, so
        // it's invisible — it only preserves the node tree.
        renderDockspace();
        // Synced unconditionally, once per frame, before any layout reads it —
        // ItemsPanel's Delete/Edit-Sketch/Combine gating on the sketch being
        // drawn must never see a stale value from a frame where the panel (or
        // a different layout) didn't render.
        if (m_itemsPanel) m_itemsPanel->setActiveSketchContext(m_inSketchMode, m_activeSketchId);
        // Per-layout chrome (src/app/layout/<name>/). A new layout gets a case
        // here; everything below this dispatch is layout-agnostic or gated on
        // the layout helpers. While the landing page is up the modern and
        // im-touch shells stand down completely (their floating chrome would
        // draw over the page); classic keeps its menu bar — the one piece of
        // chrome that is useful above the page — and drops the rest below.
        // No floating chrome until a layout says otherwise. Reset HERE, in the
        // one place every layout passes through, rather than in each layout's
        // else-branch — a third layout that forgot would inherit im-touch's
        // inset from the frame before the user switched.
        materializr::viewportTopChromeBottom() = 0.0f;
        switch (m_uiLayout) {
            case UiLayout::Classic: renderMenuBar();        break;
            case UiLayout::Modern:
                if (!landingPageUp()) renderModernLayout();
                break;
            case UiLayout::ImTouch:
                if (!landingPageUp()) renderImTouchLayout();
                break;
        }
        renderSmallScreenWarning();

        if (m_renderersReady) {
            renderViewport();
            // Modern's panel pop-in/out edge tabs go on top of the viewport —
            // submit them AFTER it (they'd otherwise render under the
            // NoBringToFrontOnFocus Viewport window until a focus reorder).
            if (m_uiLayout == UiLayout::Modern) renderModernEdgeTabs();

            m_toolbar->setGridStep(m_sketchGridStep);
            m_toolbar->setSnapToGrid(m_snapToGrid);
            m_toolbar->setCameraOrtho(m_viewport->getCamera().isOrthographic());
            // "Edit Diameter" button only appears when the picked face is a
            // cylinder on a solid-cylinder or tube body. Detection populates
            // m_resizeCyl* fields as a side effect — we throw the result away
            // here, those are only used by the actual begin path (which
            // re-runs the detection itself when the button is pressed).
            //
            // Both this detection (OCCT surface queries + a whole-body face
            // walk for edge picks) and the frozen-round scan below (surface
            // query + an ownsFace() history walk) used to run EVERY rendered
            // frame while a face was selected — the normal working state.
            // They only depend on the selection and the history, so memoize
            // on those revisions and recompute only when one changes.
            {
                static unsigned s_selRev = ~0u, s_histRev = ~0u;
                static bool s_resizeActive = false;
                static bool s_canEditDiameter = false;
                static bool s_frozenRound = false;
                static bool s_selSketchAttached = false;
                static bool s_selFacePlanar = false;
                static bool s_selFaceIsHoleWall = false;
                static bool s_selEdgeIsHoleRim = false;
                // Tab switches swap in a DIFFERENT SelectionManager/History
                // whose revision counters can coincide with the memoized ones
                // (they all start at 0) — key the memo on the session too.
                static size_t s_memoSession = ~size_t(0);
                if (s_memoSession != m_activeSession) {
                    s_memoSession = m_activeSession;
                    s_selRev = ~0u;
                    s_histRev = ~0u;
                }
                const unsigned selRev  = m_selection->revision();
                const unsigned histRev = m_history->revision();
                if (selRev != s_selRev || histRev != s_histRev ||
                    m_resizeCylCtl.active() != s_resizeActive) {
                    s_selRev = selRev;
                    s_histRev = histRev;
                    s_resizeActive = m_resizeCylCtl.active();
                    s_canEditDiameter = !m_resizeCylCtl.active() &&
                                        detectCylindricalResizeCandidate().ok;
                    // "Frozen round" hint: a selected fillet-shaped face
                    // (cylinder / torus) that NO enabled op owns reloaded as
                    // baked geometry — there's no editable FilletOp behind it.
                    // The toolbar surfaces a one-liner pointing at Repair
                    // Geometry. A FULL 2π cylinder is a hole / pin (Edit
                    // Diameter handles it), not a round, so it's excluded.
                    s_frozenRound = false;
                    s_selFacePlanar = false;
                    s_selFaceIsHoleWall = false;
                    TopoDS_Shape pf;
                    int pfBody = -1;
                    for (const auto& e : m_selection->getSelection())
                        if (e.type == SelectionType::Face && !e.shape.IsNull()) {
                            pf = e.shape; pfBody = e.bodyId; break;
                        }
                    if (!pf.IsNull() && pf.ShapeType() == TopAbs_FACE) {
                        try {
                            TopoDS_Face f = TopoDS::Face(pf);
                            s_selFacePlanar = faceIsPlanar(f);  // gates Push (#28)
                            // A round hole's WALL: #28 hides Move on curved
                            // faces, so a full-cylinder bore could never be
                            // moved by clicking its inside — probe buildVoid,
                            // and offer whole-hole Move when it recognizes one.
                            // (A square hole's walls are planar, so they reach
                            // the same slide through the ordinary Move gate.)
                            if (!s_selFacePlanar && pfBody >= 0) {
                                TopoDS_Shape v; gp_Vec n; bool pocket = false;
                                s_selFaceIsHoleWall = MoveHoleOp::buildVoid(
                                    m_document->getBody(pfBody), f, v, n, pocket);
                            }
                            Handle(Geom_Surface) s = BRep_Tool::Surface(f);
                            bool round = false;
                            if (!s.IsNull()) {
                                if (s->IsKind(STANDARD_TYPE(Geom_ToroidalSurface))) {
                                    round = true; // curved-edge fillet
                                } else if (s->IsKind(STANDARD_TYPE(Geom_CylindricalSurface))) {
                                    double u1, u2, v1, v2;
                                    BRepTools::UVBounds(f, u1, u2, v1, v2);
                                    round = (u2 - u1) < 2.0 * M_PI - 0.05; // partial = fillet
                                }
                            }
                            if (round) {
                                s_frozenRound = true; // assume frozen until an op claims it
                                for (const auto& op : m_history->operations())
                                    if (op && op->isEnabled() && op->ownsFace(pf) &&
                                        (op->kind() == Operation::Kind::Fillet ||
                                         op->kind() == Operation::Kind::Chamfer)) {
                                        s_frozenRound = false;
                                        break;
                                    }
                            }
                        } catch (...) {}
                    }
                    // Sketch attachment gate: is any selected sketch / region
                    // still driving a body? Push/Pull is offered for those (it
                    // edits the host body); a standalone sketch offers Extrude
                    // instead (a new body). A detached sketch counts as
                    // standalone — it was deliberately unlinked (issue #21).
                    // Do the selected edges form one hole's rim? Only then
                    // does Move mean anything for an edge selection. In the
                    // memo because classifyRimEdges walks the body and probes
                    // buildVoid — per-frame it burned time and (pre-verbose-
                    // gate) flooded the journal with refusals.
                    s_selEdgeIsHoleRim = false;
                    {
                        std::vector<TopoDS_Edge> picked;
                        int edgeBody = -1;
                        for (const auto& e : m_selection->getSelection()) {
                            if (e.type != SelectionType::Edge || e.shape.IsNull()) continue;
                            if (edgeBody >= 0 && e.bodyId != edgeBody) { picked.clear(); break; }
                            edgeBody = e.bodyId;
                            picked.push_back(TopoDS::Edge(e.shape));
                        }
                        if (!picked.empty() && edgeBody >= 0) {
                            try {
                                s_selEdgeIsHoleRim = MoveHoleOp::classifyRimEdges(
                                          m_document->getBody(edgeBody), picked).ok;
                            } catch (...) {}
                        }
                    }
                    s_selSketchAttached = false;
                    for (const auto& e : m_selection->getSelection()) {
                        if ((e.type == SelectionType::Sketch ||
                             e.type == SelectionType::SketchRegion) &&
                            e.sketchId >= 0) {
                            auto sk = m_document->getSketch(e.sketchId);
                            // ...and the host must still EXIST. Attachment is
                            // an either/or here (attached => Push/Pull, else
                            // Extrude), so a stale id offered the one tool that
                            // cannot work and hid the one that can — delete a
                            // sketch's body and it still claimed to be attached.
                            if (sk && sk->getSourceBody() >= 0 &&
                                !sk->isDetachedFromBody() &&
                                bodyExists(sk->getSourceBody())) {
                                s_selSketchAttached = true;
                                break;
                            }
                        }
                    }
                }
                m_toolbar->setCanEditDiameter(s_canEditDiameter);
                m_toolbar->setSelFacePlanar(s_selFacePlanar);
                m_toolbar->setSelFaceIsHoleWall(s_selFaceIsHoleWall);
                m_toolbar->setSelEdgeIsHoleRim(s_selEdgeIsHoleRim);
                m_toolbar->setSelectedFaceFrozenRound(s_frozenRound);
                m_toolbar->setSelectedSketchAttached(s_selSketchAttached);
            }
            m_toolbar->setShowTooltips(m_showToolbarTooltips);
            // Mirror the live inference level (Full/Reduced/Off) so the sketch
            // toolbar button shows the current state. Int to keep Toolbar free
            // of a SketchTool.h dependency (matches setActiveSketchMode).
            m_toolbar->setInferenceLevel(m_inSketchMode && m_sketchTool
                ? static_cast<int>(m_sketchTool->getInferenceLevel()) : 0);
            // Mirror the live rect/circle draw-origin so the per-tool toggle
            // button shows the current mode.
            if (m_inSketchMode && m_sketchTool) {
                m_toolbar->setRectMode(static_cast<int>(m_sketchTool->getRectMode()));
                m_toolbar->setCircleMode(static_cast<int>(m_sketchTool->getCircleMode()));
            }
            // Per-frame hide/show of the toolbar's inference cycle button.
            // Users who set the level once in Settings can declutter the
            // sketch toolbar; default is on (the live toggle is visible).
            m_toolbar->setShowInferenceToggle(m_showInferenceToolbarToggle);
            // Pass the active sketch tool mode so the matching button gets
            // a highlight border — disambiguates which tool is currently in
            // use (Line vs Circle vs etc.) when in sketch mode.
            m_toolbar->setActiveSketchMode(m_inSketchMode && m_sketchTool
                ? static_cast<int>(m_sketchTool->getMode()) : 0);
            // Drive the Constraints section: it only appears when sketch
            // elements are actually selected, and only shows buttons that
            // match the selection arity.
            if (m_inSketchMode && m_sketchTool) {
                m_toolbar->setSketchSelectionCounts(
                    static_cast<int>(m_sketchTool->getSelectedPoints().size()),
                    static_cast<int>(m_sketchTool->getSelectedLines().size()),
                    static_cast<int>(m_sketchTool->getSelectedCircles().size()),
                    static_cast<int>(m_sketchTool->getSelectedArcs().size()));
            } else {
                m_toolbar->setSketchSelectionCounts(0, 0, 0, 0);
            }
            // Solver-state badge. Only meaningful when in sketch mode AND
            // there are constraints to evaluate; otherwise hide the badge.
            if (m_inSketchMode && m_activeSketch && m_sketchSolver &&
                !m_activeSketch->getConstraints().empty()) {
                m_toolbar->setSketchSolverState(static_cast<int>(m_sketchSolver->getState()));
                m_toolbar->setSketchSolverDof(m_sketchSolver->degreesOfFreedom());
            } else {
                m_toolbar->setSketchSolverState(-1);
                m_toolbar->setSketchSolverDof(0);
            }
            // The Tools palette is the LEFT docked column; it collapses with the
            // left edge handle (or Hide Panels). All the setters above are
            // harmless no-ops on an unsubmitted window.
            ToolAction action = ToolAction::None;
            if (classicLayout() && !landingPageUp() && !m_leftPanelHidden &&
                m_showTools) {
                action = m_toolbar->render();
                m_sketchGridStep = m_toolbar->getGridStep();
                m_snapToGrid = m_toolbar->getSnapToGrid();
                if (m_sketchTool) {
                    m_sketchTool->setGridStep(m_sketchGridStep);
                    m_sketchTool->setSnapToGridEnabled(m_snapToGrid);
                }
            }
            if (action != ToolAction::None) {
                handleToolAction(static_cast<int>(action));
            }

            // A plugin toolbar action may have requested an interactive op that
            // needs Application's popup machinery (e.g. PatternPlugin asking for
            // the Linear / Radial pattern popup). Dispatch any pending request.
            if (m_pluginContext) {
                // Typed dispatch (was a chain of string compares — see
                // plugin/InteractiveOp.h and discussion #72). NO default: on
                // purpose — a new InteractiveOp that nobody handles here is a
                // -Wswitch warning at build time, where the string version
                // silently produced a button that did nothing.
                const InteractiveOp pending =
                    m_pluginContext->takeRequestedInteractiveOp();
                switch (pending) {
                    case InteractiveOp::None: break;
                    case InteractiveOp::LinearPattern:  beginPattern(PatternKind::Linear); break;
                    case InteractiveOp::RadialPattern:  beginPattern(PatternKind::Radial); break;
                    case InteractiveOp::Loft:           beginLoft();           break;
                    case InteractiveOp::BoundaryFill:   beginBoundaryFill();   break;
                    case InteractiveOp::LoftPickSecond: m_loftPickHintPending = true; break;
                    case InteractiveOp::ConstructionPlane: beginConstructionPlane(); break;
                    case InteractiveOp::ImportRefImage: beginRefImageImport(); break;
                    case InteractiveOp::ConstructionAxis: beginConstructionAxis(); break;
                    case InteractiveOp::Revolve:        beginRevolve();        break;
                    case InteractiveOp::Midplane:          beginConstructionPlaneMode(4); break;
                    case InteractiveOp::PlaneNormalToAxis: beginConstructionPlaneMode(5); break;
                    case InteractiveOp::TangentPlane:      beginConstructionPlaneMode(6); break;
                    case InteractiveOp::PlaneThroughAxis:  beginConstructionPlaneMode(7); break;
                    case InteractiveOp::AxisFromCylinder:  beginConstructionAxisMode(3); break;
                    case InteractiveOp::AxisAlongEdge:     beginConstructionAxisMode(4); break;
                    case InteractiveOp::AxisTwoPoints:     beginConstructionAxisMode(5); break;
                    case InteractiveOp::AxisNormalToFace:  beginConstructionAxisMode(6); break;
                    case InteractiveOp::AxisTwoPlanes:     beginConstructionAxisMode(7); break;
                    case InteractiveOp::PrimitiveBox:      beginPrimitivePopup(0); break;
                    case InteractiveOp::PrimitiveCylinder: beginPrimitivePopup(1); break;
                    case InteractiveOp::PrimitiveSphere:   beginPrimitivePopup(2); break;
                    case InteractiveOp::PrimitiveCone:     beginPrimitivePopup(3); break;
                    case InteractiveOp::PrimitiveTorus:    beginPrimitivePopup(4); break;
                    case InteractiveOp::StlImport:         beginStlImportDialog(); break;
                }
            }

            // Active interactive tool (plugin system)
            if (auto* tool = PluginRegistry::instance().activeTool()) {
                if (!tool->update(*m_pluginContext)) {
                    // The tool finished (it ran its own commit()/cancel()); just
                    // clear it. Do NOT call deactivateTool() here — that cancels,
                    // which would undo a just-committed operation (e.g. push/pull).
                    PluginRegistry::instance().finishActiveTool();
                } else {
                    tool->renderOverlay(*m_pluginContext);
                }
            }

            // The Interactions reference is docked in the RIGHT column (above
            // Items), so it collapses with the right edge handle too.
            if (classicLayout() && !landingPageUp() && !m_rightPanelHidden &&
                m_showInteractions)
                renderInteractionsPanel();
            renderSettings();
            renderMirrorPopup();

            // Loft (plugin) "pick a second sketch" hint banner. LoftPlugin
            // triggers this when the user clicks Loft with one sketch in the
            // selection. A modal would grey out the viewport and prevent
            // picking the second sketch, so we render it as a non-blocking
            // floating window pinned near the top of the viewport instead.
            // Auto-dismisses once the selection covers two sketches (or
            // two sketch regions from distinct sketches) — at which point a
            // second click on Loft will commit. Manual dismiss via the X.
            if (m_loftPickHintPending) {
                m_loftPickHintVisible = true;
                m_loftPickHintPending = false;
            }
            if (m_loftPickHintVisible) {
                // Count distinct parent sketches in the current selection.
                int distinct = 0;
                std::vector<int> seen;
                if (m_selection) {
                    for (const auto& e : m_selection->getSelection()) {
                        if ((e.type == SelectionType::Sketch ||
                             e.type == SelectionType::SketchRegion) &&
                            e.sketchId >= 0) {
                            bool dup = false;
                            for (int x : seen) if (x == e.sketchId) { dup = true; break; }
                            if (!dup) { seen.push_back(e.sketchId); ++distinct; }
                        }
                    }
                }
                if (distinct >= 2) {
                    m_loftPickHintVisible = false;
                } else {
                    ImGuiViewport* vp = ImGui::GetMainViewport();
                    ImGui::SetNextWindowPos(
                        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                               vp->WorkPos.y + 60.0f),
                        ImGuiCond_Always, ImVec2(0.5f, 0.0f));
                    ImGui::SetNextWindowBgAlpha(0.92f);
                    ImGuiWindowFlags flags =
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_NoFocusOnAppearing |
                        ImGuiWindowFlags_NoNav;
                    bool open = true;
                    if (ImGui::Begin("Pick more sketches", &open, flags)) {
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "%s", materializr::tr("Loft needs at least two profiles."));
                        ImGui::TextWrapped("%s", materializr::tr("Ctrl-click the other sketches (or their regions) in loft order — as many as you like — then click Loft again."));
                    }
                    ImGui::End();
                    if (!open) m_loftPickHintVisible = false;
                }
            }

            // Help system: dockable user guide, modal About, modal "Check for
            // Updates" popup that fires a one-shot HTTPS GET to GitHub on open.
            m_helpPanel->render();
            m_shortcutsPanel->render();
            m_aboutDialog->render();
            // Startup dialogs take turns — two dialogs that each OpenPopup
            // every frame at the same stack level close each other endlessly:
            // neither ever draws, while the modal dim eats every touch (the
            // iPad "second launch locks up" bug). Welcome goes FIRST; sketch
            // recovery and the small-screen notice hold off while it is up
            // (they check m_welcomeScreen->isVisible()).
            renderLandingPage();   // full work-area cover; modals draw above
            renderPartsPickerDialog();
            if (m_welcomeScreen->render() == WelcomeScreen::Action::MarkSupporter) {
                m_supporter = true;
                saveAppSettings();
            }
#if defined(MZ_IOS)
            // A completed or restored Supporter purchase — from the Welcome
            // screen, or an interrupted transaction redelivered at launch.
            if (iosStoreConsumeEntitled() && !m_supporter) {
                m_supporter = true;
                saveAppSettings();
                m_welcomeScreen->setVisible(false);
                showToast("Thank you for supporting Materializr!", 6.0);
            }
#endif
            renderUpdatePopup();
            renderMultiTransformPanel();
            {
                auto ctx = iopContext();
                for (auto* c : m_iops) c->renderPanel(ctx);
            }
            renderPatternPanel();
            renderThreadPanel();
            renderSectionPanel();
            renderTextToolPanel();
            renderSvgToolPanel();
            renderAirfoilToolPanel();
            renderMirrorToolPanel();
            renderOffsetToolPanel();
            renderLoftPanel();
            renderBoundaryFillPanel();
            renderRefImagePanel();
            renderConstructionPlanePanel();
            renderConstructionAxisPanel();
            renderPrimitivePopup();
            renderStlImportDialog();
            renderUnfoldDialog();
            renderRevolvePopup();
            renderRotatePlaneAboutAxisPopup();
            renderAlignFacePopup();
            renderSketchMovePanel();
            renderSketchPatternPopup();

            // Keep measurement results in sync with the current selection,
            // then draw the panel. Cheap when inactive.
            if (m_measureTool) {
                m_measureTool->update();
                m_measureTool->renderPanel();
            }

            // History / Items / Properties are the RIGHT docked column; they
            // collapse with the right edge handle (or Hide Panels).
            if (!m_rightPanelHidden) {
                m_historyPanel->setHistoryLocked(anyInteractivePreviewActive());
                // Reverse-link viewport → history: when exactly one sketch
                // element is selected with the sketch select tool, highlight the
                // step that introduced it so it's obvious where to edit it.
                {
                    int hl = -1;
                    if (m_inSketchMode && m_sketchTool && m_activeSketch && m_history) {
                        const auto& sl = m_sketchTool->getSelectedLines();
                        const auto& sa = m_sketchTool->getSelectedArcs();
                        const auto& sc = m_sketchTool->getSelectedCircles();
                        if (sl.size() + sa.size() + sc.size() == 1) {
                            int lid = sl.empty() ? -1 : *sl.begin();
                            int aid = sa.empty() ? -1 : *sa.begin();
                            int cid = sc.empty() ? -1 : *sc.begin();
                            for (int s = 0; s < m_history->stepCount(); ++s) {
                                auto* se = dynamic_cast<const SketchEditOp*>(
                                    m_history->getStep(s));
                                if (!se || se->getTarget() != m_activeSketch) continue;
                                std::set<int> L, C, A;
                                se->getEditedElements(L, C, A);
                                if ((lid >= 0 && L.count(lid)) ||
                                    (aid >= 0 && A.count(aid)) ||
                                    (cid >= 0 && C.count(cid))) { hl = s; break; }
                            }
                        }
                    }
                    m_historyPanel->setHighlightStep(hl);
                }
                if (classicLayout() && !landingPageUp() && m_showHistory &&
                    m_historyPanel->render()) {
                    m_meshesDirty = true;
                }

                if (classicLayout() && !landingPageUp() && m_showItems) {
                    if (m_itemsPanel->render()) {
                        m_hoveredBodyId = -1;
                        m_meshesDirty = true;
                    }
                }
                m_propertiesPanel->setSketchContext(
                    m_inSketchMode, m_activeSketch.get(), m_activeSketchId,
                    m_sketchTool.get());
                if (classicLayout() && !landingPageUp() && m_showProperties &&
                    m_propertiesPanel->render()) {
                    m_meshesDirty = true;
                }
            }
            // Touch edge tabs to collapse/restore each side column (drawn on top
            // of the panels, and still visible when a side is collapsed).
            if (classicLayout() && !landingPageUp()) renderPanelCollapseHandles();

            // Plugin overlays — free-floating per-frame ImGui windows (e.g. the
            // Tutorial). Drawn after the panels so they float on top; non-modal,
            // so they never block the panels or the viewport.
            for (auto& ov : PluginRegistry::instance().overlayContributions()) {
                if (ov.render) ov.render(*m_pluginContext);
            }
            m_statusBar->setSketchMode(m_inSketchMode);
            // Project name = the save file's basename (no extension), or
            // "New project" when unsaved.
            {
                std::string pn;
                if (!m_currentProjectPath.empty()) {
                    pn = projectDisplayName();
                    auto dot = pn.rfind(".materializr");
                    if (dot == std::string::npos) dot = pn.rfind(".mzr");
                    if (dot != std::string::npos) pn = pn.substr(0, dot);
                }
                m_statusBar->setProjectName(pn);
            }
            // Spline placement is the one tool that needs a keyboard step
            // to finish — without this hint it reads as "adds dots and
            // then nothing" (Steve, verbatim).
            if (m_inSketchMode && m_sketchTool &&
                m_sketchTool->getMode() == SketchToolMode::Spline) {
                size_t nPts = m_sketchTool->splinePointsInProgress().size();
                m_statusBar->setMessage(
                    nPts == 0
                        ? "Spline: click to place control points"
                        : "Spline: click FIRST point to close the loop, "
                          "LAST point (or ENTER) to finish open");
            } else {
                m_statusBar->setMessage("");
            }
            if (classicLayout() && !landingPageUp()) m_statusBar->render();
            renderTransientToast();
            FileDialogs::render();
            renderSavePrompt();
            // Project recovery takes precedence — one modal at a time, and a
            // restored project supersedes any leftover sketch draft anyway.
            renderProjectRecoveryPrompt();
            if (!m_pendingProjectRecovery) renderSketchRecoveryPrompt();

            handleShortcuts();
        }

        if (frameTouchTheme) touchui::popChrome();

        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        endFrame();

        m_window->swapBuffers();

        // Hard frame-rate cap. Desktop GL blocks in swapBuffers on vsync, but
        // iOS's presentRenderbuffer returns immediately (no CADisplayLink in
        // this loop) — so any continuously-"active" state (a focused text
        // field, a live preview) spun this loop at uncapped rate, starving
        // the UIKit main runloop that keyboard raise, keyboard animation and
        // touch delivery all run on: focusing ANY field froze the app.
        // Sleeping out the remainder of a 60 Hz budget yields the main
        // thread to the OS every frame; where vsync already paces us the
        // remainder is ~0 and this is a no-op.
        {
            constexpr Uint32 kMinFrameMs = 16;
            const Uint32 spent = SDL_GetTicks() - frameLoopStartMs;
            if (spent < kMinFrameMs) SDL_Delay(kMinFrameMs - spent);
        }
        }
        break;   // the inner loop's own break/exit conditions reached: done
      } catch (const std::exception& e) {
        // Close the half-built ImGui frame before going round again. The throw
        // almost certainly happened between NewFrame() and Render(), leaving
        // windows on the stack; re-entering NewFrame() in that state is its own
        // crash. EndFrame() unwinds what was open.
        if (ImGuiContext* g = ImGui::GetCurrentContext()) {
            if (g->WithinFrameScope) {
                try { ImGui::EndFrame(); } catch (...) {}
            }
        }
        std::fprintf(stderr,
                     "[Recovered] exception escaped a frame: %s\n"
                     "[Recovered]   this is a BUG — the frame was abandoned and "
                     "the session kept alive; fix it at the throw site.\n",
                     e.what());
        // The stack is already unwound here, so this is the trace captured AT
        // the throw (see core/ThrowTrace.h). Printed only on escape: the same
        // throw is ordinary guarded control flow at ~40 call sites.
        const std::string trace = materializr::lastThrowTrace();
        if (!trace.empty())
            std::fprintf(stderr, "[Recovered] thrown from:\n%s", trace.c_str());
        // SHORT on purpose: the first version ran past what a tablet toast
        // shows, so the part that mattered (save a copy) was the part cut off.
        showToast("A step was skipped after an error - save a copy.", 6.0);
        // Previews/tools may be half-applied; drop the ones that hold geometry
        // so the next frame draws from the document rather than a dead handle.
        m_meshesDirty = true;
      }
    }

    // Persist preferences on a clean exit (in addition to saving on each change).
    saveAppSettings();
    // Clean exit → clear the recovery snapshots, with one deliberate
    // exception: a DIRTY INACTIVE tab keeps its file. The quit prompt only
    // covers the active project, so an unsaved background tab was never
    // offered a save — deleting its snapshot here would silently destroy its
    // only copy. It is offered back on the next launch instead. (The active
    // session always clears: if it was dirty, the user answered the prompt.)
    for (size_t i = 0; i < m_sessions.size(); ++i) {
        const auto& s = m_sessions[i];
        if (i != m_activeSession) {
            const bool dirty =
                (s->history && s->history->currentStep() != s->savedAtHistoryStep) ||
                s->unsavedNonHistoryChanges;
            if (dirty) continue;
        }
        materializr::clearProjectRecovery(s->recoveryIndex);
    }
}

} // namespace materializr
