#pragma once
#include "Sketch.h"
#include "SketchSolver.h"
#include "SketchOffset.h"
#include "SvgImport.h"
#include "AirfoilImport.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <cstddef>   // size_t

namespace materializr {

// APPEND ONLY. Toolbar mirrors the active tool as this enum's raw INDEX
// (Application::setActiveSketchMode) and the sketch toolbar hardcodes those
// indices, so inserting a mode in the middle silently highlights the wrong
// button -- Dimension would become 13 while the button still tests 12.
enum class SketchToolMode { None, Select, Line, Circle, Rectangle, Arc, Spline, Polygon, Trim, Text, Svg, Mirror, Dimension, Airfoil, Offset, Point, Fill, Reference, Corner };

enum class DimEntityKind { None, Point, Line, Circle, Arc };
struct DimPick { DimEntityKind kind = DimEntityKind::None; int id = -1; };

// A pick set resolved into the constraint it would create. measured is the
// current geometry value: mm for distances, RADIUS in mm for Radius (UI
// doubles it for display), SIGNED radians for Angle (line B rel. line A).
struct PendingDimension {
    ConstraintType type = ConstraintType::Distance;
    int entityA = -1, entityB = -1;
    double measured = 0.0;
    bool valid = false;
};

// Shared "close enough to parallel/anti-parallel to dimension as a gap
// rather than an angle" threshold (1 degree), in radians. Single source of
// truth for SketchTool::resolveDimension's line-line branch AND
// Application::applyPendingDimension's mirrored-DistancePointLine dedup
// (which must not treat two genuinely angled lines as the same gap just
// because their picked point/line ids happen to cross-reference each
// other's endpoints - see SketchTool::linesParallelWithinDimTol). Written as
// a literal rather than via M_PI/180 so this header doesn't need to pull in
// <cmath> or the M_PI portability guard every other file including it uses.
constexpr double kDimParallelTolRad = 0.017453292519943295; // 1.0 deg

enum class DimPhase { PickFirst, PickSecondOrPlace, PlaceLabel };

// One drawing-time alignment hint. Inferences are transient - they describe
// what the cursor IS aligned to right now, get drawn as coloured ghost lines /
// markers, and disappear after the click is placed. No constraint metadata is
// stored on the resulting geometry; the placed point is just a point.
struct InferenceGuide {
    enum Kind {
        Endpoint,       // cursor snapped onto an existing sketch point
        Midpoint,       // cursor snapped onto the midpoint of a line / arc
        OnLine,         // cursor projected onto an existing line (not at an endpoint/midpoint)
        OnCircle,       // cursor landed on a circle's or arc's rim → same diamond marker as OnLine
        AxisHFromPoint, // cursor's Y aligns with an existing point's Y → red horizontal guide
        AxisVFromPoint, // cursor's X aligns with an existing point's X → green vertical guide
        PerpToPrev,        // cursor is on the perpendicular ray from the chain's previous segment → orange guide
        ParallelToPrev,    // cursor is on the parallel-to-previous ray → magenta guide
        AngleSnap,         // cursor is on a 15° / 30° / 45° / etc. ray from the chain anchor → grey guide
        OnLineExtension,   // cursor is on the infinite extension of an existing line → lavender dashed guide
        TangentToCircle,   // cursor lies on a tangent line touching a circle, arc or spline → dashed guide
        PerpToRef,         // cursor is on the perpendicular ray through a hover-charged point → cyan guide
        Symmetry,          // cursor snapped to the mirror image of an existing point across a centreline / axis-aligned line → purple guide
        CornerBisector,    // anchor is a vertex of two segments; cursor is on the line that bisects their angle → dashed guide
        CornerTangent,     // same vertex, the direction a smooth curve through it would take (perpendicular to the bisector) → dashed guide
    };
    Kind kind;
    glm::vec2 from;    // ghost guide line start (sketch-space)
    glm::vec2 to;      // ghost guide line end (typically the snapped cursor)
    int refId = -1;    // id of the referenced point / line, or -1 if not applicable
};

class SketchTool {
public:
    SketchTool();

    void setSketch(Sketch* sketch);
    void setSolver(SketchSolver* solver);

    void setMode(SketchToolMode mode);
    SketchToolMode getMode() const;

    // "Draw as construction": while on, everything the drawing tools (Line,
    // Circle, Rectangle, Arc, Spline, Polygon) create is construction geometry
    // (see Sketch::hasConstructionGeometry). Sticky for the sketch session; the
    // app turns it off when the sketch closes.
    void setConstructionDraw(bool on) { m_constructionDraw = on; }
    bool getConstructionDraw() const { return m_constructionDraw; }
    // Flag whatever was created since `firstId` per the mode above. Called by
    // the input entry points; public only so a scope guard outside the class
    // body can reach it.
    void flagNewElements(int firstId);

    // Input events (in sketch 2D coordinates). `addToSel` is the modifier state
    // for Select-mode multi-pick (Ctrl held); ignored by other tools.
    void onMouseDown(glm::vec2 pos, bool addToSel = false);
    void onMouseMove(glm::vec2 pos);
    void onMouseUp(glm::vec2 pos);

    // --- Sketch-element selection (Select mode) ---
    const std::set<int>& getSelectedPoints()  const { return m_selectedPoints; }
    const std::set<int>& getSelectedLines()   const { return m_selectedLines; }
    const std::set<int>& getSelectedCircles() const { return m_selectedCircles; }
    const std::set<int>& getSelectedArcs()    const { return m_selectedArcs; }
    const std::set<int>& getSelectedSplines() const { return m_selectedSplines; }
    void clearElementSelection() {
        m_selectedPoints.clear();
        m_selectedLines.clear();
        m_selectedCircles.clear();
        m_selectedArcs.clear();
        m_selectedSplines.clear();
    }
    bool hasElementSelection() const {
        return !m_selectedPoints.empty() || !m_selectedLines.empty() ||
               !m_selectedCircles.empty() || !m_selectedArcs.empty() ||
               !m_selectedSplines.empty();
    }
    // Public wrapper over the snap-tolerance coincident-point lookup, so
    // generated geometry (e.g. Mirror) can weld a new vertex onto an existing
    // one instead of leaving a duplicate. Returns the point id, or -1.
    int coincidentPoint(glm::vec2 pos, int excludeId = -1) const {
        return findCoincidentPoint(pos, excludeId);
    }
    // The generated-geometry counterpart (fixed model radius, zoom-independent).
    int exactCoincidentPoint(glm::vec2 pos, int excludeId = -1) const {
        return findExactCoincidentPoint(pos, excludeId);
    }
    // Select every element in the active sketch (used by Ctrl+A / double-click).
    void selectAll();
    // Replace the current selection with the given ids.
    void setSelection(const std::set<int>& pointIds, const std::set<int>& lineIds) {
        m_selectedPoints = pointIds;
        m_selectedLines = lineIds;
        m_selectedCircles.clear();
        m_selectedArcs.clear();
        m_selectedSplines.clear();
    }
    // Full replace, including curve types - used by box-select so a drag can
    // catch circles / arcs / splines, not just points + lines.
    void setSelectionFull(const std::set<int>& pointIds, const std::set<int>& lineIds,
                          const std::set<int>& circleIds, const std::set<int>& arcIds,
                          const std::set<int>& splineIds) {
        m_selectedPoints  = pointIds;
        m_selectedLines   = lineIds;
        m_selectedCircles = circleIds;
        m_selectedArcs    = arcIds;
        m_selectedSplines = splineIds;
    }
    void onConfirm(); // Enter/double-click to finish
    void onCancel();  // Escape to cancel

