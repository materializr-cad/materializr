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
#include <cstdint>   // uint64_t

namespace materializr {

// APPEND ONLY. Toolbar mirrors the active tool as this enum's raw INDEX
// (Application::setActiveSketchMode) and the sketch toolbar hardcodes those
// indices, so inserting a mode in the middle silently highlights the wrong
// button -- Dimension would become 13 while the button still tests 12.
enum class SketchToolMode { None, Select, Line, Circle, Rectangle, Arc, Spline, Polygon, Trim, Text, Svg, Mirror, Dimension, Airfoil, Offset };

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
// other's endpoints — see SketchTool::linesParallelWithinDimTol). Written as
// a literal rather than via M_PI/180 so this header doesn't need to pull in
// <cmath> or the M_PI portability guard every other file including it uses.
constexpr double kDimParallelTolRad = 0.017453292519943295; // 1.0 deg

enum class DimPhase { PickFirst, PickSecondOrPlace, PlaceLabel };

// One drawing-time alignment hint. Inferences are transient — they describe
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
    // Full replace, including curve types — used by box-select so a drag can
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
    void setPolygonSides(int n) { m_polygonSides = (n < 3) ? 3 : n; }
    // Text tool settings — the popup edits these; the click consumes them.
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
    // Actual glyph contours (anchor-relative, unrotated mm — same space as the
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
    // stamp — a misplaced 550-line logo is not undoable element-by-element.
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
    // --- Interactive Offset -------------------------------------------------
    // Offset builds a parallel copy of ONE closed loop taken from the element
    // selection. The cursor picks both side and distance; a typed value in the
    // Offset panel overrides the magnitude. Unlike Mirror there is no
    // whole-sketch fallback: offsetting everything at once is meaningless, and
    // an ambiguous selection is refused with a reason rather than guessed at.
    //
    // The geometry itself lives in SketchOffset.{h,cpp} (pure OCCT, headless
    // testable); this is only the interaction state around it.
    bool isOffsetActive() const { return m_offsetActive; }
    // Capture the selected loop and arm the tool. Returns the reason on
    // failure, so the host can toast something specific rather than "failed".
    OffsetError beginOffset();
    void cancelOffset();
    // Cursor -> signed distance (sign = side). Refuses nothing; the guards run
    // at commit, so the ghost can show what WOULD happen.
    void updateOffsetFromCursor(glm::vec2 cursor);
    double getOffsetDistance() const { return m_offsetDistance; }
    // Typed entry supplies magnitude only; the side stays as the cursor left it.
    void setOffsetMagnitude(double mm);
    // Swap inward/outward without dragging the cursor across the loop.
    void flipOffsetSide() { m_offsetDistance = -m_offsetDistance; }
    // Ghost polylines in sketch space, or empty when the current distance
    // would be refused. Arcs are sampled for DRAWING only -- the committed
    // geometry stays as real arcs.
    void getOffsetPreview(std::vector<std::vector<glm::vec2>>& polylines) const;
    // Apply. Returns the reason on failure; the sketch is untouched unless
    // this returns OffsetError::None.
    OffsetError commitOffset(std::set<int>& outPoints, std::set<int>& outEntities);
    // True while the captured source still matches what was captured. Undo or
    // a delete during the drag invalidates it -- see m_offsetSourceHash.
    bool offsetSourceUnchanged() const;

    // Rectangle's typed-value placement is two-stage: first Enter sets the
    // horizontal side, second Enter the vertical (and commits). Stage 0 =
    // expecting H, 1 = expecting V. Read by the UI to swap the popup label.
    int getRectDimStage() const { return m_rectDimStage; }

    // True while the tool has an in-progress placement (first click made,
    // second pending) — used by the host to give Escape two-step semantics:
    // first Esc cancels just the in-progress shape, second Esc exits the
    // sketch mode entirely.
    bool isPlacing() const { return m_isPlacing; }
    // True when a line chain has only its anchor placed (first click, no segment
    // yet). The app defers that click's undo step so the first segment absorbs
    // it — otherwise the lone anchor is a surprise extra step at the end of undo.
    bool isChainAnchorPending() const {
        return m_mode == SketchToolMode::Line && m_isPlacing && m_lineChain.size() == 1;
    }

    // Grid step (in sketch-plane mm). Used for both visual grid and snap-to-line.
    // 0 disables grid snap entirely.
    void setGridStep(float step) { m_gridStep = step; }
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
    // below behave identically on every device — desktop is never "over-snapped".
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
    // passes through the first click — handy to align a circle to a corner).
    enum class RectMode { Corner, Center };
    enum class CircleMode { Center, TwoPoint };

