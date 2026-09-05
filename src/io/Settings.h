#pragma once
#include "../platform_defs.h"
#include <string>
#include <vector>

namespace materializr {

// The three interface layouts (Settings → Appearance → Interface). One
// mutually-exclusive choice; add future layouts to the end (the numeric
// values line up with the Settings combo order). Each layout's chrome lives
// in src/app/layout/<name>/ — see src/app/layout/LayoutCommon.h for the
// keep-in-lockstep contract when adding features or plugin entry points.
//   Classic — desktop menu bar + docked panels + status bar.
//   Modern  — top app bar + tool rail + right side panel.
//   ImTouch — near-zero chrome: full-bleed viewport, floating overlays
//             (the name is an homage to ImGui).
enum class UiLayout { Classic = 0, Modern = 1, ImTouch = 2 };

// User-facing application preferences that persist between launches. Defaults
// here are the out-of-the-box behaviour and are also the fallback whenever a
// key is missing or unreadable in the settings file.
struct AppSettings {
    int  theme              = 0;    // 0 = Dark, 1 = Light
    // LEGACY — read and written so an existing settings file round-trips, but
    // NOTHING consumes it any more. The Linux desktop scale used to be a manual
    // Low/High pick here (Settings → Appearance, plus a first-run picker)
    // because DPI auto-detection was believed unreliable on X11/Xwayland. It
    // isn't: SDL reads the per-OUTPUT RandR physical size and gets the true DPI
    // (see Window::linuxAutoUiScale), so the scale is detected and both pickers
    // are gone. --ui-scale is the escape hatch. Don't wire this back up.
    float desktopUiScale    = 1.0f;
    // Touch mode: large UI + touch-gesture interaction. Defaults on for Android,
    // off elsewhere; a saved setting so a tablet with a mouse/keyboard can run
    // the desktop model. Drives materializr::setTouchMode() at startup.
#if defined(MZ_MOBILE)
    bool touchMode          = true;
#else
    bool touchMode          = false;
#endif
    // Interface layout (see the UiLayout enum above). Orthogonal to touchMode
    // (layout vs input model); switches live, no restart. Serialized as the
    // string key `uiLayout = classic | modern | imtouch`; older builds' bool
    // pair imTouchUi/imTouchLite is still read as a fallback. iPad ships
    // touch-first, so it defaults to the im-touch shell; a saved setting
    // still wins, so Settings → Appearance can switch back.
#if defined(MZ_IOS)
    UiLayout uiLayout       = UiLayout::ImTouch;
#else
    UiLayout uiLayout       = UiLayout::Classic;
#endif
    // im-touch layout only: the transparent model tree (Bodies/Sketches/
    // Construction) floating on the right edge. Toggled by the list button
    // in the top-right cluster.
    bool imTouchTree        = true;
    // im-touch layout only: the Fusion-style history timeline (one box per
    // history step) floating along the bottom edge. Toggled by the clock
    // button in the top-right cluster.
    bool imTouchTimeline    = true;
    int  touchRightTab      = 0;    // shell right panel: 0 = Items, 1 = History & Properties
    // Shell right-panel width in logical px (× uiScale at use) — written by
    // the panel's left-edge drag splitter / edge tab.
    float touchRightW       = 300.0f;
    // Shell tool-rail width, same convention (edge-tab drag).
    float touchRailW        = 92.0f;
#if defined(MZ_MOBILE)
    // Touch-first default: trackpad mode (one-finger drag = orbit, two-finger
    // pan/zoom). Just the first-run default — the Settings dialog can rebind to
    // Middle/Right for an attached mouse or trackpad, and the choice persists.
    int  orbitButton        = 0;    // ImGuiMouseButton: 0=Left, 1=Right, 2=Middle
    int  panButton          = 0;
#else
    int  orbitButton        = 2;    // ImGuiMouseButton: 0=Left, 1=Right, 2=Middle
    int  panButton          = 1;
#endif
    bool levelOrbit         = true; // turntable (level) vs free trackball orbit
    // Uniform multiplier on camera input deltas (orbit / pan / zoom). 1.0 =
    // the hard-coded baseline; below 1 calms a touchy trackpad, above 1
    // snappier. Persisted so the user doesn't have to re-set it every launch.
    float mouseSensitivity  = 1.0f;
    bool autosaveEnabled    = false;
    int  autosaveIntervalSec = 120;
    bool invertCubeDrag     = false; // ViewCube drag-to-orbit direction
    // Max seconds between two clicks for them to register as a double-click
    // (ImGui default 0.30). Trackpad users often double-tap slower than a
    // mouse; raising this stops a slow body double-click from being seen as
    // two single clicks (which would cycle-select past the body to the sketch
    // behind it).
    float doubleClickTimeSec = 0.30f;
    // Seconds a fillet may spend proving it terminates before it is refused.
    // OCCT's blend cannot be interrupted, so this is the only bound on it —
    // raise it for heavy bodies, lower it if a stalled fillet feels sluggish.
    float filletProbeSeconds = 2.5f;