    // Type a dimension during placement: completes the current shape using `value` as the
    // primary dimension (line length, circle radius, polygon radius, rectangle half-side)
    // anchored at the first click, in the direction of the current cursor.
    // Returns true if the shape was created.
    bool applyDimension(float value);

    // Anchor point of the current placement (the first click). Used by the dimension input UI.
    glm::vec2 getFirstClick() const { return m_firstClick; }
    glm::vec2 getCurrentPos() const { return m_currentPos; }
    glm::vec2 getSecondClick() const { return m_secondClick; }
    int getClickCount() const { return m_clickCount; }
    // Current side count for the polygon tool. Modifiable from the dimension
    // popup without committing the placement, so the user can preview a new
    // count before clicking.
    int getPolygonSides() const { return m_polygonSides; }

    // How the Polygon tool turns the cursor (and the panel) into a polygon.
    // By default the size and the angle of vertex 0 follow the cursor, as they
    // always did: the cursor is the circumscribed radius. The panel can read
    // the size as a radius or a diameter, of the circle through the corners
    // (circumscribed) or the one touching the sides (inscribed), and either
    // value can be typed exactly instead of dragged.
    struct PolygonSpec {
        bool diameter = false;        // size is a diameter (else a radius)
        bool inscribed = false;       // size is of the inscribed (side-touching) circle
        bool hasSize = false;         // `size` was typed: it overrides the cursor
        double size = 0.0;            //   in mm, in the sense above
        bool hasAngle = false;        // `angleDeg` was typed: it overrides the cursor
        double angleDeg = 0.0;        //   direction of vertex 0, degrees from sketch +X
    };
    PolygonSpec& polygonSpec() { return m_polygonSpec; }
    const PolygonSpec& polygonSpec() const { return m_polygonSpec; }
    // Circumscribed radius and vertex-0 angle (radians) for a cursor at `cursor`
    // with the first click at `centre`, after the spec has had its say.
    void polygonResolve(glm::vec2 centre, glm::vec2 cursor, float& circumRadius,
                        float& rotationRad) const;
    // The size as the spec reads it (radius or diameter, circum- or inscribed)
    // for a circumscribed radius - what the panel shows while the size is
    // still following the cursor.
    double polygonSizeFor(double circumRadius) const;
    // Place the polygon now, from the centre click and the current spec and
    // cursor - the panel's Place button. False when nothing is being placed.
    bool commitPolygonNow();
    void setPolygonSides(int n) { m_polygonSides = (n < 3) ? 3 : n; }

    // Typed values for the Line / Circle / Rectangle / Arc panels - the same idea
    // as PolygonSpec. Each field, once typed, overrides what the cursor would
    // have said; the rest keeps following the cursor. Used for the preview and
    // for the commit, so the two cannot disagree.
    //   hasLen   Line length, Circle size (a radius, or a diameter when
    //            circleDiameter()), Arc chord. mm.
    //   hasAngle Line direction / Rectangle rotation, degrees from sketch +X.
    //   hasW/H   Rectangle width (along the rotated X) and height.
    //   hasApex  Arc: the sweep in degrees or the radius in mm, per ArcDimMode.
    // The cursor still chooses the QUADRANT / SIDE: a size has no direction.
    struct ShapeSpec {
        bool hasLen = false;   double len = 0.0;
        bool hasAngle = false; double angleDeg = 0.0;
        bool hasW = false;     double w = 0.0;
        bool hasH = false;     double h = 0.0;
        bool hasApex = false;  double apex = 0.0;
        bool any() const { return hasLen || hasAngle || hasW || hasH || hasApex; }
    };
    ShapeSpec& shapeSpec() { return m_shapeSpec; }
    const ShapeSpec& shapeSpec() const { return m_shapeSpec; }
    // What the panel shows while a value is still following the cursor.
    struct ShapeNow { double len = 0.0, angleDeg = 0.0, w = 0.0, h = 0.0, apex = 0.0; };
    ShapeNow shapeMeasure() const;
    // Where the moving end of the current placement lands once the spec has had
    // its say (Line end, Circle rim / diameter end, Arc chord end or apex).
    glm::vec2 shapeResolve(glm::vec2 cursor) const;
    // The four corners (in order round the edge) of the rectangle being placed
    // for `cursor`, after the spec. False when not placing a rectangle.
    bool rectResolve(glm::vec2 cursor, glm::vec2 out[4]) const;
    // The panel's Place: commit the current placement from the spec and cursor.
    // True when something was placed (a chord end counts for the arc).
    bool commitShapeNow();
    void clearShapeSpec() { m_shapeSpec = ShapeSpec{}; }
    // Text tool settings - the popup edits these; the click consumes them.
    const std::string& getTextString() const { return m_textString; }
    void setTextString(const std::string& s) { m_textString = s; }
    const std::string& getTextFontPath() const { return m_textFontPath; }
    void setTextFontPath(const std::string& p) { m_textFontPath = p; }
    float getTextHeight() const { return m_textHeight; }
    void setTextHeight(float h) { m_textHeight = (h < 0.5f) ? 0.5f : h; }
    // CCW rotation about the click anchor, 90° steps. The app seeds this
    // from the camera when the tool activates so text reads upright in the
    // current view; the popup's rotate buttons adjust from there.
    int getTextAngle() const { return m_textAngle; }
    void setTextAngle(int deg) { m_textAngle = ((deg % 360) + 360) % 360; }
    // Unrotated text extents relative to the anchor (mm), pushed by the app
    // whenever string/font/height change; the viewport draws the placement
    // rectangle from these. Invalid until setTextPreviewBox is called.
    bool hasTextPreviewBox() const { return m_textPrevValid; }
    glm::vec2 getTextPreviewMin() const { return m_textPrevMin; }
    glm::vec2 getTextPreviewMax() const { return m_textPrevMax; }
    void setTextPreviewBox(glm::vec2 mn, glm::vec2 mx) {
        m_textPrevMin = mn; m_textPrevMax = mx; m_textPrevValid = true;
    }
    void clearTextPreviewBox() { m_textPrevValid = false; m_textPrevLoops.clear(); }
    // Actual glyph contours (anchor-relative, unrotated mm - same space as the
    // box) for a LIVE preview of the letters. Pushed alongside the box.
    void setTextPreviewLoops(std::vector<std::vector<glm::vec2>> loops) {
        m_textPrevLoops = std::move(loops);
    }
    const std::vector<std::vector<glm::vec2>>& getTextPreviewLoops() const {
        return m_textPrevLoops;
    }
    // Airfoil placement (shares the Text/SVG placement frame: same angle and
    // stamp/undo machinery). The profile is chord-normalised; chord is the
    // real-world size in mm, and the click lands on the LEADING EDGE.
    void setAirfoil(AirfoilProfile prof) { m_airfoil = std::move(prof); }
    const AirfoilProfile& getAirfoil() const { return m_airfoil; }
    void  setAirfoilChord(float mm) { m_airfoilChord = (mm < 0.1f) ? 0.1f : mm; }
    float getAirfoilChord() const { return m_airfoilChord; }
    void setAirfoilAnchor(AirfoilAnchor a) { m_airfoilAnchor = a; }
    AirfoilAnchor getAirfoilAnchor() const { return m_airfoilAnchor; }

