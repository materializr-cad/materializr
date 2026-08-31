#pragma once
#include <functional>
#include <string>
#include <vector>

class SelectionManager;
class History;

namespace materializr {

class PluginContext;

enum class ToolAction {
    None,
    // Sketch tools (still dispatched via ToolAction — tightly coupled to viewport)
    StartSketch, StartSketchXY, StartSketchXZ, StartSketchYZ,
    SketchOnFace, SelectSketch, Line, Circle, Rectangle, Arc, Spline, Polygon, Trim, SketchText,
    SketchSvg, SketchAirfoil, SketchDimension,
    FinishSketch, ExitSketchDiscard, EditSketch, ExtrudeSketch, SubtractSketch, PushPull, MoveFace, LookAtSketch,
    SketchCopy, SketchMirror, SketchOffset, SketchLinearPattern, SketchRadialPattern,
    // Cycle the drawing-inference level Full → Reduced → Off.
    SketchCycleInference,
    // Toggle the active rect/circle draw origin (Corner↔Center / Center↔2-Point).
    SketchToggleDrawOrigin,
    // 3D tools that still need the old dispatch path. (Face extrude is owned by
    // ExtrudePlugin's toolbar button; the inline interactive extrude is reached
    // from sketch-extrude and the viewport context menu, not via a ToolAction.)
    Fillet, Chamfer, EditFilletChamfer, EditDiameter, Shell, Thread, Taper, ScaleFace,
    ProjectSketch, RemoveFace, MergeFaces,
    // Gizmo modes + Mirror
    Move, Rotate, Scale, Mirror, Revolve,
    // Split a body with a plane you place (axis + offset, ghost-plane preview).
    // Was three plugin buttons that each cut through the bbox centre.
    Split,
    // Sketch constraints (operate on the current SketchTool element selection).
    // All opt-in — none of them runs unless the user clicks the button.
    SketchConstrainCoincident, SketchConstrainHorizontal, SketchConstrainVertical,
    SketchConstrainParallel, SketchConstrainPerpendicular, SketchConstrainEqual,
    SketchConstrainFixed,
    // Dimension constraints — captured at current geometry value, so adding
    // one is non-destructive. User edits the displayed value later.
    SketchDimDistance, SketchDimAngle, SketchDimRadius,
    // Geometric constraints that need circle/arc selection (Session 4 catalogue).
    SketchConstrainTangent, SketchConstrainConcentric,
    // General
    Measure, ResetCamera,
    // Fabrication: unfold a body into a flat pattern for laser/CNC/cut templates.
    Unfold
};

class Toolbar {
public:
    Toolbar();

    void setSelectionManager(const SelectionManager* sel);
    void setHistory(const ::History* h) { m_history = h; }
    void setPluginContext(PluginContext* ctx) { m_pluginCtx = ctx; }

    ToolAction render();

    // The modern/im-touch tool rail: the tools of the current selection
    // context (mirrors render()'s dispatch — keep the two in sync). Icons are
    // MZ_ICON_* strings, labels are short rail captions. Plugin toolbar
    // contributions are included with pluginIndex >= 0 (an index into
    // PluginRegistry::toolbarContributions()); the shells fire those through
    // fireRailPlugin() instead of handleToolAction.
    struct RailTool {
        const char* icon;
        const char* label;
        ToolAction  action;
        bool        active = false;   // highlight (current sketch tool)
        const char* tip = nullptr;    // hover tooltip (im-touch shell)
        int         pluginIndex = -1; // >= 0: plugin contribution, not a ToolAction
    };
    std::vector<RailTool> railTools() const;
    // Fire a plugin RailTool (activate its tool / run its action) — the rail
    // twin of renderPluginButtons' click handling.
    void fireRailPlugin(int index);

    // ── The catalogue is the single source of WHICH tools a context offers ──
    //
    // railTools() above is that catalogue. It used to serve the modern and
    // im-touch rails ONLY, while the classic Tools palette ran a completely
    // parallel set of render*Tools() functions — two hand-maintained lists
    // that happened to agree. They did not always: Push/Pull was missing from
    // classic, "Move Hole" shipped to the rails only, and Toolbar.cpp's own
    // comments record both. Every such gap is invisible until a user in one
    // layout can't reach a feature.
    //
    // Now classic consults the catalogue too. It keeps its OWN presentation —
    // section headers, the Move/Rotate/Scale row, the Fabrication group — and
    // asks the catalogue only whether a tool applies right now. A tool added
    // to railTools() therefore appears in all three layouts: with bespoke
    // placement in classic if someone wrote a button for it, and via
    // renderCatalogRemainder() at the end of its section if nobody did.
    //
    // Valid only during render() (the catalogue is snapshotted per frame).
    bool catalogOffers(ToolAction a) const;