    // --- Rendering ---
    float lightAmbient   = 0.40f; // 0..1 base illumination; higher = softer shadows
    bool  lightHeadlight = false; // key light tracks the camera (no large shadows)
    bool  lightFill      = true;  // soft opposing fill light to lift dark sides
    int   msaaSamples    = 4;     // viewport anti-aliasing: 0=off, 2, 4, 8
    int   meshQuality    = 1;     // tessellation density: 0=Low,1=Medium,2=High,3=Ultra
    float selectionLineWidth = 3.0f; // px width of highlighted edges/body outlines (1..10)
    float sketchLineWidth = 2.5f;    // px width of sketch geometry — thicker reads better over the grid (1..6)
    float sketchGridOpacity = 0.55f; // opacity of the sketch-plane grid (0..1)
    float sketchGridThickness = 1.0f; // grid line-width multiplier (0.1..2)
    bool  smallScreenWarned = false; // user dismissed the "designed for larger screens" notice
    bool  leftPanelHidden   = false; // Tools column collapsed (max-viewport / small-screen fallback)
    bool  rightPanelHidden  = false; // Items/History/Properties column collapsed
    // Per-panel visibility (Settings > Panels). Default all on.
    bool  showTools         = true;
    bool  showInteractions  = true;
    bool  showHistory       = true;
    bool  showItems         = true;
    bool  showProperties    = true;
    // Touch-mode camera sensitivity multipliers (1.0 = default).
    float touchOrbitSens    = 1.0f;
    float touchPanSens      = 1.0f;
    float touchZoomSens     = 1.0f;
    bool  showToolbarTooltips = true; // hover-tip describing each toolbar button
    bool  showFps           = true;   // small FPS readout (im-touch layout, top-centre)

    // --- Session ---
    // Restore the previous session on launch: every project that was open in a
    // TAB comes back, instead of the home screen. (Named for its single-project
    // past — the key is kept so users who had it on keep it on.)
    bool  autoOpenLastProject = false;
    // Path of the project currently open. Updated on save / load; cleared on
    // File → Close Project. On launch (with autoOpenLastProject on) this is
    // read and the file is loaded if it still exists — so "I closed the
    // project before quitting" produces an empty launch next time, and
    // "I just quit while working" reopens where you left off. Kept as the
    // single-tab case of sessionPaths below, and as the fallback when a build
    // that predates tabs wrote the settings file.
    std::string lastProjectPath;
    // Every open tab's project ref, in tab order, as of the last settings
    // write; `sessionActive` is the index that was in front. Unsaved tabs
    // contribute an empty entry and are skipped on restore — their work lives
    // in the recovery snapshots, which have their own prompt. Machine-local
    // (omitted from JSON import/export). Serialized as indexed sessionN_path.
    std::vector<std::string> sessionPaths;
    int sessionActive = 0;
    // Directory the file picker last landed in (open OR save). Reused as
    // pfd's default_path on the next open / save so the user doesn't have
    // to re-navigate to their projects folder every time. Machine-local
    // (omitted from JSON import/export, same as lastProjectPath).
    std::string lastFileDir;

    // Recently opened / saved projects, most-recent-first (capped; see
    // Application::addRecentProject). `ref` is what re-opens the project — a
    // filesystem path on desktop, or a persisted SAF content:// URI on
    // Android — and `name` is the display label. Machine-local (omitted from
    // JSON import/export like lastProjectPath). Serialized as indexed
    // recentN_ref / recentN_name keys.
    struct RecentProject { std::string ref; std::string name; };
    std::vector<RecentProject> recentProjects;