    // SVG placement (shares the Text tool's placement frame: same angle,
    // same cursor preview box, same fromText suppression on the result).
    void setSvgPaths(SvgPaths svg) { m_svgPaths = std::move(svg); }
    const SvgPaths& getSvgPaths() const { return m_svgPaths; }
    float getSvgWidth() const { return m_svgWidth; }
    void setSvgWidth(float w) { m_svgWidth = (w < 0.1f) ? 0.1f : w; }
    // Backspace while the Text/SVG tool is active yanks the whole last
    // stamp - a misplaced 550-line logo is not undoable element-by-element.
    bool hasLastStamp() const { return !m_stampStack.empty(); }
    void undoLastStamp();
    // Commit a Text/SVG stamp at the current anchor (touch "Place" button).
    void commitStamp();

    // --- Interactive Mirror ------------------------------------------------
    // Mirror reflects the current element selection (or the whole sketch when
    // nothing is selected) across a user-placed line. The line is an on-canvas
    // widget: a move handle at the anchor and a rotate handle along it. The
    // reflected geometry previews live; "Mirror" commits, "Cancel" aborts.
    bool isMirrorActive() const { return m_mirrorActive; }
    glm::vec2 getMirrorAnchor() const { return m_mirrorAnchor; }
    void setMirrorAnchor(glm::vec2 p) { m_mirrorAnchor = p; }
    float getMirrorAngle() const { return m_mirrorAngleRad; } // line dir, radians
    void setMirrorAngle(float r) { m_mirrorAngleRad = r; }
    // Capture the source elements + seed the line at the selection centroid
    // (vertical). Switches the tool to Mirror mode. No-op if there's nothing
    // to mirror.
    bool beginMirror();
    void cancelMirror();
    // Reflect a sketch-space point across the current mirror line.
    glm::vec2 mirrorReflect(glm::vec2 p) const;
    // Reflected ghost geometry for the live preview: closed/open polylines plus
    // standalone points (lone vertices). Already in sketch-space.
    void getMirrorPreview(std::vector<std::vector<glm::vec2>>& polylines,
                          std::vector<glm::vec2>& points) const;
    // Create the reflected elements; returns the new point + line ids so the
    // host can select them. Coincident vertices weld onto existing geometry.
    void commitMirror(std::set<int>& outPoints, std::set<int>& outLines);
    // --- Offset ------------------------------------------------------------
    // Two phases. Pick: hovering highlights the whole connected chain under the
    // cursor, a click captures it. Distance: the cursor drives both the
    // distance and the SIDE (which side of the chain it is on), previewing
    // live; a click, Enter or a typed value commits. Escape steps back one
    // phase, matching the two-step Escape convention on isPlacing().
    enum class OffsetPhase { Pick, Distance };
    OffsetPhase getOffsetPhase() const { return m_offsetPhase; }
    bool  hasOffsetChain() const { return m_offsetChain.valid(); }
    float getOffsetDistance() const { return m_offsetDistance; }
    void  setOffsetDistance(float d);
    void  flipOffsetSide() { setOffsetDistance(-m_offsetDistance); }
    OffsetCorners getOffsetCorners() const { return m_offsetCorners; }
    void  setOffsetCorners(OffsetCorners c) { m_offsetCorners = c; recomputeOffsetPreview(); }
    // True when the current distance yields geometry that can be committed.
    bool  offsetReady() const { return m_offsetResult.valid; }
    // Why the current distance produces nothing, or nullptr. Unlike the
    // Dimension tool's one-shot rejection this is a STANDING state (it
    // describes the live preview), so reading it does not clear it.
    const char* offsetRejection() const { return m_offsetResult.rejectReason; }
    // Chain highlight (Pick phase) and result ghost (Distance phase), as
    // sketch-space polylines - one per contiguous run, so a pruned offset with
    // gaps draws correctly.
    const std::vector<std::vector<glm::vec2>>& getOffsetChainHover() const {
        return m_offsetChainHover;
    }
    const std::vector<std::vector<glm::vec2>>& getOffsetPreview() const {
        return m_offsetPreview;
    }
    // Create the offset geometry; returns the new point + element ids so the
    // host can report or select them. Leaves the tool in the Pick phase ready
    // for the next chain (Trim likewise stays active after a click).
    // Set by a Distance-phase click or a typed value; the app drains it, wraps
    // commitOffset in recordSketchMutation and gets one undo step. Same
    // arrangement as the Dimension tool's dimReadyToCommit().
    bool offsetReadyToCommit() const { return m_offsetCommitRequested; }
    void commitOffset(std::set<int>& outPoints, std::set<int>& outElements);
    // Elements that were selected when the tool was chosen: while any stand,
    // only they can be picked (and only they join the chain). Dropped by the
    // first commit or by clearOffsetSelection().
    int  offsetSelectionCount() const { return static_cast<int>(m_offsetOnly.size()); }
    void clearOffsetSelection() { m_offsetOnly.clear(); m_offsetChainHover.clear(); }

    // Reference tool: copy geometry that lies ON the sketch plane - body edges,
    // a body's cross-section, another sketch's lines and circles (the snap
    // references gathered on entry, see PlaneReferences) - into this sketch as
    // real elements. Click takes one line or circle; Ctrl+click takes the whole
    // connected run of lines it belongs to.
    const std::vector<std::vector<glm::vec2>>& getReferenceHover() const { return m_refHover; }