    void setSketchMode(bool active);
    bool isSketchMode() const;

    void setGridStep(float step) { m_gridStep = step; }
    float getGridStep() const { return m_gridStep; }

    void setCameraOrtho(bool ortho) { m_cameraOrtho = ortho; }

    void setSnapToGrid(bool snap) { m_snapToGrid = snap; }
    bool getSnapToGrid() const { return m_snapToGrid; }

    // HiDPI/touch scale for button heights (1.0 on desktop). Without this the
    // fixed-height buttons stay 30px tall while the font scales up on a tablet,
    // so the labels overlap. bh() scales a base height by it.
    void setUiScale(float s) { m_uiScale = s; }

    // Set each frame by Application from a cheap face/body inspection: shows
    // the "Edit Diameter" button in Face Operations when the picked face is a
    // cylinder on a recognized cylinder-or-tube body.
    void setCanEditDiameter(bool b) { m_canEditDiameter = b; }
    // The selected face is flat — Push/Pull is only offered on flat faces (a
    // curved/fillet face makes the boolean freak out; #28).
    void setSelFacePlanar(bool b) { m_selFacePlanar = b; }
    // Selected edges resolve to one hole's rim, so Move has a meaning for them
    // (tilt / reshape / slide — MoveHoleOp::classifyRimEdges picks which).
    void setSelEdgeIsHoleRim(bool b) { m_selEdgeIsHoleRim = b; }
    void setSelFaceIsHoleWall(bool b) { m_selFaceIsHoleWall = b; }
    void setSelectedFaceFrozenRound(bool b) { m_selFrozenRound = b; }

    // Set each frame by Application: true when the selected sketch / sketch
    // region is still bound to a body (getSourceBody() >= 0 && !detached). It
    // gates which tool the rail offers — Push/Pull (modify the host body) for a
    // body-attached sketch, Extrude (make a new body) for a standalone one. A
    // face selection is unaffected: it always offers both.
    void setSelectedSketchAttached(bool b) { m_selSketchAttached = b; }

    // Active SketchToolMode (int — Toolbar avoids depending on SketchTool.h).
    // Matches SketchToolMode enum: 0=None, 1=Select, 2=Line, 3=Circle,
    // 4=Rectangle, 5=Arc, 6=Spline, 7=Polygon, 8=Trim. Used to draw a
    // highlight border around the matching button so the active tool is
    // unambiguous at a glance.
    void setActiveSketchMode(int mode) { m_activeSketchMode = mode; }

    // Current sketch-element selection counts. Application updates these each
    // frame from SketchTool so the Constraints section of the sketch toolbar
    // can show only the buttons that match the selection arity, and stay
    // hidden entirely when nothing is selected.
    void setSketchSelectionCounts(int points, int lines,
                                  int circles = 0, int arcs = 0) {
        m_selPoints  = points;
        m_selLines   = lines;
        m_selCircles = circles;
        m_selArcs    = arcs;
    }
    // Live inference level shown on the sketch toolbar toggle. Int mirrors
    // SketchTool::InferenceLevel (0=Full, 1=Reduced, 2=Off, 3=Max) to keep Toolbar
    // free of a SketchTool.h dependency.
    void setInferenceLevel(int lvl) { m_inferenceLevel = lvl; }
    // Side count chosen from the Polygon popout; read by the app when it
    // handles the resulting ToolAction::Polygon.
    int  getRequestedPolygonSides() const { return m_requestedPolygonSides; }
    // Lets the im-touch rail's Polygon sides popout set the count too (the
    // classic toolbar sets m_requestedPolygonSides inline), so all three
    // layouts drive the same polygon flow.
    void setRequestedPolygonSides(int n) { m_requestedPolygonSides = n; }
    // Live rect/circle draw origin shown on the per-tool toggle. Ints mirror
    // SketchTool::RectMode (0=Corner,1=Center) and CircleMode (0=Center,1=2-Point).
    void setRectMode(int m) { m_rectMode = m; }
    void setCircleMode(int m) { m_circleMode = m; }
    // Off hides the inference cycle button from the sketch toolbar so a user
    // who set the level once in Settings can declutter. Default on.
    void setShowInferenceToggle(bool b) { m_showInferenceToggle = b; }
    // Most recent SketchSolver state: 0 = Fully, 1 = Under, 2 = Over (matches
    // the SketchState enum). Drives the small status badge at the top of the
    // sketch toolbar so the user knows whether the current constraint set is
    // satisfiable / has slack / is impossible. -1 = no status (no constraints).
    void setSketchSolverState(int state) { m_sketchSolverState = state; }
    void setSketchSolverDof(int dof) { m_sketchSolverDof = dof; }