    // Hit the GitHub releases API at startup and pop a "newer release
    // available" dialog if the running build is older. Off in --safe-mode
    // (no surprise network calls when the user is recovering from a crash).
    bool  checkForUpdatesOnLaunch = true;
    // Opt in to the beta channel: the update check also considers GitHub
    // pre-releases (tags like 1.3.0-beta.1) instead of only final releases.
    // Off by default so stable users are never offered test builds.
    bool  includePrereleases      = false;
    // The user supports the project (tapped "I already support" in the launch
    // support prompt; store builds will later also set it from a completed or
    // restored tip in-app purchase). Permanently silences the every-launch
    // support prompt.
    bool  supporter               = false;

    // --- Snap / grid (persisted) ---
    // Snap-to-grid toggle and step (mm) shared by the sketch grid and the
    // body/sketch gizmo translate. Persisting these means the user doesn't
    // have to re-enable snap and re-pick a 1 mm step every launch.
    bool  snapToGrid    = true;
    // In DISPLAY UNITS, not millimetres: "1" means one of whatever unit is
    // showing, which is what the presets (0.1 / 0.5 / 1 / 10) offer. Converted
    // to mm on load, after the display unit is applied.
    float sketchGridStep = 1.0f;

    // --- Sketch inferences ---
    // Default inference level applied at launch and whenever no project
    // override exists. 0 = Full (includes hover-to-charge), 1 = Reduced
    // (classic guides), 2 = Off (grid + endpoint only). Mirrors
    // SketchTool::InferenceLevel; kept as int to keep this header free of
    // a sketch-tool dependency.
    int  inferenceLevel = 0;
    // Whether the sketch toolbar shows the live Full/Reduced/Off cycle
    // button. Off lets users who set the level once in Settings declutter
    // the toolbar; on (default) keeps the per-session live toggle visible.
    bool showInferenceToolbarToggle = true;
    // Line angle-snap increment in degrees (0 = off). The line tool snaps its
    // direction to multiples of this from the segment anchor. Default 15.
    int  angleSnapDeg = 15;

    // --- Language ---
    // UI language index; mirrors materializr::Lang (0 = English, then Spanish,
    // Portuguese, French, German, Italian). Kept as an int so this header does
    // not depend on i18n.h. -1 means "never chosen", which is what makes the
    // setup wizard ask on first run.
    int  language = -1;
    // Display unit for every length readout and input: materializr::LengthUnit
    // (0 mm, 1 cm, 2 m, 3 in, 4 ft). An int for the same reason language is —
    // this header stays free of core/Units.h. The model is always mm; this only
    // changes what the user sees and types.
    int  displayUnit = 0;

    // --- STL import ---
    // Default fidelity for STL import, 0..1 (coarse/fast .. faithful/slow). Pre-
    // fills the import dialog's accuracy slider. See StlIO::import.
    float stlImportAccuracy = 0.5f;
    // Whether imported mesh (STL) bodies draw their facet wireframe. Off gives a
    // clean shaded body to sketch on; the merged flat-region edges are still
    // useful, so it defaults on and the import dialog/Settings can disable it.
    bool  meshShowWireframe  = true;
};

// Reads/writes AppSettings as a simple `key = value` text file. The reader is
// intentionally tolerant: unknown keys are ignored and missing/garbled keys
// fall back to defaults, so a settings file written by a newer or older build
// never prevents the app from starting. The writer always emits the full set
// of currently-known keys, so new settings are added to the file automatically.
namespace SettingsIO {
    // Default location: $XDG_CONFIG_HOME/materializr/settings.cfg
    // (or ~/.config/materializr/settings.cfg).
    std::string defaultPath();

    AppSettings load(const std::string& path);
    bool        save(const std::string& path, const AppSettings& s);

    // Portable import/export as JSON, for backing up preferences or moving them
    // between machines (File → Import/Export Settings). Same tolerance contract
    // as load(): unknown keys are ignored and missing keys keep their defaults.
    // `lastProjectPath` is deliberately omitted — it is machine-specific session
    // state, not a portable preference. On a read error or unparseable file,
    // importJson returns defaults and sets *ok (if provided) to false.
    bool        exportJson(const std::string& path, const AppSettings& s);
    AppSettings importJson(const std::string& path, bool* ok = nullptr);
}

} // namespace materializr