    // Corner tool: click a corner where two lines meet to round it (fillet,
    // `size` = radius) or bevel it (chamfer, `size` = setback along each line).
    // The size and kind are set from the panel and stay until changed, so a run
    // of clicks does a run of corners.
    bool   cornerChamfer() const { return m_cornerChamfer; }
    void   setCornerChamfer(bool c) { m_cornerChamfer = c; }
    double cornerSize() const { return m_cornerSize; }
    void   setCornerSize(double s) { m_cornerSize = s; }
    // The corner a click would act on (-1 = none) and the size of its marker.
    int    cornerHoverPoint() const { return m_cornerHover; }
    float  cornerHoverRadius() const { return pickTol(0.3f, 0.3f); }
    // Why the last click did nothing, or nullptr. Read-and-clear.
    const char* consumeCornerRejection() { const char* r = m_cornerRejection; m_cornerRejection = nullptr; return r; }
    void cancelOffset();

    // Rectangle's typed-value placement is two-stage: first Enter sets the
    // horizontal side, second Enter the vertical (and commits). Stage 0 =
    // expecting H, 1 = expecting V. Read by the UI to swap the popup label.
    int getRectDimStage() const { return m_rectDimStage; }

    // True while the tool has an in-progress placement (first click made,
    // second pending) - used by the host to give Escape two-step semantics:
    // first Esc cancels just the in-progress shape, second Esc exits the
    // sketch mode entirely.
    bool isPlacing() const { return m_isPlacing; }
    // True when a line chain has only its anchor placed (first click, no segment
    // yet). The app defers that click's undo step so the first segment absorbs
    // it - otherwise the lone anchor is a surprise extra step at the end of undo.
    bool isChainAnchorPending() const {
        return m_mode == SketchToolMode::Line && m_isPlacing && m_lineChain.size() == 1;
    }

    // Grid step (in sketch-plane mm). Used for both visual grid and snap-to-line.
    // 0 disables grid snap entirely.
    // POINTING precision, not grid coarseness. Trim, pick, inference and hover
    // distances track the user's chosen step so a fine grid gives fine picking
    // - but they must not grow without bound when that step is coarse. With
    // the step following the display unit, a 1 ft grid put the trim threshold
    // at 152 mm: a click on empty space could cut geometry 15 cm away, with
    // grid snapping OFF. The cap is the largest step the presets ever offered
    // in millimetres.
    //
    // It is m_toleranceStep that is capped here, not the snap lattice. Since
    // the lattice started following the zoom, capping THAT would have pinned
    // the pick radius at 10 mm the moment a 1 mm grid coarsened - see tolStep.
    static constexpr float kToleranceStepCapMm = 10.0f;
    // How many SCREEN PIXELS a click may be from an existing point and still
    // WELD onto it. Deliberately tighter than the pointing radius below:
    // welding joins topology (it is what closes a loop into an extrudable
    // region), so it should ask for a more deliberate aim than a mere pick.
    static constexpr float kWeldRadiusPx = 6.0f;
    // Ceiling on the screen-derived weld radius, in millimetres. Six pixels is
    // an aim radius; at 3 mm/px it is an 18 mm topological merge and it grows
    // without bound as the view pulls back. Nearest-wins picks the best of
    // several candidates, it does not stop two deliberately distinct vertices
    // merging. At the zooms sketching actually happens (under ~1.7 mm/px) this
    // never engages; it is a rail against the absurd end.
    static constexpr float kWeldRadiusCapMm = 10.0f;
    // How many SCREEN PIXELS of slop a pointing gesture gets. Tuned so that at
    // the default millimetre framing this lands on the 0.3-1 mm the tolerances
    // have always used.
    static constexpr float kPointingRadiusPx = 12.0f;

    // Sketch millimetres per screen pixel, pushed in each frame by the
    // viewport. 0 until the first frame, which falls back to the grid.
    void setPixelScale(float mmPerPx) { m_mmPerPixel = mmPerPx > 0.0f ? mmPerPx : 0.0f; }

    // Pointing tolerance, in sketch millimetres. A tolerance is a SCREEN
    // distance - how near the cursor is in pixels - not a property of the
    // model. Deriving it from the grid alone gave 152 mm under a foot grid
    // (a click on empty space cut distant geometry) and, once capped at 10 mm,
    // gave 5 mm in a view where one pixel is 8 mm: sub-pixel, so nothing could
    // be picked at all. Both failures are the same mistake in opposite
    // directions.
    //
    // The screen term is what makes this usable at any zoom; the grid term is
    // a floor, so a fine grid still gives fine picking and every existing
    // millimetre sketch behaves exactly as it did.
    //
    // "Floor" only holds when fromGrid is the smaller one AND still clickable
    // on screen. Plain max(fromGrid, fromScreen) instead let fromScreen become
    // a CEILING: fromScreen depends only on zoom, not on the chosen step, so
    // at ordinary zoom (mmPerPixel small but not tiny) it commonly sat
    // around 1-2 mm and silently overrode a sub-1 mm grid choice - the
    // tolerance stayed pinned near a ~1 mm grid's proximity no matter how
    // fine a step the user picked, unless they *also* zoomed in enough to
    // shrink mmPerPixel to match (Steve, 2026-09-26). fromScreen's actual
    // job is only to rescue a tolerance that's gone sub-pixel-unusable
    // (the "5 mm in a view where one pixel is 8 mm" case above); once
    // fromGrid already clears a comfortable click radius on screen, honour
    // the user's finer choice instead of reflating it.
    float tolStep() const {
        // m_toleranceStep, NOT m_gridStep. The snap lattice now follows the
        // ZOOM (it is the user's base scaled by decades), and a tolerance that
        // followed it with it would grow every time the view pulled back: a
        // 1 mm base coarsening to 10 mm pins this at the cap below, turning a
        // ~1.5 mm pick radius into 10 mm - an 80-pixel grab - for no reason
        // the user expressed. The base is the precision they actually chose.
        const float fromGrid = std::min(m_toleranceStep, kToleranceStepCapMm);
        if (m_mmPerPixel <= 0.0f) return fromGrid; // no scale yet - grid alone
        const float fromScreen = kPointingRadiusPx * m_mmPerPixel;
        // Rescue only when the grid's own tolerance would be too small to
        // reliably click (under ~kMinClickablePx on screen); otherwise the
        // user's finer step wins outright.
        constexpr float kMinClickablePx = 3.0f;
        float result = (fromGrid / m_mmPerPixel >= kMinClickablePx)
                            ? fromGrid : std::max(fromGrid, fromScreen);
        // m_toleranceStep is the BASE preference, and the toolbar presets
        // never go below 0.1 mm - but the VISIBLE lattice (m_gridStep) is a
        // separate value the viewport scales down by whole decades as you
        // zoom in (GridScale::gridStepForZoom), so it legitimately gets
        // finer than that 0.1 mm floor. Without this, zooming in on a
        // visibly-0.01 mm grid still inferred/snapped at whatever coarser
        // base preset was chosen - "I'm zoomed in to a 0.01 mm grid and it's
        // still sticking to 0.1 mm" (Steve, 2026-09-26). Only follow the
        // lattice down when it is ITSELF comfortably resolvable on screen -
        // gridStepForZoom guarantees that for the real viewport (the cell is
        // always >= kGridMinPx wide), this guard just keeps tolStep() honest
        // if some other caller (or a test) ever hands it a lattice that
        // doesn't respect that.
        if (m_snapToGridEnabled && m_gridStep > 0.0f && m_gridStep < result &&
            m_gridStep / m_mmPerPixel >= kMinClickablePx) {
            result = m_gridStep;
        }
        return result;
    }