    // What a typed value means at the arc's THIRD click. Clicks 1 and 2 fix the
    // chord, so either the swept angle or the radius pins the apex exactly —
    // two ways of saying the same thing, and which one you have to hand depends
    // on the drawing (a 90-degree corner versus a 6 mm fillet run). The cursor's
    // side of the chord still decides which way the arc bows: that is a
    // direction, not a dimension, so no number can express it.
    enum class ArcDimMode { Sweep, Radius };
    void setArcDimMode(ArcDimMode m) { m_arcDimMode = m; }
    ArcDimMode getArcDimMode() const { return m_arcDimMode; }
    // Half the chord — the smallest radius any arc through these two endpoints
    // can have. Below it no arc exists, so the UI can grey out Apply rather
    // than let applyDimension silently refuse. 0 when not at the apex stage.
    float arcMinRadius() const;
    void setRectMode(RectMode m) { m_rectMode = m; }
    RectMode getRectMode() const { return m_rectMode; }
    void setCircleMode(CircleMode m) { m_circleMode = m; }
    CircleMode getCircleMode() const { return m_circleMode; }

    // Hover-to-charge references (Full level only). Call once per frame during
    // sketch placement with the current time and the sketch-space cursor. After
    // the cursor dwells ~0.3 s on an existing reference (sketch point, sketch
    // line midpoint, host-face vertex, or host-face edge midpoint) it becomes
    // "charged" and projects axis + perpendicular guides anchored AT that
    // position — until a different one charges or the placement ends. The
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
    // Legacy accessor used elsewhere — returns the sketch-point id only when
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
    // like it had simply missed — indistinguishable from a mis-aim. The app
    // drains this each frame and toasts it. Reading clears it.
    const char* consumeDimRejection() {
        const char* r = m_dimRejectReason;
        m_dimRejectReason = nullptr;
        return r;
    }
    DimPick dimHitTest(glm::vec2 pos) const { return hitTestDimEntity(pos); } // hover highlight for the viewport
    static PendingDimension resolveDimension(const Sketch& sk, DimPick a, DimPick b);
    // True when the two lines' directions are parallel or anti-parallel
    // within kDimParallelTolRad — the same test resolveDimension's line-line
    // branch uses to decide DistancePointLine vs Angle. Exposed standalone
    // so callers outside resolveDimension (the mirrored-DistancePointLine
    // dedup in Application::applyPendingDimension) can gate on the same
    // definition of "parallel enough" instead of re-deriving it. False for
    // a missing line id or a degenerate (zero-length) line.
    static bool linesParallelWithinDimTol(const Sketch& sk, int lineIdA, int lineIdB);

private:
    // Catch-range multipliers — >1 only at the Max inference tier, so Full and
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

    // Snap to grid/points
    glm::vec2 snap(glm::vec2 pos) const;

    // Find an existing point near the given position (returns -1 if none)
    int findCoincidentPoint(glm::vec2 pos, int excludeId = -1) const;

    void handleLineTool(glm::vec2 pos);
    // exact = the position came from a typed value; skip the grid rounding.
    void handleCircleTool(glm::vec2 pos, bool exact = false);
    void handleRectangleTool(glm::vec2 pos);
    void handleArcTool(glm::vec2 pos);
    // Snap the arc's swept apex to a 15°-multiple sweep when within ±5° of one
    // (the 180° semicircle case especially). Used by BOTH the live preview
    // (onMouseMove) and the commit (handleArcTool) so they can't diverge —
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
    // it's within a few degrees of horizontal/vertical — crooked-line guard.
    glm::vec2 rectifyNearAxis(glm::vec2 target) const;
    void handleTrimTool(glm::vec2 pos);
    void computeTrimHover(glm::vec2 pos); // updates m_trimHoverPoints (no mutation)

    // Select/drag state
    int m_dragPointId = -1;
    bool m_isDragging = false;
    bool m_lastDownAddedToSel = false; // Ctrl state for the current click
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
    // it — e.g. the user snapped onto an existing vertex).
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
    // references). A STACK — not a single slot — so undoLastStamp can walk back
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

    // --- Interactive Offset state ---
    bool m_offsetActive = false;
    OffsetSource m_offsetSource;      // the captured loop
    double m_offsetDistance = 0.0;    // signed: + is outward
    // Hash of the CAPTURED SOURCE ONLY -- deliberately not Sketch::geometryHash(),
    // which covers the whole sketch: committing an offset changes that, and
    // since the tool re-arms after a commit the next preview would read its own
    // output as a stale-source edit and cancel itself.
    uint64_t m_offsetSourceHash = 0;
    uint64_t hashOffsetSource() const;

    // Rectangle's typed-value placement is two-stage: first Enter sets the
    // horizontal side, second Enter sets the vertical side and commits.
    // m_rectDimStage tracks where we are (0 = expecting H, 1 = expecting V);
    // m_rectDimH stores the locked-in horizontal value between stages.
    int   m_rectDimStage = 0;
    float m_rectDimH = 0.0f;

    float m_gridStep = 1.0f; // default 1 mm grid
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

    // --- Dimension tool state ---
    DimPhase m_dimPhase = DimPhase::PickFirst;
    DimPick m_dimPickA;
    PendingDimension m_dimPending;
    glm::vec2 m_dimLabelPos{0.0f};
    bool m_dimReady = false;
    // Static string literal (never owns storage) — see consumeDimRejection.
    const char* m_dimRejectReason = nullptr;
    DimPick hitTestDimEntity(glm::vec2 pos) const;
    void handleDimensionTool(glm::vec2 pos);
};

} // namespace materializr