    // When true (the default) every toolbar button shows a hover tooltip
    // describing what it does. Off via Settings → Interface for users who
    // don't want them. Settable any frame; takes effect on the next frame.
    void setShowTooltips(bool b) { m_showTooltips = b; }

private:
    // Scale a base button height by the UI scale (1.0 on desktop).
    float bh(float h) const { return h * m_uiScale; }
    float m_uiScale = 1.0f;

    const SelectionManager* m_selection = nullptr;
    const ::History* m_history = nullptr;
    PluginContext* m_pluginCtx = nullptr;
    bool m_sketchMode = false;
    float m_gridStep = 1.0f;
    bool m_cameraOrtho = true;
    bool m_snapToGrid = true;
    bool m_canEditDiameter = false;
    bool m_selFacePlanar = false;  // selected face is flat (gates Push, #28)
    bool m_selEdgeIsHoleRim = false; // selected edges are one hole's rim
    bool m_selFaceIsHoleWall = false; // selected curved face is a hole's bore
    bool m_selFrozenRound  = false;
    bool m_selSketchAttached = false; // selected sketch still drives a body (see setter)
    bool m_showTooltips = true;
    int  m_activeSketchMode = 0; // SketchToolMode (see setActiveSketchMode)
    int  m_selPoints = 0;        // sketch points currently selected (see setSketchSelectionCounts)
    int  m_selLines = 0;         // sketch lines currently selected
    int  m_selCircles = 0;       // sketch circles currently selected
    int  m_selArcs = 0;          // sketch arcs currently selected
    int  m_sketchSolverState = -1; // -1=none, 0=Fully, 1=Under, 2=Over
    int  m_sketchSolverDof = 0;
    int  m_inferenceLevel = 0; // 0=Full, 1=Reduced, 2=Off (see setInferenceLevel)
    int  m_requestedPolygonSides = 6; // last pick from the Polygon popout
    int  m_rectMode = 0;       // 0=Corner, 1=Center (see setRectMode)
    int  m_circleMode = 0;     // 0=Center, 1=2-Point (see setCircleMode)
    bool m_showInferenceToggle = true; // see setShowInferenceToggle

    ToolAction renderSketchTools();
    ToolAction renderSketchSelectedTools();
    ToolAction renderPlaneSelectedTools();
    ToolAction renderAxisSelectedTools();
    ToolAction renderSketchRegionTools();
    ToolAction renderNoSelectionTools();
    // primaryContext=false means "rendered as a FALL-THROUGH under a Face
    // selection, purely for the Transform row". It then suppresses the
    // HasBodies plugin contributions (Split / Duplicate / Pattern — whole-body
    // operations that don't apply while the user is interacting with a face),
    // the Fabrication group, and the catalogue-remainder net, which would
    // otherwise re-render every FACE tool renderFaceTools just placed.
    ToolAction renderBodyTools(bool primaryContext = true);
    ToolAction renderFaceTools();
    ToolAction renderEdgeTools();

    void renderPluginButtons(int contextMask);
    // Single "Add Plane…" button + dropdown listing the construction-plane
    // creation modes the current selection supports. Shared across the face /
    // plane / edge / axis context renderers; renders nothing when no mode
    // applies.
    // OCCT primitives (Box / Cylinder / Sphere / Cone / Torus) — a single
    // "Primitives..." button that opens a popup with one menu item per kind.
    // Each item fires a requestInteractiveOp the PrimitivesPlugin set up
    // and Application opens the per-kind parameter popup. Keeps the empty-
    // canvas toolbar from sprouting five sibling buttons.
    void renderPrimitivesMenu();
    void renderAddPlaneMenu();
    // Single "Add Axis…" button + dropdown listing the construction-axis
    // creation modes the current selection supports (cylinder centreline,
    // straight edge, two vertices, face normal, two-plane intersection).
    void renderAddAxisMenu();

    void tip(const char* text) const;

    // The catalogue for THIS frame, snapshotted at the top of render() so the
    // per-context renderers can consult it without rebuilding it per button
    // (it walks the plugin registry and, on a face, probes history for the
    // owning fillet/chamfer).
    std::vector<RailTool> m_catalog;
    // Render every catalogue entry the caller did NOT handle itself, as a
    // plain full-width button. This is the anti-drift net: a tool added to
    // railTools() shows up in classic even if nobody wrote a bespoke button
    // for it. `handled` lists the actions the caller has already placed — or
    // deliberately suppresses (classic drops Measure, which lives in the View
    // menu). Plugin entries are skipped: classic renders those through
    // renderPluginButtons at its own chosen point.
    ToolAction renderCatalogRemainder(std::initializer_list<ToolAction> handled);
};

} // namespace materializr