    // A click's pick radius: `frac` of the pointing step, but never below
    // `floorMm` - EXCEPT that the floor itself is capped at a few pixels, so
    // zooming in tightens the catch range instead of leaving it a fixed
    // 0.3-0.5 mm (which at a 0.001 mm view is hundreds of pixels: clicks landed
    // on geometry far from the cursor).
    //
    // Capped at kPickMaxPx on screen as well: tolStep() honours the user's own
    // step outright once it clears a few pixels, and that step does not shrink
    // with the zoom - so at the deepest zoom a 0.5 mm radius covered the whole
    // window, and there was no spot left to click that did not pick the point
    // (11-3-26).
    static constexpr float kPickFloorPx = 10.0f;
    static constexpr float kPickMaxPx = 16.0f;
    float pickTol(float frac, float floorMm) const {
        if (m_mmPerPixel > 0.0f) floorMm = std::min(floorMm, kPickFloorPx * m_mmPerPixel);
        float tol = std::max(tolStep() * frac, floorMm);
        if (m_mmPerPixel > 0.0f) tol = std::min(tol, kPickMaxPx * m_mmPerPixel);
        return tol;
    }

    // The lattice points snap to - scaled by zoom, so it changes as you zoom.
    // Also moves the tolerance step, so a caller that only ever sets this one
    // behaves exactly as this class did before the two were separated. The
    // viewport calls setToleranceStep straight after, to pin tolerances to the
    // user's base while snapping follows the zoom.
    void setGridStep(float step) { m_gridStep = step; m_toleranceStep = step; }
    // The user's chosen step, which zoom does NOT scale. Only tolerances read
    // it. Must be called AFTER setGridStep, which resets it.
    void setToleranceStep(float step) {
        if (step > 0.0f) m_toleranceStep = step;
    }
    float getGridStep() const { return m_gridStep; }
    // Mirrors the toolbar "Snap to grid" checkbox. When on (default), placed
    // points always round to the nearest grid increment; when off, only
    // inferences snap and the cursor lands at sub-grid precision.
    void setSnapToGridEnabled(bool b) { m_snapToGridEnabled = b; }

    // How much drawing-time inference assistance to apply. Cycled live from the
    // sketch toolbar (no longer a persisted setting). Full = every guide incl.
    // hover-charged references; Reduced = lock-onto-existing-geometry snaps only
    // (endpoint / midpoint / on-line / axis-from-point), no directional ray
    // guides or dwell-charging; Off = grid + endpoint only.
    // Max is the touch-oriented tier: everything Full does, but with widened
    // snap / inference catch ranges (see snapScale/angleScale) so an imprecise
    // fingertip still grabs the intended point / endpoint / alignment. Full and
    // below behave identically on every device - desktop is never "over-snapped".
    // Listed last so the persisted int values for Full/Reduced/Off stay 0/1/2.
    enum class InferenceLevel { Full, Reduced, Off, Max };
    void setInferenceLevel(InferenceLevel lvl) { m_inferenceLevel = lvl; }
    InferenceLevel getInferenceLevel() const { return m_inferenceLevel; }

    // Increment (degrees) for the line angle-snap (0/15/30/45/90 …). The line
    // tool snaps its direction to multiples of this from the segment anchor.
    // 0 disables angle-snap entirely. Persisted via AppSettings.
    void setAngleSnapDeg(int deg) { m_angleSnapDeg = deg < 0 ? 0 : deg; }
    int  getAngleSnapDeg() const { return m_angleSnapDeg; }

    // Rectangle / circle draw origin (sketch toolbar toggle). Rectangle:
    // Corner = first click is one corner, drag to the opposite (default);
    // Center = first click is the centre, drag to a corner. Circle:
    // Center = first click is the centre, drag the radius (default);
    // TwoPoint = the two clicks are opposite ends of the diameter (the rim
    // passes through the first click - handy to align a circle to a corner).
    enum class RectMode { Corner, Center };
    enum class CircleMode { Center, TwoPoint };

    // What a typed value means at the arc's THIRD click. Clicks 1 and 2 fix the
    // chord, so either the swept angle or the radius pins the apex exactly -
    // two ways of saying the same thing, and which one you have to hand depends
    // on the drawing (a 90-degree corner versus a 6 mm fillet run). The cursor's
    // side of the chord still decides which way the arc bows: that is a
    // direction, not a dimension, so no number can express it.
    enum class ArcDimMode { Sweep, Radius };
    // Is the value applyDimension() expects right now a LENGTH? Not always:
    // an arc's second click in Sweep mode takes DEGREES (clamped 0.1..359.9),
    // and a polygon's first takes a SIDE COUNT. Both were being converted
    // display->mm on the way in, so under inches a typed 180 deg arrived as
    // 4572 and a typed 6 sides as 152. Callers that convert must ask first;
    // this lives here because applyDimension's own switch is the only place
    // that knows.
    bool dimensionValueIsLength() const {
        if (m_mode == SketchToolMode::Polygon && m_clickCount == 0) return false;
        if (m_mode == SketchToolMode::Arc && m_clickCount == 2 &&
            m_arcDimMode == ArcDimMode::Sweep) return false;
        return true;
    }
    void setArcDimMode(ArcDimMode m) { m_arcDimMode = m; }
    ArcDimMode getArcDimMode() const { return m_arcDimMode; }
    // Half the chord - the smallest radius any arc through these two endpoints
    // can have. Below it no arc exists, so the UI can grey out Apply rather
    // than let applyDimension silently refuse. 0 when not at the apex stage.
    float arcMinRadius() const;
    void setRectMode(RectMode m) { m_rectMode = m; }
    RectMode getRectMode() const { return m_rectMode; }
    void setCircleMode(CircleMode m) { m_circleMode = m; }
    // Circle tool: the typed size is a diameter rather than a radius. Sticky.
    bool circleDiameter() const { return m_circleDiameter; }
    void setCircleDiameter(bool d) { m_circleDiameter = d; }
    CircleMode getCircleMode() const { return m_circleMode; }

    // Hover-to-charge references (Full level only). Call once per frame during
    // sketch placement with the current time and the sketch-space cursor. After
    // the cursor dwells ~0.3 s on an existing reference (sketch point, sketch
    // line midpoint, host-face vertex, or host-face edge midpoint) it becomes
    // "charged" and projects axis + perpendicular guides anchored AT that
    // position - until a different one charges or the placement ends. The
    // renderer reads hasChargedRef() / getChargedPos() to draw the cyan ring.
    struct ChargedRef {
        // None = nothing charged; the other kinds carry an anchor position the
        // snap engine projects V / H / perpendicular guides from. SketchLineMid
        // and FaceLineMid each have exactly one PerpToRef direction (perp to
        // the segment); the Point kinds scan touching lines / face edges.
        enum class Kind { None, SketchPoint, SketchLineMid, FacePoint, FaceLineMid };
        Kind kind = Kind::None;
        glm::vec2 pos{0.0f};
        // Sketch point or line id for the Sketch* kinds; -1 for Face*.
        int sourceId = -1;
    };
    void updateHoverCharge(double tNow, glm::vec2 cursorSketchPos);
    bool hasChargedRef() const { return m_charged.kind != ChargedRef::Kind::None; }
    glm::vec2 getChargedPos() const { return m_charged.pos; }
    // Legacy accessor used elsewhere - returns the sketch-point id only when
    // the charged ref happens to BE a sketch point; -1 for the new kinds.
    int  getChargedRefPoint() const {
        return m_charged.kind == ChargedRef::Kind::SketchPoint ? m_charged.sourceId : -1;
    }

    // Current state for rendering preview
    bool hasPreview() const;
    glm::vec2 getPreviewStart() const;
    glm::vec2 getPreviewEnd() const;
    SketchToolMode getPreviewType() const;

    // Trim hover: densified 2D points outlining the segment that would be
    // removed on the next click. Empty when nothing is hovered in Trim mode.
    const std::vector<glm::vec2>& getTrimHoverPoints() const { return m_trimHoverPoints; }

    // Where the directional inference family (perpendicular/parallel-to-
    // previous, tangent-to-curve, angle snap, hover-charged guides) measures
    // FROM for the point being placed right now, and whether it applies at all
    // in the current mode and stage. Every draw tool that asks the user to
    // choose a DIRECTION from a fixed anchor wants these guides; the family was
    // written for the Line tool and gated on it, so arcs, circles, polygons and
    // splines drew with no directional assistance whatsoever. Returns false for
    // the modes and stages where a direction is not the thing being picked.
    bool directionalAnchor(glm::vec2& out) const;

    // The set of inferences active at the most recent snap. The renderer reads
    // this each frame to draw ghost guide lines. Cleared whenever the cursor
    // doesn't align with anything.
    const std::vector<InferenceGuide>& getActiveInferences() const { return m_activeInferences; }

    // Is the tool actively placing something?
    bool isActive() const;

    // --- Line-chain step-back support (touch context bar: Back / Cancel) ---
    // Number of committed segments in the line chain currently being placed
    // (0 = only the start vertex is down, nothing to back out). Line only.
    int lineSegmentCount() const {
        return m_mode == SketchToolMode::Line
                   ? std::max(0, static_cast<int>(m_lineChain.size()) - 1)
                   : 0;
    }
    // Remove the chain's last segment (the line + its tail vertex, by the IDs
    // tracked in m_lineChain) and re-anchor placement on the new tail so the
    // chain keeps going from there. Returns false if there was no segment to
    // back out. Wrap the call in recordSketchMutation for one undo step. Line only.
    bool dropLineChainTail();

    // --- Dimension tool (Onshape-style: pick 1-2 entities, place label) ---
    DimPhase getDimPhase() const { return m_dimPhase; }
    DimPick getDimPickA() const { return m_dimPickA; }
    const PendingDimension& getPendingDimension() const { return m_dimPending; }
    glm::vec2 getDimLabelPos() const { return m_dimLabelPos; } // valid when dimReadyToCommit()
    bool dimReadyToCommit() const { return m_dimReady; }       // label placed; app commits + calls clearDimState()
    void clearDimState();                                       // back to PickFirst, pending invalidated
    // One-shot reason the last Dimension click was refused, or nullptr. The
    // tool used to just `return` on an unusable pick, so the click looked
    // like it had simply missed - indistinguishable from a mis-aim. The app
    // drains this each frame and toasts it. Reading clears it.
    const char* consumeDimRejection() {
        const char* r = m_dimRejectReason;
        m_dimRejectReason = nullptr;
        return r;
    }
    DimPick dimHitTest(glm::vec2 pos) const { return hitTestDimEntity(pos); } // hover highlight for the viewport
    static PendingDimension resolveDimension(const Sketch& sk, DimPick a, DimPick b);
    // True when the two lines' directions are parallel or anti-parallel
    // within kDimParallelTolRad - the same test resolveDimension's line-line
    // branch uses to decide DistancePointLine vs Angle. Exposed standalone
    // so callers outside resolveDimension (the mirrored-DistancePointLine
    // dedup in Application::applyPendingDimension) can gate on the same
    // definition of "parallel enough" instead of re-deriving it. False for
    // a missing line id or a degenerate (zero-length) line.
    static bool linesParallelWithinDimTol(const Sketch& sk, int lineIdA, int lineIdB);

private:
    // Catch-range multipliers - >1 only at the Max inference tier, so Full and
    // below snap exactly as before on every device. See enum InferenceLevel.
    float snapScale()  const { return m_inferenceLevel == InferenceLevel::Max ? 1.8f : 1.0f; } // distances
    float angleScale() const { return m_inferenceLevel == InferenceLevel::Max ? 1.5f : 1.0f; } // angles

    SketchToolMode m_mode = SketchToolMode::None;
    Sketch* m_sketch = nullptr;
    SketchSolver* m_solver = nullptr;
    InferenceLevel m_inferenceLevel = InferenceLevel::Full;
    int m_angleSnapDeg = 15; // line angle-snap increment (0 = off)
    RectMode   m_rectMode   = RectMode::Corner;
    CircleMode m_circleMode = CircleMode::Center;
    bool       m_circleDiameter = false;
    // Session-sticky: whichever the user last drew arcs with stays selected, so
    // a run of fillet arcs is not a mode toggle per arc.
    ArcDimMode m_arcDimMode = ArcDimMode::Sweep;
    // Hover-charge state (see updateHoverCharge). m_charged is the active
    // reference (Kind::None when nothing's charged); the m_hover* fields
    // track the in-progress dwell on a candidate before it commits.
    ChargedRef m_charged;
    ChargedRef m_hoverCandidate;
    double     m_hoverProbeStart = 0.0;
    glm::vec2  m_hoverProbePos{0.0f};

    // State for multi-click tools
    bool m_isPlacing = false;
    int m_clickCount = 0;
    glm::vec2 m_firstClick{0};
    glm::vec2 m_secondClick{0};
    glm::vec2 m_currentPos{0};

    // For line chaining. m_lastPointId is the running tail of the chain that
    // each new segment extends from. m_chainStartPointId remembers where the
    // chain *began*, so when the user clicks back onto it we can auto-close
    // the loop (commit the final segment and end placement).
    int m_lastPointId = -1;
    int m_chainStartPointId = -1;
    // Did the first click of the current line chain add a brand-new point
    // (vs reusing an existing one)? If yes and the chain is cancelled before
    // any segment is committed, we delete the orphan to avoid leaving a
    // stray vertex with no lines attached.
    bool m_chainStartPointCreated = false;
    // Ordered point IDs of the line chain in progress (front = start vertex,
    // back = current tail). Mirrors the committed segments so the touch "Back"
    // button can drop the tail and re-anchor after the host undoes a segment.
    std::vector<int> m_lineChain;
    // Line id of each committed segment, in lockstep with m_lineChain (one
    // shorter - segment i connects m_lineChain[i] to m_lineChain[i+1]).
    // dropLineChainTail pops the exact id here instead of re-deriving "the"
    // segment by matching endpoints, which picks the WRONG line whenever
    // another line (e.g. a pre-existing polygon edge) already shares those
    // same two endpoints. Cleared everywhere m_lineChain is.
    std::vector<int> m_lineChainSegmentIds;

    // Snap to grid/points
    glm::vec2 snap(glm::vec2 pos) const;

    // Find an existing point near the given position (returns -1 if none).
    // INTERACTIVE: the radius is a screen distance, because this answers "did
    // the user aim at that point". Only for positions the user actually
    // clicked. Generated geometry must use findExactCoincidentPoint.
    int findCoincidentPoint(glm::vec2 pos, int excludeId = -1) const;

    // Coincidence for GENERATED geometry - a mirrored vertex, an offset
    // endpoint, a derived circle centre. Fixed model-space radius, so the same
    // operation on the same sketch produces the same topology no matter where
    // the camera is. A screen radius here would make the model a function of
    // the view: mirroring while zoomed out would weld vertices that mirroring
    // while zoomed in leaves separate. Same reasoning the arc circumcentre
    // already documents (see handleArcTool).
    int findExactCoincidentPoint(glm::vec2 pos, int excludeId = -1) const;

    void handleLineTool(glm::vec2 pos);
    // Single-click placement: weld onto an existing point within range, else
    // add a brand-new one. No chain/placing state - unlike Line, there's
    // nothing to preview between click and commit.
    void handlePointTool(glm::vec2 pos);
    // exact = the position came from a typed value; skip the grid rounding.
    void handleCircleTool(glm::vec2 pos, bool exact = false);
    void handleRectangleTool(glm::vec2 pos);
    void handleArcTool(glm::vec2 pos);
    // Snap the arc's swept apex to a 15°-multiple sweep when within ±5° of one
    // (the 180° semicircle case especially). Used by BOTH the live preview
    // (onMouseMove) and the commit (handleArcTool) so they can't diverge -
    // previously only the preview snapped, so the committed arc landed at the
    // raw cursor and its centre drifted off the intended (e.g. semicircle) one.
    glm::vec2 snapArcApex(glm::vec2 start, glm::vec2 end, glm::vec2 apex) const;
    // Final arc-apex snap for the 3rd click: prefer a clean sweep angle over a
    // grid square. Runs snapArcApex on the RAW cursor (so grid quantization
    // can't suppress a 90°/180° hit) and only falls back to the grid when no
    // angle target is near. Used by both the live preview and the commit so
    // they always agree (issue #27).
    glm::vec2 arcApexSnap(glm::vec2 rawPos) const;
    // Snap the radial distance from `fixed` to `moving` onto the grid step,
    // keeping the direction. For a circle this snaps the radius (Center mode)
    // or diameter (2-point mode) to whole grid units, so a 1 mm grid can't
    // produce a 10.05 mm circle. No-op when grid snap is off.
    glm::vec2 snapRadialToGrid(glm::vec2 fixed, glm::vec2 moving) const;
    // How far a drag may pull a sticky point off its rim before the
    // attachment simply lets go. Sized to the snap band so attaching and
    // detaching feel like the same gesture at the same scale.
    float rimBreakBand(bool wholeSelection) const;
    // Slide `p` along the rim it is stuck to, or release it. Returns the
    // position to use. Clears onCurveId when the drag has left the rim.
    glm::vec2 slideOrRelease(int pointId, glm::vec2 target, bool wholeSelection);
    // The circle/arc the last snap() landed on, or -1. mutable: snap() is
    // const and every caller wants the position, not a second return value.
    mutable int m_snapRimId = -1;
    void noteRimGuide(glm::vec2 at, int curveId) const;
    void handleSelectTool(glm::vec2 pos);
    void handleSplineTool(glm::vec2 pos);
    void handlePolygonTool(glm::vec2 pos);
    void handleTextTool(glm::vec2 pos);
    void handleSvgTool(glm::vec2 pos);
    void handleAirfoilTool(glm::vec2 pos);
    // Collapse a directional-inference result onto the axis (+ grid) when
    // it's within a few degrees of horizontal/vertical - crooked-line guard.
    glm::vec2 rectifyNearAxis(glm::vec2 target) const;
    void handleTrimTool(glm::vec2 pos);
    void computeTrimHover(glm::vec2 pos); // updates m_trimHoverPoints (no mutation)

    // Select/drag state
    int m_dragPointId = -1;
    bool m_isDragging = false;
    bool m_lastDownAddedToSel = false; // Ctrl state for the current click
    // The line the last Select click landed on, so a repeat click at the same
    // spot can walk to the next of several overlapping lines.
    struct RefPick { int kind = 0; size_t idx = 0; };   // kind: 0 none, 1 line, 2 circle, 3 point, 4 spline
    RefPick pickReference(glm::vec2 pos) const;
    void updateReferenceHover(glm::vec2 pos);
    void handleReferenceTool(glm::vec2 pos);
    int  pickCornerPoint(glm::vec2 pos) const;
    void handleCornerTool(glm::vec2 pos);
    bool   m_cornerChamfer = false;
    double m_cornerSize = 2.0;
    int    m_cornerHover = -1;
    PolygonSpec m_polygonSpec;
    ShapeSpec   m_shapeSpec;
    // Click / Place with typed values: the one commit path. Handled = true.
    bool commitTypedShape(glm::vec2 cursor);
    void addLineDistanceConstraint(int aId, int bId, double value);
    const char* m_cornerRejection = nullptr;
    std::vector<std::vector<glm::vec2>> m_refHover;
    std::set<int> m_offsetOnly;   // see offsetSelectionCount()
    const std::set<int>* offsetOnly() const { return m_offsetOnly.empty() ? nullptr : &m_offsetOnly; }
    int       m_lastLinePickId = -1;
    glm::vec2 m_lastLinePickPos{0.0f};
    bool m_constructionDraw = false;
    std::set<int> m_selectedPoints;
    std::set<int> m_selectedLines;
    std::set<int> m_selectedCircles;
    std::set<int> m_selectedArcs;
    std::set<int> m_selectedSplines;

    std::vector<int> m_splinePoints; // temp storage during spline creation
    int m_activeSplineId = -1;       // committed-and-growing spline element

public:
    // Control points of the spline currently being placed (live preview).
    const std::vector<int>& splinePointsInProgress() const {
        return m_splinePoints;
    }

    // Backspace during spline placement: drop the last control point
    // (removing it from the sketch too unless something else references
    // it - e.g. the user snapped onto an existing vertex).
    void removeLastSplinePoint();

private:
    int m_polygonSides = 6; // default hexagon

    // Text tool settings (see TextSketchOp.h for the generator)
    std::string m_textString = "TEXT";
    std::string m_textFontPath; // resolved by the app at tool activation
    float m_textHeight = 8.0f;  // capital height, mm
    int   m_textAngle = 0;      // CCW degrees, 90° steps
    bool  m_textPrevValid = false;
    glm::vec2 m_textPrevMin{0.0f};
    glm::vec2 m_textPrevMax{0.0f};
    std::vector<std::vector<glm::vec2>> m_textPrevLoops; // live glyph preview

    // SVG placement state (see SvgImport.h)
    SvgPaths m_svgPaths;
    AirfoilProfile m_airfoil;
    float m_airfoilChord = 100.0f;  // mm, a typical model-wing root chord
    AirfoilAnchor m_airfoilAnchor = AirfoilAnchor::LeadingEdge;
    float m_svgWidth = 50.0f; // target artwork width, mm

    // One entry per Text/SVG stamp (newest last), each holding that stamp's
    // element ids (lines first, then points, so removal order never orphans
    // references). A STACK - not a single slot - so undoLastStamp can walk back
    // through every stamp to the original, not just the most recent one.
    // Cleared on setMode so each tool session starts fresh.
    std::vector<std::vector<int>> m_stampStack;
    void recordStamp(size_t pointsBefore, size_t linesBefore,
                     size_t splinesBefore = static_cast<size_t>(-1));

    // --- Interactive Mirror state ---
    bool m_mirrorActive = false;
    glm::vec2 m_mirrorAnchor{0.0f};        // a point the mirror line passes through
    float m_mirrorAngleRad = 1.5707963268f; // line direction; default vertical
    // Captured source element ids (resolved at beginMirror).
    std::set<int> m_mirrorPoints, m_mirrorLines, m_mirrorCircles,
                  m_mirrorArcs, m_mirrorSplines;

    // Rectangle's typed-value placement is two-stage: first Enter sets the
    // horizontal side, second Enter sets the vertical side and commits.
    // m_rectDimStage tracks where we are (0 = expecting H, 1 = expecting V);
    // m_rectDimH stores the locked-in horizontal value between stages.
    int   m_rectDimStage = 0;
    float m_rectDimH = 0.0f;

    float m_gridStep = 1.0f; // default 1 mm grid (zoom-scaled; snapping)
    float m_toleranceStep = 1.0f; // the user's base step (pointing tolerances)
    float m_mmPerPixel = 0.0f;   // set per frame; see setPixelScale
    bool  m_snapToGridEnabled = true; // toolbar checkbox, see setSnapToGridEnabled

    // Updated each frame in Trim mode so the renderer can outline the segment
    // that would be deleted on click.
    std::vector<glm::vec2> m_trimHoverPoints;

    // Direction (unit vector) of the previous segment in the current chain.
    // Used so "perpendicular to previous" and "parallel to previous" inferences
    // have something to anchor to while the user draws the next vertex.
    // Reset on chain-break (onConfirm/onCancel/mode-switch).
    glm::vec2 m_prevLineDir{0.0f, 0.0f};
    bool m_hasPrevLineDir = false;

    // Populated as a side-effect of snap(); read by the viewport overlay to
    // draw ghost guide lines. Mutable so snap() can stay const.
    mutable std::vector<InferenceGuide> m_activeInferences;

    // Point ids the next snap() call should skip when looking for endpoint /
    // axis-from-point / on-line candidates. Populated during a drag to
    // exclude the dragged points themselves (otherwise the cursor would snap
    // to its own starting position and the drag would feel sticky-broken).
    // Cleared in onMouseUp.
    std::set<int> m_snapExcludePoints;

    // --- Offset tool state ---
    OffsetPhase   m_offsetPhase = OffsetPhase::Pick;
    OffsetChain   m_offsetChain;      // captured on the Pick-phase click
    OffsetResult  m_offsetResult;     // live, recomputed as the cursor moves
    // Offset from a pre-selection that spans several separate chains: the other
    // chains ride along with the primary one. `m_offsetExtraSign` keeps each on
    // the same side as the primary (outward stays outward for closed loops), so
    // Flip and a typed distance move them all together.
    std::vector<OffsetChain>  m_offsetExtra;
    std::vector<OffsetResult> m_offsetExtraRes;
    std::vector<float>        m_offsetExtraSign;
    bool beginOffsetFromSelection();
    OffsetCorners m_offsetCorners = OffsetCorners::Round;
    float         m_offsetDistance = 0.0f;   // signed: + is right of travel
    bool          m_offsetCommitRequested = false;
    // After a commit the cursor still sits on the new geometry; without this
    // the hover would re-highlight it instantly and the commit would look like
    // it did nothing. Cleared once the cursor moves clear.
    bool          m_offsetSuppressHover = false;
    glm::vec2     m_offsetCommitPos{0.0f};
    std::vector<std::vector<glm::vec2>> m_offsetChainHover;
    std::vector<std::vector<glm::vec2>> m_offsetPreview;
    void handleOffsetTool(glm::vec2 pos);
    void updateOffsetHover(glm::vec2 pos);     // Pick phase
    void updateOffsetDistance(glm::vec2 pos);  // Distance phase: sign + magnitude
    void recomputeOffsetPreview();

    // --- Dimension tool state ---
    DimPhase m_dimPhase = DimPhase::PickFirst;
    DimPick m_dimPickA;
    PendingDimension m_dimPending;
    glm::vec2 m_dimLabelPos{0.0f};
    bool m_dimReady = false;
    // Static string literal (never owns storage) - see consumeDimRejection.
    const char* m_dimRejectReason = nullptr;
    DimPick hitTestDimEntity(glm::vec2 pos) const;
    void handleDimensionTool(glm::vec2 pos);
};

} // namespace materializr
