#include "SketchTool.h"
#include "SketchConstraints.h"
#include "TextSketchOp.h"
#include "../touch_mode.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {

SketchTool::SketchTool() {
    // Fresh installs on a touch device default to the Max inference tier (wider
    // catch ranges for fingertips); everywhere else starts at Full. A saved
    // setting overrides this on load (applyAppSettings).
    m_inferenceLevel = touchMode() ? InferenceLevel::Max : InferenceLevel::Full;
}

void SketchTool::setSketch(Sketch* sketch) {
    m_sketch = sketch;
}

void SketchTool::setSolver(SketchSolver* solver) {
    m_solver = solver;
}

void SketchTool::setMode(SketchToolMode mode) {
    // Cancel any in-progress operation when switching modes
    if (m_isPlacing) {
        onCancel();
    }
    m_mode = mode;
    // Leaving select/move: drop element highlights so they don't linger as
    // golden geometry once the user goes back to drawing.
    if (mode != SketchToolMode::Select) clearElementSelection();
    m_clickCount = 0;
    m_isPlacing = false;
    m_lastPointId = -1;
    m_chainStartPointId = -1;
    m_isDragging = false;
    m_dragPointId = -1;
    m_splinePoints.clear();
    m_activeSplineId = -1; // a partial spline stays committed (>=2 points)
    m_stampStack.clear(); // stamp-undo history is per tool session
    m_hasPrevLineDir = false;
    m_activeInferences.clear();
    m_rectDimStage = 0;
    m_rectDimH = 0.0f;
    m_mirrorActive = false; // switching tools aborts any in-progress mirror
    cancelOffset();         // ... and any in-progress offset
    // Entering or leaving Dimension mode always starts a fresh pick sequence.
    clearDimState();
}

SketchToolMode SketchTool::getMode() const {
    return m_mode;
}

void SketchTool::onMouseDown(glm::vec2 pos, bool addToSel) {
    if (!m_sketch) return;
    m_lastDownAddedToSel = addToSel;
    // Placing a point ends this segment's charge — re-hover to charge again.
    m_charged = {};
    m_hoverCandidate = {};

    // Trim (and Dimension) pick by proximity to existing geometry; snapping
    // the cursor first would pull the click toward unrelated nearby
    // points/edges and pick wrong.
    glm::vec2 snapped = (m_mode == SketchToolMode::Trim || m_mode == SketchToolMode::Dimension)
                             ? pos
                             : snap(pos);

    // Rectangle stage-1 (width typed-in, cursor still drives height): the
    // click that commits the second corner should use the LOCKED width on the
    // X axis, not whatever the cursor happens to be at horizontally.
    //
    // The HALF matters in Center mode: m_firstClick is the centre and
    // handleRectangleTool mirrors the corner through it, so the corner sits
    // half the typed width away. applyDimension has always halved here; these
    // re-application sites did not, which doubled any typed width the moment
    // the user clicked (rather than typed) the second side.
    if (m_mode == SketchToolMode::Rectangle && m_rectDimStage == 1 && m_isPlacing) {
        const float half = (m_rectMode == RectMode::Center) ? 0.5f : 1.0f;
        snapped.x = m_firstClick.x + m_rectDimH * half;
    }

    if (m_mode == SketchToolMode::None) return;

    // Touch stamp-placement flow: a finger has no hover, so a tap/drag POSITIONS
    // the preview anchor (the Move toggle frees the camera; two-finger still
    // pans/zooms) and the dialog's "Place" button commits. Desktop keeps its
    // hover→click-to-stamp flow untouched.
    if (touchMode() &&
        (m_mode == SketchToolMode::Text || m_mode == SketchToolMode::Svg)) {
        m_currentPos = snapped; // move the anchor; don't stamp yet
        return;
    }

    switch (m_mode) {
        case SketchToolMode::Select:
            handleSelectTool(snapped);
            break;
        case SketchToolMode::Line:
            handleLineTool(snapped);
            break;
        case SketchToolMode::Circle:
            handleCircleTool(snapped);
            break;
        case SketchToolMode::Rectangle:
            handleRectangleTool(snapped);
            break;
        case SketchToolMode::Arc:
            // The 3rd click (apex) prefers a clean sweep angle over the grid,
            // computed from the RAW cursor; the chord ends still grid-snap.
            handleArcTool(m_clickCount == 2 ? arcApexSnap(pos) : snapped);
            break;
        case SketchToolMode::Spline:
            handleSplineTool(snapped);
            break;
        case SketchToolMode::Polygon:
            handlePolygonTool(snapped);
            break;
        case SketchToolMode::Trim:
            handleTrimTool(snapped);
            break;
        case SketchToolMode::Text:
            handleTextTool(snapped);
            break;
        case SketchToolMode::Svg:
            handleSvgTool(snapped);
            break;
        case SketchToolMode::Airfoil:
            handleAirfoilTool(snapped);
            break;
        case SketchToolMode::Dimension:
            handleDimensionTool(snapped);
            break;
        case SketchToolMode::Offset:
            // Raw cursor, like Trim: snapping first would pull the pick toward
            // an unrelated nearby point and grab the wrong chain.
            handleOffsetTool(pos);
            break;
        default:
            break;
    }

    // If that click landed on a circle/arc rim, mark whatever point ended up
    // there as stuck to it. Done HERE, once, rather than in each tool's
    // handler: every draw tool routes through this dispatch, so a new tool
    // cannot forget to do it.
    if (m_snapRimId >= 0 && m_sketch) {
        const int pid = findCoincidentPoint(snapped, -1);
        if (pid >= 0) m_sketch->setPointOnCurve(pid, m_snapRimId);
    }
}

void SketchTool::onMouseMove(glm::vec2 pos) {
    // Dimension mode has no drag/preview snapping of its own — just record the
    // raw cursor for the ghost-label / hover-highlight overlay and bail before
    // the snapping logic below (which doesn't apply to picking entities).
    if (m_mode == SketchToolMode::Dimension) {
        m_currentPos = pos;
        return;
    }
    // Offset: the Pick phase hit-tests with the RAW cursor (as Trim does), the
    // Distance phase measures from the SNAPPED one so grid snap quantises the
    // distance to whole increments.
    if (m_mode == SketchToolMode::Offset) {
        if (m_offsetPhase == OffsetPhase::Pick) {
            m_currentPos = pos;
            updateOffsetHover(pos);
        } else {
            m_currentPos = snap(pos);
            updateOffsetDistance(m_currentPos);
        }
        return;
    }
    // Trim uses the raw cursor for picking; snapping would pull the click toward
    // unrelated nearby targets and pick the wrong element.
    glm::vec2 newPos = (m_mode == SketchToolMode::Trim) ? pos : snap(pos);
    // Select/move only manipulates existing geometry — inference guides are
    // visual noise here. Keep the snapped position (handy when dragging an
    // element onto a grid point/endpoint) but drop the guide markers.
    if (m_mode == SketchToolMode::Select) m_activeInferences.clear();
    glm::vec2 delta  = newPos - m_currentPos;
    m_currentPos = newPos;

    // Rectangle two-stage typed input: after the user has typed width + Enter
    // (stage 1), the X side is locked. Override the cursor-driven X so the
    // live preview keeps the typed width visible while the user adjusts /
    // types / clicks for the height.
    if (m_mode == SketchToolMode::Rectangle && m_rectDimStage == 1) {
        const float half = (m_rectMode == RectMode::Center) ? 0.5f : 1.0f;
        m_currentPos.x = m_firstClick.x + m_rectDimH * half;
    }

    // Arc 3rd-click sweep snap to 15° increments. Snap is only meaningful
    // while the user is sweeping the bulge of the arc (clicks==2: two clicks
    // for the chord ends already placed, cursor steering which side and how
    // far the arc bows). Skipped when the global snap toggle is off.
    //
    // Geometry: given the chord (A=firstClick, B=secondClick) and a target
    // sweep θ, the apex of the arc passing through A and B with that sweep
    // sits on the perpendicular bisector of AB at signed distance
    //   d = (L/2) * tan(θ/4)
    // from the chord's midpoint, on whichever side the cursor currently is.
    // The cursor jumps to that apex when the natural sweep is within ±5° of
    // a 15° multiple, giving a sticky "click into a quarter / third / etc."
    // feel without locking out finer adjustments.
    // Live preview: snap the swept apex to a 15°-multiple sweep (esp. 180°).
    // Same helper the commit uses (handleArcTool), so preview == result.
    if (m_mode == SketchToolMode::Arc && m_clickCount == 2) {
        // Feed the RAW cursor (not the already-grid-snapped m_currentPos): the
        // apex should prefer a clean sweep angle over a grid square, which grid
        // quantization would otherwise block (issue #27).
        m_currentPos = arcApexSnap(pos);
    }

    // Circle live preview: snap the radius/diameter to the grid so what's
    // shown (and committed) is a clean whole-unit circle, not whatever
    // irrational distance two grid-snapped endpoints happen to span. Same
    // helper the commit uses, so preview == result.
    if (m_mode == SketchToolMode::Circle && m_isPlacing && m_snapToGridEnabled) {
        m_currentPos = snapRadialToGrid(m_firstClick, m_currentPos);
    }

    if (m_mode == SketchToolMode::Trim) {
        computeTrimHover(pos);
    } else if (!m_trimHoverPoints.empty()) {
        m_trimHoverPoints.clear();
    }

    // Drag the active selection (or the single dragged point if no broader
    // selection exists) by the cursor delta each frame.
    if (m_isDragging && m_sketch) {
        std::set<int> pts = m_selectedPoints;
        for (int lid : m_selectedLines) {
            for (const auto& l : m_sketch->getLines()) {
                if (l.id == lid) {
                    pts.insert(l.startPointId);
                    pts.insert(l.endPointId);
                    break;
                }
            }
        }
        if (pts.empty() && m_dragPointId >= 0) pts.insert(m_dragPointId);

        // When a single point is being dragged, run inference snap on its
        // target position so endpoint / midpoint / on-line / axis-from-point
        // guides fire mid-drag (the same as during placement). The dragged
        // point itself is excluded so it doesn't snap to its starting spot.
        // For multi-point drags we skip inference snap — there's no single
        // "cursor follows this point" semantic, and snapping one would slide
        // the others off.
        if (pts.size() == 1 && m_dragPointId >= 0) {
            const SketchPoint* p = m_sketch->getPoint(m_dragPointId);
            if (p) {
                m_snapExcludePoints = pts;
                glm::vec2 target = p->pos + delta;
                glm::vec2 inferred = snap(target);
                m_snapExcludePoints.clear();
                // A point already stuck to a rim rides it; snap()'s answer only
                // applies once the attachment has been let go.
                inferred = slideOrRelease(m_dragPointId, inferred,
                                          /*wholeSelection=*/false);
                m_sketch->movePoint(m_dragPointId, inferred);
            }
        } else {
            // Multi-point drag: every dragged point translates by the cursor
            // delta. With Snap-to-grid on, each resulting position is then
            // rounded to the nearest grid increment so a group drag still
            // adheres to the chosen step (otherwise the offset accumulates
            // sub-grid float drift across many drags).
            // Quantise the DELTA, not each point. Rounding every point to the
            // lattice teleported anything that was deliberately off it — a line
            // with its ends on two circles lost both, 0.6mm off the rim, and
            // came out rotated 30 -> 33.7 degrees and shorter. It also fired on
            // a drag of nearly zero length, so merely pressing and twitching on
            // a line moved it. A whole-step delta keeps the shape rigid and
            // still prevents sub-grid drift accumulating across many drags.
            glm::vec2 step = delta;
            if (m_snapToGridEnabled && m_gridStep > 0.0f) {
                step.x = std::round(step.x / m_gridStep) * m_gridStep;
                step.y = std::round(step.y / m_gridStep) * m_gridStep;
            }
            if (step != glm::vec2(0.0f)) {
                for (int pid : pts) {
                    const SketchPoint* p = m_sketch->getPoint(pid);
                    if (!p) continue;
                    m_sketch->movePoint(
                        pid, slideOrRelease(pid, p->pos + step,
                                            /*wholeSelection=*/true));
                }
            }
            // Multi-point drag doesn't fire inferences; clear any stale ones
            // so the overlay doesn't draw guides from the previous frame.
            m_activeInferences.clear();
        }
        if (m_solver) m_solver->solve(*m_sketch);
    }
}

void SketchTool::onMouseUp(glm::vec2 /*pos*/) {
    if (m_isDragging) {
        m_isDragging = false;
        m_dragPointId = -1;
        // No more drag — clear any drag-time inference guides so the overlay
        // doesn't linger after the user releases the mouse.
        m_activeInferences.clear();
    }
}

void SketchTool::onConfirm() {
    // Finalize spline if in spline mode with enough points
    if (m_mode == SketchToolMode::Spline && m_sketch) {
        // The spline is committed and grown per click; just reset the tool.
        m_splinePoints.clear();
        m_activeSplineId = -1;
    }

    // Finish the current chain (e.g., stop line chaining)
    m_isPlacing = false;
    m_clickCount = 0;
    m_lastPointId = -1;
    m_chainStartPointId = -1;
    m_chainStartPointCreated = false;
    m_hasPrevLineDir = false;
    m_activeInferences.clear();
    m_rectDimStage = 0;
    m_rectDimH = 0.0f;
}

void SketchTool::onCancel() {
    // Dimension mode: Escape backs out of an in-progress pick/placement
    // first; only once nothing is pending does it fall through to exiting
    // the tool (the app maps that by switching to Select).
    if (m_mode == SketchToolMode::Dimension) {
        if (m_dimPhase != DimPhase::PickFirst || m_dimReady) {
            clearDimState();
        } else {
            setMode(SketchToolMode::Select);
        }
        return;
    }
    // Offset: Escape drops the captured chain first, so a mis-picked chain
    // costs one keystroke rather than exiting the tool; a second Escape then
    // falls through to the app's "leave the tool" handling.
    if (m_mode == SketchToolMode::Offset) {
        if (m_offsetPhase == OffsetPhase::Distance) {
            cancelOffset();
        } else {
            setMode(SketchToolMode::Select);
        }
        return;
    }
    // If only the first click of a line chain was made and we created its
    // anchor point fresh (no existing point was reused), drop that orphan so
    // a cancelled draw doesn't leave a stray yellow endpoint marker behind.
    if (m_sketch && m_mode == SketchToolMode::Line &&
        m_clickCount == 1 && m_chainStartPointCreated &&
        m_chainStartPointId >= 0) {
        m_sketch->removeElement(m_chainStartPointId);
    }
    // Spline-in-progress: drop every placed control point properly (each
    // freshly-created one is removed from the sketch) instead of clearing
    // the list and stranding orphan dots.
    while (!m_splinePoints.empty()) removeLastSplinePoint();
    m_isPlacing = false;
    m_clickCount = 0;
    m_lastPointId = -1;
    m_chainStartPointId = -1;
    m_chainStartPointCreated = false;
    m_hasPrevLineDir = false;
    m_activeInferences.clear();
    m_rectDimStage = 0;
    m_rectDimH = 0.0f;
    m_lineChain.clear();
}

bool SketchTool::dropLineChainTail() {
    // Surgically remove the chain's LAST segment by the IDs we tracked in
    // m_lineChain — not by undoing the top history step, which isn't reliably
    // the last segment — then re-anchor the live chain on the new tail so the
    // next press-drag continues from there. The front (start vertex) is the
    // floor: with only the start left there is no segment to back out.
    // (The caller wraps this in recordSketchMutation, so it's one undo step.)
    if (m_mode != SketchToolMode::Line || m_lineChain.size() < 2 || !m_sketch)
        return false;
    int tail = m_lineChain.back();
    int prev = m_lineChain[m_lineChain.size() - 2];

    // Delete the segment line joining prev <-> tail.
    for (const auto& l : m_sketch->getLines()) {
        if ((l.startPointId == prev && l.endPointId == tail) ||
            (l.startPointId == tail && l.endPointId == prev)) {
            m_sketch->removeElement(l.id);
            break;
        }
    }
    // Delete the tail vertex too, but only if nothing else references it — it
    // may have been snapped onto pre-existing geometry we mustn't disturb.
    bool stillUsed = false;
    for (const auto& l : m_sketch->getLines())
        if (l.startPointId == tail || l.endPointId == tail) { stillUsed = true; break; }
    if (!stillUsed) m_sketch->removeElement(tail);

    m_lineChain.pop_back();
    m_lastPointId = m_lineChain.back();
    if (const SketchPoint* p = m_sketch->getPoint(m_lastPointId)) {
        m_firstClick = p->pos;
        // Collapse the live preview onto the new tail. On touch there's no hover
        // to move the cursor, so m_currentPos is left at the old release point —
        // the rubber-band would otherwise redraw a phantom segment from the new
        // tail back toward the just-removed endpoint (looks like Back left the
        // endpoint behind). Zero-length preview until the user draws again.
        m_currentPos = p->pos;
    }
    if (m_clickCount > 1) m_clickCount--;
    m_hasPrevLineDir = false;   // recompute the perpendicular/parallel guide on next move
    m_activeInferences.clear();
    return true;
}

bool SketchTool::applyDimension(float value) {
    if (!m_sketch || !m_isPlacing || value <= 0.0f) return false;

    // Offset takes the typed value as a MAGNITUDE and keeps the side the
    // cursor already chose — a distance cannot express a direction. It only
    // requests the commit; see handleOffsetTool.
    if (m_mode == SketchToolMode::Offset) {
        if (m_offsetPhase != OffsetPhase::Distance || !m_offsetChain.valid())
            return false;
        setOffsetDistance((m_offsetDistance < 0.0f) ? -value : value);
        if (!m_offsetResult.valid) return false;
        m_offsetCommitRequested = true;
        return true;
    }

    // Direction from anchor toward current cursor position. If the cursor is on top
    // of the anchor, default to +X so something happens.
    glm::vec2 delta = m_currentPos - m_firstClick;
    float curLen = glm::length(delta);
    glm::vec2 dir = (curLen > 1e-6f) ? (delta / curLen) : glm::vec2(1.0f, 0.0f);

    switch (m_mode) {
        case SketchToolMode::Line: {
            auto addDistance = [&](int aId, int bId) {
                // One Distance constraint per segment: drop any prior one on
                // this pair so repeated refinements don't stack duplicates.
                for (const auto& c : m_sketch->getConstraints())
                    if (c.type == ConstraintType::Distance &&
                        ((c.entityA == aId && c.entityB == bId) ||
                         (c.entityA == bId && c.entityB == aId)))
                        m_sketch->removeConstraint(c.id);
                Constraint c;
                c.id = 0;
                c.type = ConstraintType::Distance;
                c.entityA = aId;
                c.entityB = bId;
                c.value = static_cast<double>(value);
                c.isSatisfied = true;
                m_sketch->addConstraint(c);
            };

            // `dir` is the AIM direction: anchor -> the last tap / press-hold
            // (m_currentPos), set WITHOUT committing a segment (the touch tap
            // handler routes to onMouseMove while placing). Typing the length
            // is what commits the segment, at anchor + dir*length. On TOUCH the
            // chain stays live so you can aim + type the next segment
            // (Steve: tap sets direction, type sets length, repeat). Desktop
            // keeps its place-and-finish behaviour (onConfirm ends the chain,
            // stopping the next click landing on the just-drawn line).
            glm::vec2 endPos = m_firstClick + dir * value;
            size_t lnBefore = m_sketch->getLines().size();
            handleLineTool(endPos);
            if (!materializr::touchMode()) onConfirm();
            if (m_sketch->getLines().size() > lnBefore) {
                const auto& l = m_sketch->getLines().back();
                addDistance(l.startPointId, l.endPointId);
            }
            return true;
        }
        case SketchToolMode::Circle: {
            // Popup asks for DIAMETER (matching the on-canvas "X.X mm dia"
            // readout). Convert to radius for the underlying circle.
            float radius = value * 0.5f;
            // Center mode: the second point sits on the rim (radius away).
            // TwoPoint mode: the typed value is the diameter, so the second
            // click is a full diameter from the first along the cursor dir.
            glm::vec2 second = (m_circleMode == CircleMode::TwoPoint)
                                   ? m_firstClick + dir * value
                                   : m_firstClick + dir * radius;
            size_t cBefore = m_sketch->getCircles().size();
            handleCircleTool(second, /*exact=*/true);
            // Typed diameter → Radius constraint on the new circle.
            if (m_sketch->getCircles().size() > cBefore) {
                const auto& circ = m_sketch->getCircles().back();
                Constraint c;
                c.id = 0;
                c.type = ConstraintType::Radius;
                c.entityA = circ.id;
                c.value = static_cast<double>(radius);
                c.isSatisfied = true;
                m_sketch->addConstraint(c);
            }
            return true;
        }
        case SketchToolMode::Polygon: {
            // For polygon the typed value is the SIDE COUNT (≥3), not the
            // radius. We DON'T commit here — just update m_polygonSides so
            // the live preview re-renders with the new count, and the user
            // can keep dragging to size + rotation. The actual placement
            // commits on the second click as for any other polygon.
            m_polygonSides = std::max(3, static_cast<int>(std::round(value)));
            return true;
        }
        case SketchToolMode::Rectangle: {
            // Two-stage typed input: first Enter sets the horizontal side
            // (cursor X locks, user can still move cursor for Y), second Enter
            // sets the vertical side AND commits. Quadrant comes from where
            // the cursor is relative to the first click so the rectangle
            // grows in the user's intended direction.
            float sx = (delta.x >= 0.0f) ? 1.0f : -1.0f;
            float sy = (delta.y >= 0.0f) ? 1.0f : -1.0f;
            // In Center mode the typed value is the FULL side, but the cursor
            // (and the second corner handed to handleRectangleTool, which
            // mirrors it through the centre) is only half that from the centre.
            const float half = (m_rectMode == RectMode::Center) ? 0.5f : 1.0f;
            if (m_rectDimStage == 0) {
                m_rectDimH = value * sx;
                m_rectDimStage = 1;
                // Lock the preview's X to the typed horizontal while the user
                // either drags for vertical or types the second value.
                m_currentPos.x = m_firstClick.x + m_rectDimH * half;
                return true;
            } else {
                glm::vec2 corner =
                    m_firstClick + glm::vec2(m_rectDimH * half, value * sy * half);
                handleRectangleTool(corner);
                m_rectDimStage = 0;
                m_rectDimH = 0.0f;
                return true;
            }
        }
        case SketchToolMode::Arc: {
            // Click 2 — type the CHORD: the straight-line distance between the
            // arc's two ends, exactly like a line's length.
            if (m_clickCount == 1) {
                handleArcTool(m_firstClick + dir * value);
                return true;
            }
            if (m_clickCount != 2) return false;

            // Click 3 — the chord is already fixed, so ONE number pins the apex.
            // Which way the arc bows is a direction, not a dimension, so it
            // still comes from the side of the chord the cursor is on.
            const glm::vec2 A = m_firstClick, B = m_secondClick;
            const glm::vec2 chord = B - A;
            const float L = glm::length(chord);
            if (L < 1e-4f) return false;
            const glm::vec2 chordDir = chord / L;
            glm::vec2 perp(-chordDir.y, chordDir.x);
            const glm::vec2 M = 0.5f * (A + B);
            if (glm::dot(m_currentPos - M, perp) < 0.0f) perp = -perp;

            float d = 0.0f;
            if (m_arcDimMode == ArcDimMode::Sweep) {
                // Apex sits on the chord's perpendicular bisector at
                // (L/2)·tan(θ/4) — the same relation snapArcApex uses for its
                // 15° steps, so a typed 90 lands exactly where the snap would.
                // A full 360 has no apex to place (tan(90°) is infinite) and
                // would be a circle, not an arc; clamp just short of it.
                const float deg = glm::clamp(value, 0.1f, 359.9f);
                d = (L * 0.5f) *
                    std::tan(deg * static_cast<float>(M_PI) / 180.0f * 0.25f);
            } else {
                // Radius: the centre lies on the bisector at √(R² − (L/2)²)
                // from the midpoint, and the MINOR arc's apex is on the far
                // side of the chord, R minus that, away. Half the chord is the
                // floor — under it no arc passes through both endpoints at all.
                const float half = L * 0.5f;
                if (value < half - 1e-4f) return false;
                const float h =
                    std::sqrt(std::max(0.0f, value * value - half * half));
                d = value - h;
            }
            // Straight to the handler, NOT through arcApexSnap: a typed value
            // is exact and must not be pulled onto the nearest 15° sweep.
            handleArcTool(M + perp * d);
            return true;
        }
        default:
            return false;
    }
}

bool SketchTool::hasPreview() const {
    return m_isPlacing && m_mode != SketchToolMode::None;
}

glm::vec2 SketchTool::getPreviewStart() const {
    // The renderer + dimension overlay read (start, end) as the circle's
    // (centre, rim) and the rectangle's (corner, opposite corner). Return the
    // EFFECTIVE start for the active draw mode so both visuals match what the
    // click will actually create — no renderer changes needed.
    if (m_mode == SketchToolMode::Rectangle && m_rectMode == RectMode::Center)
        return 2.0f * m_firstClick - m_currentPos; // opposite corner
    if (m_mode == SketchToolMode::Circle && m_circleMode == CircleMode::TwoPoint)
        return 0.5f * (m_firstClick + m_currentPos); // diameter midpoint = centre
    return m_firstClick;
}

glm::vec2 SketchTool::getPreviewEnd() const {
    return m_currentPos;
}

SketchToolMode SketchTool::getPreviewType() const {
    return m_mode;
}

bool SketchTool::isActive() const {
    return m_isPlacing;
}

float SketchTool::arcMinRadius() const {
    if (m_mode != SketchToolMode::Arc || m_clickCount != 2) return 0.0f;
    return 0.5f * glm::length(m_secondClick - m_firstClick);
}

bool SketchTool::directionalAnchor(glm::vec2& out) const {
    if (!m_isPlacing || !m_sketch) return false;
    switch (m_mode) {
    case SketchToolMode::Line:
        // Chains: handleLineTool re-seats m_firstClick on every committed
        // vertex, so this is always the point being drawn FROM.
        out = m_firstClick;
        return true;
    case SketchToolMode::Spline:
        // m_firstClick stays on control point #1 for the life of the spline,
        // so it is the wrong anchor from the third point on — measure from the
        // point actually being extended.
        if (m_splinePoints.empty()) return false;
        if (const SketchPoint* p = m_sketch->getPoint(m_splinePoints.back())) {
            out = p->pos;
            return true;
        }
        return false;
    case SketchToolMode::Arc:
        // Click 2 places the far end of the chord — a direction from the start
        // point, exactly like a line. Click 3 sweeps the apex, which has its
        // own 15-degree sweep snap (arcApexSnap); a directional guide there
        // would pull against it.
        if (m_clickCount != 1) return false;
        out = m_firstClick;
        return true;
    case SketchToolMode::Polygon:
        // The drag sets the circumradius AND the polygon's rotation, so the
        // direction is real geometry — snapping it is how you get a hexagon
        // sitting flat instead of a degree and a half off.
        out = m_firstClick;
        return true;
    case SketchToolMode::Circle:
        // Two-point mode drags a DIAMETER, whose direction is real. In
        // centre-radius the direction means nothing — only the distance does —
        // and steering it would just perturb the radius, which
        // snapRadialToGrid already looks after.
        if (m_circleMode != CircleMode::TwoPoint) return false;
        out = m_firstClick;
        return true;
    default:
        // Rectangle is axis-aligned by construction; Select/Trim/Dimension/
        // Text/Svg place no directed point.
        return false;
    }
}

glm::vec2 SketchTool::rectifyNearAxis(glm::vec2 target) const {
    // Directional inferences (perpendicular / parallel / tangent / axis /
    // angle) override grid snap by design — but when the inferred segment
    // comes out NEARLY axis-aligned, "nearly" is the bug: 1° over 80 mm is
    // a visibly crooked line the user never asked for, and every later
    // inference aligns to it, breeding more. Rule (Steve's): keep the
    // inference while it's genuinely slanted; once it gets close to
    // parallel with an axis, the axis wins — flatten exactly, and re-grid
    // the free coordinate so the endpoint is lattice-true again.
    glm::vec2 anchor;
    if (!directionalAnchor(anchor)) return target;
    glm::vec2 d = target - anchor;
    float len = glm::length(d);
    if (len < 1e-4f) return target;
    const float axisTol = glm::radians(4.0f) * angleScale();
    float ang = std::atan2(d.y, d.x);
    const float PI = static_cast<float>(M_PI);
    auto nearAng = [&](float ref) {
        float dd = std::abs(ang - ref);
        if (dd > PI) dd = 2.0f * PI - dd;
        return dd < axisTol;
    };
    const bool gridOn = m_snapToGridEnabled && m_gridStep > 0.0f;
    auto grid = [&](float v) {
        return gridOn ? std::round(v / m_gridStep) * m_gridStep : v;
    };
    // With grid on, only flatten genuinely sub-cell drift: if the segment ends
    // a full grid row (or more) off the axis, that's a deliberate 1/X-slope
    // line, not drift — leave it. Slightly over half a cell so the exact
    // boundary still flattens onto the axis grid line. Grid off keeps the pure
    // 4° rule. (Steve: a 1 mm rise over a long run must not snap to horizontal.)
    const float crossCap = gridOn ? m_gridStep * 0.5f + 1e-3f : 1e30f;
    if ((nearAng(0.0f) || nearAng(PI) || nearAng(-PI)) &&
        std::abs(target.y - anchor.y) < crossCap) {
        target.y = anchor.y;             // exactly horizontal
        target.x = grid(target.x);
    } else if ((nearAng(PI * 0.5f) || nearAng(-PI * 0.5f)) &&
               std::abs(target.x - anchor.x) < crossCap) {
        target.x = anchor.x;             // exactly vertical
        target.y = grid(target.y);
    }
    return target;
}

void SketchTool::updateHoverCharge(double tNow, glm::vec2 cursor) {
    // Only charge while actively placing a DIRECTED point at the Full or Max
    // tier — the charged reference exists to give perpendicular/axis guides off
    // the dwelt-on feature, which is meaningless where no direction is picked.
    glm::vec2 chargeAnchor;
    if (!m_sketch || !directionalAnchor(chargeAnchor) ||
        (m_inferenceLevel != InferenceLevel::Full &&
         m_inferenceLevel != InferenceLevel::Max)) {
        m_hoverCandidate = {};
        m_charged = {};
        return;
    }
    // Nearest chargeable reference within the hover band. Wider than the
    // endpoint-snap radius so you can charge by hovering NEAR a candidate
    // without landing exactly on top of it.
    // Candidates (priority by closeness, not by kind):
    //   - sketch points (vertices)
    //   - sketch line midpoints
    //   - host-face vertices ("corners")
    //   - host-face edge midpoints
    // The face-ref kinds give axis + perpendicular guides off the host
    // geometry without needing a sketch element there yet — they only
    // exist while the sketch is open, so the cyan ring is naturally
    // hidden in the regular 3D view.
    // Band scales with the grid but gently: the old 0.9x grid band plus a
    // 0.6x linger tolerance meant a SLOW drive-by anywhere near a candidate
    // charged it at coarse grid steps (zoomed out) — "latching to edges I
    // never hovered". Charging is a deliberate act: tighter catch, and the
    // linger tolerance below is a small fraction of a cell.
    const float band = std::max(0.4f, tolStep() * 0.6f);
    ChargedRef best;
    float bestD = band;
    for (const auto& pt : m_sketch->getPoints()) {
        if (pt.fromText) continue;
        if (m_snapExcludePoints.count(pt.id)) continue;
        float d = glm::length(cursor - pt.pos);
        if (d < bestD) {
            bestD = d;
            best.kind = ChargedRef::Kind::SketchPoint;
            best.pos = pt.pos;
            best.sourceId = pt.id;
        }
    }
    for (const auto& ln : m_sketch->getLines()) {
        if (ln.fromText) continue;
        if (m_snapExcludePoints.count(ln.startPointId) ||
            m_snapExcludePoints.count(ln.endPointId)) continue;
        const SketchPoint* p1 = m_sketch->getPoint(ln.startPointId);
        const SketchPoint* p2 = m_sketch->getPoint(ln.endPointId);
        if (!p1 || !p2) continue;
        glm::vec2 mid = 0.5f * (p1->pos + p2->pos);
        float d = glm::length(cursor - mid);
        if (d < bestD) {
            bestD = d;
            best.kind = ChargedRef::Kind::SketchLineMid;
            best.pos = mid;
            best.sourceId = ln.id;
        }
    }
    for (const auto& fp : m_sketch->getFaceReferences().points) {
        float d = glm::length(cursor - fp);
        if (d < bestD) {
            bestD = d;
            best.kind = ChargedRef::Kind::FacePoint;
            best.pos = fp;
            best.sourceId = -1;
        }
    }
    for (const auto& fl : m_sketch->getFaceReferences().lines) {
        glm::vec2 mid = 0.5f * (fl.first + fl.second);
        float d = glm::length(cursor - mid);
        if (d < bestD) {
            bestD = d;
            best.kind = ChargedRef::Kind::FaceLineMid;
            best.pos = mid;
            best.sourceId = -1;
        }
    }
    if (best.kind == ChargedRef::Kind::None) {
        // Not hovering any candidate — keep whatever is already charged
        // (so the guide survives while you drag away to align against it).
        m_hoverCandidate = {};
        return;
    }
    // Dwell: cursor must linger ~0.3 s on the same candidate (without
    // wandering off it) before it charges. "Same candidate" = same kind
    // and same sourceId; FacePoint and FaceLineMid match by position
    // since they have no id.
    const double dwell = 0.30;
    const float  moveTol = std::max(0.25f, tolStep() * 0.25f);
    bool sameCandidate =
        m_hoverCandidate.kind == best.kind &&
        ((best.sourceId >= 0 && m_hoverCandidate.sourceId == best.sourceId) ||
         (best.sourceId < 0  && glm::length(m_hoverCandidate.pos - best.pos) < 1e-4f));
    if (sameCandidate &&
        glm::length(cursor - m_hoverProbePos) < moveTol) {
        if (tNow - m_hoverProbeStart >= dwell) m_charged = best;
    } else {
        m_hoverCandidate = best;
        m_hoverProbeStart = tNow;
        m_hoverProbePos = cursor;
    }
}

// Full-inference curve snap (Steve's idea): instead of landing on the bare
// nearest point of a circle/arc, land where the curve crosses a grid LINE, so
// the snap is BOTH on-curve AND grid-aligned. The grid line is chosen by the
// local tangent so the crossing is well-defined:
//   • point near top/bottom (radius ~vertical, tangent ~horizontal) → nearest
//     VERTICAL grid line (x snapped to grid, y solved on the curve)
//   • point near the sides (radius ~horizontal, tangent ~vertical)  → nearest
//     HORIZONTAL grid line
//   • ~45° (radius diagonal) → whichever of the two crossings is nearer the
//     cursor, so the 45° point on a circle stays snappable.
// Returns false if no crossing lands within `band` of the cursor.
static bool snapCurveToGrid(glm::vec2 center, float radius, glm::vec2 pos,
                            float gridStep, float band, glm::vec2& out) {
    if (radius < 1e-6f || gridStep <= 0.0f) return false;
    glm::vec2 v = pos - center;
    float dist = glm::length(v);
    if (dist < 1e-6f) return false;
    glm::vec2 rdir = v / dist; // radius dir at nearest perimeter point

    auto vCross = [&](glm::vec2& res) -> bool {       // nearest vertical grid line
        float gx = std::round(pos.x / gridStep) * gridStep;
        float dx = gx - center.x;
        if (std::abs(dx) > radius) return false;
        float dy = std::sqrt(radius * radius - dx * dx);
        res = glm::vec2(gx, center.y + (pos.y >= center.y ? dy : -dy));
        return true;
    };
    auto hCross = [&](glm::vec2& res) -> bool {       // nearest horizontal grid line
        float gy = std::round(pos.y / gridStep) * gridStep;
        float dy = gy - center.y;
        if (std::abs(dy) > radius) return false;
        float dx = std::sqrt(radius * radius - dy * dy);
        res = glm::vec2(center.x + (pos.x >= center.x ? dx : -dx), gy);
        return true;
    };

    const float diagEps = 0.18f; // |rdir.x|≈|rdir.y| ⇒ ~45° (±7° band)
    glm::vec2 cand;
    bool ok = false;
    if (std::abs(rdir.y) - std::abs(rdir.x) > diagEps) {
        ok = vCross(cand);                            // tangent ~horizontal
    } else if (std::abs(rdir.x) - std::abs(rdir.y) > diagEps) {
        ok = hCross(cand);                            // tangent ~vertical
    } else {                                          // ~45°: nearer crossing
        glm::vec2 a, b;
        bool oa = vCross(a), ob = hCross(b);
        if (oa && ob) { cand = (glm::length(a - pos) <= glm::length(b - pos)) ? a : b; ok = true; }
        else if (oa) { cand = a; ok = true; }
        else if (ob) { cand = b; ok = true; }
    }
    if (!ok || glm::length(cand - pos) > band) return false;
    out = cand;
    return true;
}

// Rim snapping predates the guide overlay and drew nothing, so landing on a
// circle looked identical to landing nowhere. Publish a guide so it reads like
// every other inference — same diamond marker as On Line, its own label.
void SketchTool::noteRimGuide(glm::vec2 at, int curveId) const {
    m_activeInferences.push_back({InferenceGuide::OnCircle, at, at, curveId});
}

glm::vec2 SketchTool::snap(glm::vec2 pos) const {
    m_snapRimId = -1;
    // Fresh inference set every snap — the renderer treats this as "what's
    // active right now".
    m_activeInferences.clear();

    // Inference-level gates (sketch toolbar Full/Reduced/Off):
    //   Reduced == the classic inference set (everything below). Full adds the
    //   hover-charged references on top. Off keeps ONLY grid snap — every point
    //   snap (endpoints, incl. loop closure onto the chain start, plus face-
    //   reference vertices) is now gated too, so "inferences off" means the
    //   cursor never jumps to geometry (Steve: closing a triangle still snapped
    //   the last vertex to the start with inferences off — that IS an inference).
    //   allowSnaps       — endpoint / midpoint / on-line / face-ref snaps.
    //   allowDirectional — perp/parallel-to-prev, angle, on-line-extension,
    //                      tangent, axis-from-point. ON for both Full & Reduced.
    //   allowCharge      — hover-to-charge references (Full only).
    const bool allowSnaps       = (m_inferenceLevel != InferenceLevel::Off);
    const bool allowDirectional = (m_inferenceLevel != InferenceLevel::Off);
    const bool allowCharge      = (m_inferenceLevel == InferenceLevel::Full ||
                                   m_inferenceLevel == InferenceLevel::Max);

    // Point snap radius. When grid snap is ACTIVE the band is tied PURELY to
    // the grid step (0.6× < one increment), so it can never reach past a
    // neighbouring grid intersection. The old absolute 0.25 mm floor meant a
    // fine grid was swamped by it: at a 0.1 mm grid a second point placed one
    // or two steps (0.1–0.2 mm) from the first snapped straight back onto it —
    // so nothing shorter than ~0.3 mm could be drawn, and an endpoint snap
    // hijacked the cursor within 0.3 mm of any point (Steve's report). Coarse
    // grids are unaffected (gridStep·0.6 already dominated the floor above
    // ~0.42 mm). Grid OFF: the cursor is freehand, so keep an absolute band to
    // grab endpoints reliably.
    // Master snap band — endpoints, midpoints, face centres, on-line and
    // extension guides all derive from it, so the touch widening flows to all.
    const bool gridSnapOnForBand = m_snapToGridEnabled && m_gridStep > 0.0f;
    float pointSnapThreshold =
        (gridSnapOnForBand ? tolStep() * 0.6f : 0.25f) * snapScale();
    float curveSnapThreshold = pointSnapThreshold; // same band for circle/arc perimeters

    // Without a sketch only grid snap can apply.
    if (!m_sketch) {
        if (!m_snapToGridEnabled || m_gridStep <= 0.0f) return pos;
        glm::vec2 r;
        r.x = std::round(pos.x / m_gridStep) * m_gridStep;
        r.y = std::round(pos.y / m_gridStep) * m_gridStep;
        return r;
    }

    // Nearest grid point distance, used so the circle/arc PERIMETER snaps
    // below only win when they sit closer to the cursor than the grid does
    // (grid-snap on). Endpoints still win outright — only the continuous
    // curve competes. (Steve: a rectangle corner dragged near a big circle
    // was snapping to the circle's edge instead of the grid intersection.)
    const bool gridActive = m_snapToGridEnabled && m_gridStep > 0.0f;
    float gridDist = 0.0f;
    if (gridActive) {
        glm::vec2 gp(std::round(pos.x / m_gridStep) * m_gridStep,
                     std::round(pos.y / m_gridStep) * m_gridStep);
        gridDist = glm::length(pos - gp);
    }

    // ─── PHASE 1: Hard point snaps (each early-returns) ──────────────────────
    // Points are unambiguous: there's a specific target to land on, nothing
    // to combine with. Endpoint → circle/arc perimeter → midpoint → face
    // centroid, in priority order.
    // Point snaps (endpoints) are an INFERENCE: gated OFF entirely when the
    // inference level is Off, so nothing captures the cursor — including loop
    // closure onto the chain start (Steve: closing a triangle still snapped
    // the last vertex to the start with inferences off). Grid snap at the end
    // of this function is independent and still applies when enabled.
    const auto& points = m_sketch->getPoints();
    if (allowSnaps) for (const auto& pt : points) {
        // During a drag, skip the points being moved — they'd snap to
        // their own starting position and lock the drag in place.
        if (m_snapExcludePoints.count(pt.id)) continue;
        // Don't SELF-WELD the active segment: while positioning the next
        // vertex, the point just placed (m_lastPointId — the segment's start /
        // previous chain vertex) must not capture the cursor, or the endpoint
        // band welds the new point straight back onto it and you can't draw a
        // segment shorter than the band. This is grid-INDEPENDENT (the band is
        // 0.25 mm even with snap off), which is why Steve couldn't make a line
        // under ~0.25 mm with grid AND inferences both off. Loop closure welds
        // to the CHAIN start (m_chainStartPointId — a different id after the
        // first segment), so auto-close is unaffected.
        if (m_isPlacing && m_lastPointId >= 0 && pt.id == m_lastPointId) continue;
        // Glyph vertices are never snap targets — a word is hundreds of
        // points and drawing near text was impossible.
        if (pt.fromText) continue;
        if (glm::length(pos - pt.pos) < pointSnapThreshold) {
            m_activeInferences.push_back({InferenceGuide::Endpoint, pt.pos, pt.pos, pt.id});
            return pt.pos;
        }
    }
    // Face-reference points (vertices and curve samples from the host face).
    // Same endpoint inference, just sourced from the 3D geometry the sketch
    // was started on. refId of -1 since these aren't sketch points. Gated with
    // the sketch-point snaps above (inferences Off → no capture).
    if (allowSnaps) for (const auto& fp : m_sketch->getFaceReferences().points) {
        if (glm::length(pos - fp) < pointSnapThreshold) {
            m_activeInferences.push_back({InferenceGuide::Endpoint, fp, fp, -1});
            return fp;
        }
    }
    const auto& circles = m_sketch->getCircles();
    for (const auto& c : circles) {
        const SketchPoint* center = m_sketch->getPoint(c.centerPointId);
        if (!center) continue;
        glm::vec2 v = pos - center->pos;
        float dist = glm::length(v);
        if (dist < 1e-6f) continue;
        float r = static_cast<float>(c.radius);
        if (std::abs(dist - r) < curveSnapThreshold) {
            // Off: curve perimeter never snaps (endpoint + grid only).
            if (m_inferenceLevel == InferenceLevel::Off) continue;
            // Full / Max: land where the curve crosses a grid line (on-curve
            // AND grid-aligned). Falls through to the plain perimeter point if
            // no grid crossing sits near the cursor.
            if ((m_inferenceLevel == InferenceLevel::Full ||
                 m_inferenceLevel == InferenceLevel::Max) && gridActive) {
                glm::vec2 gc;
                if (snapCurveToGrid(center->pos, r, pos, m_gridStep,
                                    std::max(curveSnapThreshold, tolStep() * 0.6f), gc))
                    { m_snapRimId = c.id; noteRimGuide(gc, c.id); return gc; }
            }
            // Reduced (and Full/Max fallback): grid wins ties — only land on the
            // bare perimeter when it's genuinely closer than the nearest grid pt.
            if (gridActive && std::abs(dist - r) >= gridDist) continue;
            m_snapRimId = c.id;
            { const glm::vec2 rp = center->pos + (v / dist) * r;
              noteRimGuide(rp, c.id); return rp; }
        }
    }
    const auto& arcs = m_sketch->getArcs();
    for (const auto& a : arcs) {
        const SketchPoint* center = m_sketch->getPoint(a.centerPointId);
        const SketchPoint* spt   = m_sketch->getPoint(a.startPointId);
        const SketchPoint* ept   = m_sketch->getPoint(a.endPointId);
        if (!center || !spt || !ept) continue;
        glm::vec2 v = pos - center->pos;
        float dist = glm::length(v);
        if (dist < 1e-6f) continue;
        float r = static_cast<float>(a.radius);
        if (std::abs(dist - r) < curveSnapThreshold) {
            if (m_inferenceLevel == InferenceLevel::Off) continue;

            // Compute arc span so perimeter snaps are limited to the actual arc,
            // not the extended full circle. Same convention as the midpoint calc.
            const float TWO_PI = 2.0f * static_cast<float>(M_PI);
            float startA = std::atan2(spt->pos.y - center->pos.y,
                                      spt->pos.x - center->pos.x);
            float sweep  = std::atan2(ept->pos.y - center->pos.y,
                                      ept->pos.x - center->pos.x) - startA;
            while (sweep < 0.0f)    sweep += TWO_PI;
            while (sweep >= TWO_PI) sweep -= TWO_PI;
            // Skip if cursor is not within the arc's angular span.
            float cursorA = std::atan2(v.y, v.x) - startA;
            while (cursorA < 0.0f)    cursorA += TWO_PI;
            while (cursorA >= TWO_PI) cursorA -= TWO_PI;
            if (cursorA > sweep) continue;

            if ((m_inferenceLevel == InferenceLevel::Full ||
                 m_inferenceLevel == InferenceLevel::Max) && gridActive) {
                glm::vec2 gc;
                if (snapCurveToGrid(center->pos, r, pos, m_gridStep,
                                    std::max(curveSnapThreshold, tolStep() * 0.6f), gc)) {
                    // Accept grid crossing only when it lies on the arc.
                    float gcA = std::atan2(gc.y - center->pos.y,
                                           gc.x - center->pos.x) - startA;
                    while (gcA < 0.0f)    gcA += TWO_PI;
                    while (gcA >= TWO_PI) gcA -= TWO_PI;
                    if (gcA <= sweep) { m_snapRimId = a.id; noteRimGuide(gc, a.id); return gc; }
                }
                // No grid crossing on the arc — fall through to the plain
                // perimeter point so arcs behave like circles (any on-arc point
                // is reachable even when no grid line crosses the arc span).
                m_snapRimId = a.id;
                { const glm::vec2 rp = center->pos + (v / dist) * r;
                  noteRimGuide(rp, a.id); return rp; }
            }
            if (gridActive && std::abs(dist - r) >= gridDist) continue;
            m_snapRimId = a.id;
            { const glm::vec2 rp = center->pos + (v / dist) * r;
              noteRimGuide(rp, a.id); return rp; }
        }
    }
    // Face-reference circular / arc edges — continuous perimeter snapping for
    // in-plane host/neighbour circles (hole rims, fillet arcs). Mirrors the
    // sketch-circle behaviour: grid wins ties; never fires when inference is Off.
    if (m_inferenceLevel != InferenceLevel::Off) {
        const float TWO_PI = 2.0f * static_cast<float>(M_PI);
        for (const auto& fc : m_sketch->getFaceReferences().circles) {
            glm::vec2 v = pos - fc.center;
            float dist = glm::length(v);
            if (dist < 1e-6f) continue;
            if (std::abs(dist - fc.radius) >= curveSnapThreshold) continue;
            // Honour the arc's angular span (full circles have sweep == 2*PI).
            if (fc.sweep < TWO_PI - 1e-3f) {
                float a = std::atan2(v.y, v.x) - fc.startAngle;
                while (a < 0.0f)    a += TWO_PI;
                while (a >= TWO_PI) a -= TWO_PI;
                if (a > fc.sweep) continue;
            }
            if (gridActive && std::abs(dist - fc.radius) >= gridDist) continue;
            return fc.center + (v / dist) * fc.radius;
        }
    }
    // Line midpoints (matches the green dots drawn by the renderer).
    const auto& lines = m_sketch->getLines();
    for (const auto& ln : lines) {
        if (ln.fromText) continue;
        if (m_snapExcludePoints.count(ln.startPointId) ||
            m_snapExcludePoints.count(ln.endPointId)) continue;
        const SketchPoint* p1 = m_sketch->getPoint(ln.startPointId);
        const SketchPoint* p2 = m_sketch->getPoint(ln.endPointId);
        if (!p1 || !p2) continue;
        glm::vec2 mid = 0.5f * (p1->pos + p2->pos);
        if (allowSnaps && glm::length(pos - mid) < pointSnapThreshold) {
            m_activeInferences.push_back({InferenceGuide::Midpoint, mid, mid, ln.id});
            return mid;
        }
    }
    // Face-reference line midpoints.
    for (const auto& fl : m_sketch->getFaceReferences().lines) {
        glm::vec2 mid = 0.5f * (fl.first + fl.second);
        if (allowSnaps && glm::length(pos - mid) < pointSnapThreshold) {
            m_activeInferences.push_back({InferenceGuide::Midpoint, mid, mid, -1});
            return mid;
        }
    }
    // Arc midpoints.
    for (const auto& a : arcs) {
        const SketchPoint* center = m_sketch->getPoint(a.centerPointId);
        const SketchPoint* s = m_sketch->getPoint(a.startPointId);
        const SketchPoint* e = m_sketch->getPoint(a.endPointId);
        if (!center || !s || !e) continue;
        float startA = std::atan2(s->pos.y - center->pos.y, s->pos.x - center->pos.x);
        float endA = std::atan2(e->pos.y - center->pos.y, e->pos.x - center->pos.x);
        const float TWO_PI = 2.0f * static_cast<float>(M_PI);
        float sweep = endA - startA;
        while (sweep < 0.0f) sweep += TWO_PI;
        while (sweep >= TWO_PI) sweep -= TWO_PI;
        float midA = startA + sweep * 0.5f;
        glm::vec2 mid = center->pos + glm::vec2(std::cos(midA), std::sin(midA))
                                        * static_cast<float>(a.radius);
        if (allowSnaps && glm::length(pos - mid) < pointSnapThreshold) return mid;
    }
    // Centres of face-reference circles (hole rims, and the crest arcs of a
    // threaded rod's cap): the TRUE axis point. Runs before the centroid
    // snap below — on an asymmetric face (thread runout bites a chunk out
    // of a cap) the area centroid sits visibly OFF the axis, and Steve
    // hunting the cylinder centre kept landing on that shifted point.
    if (allowSnaps && m_inferenceLevel != InferenceLevel::Off) {
        for (const auto& fc : m_sketch->getFaceReferences().circles) {
            if (glm::length(pos - fc.center) < pointSnapThreshold)
                return fc.center;
        }
    }
    // The host body's TRUE centre (thread axis ∩ sketch plane, recomputed on
    // every sketch entry — new AND re-edit). It outranks the area centroid,
    // and while it exists the centroid snap is suppressed ENTIRELY: on a
    // threaded cap the centroid sits ~0.3mm off-axis, and with both points
    // live the snap flip-flopped between them ("face center vs object
    // center"), grabbing the wrong one whenever the cursor leaned its way.
    glm::vec2 trueCenter;
    const bool hasTrueCenter = m_sketch->getCenterPoint(trueCenter);
    if (allowSnaps && hasTrueCenter &&
        glm::length(pos - trueCenter) < pointSnapThreshold) {
        return trueCenter;
    }
    // Host face centroid (if sketch was started on a face) — only when no
    // true centre is known.
    glm::vec2 faceCenter;
    if (allowSnaps && !hasTrueCenter &&
        m_sketch->getSourceFaceCentroid(faceCenter) &&
        glm::length(pos - faceCenter) < pointSnapThreshold) {
        return faceCenter;
    }

    // Symmetry: snap to the MIRROR image of an existing point across a candidate
    // axis, so you can place geometry symmetric to the other side (the 4th
    // corner hole mirrors the others; an internal feature mirrors across the
    // centreline). Full / Max only — it's a richer guide, off at Reduced. Axes:
    //   • the sketch's bounding-box vertical & horizontal centrelines, and
    //   • any axis-aligned existing line (a centreline the user drew).
    // Only axis-aligned mirrors, so the snapped point keeps the source's other
    // coordinate — predictable and low-noise. Runs after the hard point snaps,
    // so landing exactly on an existing point still wins.
    if (m_inferenceLevel == InferenceLevel::Full ||
        m_inferenceLevel == InferenceLevel::Max) {
        std::vector<const SketchPoint*> pv;
        float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
        for (const auto& pt : points) {
            if (m_snapExcludePoints.count(pt.id) || pt.fromText) continue;
            pv.push_back(&pt);
            minX = std::min(minX, pt.pos.x); maxX = std::max(maxX, pt.pos.x);
            minY = std::min(minY, pt.pos.y); maxY = std::max(maxY, pt.pos.y);
        }
        // Skip pathologically large sketches (text/dense splines) — symmetry
        // inference there is rarely the intent and the scan would add up.
        if (pv.size() >= 2 && pv.size() <= 200) {
            float bestD = pointSnapThreshold;
            glm::vec2 bestPos = pos, bestSrc(0);
            int bestSrcId = -1;
            bool found = false;
            // DRAW-TIME symmetry, anchored at the LAST placed point of the
            // in-progress chain: the useful suggestions while drawing are the
            // previous point mirrored across the vertical/horizontal line
            // through the anchor (a symmetric V / arch) and its point
            // reflection through the anchor (a smooth S). The bbox-centreline
            // mirrors below can't express these — with two points they only
            // ever suggest the degenerate bbox corner.
            {
                glm::vec2 L(0), prev(0);
                int prevId = -1;
                bool haveChain = false;
                if (m_mode == SketchToolMode::Spline &&
                    m_splinePoints.size() >= 2) {
                    const SketchPoint* pL =
                        m_sketch->getPoint(m_splinePoints.back());
                    const SketchPoint* pP =
                        m_sketch->getPoint(m_splinePoints[m_splinePoints.size() - 2]);
                    if (pL && pP) {
                        L = pL->pos; prev = pP->pos;
                        prevId = pP->id; haveChain = true;
                    }
                } else if (m_mode == SketchToolMode::Line && m_isPlacing &&
                           m_lastPointId >= 0) {
                    // The chain anchor is the current segment start; the
                    // previous vertex is the other end of the newest line
                    // ending there.
                    const SketchPoint* pL = m_sketch->getPoint(m_lastPointId);
                    if (pL) {
                        const auto& lns = m_sketch->getLines();
                        for (auto it = lns.rbegin(); it != lns.rend(); ++it) {
                            int other = -1;
                            if (it->endPointId == m_lastPointId) other = it->startPointId;
                            else if (it->startPointId == m_lastPointId) other = it->endPointId;
                            if (other < 0) continue;
                            const SketchPoint* pP = m_sketch->getPoint(other);
                            if (!pP) continue;
                            L = pL->pos; prev = pP->pos;
                            prevId = pP->id; haveChain = true;
                            break;
                        }
                    }
                }
                if (haveChain) {
                    const glm::vec2 cand[3] = {
                        {2.0f * L.x - prev.x, prev.y},            // mirror across vertical through L
                        {prev.x, 2.0f * L.y - prev.y},            // mirror across horizontal through L
                        {2.0f * L.x - prev.x, 2.0f * L.y - prev.y} // point reflection through L (S)
                    };
                    for (const glm::vec2& m : cand) {
                        if (glm::length(m - prev) < 1e-4f) continue; // degenerate
                        float d = glm::length(pos - m);
                        if (d < bestD) {
                            bestD = d; bestPos = m; bestSrc = prev;
                            bestSrcId = prevId; found = true;
                        }
                    }
                }
            }
            auto tryVertical = [&](float axisX) {
                for (const SketchPoint* b : pv) {
                    if (std::abs(b->pos.x - axisX) < 1e-4f) continue; // on axis
                    glm::vec2 m(2.0f * axisX - b->pos.x, b->pos.y);
                    float d = glm::length(pos - m);
                    if (d < bestD) {
                        bestD = d; bestPos = m; bestSrc = b->pos;
                        bestSrcId = b->id; found = true;
                    }
                }
            };
            auto tryHorizontal = [&](float axisY) {
                for (const SketchPoint* b : pv) {
                    if (std::abs(b->pos.y - axisY) < 1e-4f) continue;
                    glm::vec2 m(b->pos.x, 2.0f * axisY - b->pos.y);
                    float d = glm::length(pos - m);
                    if (d < bestD) {
                        bestD = d; bestPos = m; bestSrc = b->pos;
                        bestSrcId = b->id; found = true;
                    }
                }
            };
            // Bounding-box centrelines — only with 3+ points; with two, the
            // centreline mirror is ALWAYS the degenerate bbox corner (level
            // with one point, above the other), which reads as noise.
            if (pv.size() >= 3) {
                tryVertical((minX + maxX) * 0.5f);
                tryHorizontal((minY + maxY) * 0.5f);
            }
            // Axis-aligned existing lines as mirror axes.
            for (const auto& ln : lines) {
                if (ln.fromText) continue;
                const SketchPoint* a = m_sketch->getPoint(ln.startPointId);
                const SketchPoint* b = m_sketch->getPoint(ln.endPointId);
                if (!a || !b) continue;
                if (std::abs(a->pos.x - b->pos.x) < 1e-4f) tryVertical(a->pos.x);
                else if (std::abs(a->pos.y - b->pos.y) < 1e-4f) tryHorizontal(a->pos.y);
            }
            if (found) {
                // Guide pairs the source with its mirror (a dashed line crossing
                // the axis); the label names it "Symmetry".
                m_activeInferences.push_back(
                    {InferenceGuide::Symmetry, bestSrc, bestPos, bestSrcId});
                return bestPos;
            }
        }
    }

    // ─── PHASE 2: Collect line-shaped inference candidates ───────────────────
    // Every inference whose shape is a LINE (not a point) registers itself
    // here rather than snapping immediately. The resolver in phase 3 picks
    // either the intersection of two candidates or a single projection — so
    // "perpendicular to charged point AND on edge" lands on the actual
    // crossing instead of one displacing the other.
    // (Steve's rule: two inferences should take hold at once when they
    //  line up at a desirable point.)
    struct LineCand {
        glm::vec2 anchor;          // point on the line
        glm::vec2 dir;             // unit direction
        InferenceGuide::Kind kind;
        int refId;
        glm::vec2 visFrom;         // "from" anchor for the rendered guide line
        bool isSegment;            // if true, t∈[0,segLen] required for snaps & intersections
        float segLen;
        glm::vec2 proj;            // cursor's projection onto this line
        float perpDist;            // |proj - pos|
        // Can fire as the LONE snap? Incidental axis-from-point is false —
        // sketch points + face vertices are dense and a standalone guide
        // for every drift was visual noise. Such cands still participate
        // in pair-intersection (as one half of a useful composite).
        bool standaloneAllowed;
    };
    std::vector<LineCand> cands;

    // Axis-from-point guide band — same grid-relative treatment as the point
    // snap above: with grid snap on, an absolute 0.2 mm floor would fire a
    // horizontal/vertical guide off a point within 0.2 mm on a fine grid,
    // hijacking the cursor within one increment. Tie it to the grid instead.
    const float axisThresh   = (gridActive ? tolStep() * 0.3f : 0.2f);
    const float onLineThresh = pointSnapThreshold * 0.7f;
    const float extThresh    = pointSnapThreshold * 0.6f;
    // POSITIONAL cap on directional / charged inferences: fires-checks are
    // ANGULAR for those, so capture distance grows with segment length (3°
    // at 100 mm = 5 mm of cursor theft). An inference may only pull the
    // cursor a short distance from where the user actually is. Grid-relative
    // when snap is on (same reasoning as the point/axis bands): the old
    // absolute 1.5 mm floor let a directional guide yank the cursor ~1.5 mm —
    // 15 increments at a 0.1 mm grid — so once the (now tight) endpoint band
    // stopped grabbing, these took over and the preview wouldn't start until
    // ~1.3 mm out. Tie the pull to the grid so it can't reach past ~1.5
    // increments; coarse grids and grid-off keep the absolute cap.
    const float posCap       = (gridActive ? tolStep() * 1.5f : 1.5f);

    // On-line: cursor's perpendicular projection lands within an existing
    // sketch segment.
    if (allowSnaps) {
        for (const auto& ln : lines) {
            // fromText (SVG-import / Text-tool) segments ARE valid on-edge
            // targets: landing a point on an imported outline is how you anchor
            // new geometry to it and close an extrudable region against it
            // (Sketch::buildWires then splits the segment at the contact point,
            // so the loop-walker can route the new loop through it). They stay
            // excluded from endpoint/midpoint/symmetry snaps and every
            // directional guide, so a dense outline never spams inference — only
            // this perpendicular on-edge landing, cheap and unambiguous, fires.
            if (m_snapExcludePoints.count(ln.startPointId) ||
                m_snapExcludePoints.count(ln.endPointId)) continue;
            const SketchPoint* p1 = m_sketch->getPoint(ln.startPointId);
            const SketchPoint* p2 = m_sketch->getPoint(ln.endPointId);
            if (!p1 || !p2) continue;
            glm::vec2 ab = p2->pos - p1->pos;
            float len = glm::length(ab);
            if (len < 1e-6f) continue;
            glm::vec2 dir = ab / len;
            float t = glm::dot(pos - p1->pos, dir);
            float tClamped = glm::clamp(t, 0.0f, len);
            glm::vec2 proj = p1->pos + dir * tClamped;
            float d = glm::distance(pos, proj);
            if (d < onLineThresh) {
                cands.push_back({p1->pos, dir, InferenceGuide::OnLine, ln.id,
                                 proj, true, len, proj, d, true});
            }
        }
        // Face-ref straight edges.
        for (const auto& fl : m_sketch->getFaceReferences().lines) {
            glm::vec2 ab = fl.second - fl.first;
            float len = glm::length(ab);
            if (len < 1e-6f) continue;
            glm::vec2 dir = ab / len;
            float t = glm::dot(pos - fl.first, dir);
            float tClamped = glm::clamp(t, 0.0f, len);
            glm::vec2 proj = fl.first + dir * tClamped;
            float d = glm::distance(pos, proj);
            if (d < onLineThresh) {
                cands.push_back({fl.first, dir, InferenceGuide::OnLine, -1,
                                 proj, true, len, proj, d, true});
            }
        }
        // On-spline: cursor's projection lands anywhere along a spline curve.
        // Splines were invisible to snapping, so you couldn't anchor a line onto
        // an imported cursive stroke. Project onto the sampled curve and register
        // the hit with the LOCAL tangent so it resolves like an on-edge landing;
        // a line drawn across the region then divides it via the region splitter.
        for (const auto& sp : m_sketch->getSplines()) {
            if (sp.isConstruction || sp.controlPointIds.size() < 2) continue;
            bool excluded = false;
            for (int cpid : sp.controlPointIds)
                if (m_snapExcludePoints.count(cpid)) { excluded = true; break; }
            if (excluded) continue;
            std::vector<glm::vec2> samp = m_sketch->sampleSpline2D(sp, 24); // match buildWires
            float bestD = onLineThresh; glm::vec2 bestProj(0.0f), bestDir(1.0f, 0.0f);
            bool found = false;
            for (size_t k = 0; k + 1 < samp.size(); ++k) {
                glm::vec2 a = samp[k], b = samp[k + 1], ab = b - a;
                float len = glm::length(ab);
                if (len < 1e-6f) continue;
                glm::vec2 dir = ab / len;
                float t = glm::clamp(glm::dot(pos - a, dir), 0.0f, len);
                glm::vec2 proj = a + dir * t;
                float d = glm::distance(pos, proj);
                if (d < bestD) { bestD = d; bestProj = proj; bestDir = dir; found = true; }
            }
            if (found) {
                cands.push_back({bestProj, bestDir, InferenceGuide::OnLine, -1,
                                 bestProj, true, onLineThresh * 4.0f, bestProj, bestD, true});
            }
        }
    }

    // On-line-extension: cursor lies on the infinite line through an
    // existing segment, but OUTSIDE the segment's endpoints.
    if (allowDirectional) {
        for (const auto& ln : lines) {
            if (ln.fromText) continue;
            if (m_snapExcludePoints.count(ln.startPointId) ||
                m_snapExcludePoints.count(ln.endPointId)) continue;
            const SketchPoint* p1 = m_sketch->getPoint(ln.startPointId);
            const SketchPoint* p2 = m_sketch->getPoint(ln.endPointId);
            if (!p1 || !p2) continue;
            glm::vec2 ab = p2->pos - p1->pos;
            float len = glm::length(ab);
            if (len < 1e-6f) continue;
            glm::vec2 dir = ab / len;
            float t = glm::dot(pos - p1->pos, dir);
            if (t >= 0.0f && t <= len) continue; // in-segment handled by on-line above
            glm::vec2 proj = p1->pos + dir * t;
            float d = glm::distance(pos, proj);
            if (d < extThresh) {
                // Visual anchor: existing segment's midpoint, so the dashed
                // "extension" overlay draws the whole infinite line cleanly
                // through both the segment and the snapped cursor.
                glm::vec2 visFrom = 0.5f * (p1->pos + p2->pos);
                cands.push_back({p1->pos, dir, InferenceGuide::OnLineExtension,
                                 ln.id, visFrom, false, 0.0f, proj, d, true});
            }
        }
    }

    // Axis-from-point: each near sketch / face-ref point pushes its vertical
    // and/or horizontal guide. Multiple points contribute — cursor can be
    // near two points' axes at once and intersect them.
    // Marked standaloneAllowed=false: cursor is almost always incidentally
    // aligned with SOMEONE's coord, so a lone guide for every drift was
    // noise. They still serve as one half of a pair-intersection (the
    // "axis-from-point + perp-to-prev" composite this code originally
    // handled inline via applyDirLock).
    // Includes the chain anchor itself — drawing horizontal / vertical FROM
    // the anchor is one of the most common cases. Dragged points are
    // skipped (a guide from a point to itself is meaningless).
    // Before the FIRST point of an item there's no chain guide to pair with, so
    // a lone axis-from-point alignment would never fire — yet aligning the start
    // point to an existing vertex's X/Y is exactly what you want there. Let these
    // stand alone pre-placement (Full / Max only). During placement they stay
    // pair-only (standaloneAllowed=false) to avoid the "aligned with someone's
    // coord on every drift" noise the comment above describes.
    const bool preStartAxis =
        !m_isPlacing && (m_inferenceLevel == InferenceLevel::Full ||
                         m_inferenceLevel == InferenceLevel::Max);
    if (allowDirectional) {
        for (const auto& pt : m_sketch->getPoints()) {
            if (m_snapExcludePoints.count(pt.id)) continue;
            if (pt.fromText) continue;
            // Charged ref is added with bigger tolerance + standaloneAllowed
            // by the charged block below — skip here to avoid two cands for
            // the same line. Only matters when the charged ref IS this sketch
            // point; midpoint / face-ref charged kinds don't shadow this pt.
            if (allowCharge &&
                m_charged.kind == ChargedRef::Kind::SketchPoint &&
                pt.id == m_charged.sourceId) continue;
            float dX = std::abs(pos.x - pt.pos.x);
            float dY = std::abs(pos.y - pt.pos.y);
            if (dX < axisThresh) {
                glm::vec2 proj(pt.pos.x, pos.y);
                cands.push_back({pt.pos, glm::vec2(0,1), InferenceGuide::AxisVFromPoint,
                                 pt.id, pt.pos, false, 0.0f, proj, dX, preStartAxis});
            }
            if (dY < axisThresh) {
                glm::vec2 proj(pos.x, pt.pos.y);
                cands.push_back({pt.pos, glm::vec2(1,0), InferenceGuide::AxisHFromPoint,
                                 pt.id, pt.pos, false, 0.0f, proj, dY, preStartAxis});
            }
        }
        for (const auto& fp : m_sketch->getFaceReferences().points) {
            float dX = std::abs(pos.x - fp.x);
            float dY = std::abs(pos.y - fp.y);
            if (dX < axisThresh) {
                glm::vec2 proj(fp.x, pos.y);
                cands.push_back({fp, glm::vec2(0,1), InferenceGuide::AxisVFromPoint,
                                 -1, fp, false, 0.0f, proj, dX, preStartAxis});
            }
            if (dY < axisThresh) {
                glm::vec2 proj(pos.x, fp.y);
                cands.push_back({fp, glm::vec2(1,0), InferenceGuide::AxisHFromPoint,
                                 -1, fp, false, 0.0f, proj, dY, preStartAxis});
            }
        }
    }

    // Perpendicular / parallel to previous: within ~5° of perp or parallel
    // to the chain's last committed segment, anchored at the current first
    // click. Geometrically mutually exclusive — whichever is closer fires.
    glm::vec2 dirAnchor;
    const bool haveDirAnchor = directionalAnchor(dirAnchor);
    // m_hasPrevLineDir is the mode guard here: only the Line chain and the
    // spline control polygon ever set it, and setMode clears it, so an arc or
    // polygon can never inherit a previous leg from an earlier draw.
    if (allowDirectional && haveDirAnchor && m_hasPrevLineDir) {
        glm::vec2 perpDir(-m_prevLineDir.y, m_prevLineDir.x);
        glm::vec2 parDir = m_prevLineDir;
        glm::vec2 v = pos - dirAnchor;
        float len = glm::length(v);
        if (len > 1e-6f) {
            glm::vec2 perpProj = dirAnchor + perpDir * glm::dot(v, perpDir);
            glm::vec2 parProj  = dirAnchor + parDir  * glm::dot(v, parDir);
            float perpOffset = glm::distance(pos, perpProj);
            float parOffset  = glm::distance(pos, parProj);
            // Within ~5° (sin5° ≈ 0.087) of perp / parallel, OR within
            // axisThresh in absolute world units — whichever is more
            // generous at this segment length.
            float tol = std::max(axisThresh, 0.087f * len) * angleScale();
            bool perpClose = perpOffset < tol;
            bool parClose  = parOffset  < tol;
            if (perpClose && (!parClose || perpOffset <= parOffset)) {
                cands.push_back({dirAnchor, perpDir, InferenceGuide::PerpToPrev,
                                 -1, dirAnchor, false, 0.0f, perpProj, perpOffset, true});
            } else if (parClose) {
                cands.push_back({dirAnchor, parDir, InferenceGuide::ParallelToPrev,
                                 -1, dirAnchor, false, 0.0f, parProj, parOffset, true});
            }
        }
    }

    // Tangent-to-circle / arc: cursor direction from anchor within ~3° of
    // a tangent ray to a circle/arc. The BEST-fitting curve wins — with two
    // holes near the cursor, first-match-wins latched onto whichever happened
    // to be earlier in the list rather than the one being aimed at.
    if (allowDirectional && haveDirAnchor) {
        glm::vec2 v = pos - dirAnchor;
        float len = glm::length(v);
        if (len > 0.5f) {
            float cursorAngle = std::atan2(v.y, v.x);
            const float angTol = 3.0f * static_cast<float>(M_PI) / 180.0f * angleScale();
            const float TWO_PI = 2.0f * static_cast<float>(M_PI);
            auto angDiff = [&](float a, float b) {
                float d = std::fmod(std::abs(a - b), TWO_PI);
                if (d > static_cast<float>(M_PI)) d = TWO_PI - d;
                return d;
            };
            float bestFit = angTol;
            bool  haveFit = false;
            glm::vec2 bestDir(1.0f, 0.0f);
            int   bestRef = -1;
            // Every curve kind ends up here: a candidate tangent RAY leaving
            // the anchor, keyed by how far the cursor is off it.
            auto considerRay = [&](float ang, int refId) {
                const float delta = angDiff(cursorAngle, ang);
                if (delta >= bestFit) return;
                bestFit = delta;
                bestDir = glm::vec2(std::cos(ang), std::sin(ang));
                bestRef = refId;
                haveFit = true;
            };
            // `span` non-null limits the tangency POINT to a real arc: an arc
            // is not its full circle, and a guide tangent to the phantom part
            // of the circle points at geometry that isn't there.
            struct ArcSpan { float startA; float sweep; };
            auto checkTangent = [&](glm::vec2 cpos, double cradius, int refId,
                                    const ArcSpan* span) {
                const float R = static_cast<float>(cradius);
                if (R <= 1e-6f) return;
                glm::vec2 toC = cpos - dirAnchor;
                float D = glm::length(toC);
                // Only an anchor strictly INSIDE the circle has no tangent
                // through it. ON the rim there is exactly one, and the
                // two-tangent formula degenerates cleanly onto it —
                // asin(R/D) = asin(1) = 90° off the radius, both ways. That
                // is the case people actually mean by "tangent" (start the
                // line on the circle and run it off tangentially, or carry on
                // tangentially from the end of an arc), and the old
                // `D <= R` guard threw away every one of them: it fired only
                // for an anchor standing clear of the curve. asin's argument
                // is clamped because at D == R float error can push R/D a hair
                // over 1 and hand back a NaN.
                if (D < R - 1e-3f || D < 1e-6f) return;
                float baseAngle = std::atan2(toC.y, toC.x);
                float tangentOffset = std::asin(std::min(1.0f, R / D));
                // Distance from the anchor to the point of tangency; 0 when the
                // anchor is itself on the rim.
                float tanLen = std::sqrt(std::max(0.0f, D * D - R * R));
                for (int s = 0; s < 2; ++s) {
                    const float ang = (s == 0) ? baseAngle + tangentOffset
                                               : baseAngle - tangentOffset;
                    if (span) {
                        const glm::vec2 tDir(std::cos(ang), std::sin(ang));
                        const glm::vec2 T = dirAnchor + tDir * tanLen;
                        float tA = std::atan2(T.y - cpos.y, T.x - cpos.x) - span->startA;
                        while (tA < 0.0f)     tA += TWO_PI;
                        while (tA >= TWO_PI)  tA -= TWO_PI;
                        if (tA > span->sweep + 1e-4f) continue;
                    }
                    considerRay(ang, refId);
                }
            };
            for (const auto& circle : m_sketch->getCircles()) {
                const SketchPoint* center = m_sketch->getPoint(circle.centerPointId);
                if (center) checkTangent(center->pos, circle.radius, circle.id, nullptr);
            }
            for (const auto& arc : m_sketch->getArcs()) {
                const SketchPoint* center = m_sketch->getPoint(arc.centerPointId);
                const SketchPoint* spt    = m_sketch->getPoint(arc.startPointId);
                const SketchPoint* ept    = m_sketch->getPoint(arc.endPointId);
                if (!center || !spt || !ept) continue;
                // Same CCW start→end convention as the perimeter snap above.
                ArcSpan span;
                span.startA = std::atan2(spt->pos.y - center->pos.y,
                                         spt->pos.x - center->pos.x);
                span.sweep  = std::atan2(ept->pos.y - center->pos.y,
                                         ept->pos.x - center->pos.x) - span.startA;
                while (span.sweep < 0.0f)    span.sweep += TWO_PI;
                while (span.sweep >= TWO_PI) span.sweep -= TWO_PI;
                checkTangent(center->pos, arc.radius, arc.id, &span);
            }
            // Splines. A spline has no closed-form tangent-from-a-point, but
            // the sampled polyline buildWires already uses gives one cheaply:
            // the ray anchor->P is tangent exactly where (P - anchor) turns
            // parallel to the local tangent there, i.e. where their cross
            // product changes sign. Walk the samples and interpolate onto each
            // crossing. fromText splines (SVG / Text import) are skipped for
            // the same reason every other directional guide skips them — an
            // imported outline is dense enough to spam a guide per stroke.
            for (const auto& sp : m_sketch->getSplines()) {
                if (sp.controlPointIds.size() < 2) continue;
                bool skip = false;
                for (int cpid : sp.controlPointIds) {
                    if (m_snapExcludePoints.count(cpid)) { skip = true; break; }
                    const SketchPoint* cp = m_sketch->getPoint(cpid);
                    if (cp && cp->fromText) { skip = true; break; }
                }
                if (skip) continue;
                // Coarser than the 24-per-span the on-curve snap uses: this
                // scan only needs to BRACKET the sign change, and the linear
                // interpolation onto the crossing supplies the precision.
                // Measured against control points laid on a known circle, 8
                // per span puts the guide within a small fraction of a degree
                // of the true tangent — against a 3-degree catch window — for
                // a third of the sampling work on every mouse move.
                const std::vector<glm::vec2> samp = m_sketch->sampleSpline2D(sp, 8);
                if (samp.size() < 3) continue;
                auto cross2 = [](glm::vec2 a, glm::vec2 b) {
                    return a.x * b.y - a.y * b.x;
                };
                bool havePrev = false;
                float prevF = 0.0f;
                glm::vec2 prevMid(0.0f);
                for (size_t k = 0; k + 1 < samp.size(); ++k) {
                    const glm::vec2 T = samp[k + 1] - samp[k];
                    const float tl = glm::length(T);
                    if (tl < 1e-9f) continue;
                    const glm::vec2 mid = 0.5f * (samp[k] + samp[k + 1]);
                    const glm::vec2 toP = mid - dirAnchor;
                    const float pl = glm::length(toP);
                    if (pl < 1e-6f) { havePrev = false; continue; }
                    // Normalized so the crossing test is scale-free.
                    const float f = cross2(toP, T) / (pl * tl);
                    if (havePrev && ((prevF <= 0.0f && f > 0.0f) ||
                                     (prevF >= 0.0f && f < 0.0f))) {
                        const float den = prevF - f;
                        const float t = (std::abs(den) > 1e-12f)
                                            ? glm::clamp(prevF / den, 0.0f, 1.0f)
                                            : 0.5f;
                        const glm::vec2 touch = prevMid + (mid - prevMid) * t;
                        const glm::vec2 d = touch - dirAnchor;
                        if (glm::length(d) > 1e-6f)
                            considerRay(std::atan2(d.y, d.x), sp.id);
                    }
                    prevF = f;
                    prevMid = mid;
                    havePrev = true;
                }
            }

            if (haveFit) {
                glm::vec2 tProj = dirAnchor + bestDir * len;
                float perpOffset = glm::distance(tProj, pos);
                cands.push_back({dirAnchor, bestDir, InferenceGuide::TangentToCircle,
                                 bestRef, dirAnchor, false, 0.0f,
                                 tProj, perpOffset, true});
            }
        }
    }

    // Corner bisector / corner tangent: the anchor is a VERTEX where two
    // existing segments meet. Tangency has no single answer at a corner — the
    // chain has TWO tangent directions there, one per incident segment, and
    // OnLineExtension already offers both. What a corner does define uniquely
    // are the two directions built from the pair, with a and b the unit rays
    // from the vertex to its neighbours:
    //
    //   • Bisector — norm(a + b). "The angle directly between the two lines":
    //     the miter direction a corner offset follows, and the axis anything
    //     symmetric about the corner sits on (a centred slot, a witness line
    //     down the middle of a chamfer).
    //   • Corner tangent — norm(b − a): the average of the two TRAVEL
    //     directions (in = −a, out = b), i.e. the tangent a smooth curve
    //     through the three points would carry through the vertex. Exactly
    //     perpendicular to the bisector, since (b − a)·(b + a) = |b|² − |a|²
    //     = 0 for unit vectors — so the two are one pair of guides at right
    //     angles, not two arbitrary rays.
    //
    // Each is registered as an infinite LINE through the vertex, so pointing
    // either way along it counts. Full / Max only: at Reduced the corner
    // already publishes two extension guides and these would crowd them.
    if (allowDirectional && haveDirAnchor &&
        (m_inferenceLevel == InferenceLevel::Full ||
         m_inferenceLevel == InferenceLevel::Max)) {
        glm::vec2 v = pos - dirAnchor;
        const float len = glm::length(v);
        if (len > 1e-3f) {
            // Unit rays from the anchor to every neighbour joined to it by a
            // straight segment. Position-matched rather than id-matched so
            // face-reference edges (which carry no ids) contribute the same
            // way sketch lines do — sketching on a face, the corner you
            // anchor on is usually the face's own.
            const float vtxTol = 1e-4f;
            std::vector<glm::vec2> rays;
            auto addRay = [&](glm::vec2 from, glm::vec2 to) {
                if (rays.size() >= 4) return;             // dense hub: bail below
                glm::vec2 d = to - from;
                const float l = glm::length(d);
                if (l < 1e-6f) return;
                d /= l;
                // One ray per DIRECTION: two collinear segments meeting at the
                // vertex describe the same neighbour twice.
                for (const auto& r : rays)
                    if (glm::dot(r, d) > 0.9998f) return;
                rays.push_back(d);
            };
            for (const auto& ln : lines) {
                if (ln.fromText) continue;
                if (m_snapExcludePoints.count(ln.startPointId) ||
                    m_snapExcludePoints.count(ln.endPointId)) continue;
                const SketchPoint* p1 = m_sketch->getPoint(ln.startPointId);
                const SketchPoint* p2 = m_sketch->getPoint(ln.endPointId);
                if (!p1 || !p2) continue;
                if (glm::length(p1->pos - dirAnchor) < vtxTol) addRay(dirAnchor, p2->pos);
                else if (glm::length(p2->pos - dirAnchor) < vtxTol) addRay(dirAnchor, p1->pos);
            }
            for (const auto& fl : m_sketch->getFaceReferences().lines) {
                if (glm::length(fl.first - dirAnchor) < vtxTol) addRay(dirAnchor, fl.second);
                else if (glm::length(fl.second - dirAnchor) < vtxTol) addRay(dirAnchor, fl.first);
            }
            // Two or three segments meeting is a corner; a denser hub would
            // publish a bisector for every pair, which is noise, not guidance.
            if (rays.size() >= 2 && rays.size() <= 3) {
                // A pure 3 deg window, like the tangent guide — deliberately
                // WITHOUT perp/parallel-to-prev's absolute floor. That floor
                // is a fixed distance, so as the segment shrinks it opens the
                // angular window up: at a 1 mm grid it means 5.7 deg of catch
                // on a 3 mm leg and worse below that, which is exactly the
                // "it highlights when I'm nowhere near the bisector" Steve
                // reported. An angular relationship deserves an angular
                // tolerance at every length.
                const float tol = std::sin(0.0524f * angleScale()) * len;
                auto pushAxis = [&](glm::vec2 dir, InferenceGuide::Kind kind) {
                    glm::vec2 proj = dirAnchor + dir * glm::dot(v, dir);
                    const float off = glm::distance(pos, proj);
                    if (off >= tol) return;
                    cands.push_back({dirAnchor, dir, kind, -1, dirAnchor,
                                     false, 0.0f, proj, off, true});
                };
                for (size_t i = 0; i < rays.size(); ++i) {
                    for (size_t j = i + 1; j < rays.size(); ++j) {
                        const glm::vec2 a = rays[i], b = rays[j];
                        // Degenerate pairs have no guide to give: a straight
                        // vertex (a ≈ −b) has no bisector, and a doubled-back
                        // spike (a ≈ b) has no distinct corner tangent. Both
                        // survivors are already covered by OnLineExtension.
                        const glm::vec2 bis = a + b;
                        if (glm::length(bis) > 1e-3f)
                            pushAxis(glm::normalize(bis), InferenceGuide::CornerBisector);
                        const glm::vec2 tan = b - a;
                        if (glm::length(tan) > 1e-3f)
                            pushAxis(glm::normalize(tan), InferenceGuide::CornerTangent);
                    }
                }
            }
        }
    }

    // Hover-charged reference (Full level): the dwelt-on reference projects
    // vertical, horizontal, and per-touching-line perpendicular guides AT
    // its position. Wider tolerance than incidental axis-from-point
    // (posCap, not axisThresh) — the user deliberately charged this — and
    // standaloneAllowed=true so it can be the lone snap.
    //
    // The set of "touching lines" feeding PerpToRef depends on the charged
    // kind: a sketch point scans every sketch line whose endpoint is it; a
    // sketch line midpoint has exactly one touching line (the segment
    // itself); a face vertex / face-edge midpoint use the host-face
    // references in the same way.
    if (allowCharge && haveDirAnchor && m_charged.kind != ChargedRef::Kind::None) {
        const glm::vec2 R = m_charged.pos;
        const int chargedSrc = m_charged.sourceId; // sketch refId for the V/H/perp guides
        float dxR = std::abs(pos.x - R.x);
        if (dxR < posCap) {
            glm::vec2 proj(R.x, pos.y);
            cands.push_back({R, glm::vec2(0,1), InferenceGuide::AxisVFromPoint,
                             chargedSrc, R, false, 0.0f, proj, dxR, true});
        }
        float dyR = std::abs(pos.y - R.y);
        if (dyR < posCap) {
            glm::vec2 proj(pos.x, R.y);
            cands.push_back({R, glm::vec2(1,0), InferenceGuide::AxisHFromPoint,
                             chargedSrc, R, false, 0.0f, proj, dyR, true});
        }
        // Helper: push a PerpToRef cand perpendicular to a (p1, p2) segment
        // through R, if the cursor is near that ray.
        auto pushPerp = [&](glm::vec2 p1, glm::vec2 p2) {
            glm::vec2 e = p2 - p1;
            if (glm::length(e) < 1e-6f) return;
            e = glm::normalize(e);
            glm::vec2 perp(-e.y, e.x);
            float t = glm::dot(pos - R, perp);
            glm::vec2 proj = R + perp * t;
            float d = glm::length(proj - pos);
            if (d < posCap) {
                cands.push_back({R, perp, InferenceGuide::PerpToRef,
                                 chargedSrc, R, false, 0.0f, proj, d, true});
            }
        };
        switch (m_charged.kind) {
        case ChargedRef::Kind::SketchPoint:
            for (const auto& ln : m_sketch->getLines()) {
                if (ln.fromText) continue;
                if (ln.startPointId != m_charged.sourceId &&
                    ln.endPointId   != m_charged.sourceId) continue;
                const SketchPoint* p1 = m_sketch->getPoint(ln.startPointId);
                const SketchPoint* p2 = m_sketch->getPoint(ln.endPointId);
                if (p1 && p2) pushPerp(p1->pos, p2->pos);
            }
            break;
        case ChargedRef::Kind::SketchLineMid: {
            const SketchLine* ln = nullptr;
            for (const auto& l : m_sketch->getLines()) {
                if (l.id == m_charged.sourceId) { ln = &l; break; }
            }
            if (ln) {
                const SketchPoint* p1 = m_sketch->getPoint(ln->startPointId);
                const SketchPoint* p2 = m_sketch->getPoint(ln->endPointId);
                if (p1 && p2) pushPerp(p1->pos, p2->pos);
            }
            break;
        }
        case ChargedRef::Kind::FacePoint:
            // Face edges meeting at this corner — matched by endpoint
            // position since face refs have no ids.
            for (const auto& fl : m_sketch->getFaceReferences().lines) {
                const float tol = 1e-4f;
                if (glm::length(fl.first - R) < tol ||
                    glm::length(fl.second - R) < tol) {
                    pushPerp(fl.first, fl.second);
                }
            }
            break;
        case ChargedRef::Kind::FaceLineMid:
            for (const auto& fl : m_sketch->getFaceReferences().lines) {
                glm::vec2 mid = 0.5f * (fl.first + fl.second);
                if (glm::length(mid - R) < 1e-4f) {
                    pushPerp(fl.first, fl.second);
                    break;
                }
            }
            break;
        case ChargedRef::Kind::None: break;
        }
    }

    // ─── PHASE 3: Resolve ────────────────────────────────────────────────────
    // (a) Pair intersection — if two cands intersect at a point within posCap
    //     of the cursor (and within each one's segment bounds), prefer the
    //     intersection closest to the cursor. Both guides render. This is the
    //     "perpendicular AND on edge" composite.
    // (b) Single-line projection — smallest perpDist among standaloneAllowed
    //     cands.
    // (c) Angle-snap fallback — 15° increment from the chain anchor.
    // (d) Grid snap.
    auto intersect2 = [](glm::vec2 a1, glm::vec2 d1, glm::vec2 a2, glm::vec2 d2,
                         glm::vec2& out, float& outT1, float& outT2) -> bool {
        // Cramer's on  [d1.x, -d2.x; d1.y, -d2.y] [t1; t2] = a2 - a1.
        float det = d1.y * d2.x - d1.x * d2.y;
        if (std::abs(det) < 1e-9f) return false; // parallel / coincident
        glm::vec2 dp = a2 - a1;
        outT1 = (d2.x * dp.y - d2.y * dp.x) / det;
        outT2 = (d1.x * dp.y - d1.y * dp.x) / det;
        out = a1 + d1 * outT1;
        return true;
    };

    // Snap a point ON a line constraint to the grid lattice. Picks the
    // dominant axis of the line direction (X if mostly horizontal, Y if
    // mostly vertical, X arbitrarily at 45°), rounds that coord to the
    // nearest grid step, and re-solves the other coord on the line. So
    // placements along an edge / charged guide / perp ray / angle-snap
    // land on the grid instead of wherever the cursor's perpendicular
    // foot happened to be. Skipped when grid is off or step is 0.
    // (Steve: any guide that uses an edge / face feature should still
    //  adhere to the snap grid.)
    auto gridAlongLine = [&](glm::vec2 anchor, glm::vec2 dir,
                              bool isSegment, float segLen,
                              glm::vec2 unsnapped) -> glm::vec2 {
        if (!m_snapToGridEnabled || m_gridStep <= 0.0f) return unsnapped;
        if (std::abs(dir.x) >= std::abs(dir.y) && std::abs(dir.x) > 1e-6f) {
            float snappedX = std::round(unsnapped.x / m_gridStep) * m_gridStep;
            float t = (snappedX - anchor.x) / dir.x;
            if (isSegment) t = glm::clamp(t, 0.0f, segLen);
            return anchor + dir * t;
        }
        if (std::abs(dir.y) > 1e-6f) {
            float snappedY = std::round(unsnapped.y / m_gridStep) * m_gridStep;
            float t = (snappedY - anchor.y) / dir.y;
            if (isSegment) t = glm::clamp(t, 0.0f, segLen);
            return anchor + dir * t;
        }
        return unsnapped;
    };

    // Land a guide result ON the snap lattice. With snap-to-grid on, the grid
    // is an explicit precision request, and a DIRECTIONAL guide (perpendicular
    // / parallel / axis-from-point / angle / tangent) only says which WAY to
    // go — it has no business deciding where between two grid lines you end
    // up. gridAlongLine below only rounds the guide's dominant axis and solves
    // the other one on the line, so the free coordinate came out at whatever
    // the geometry happened to give: a perpendicular off a chain landed at
    // x=9.0033 on a 0.1 mm grid, a guide pair at y=12.2012. Over a few
    // segments that reads as the grid wandering arbitrarily (Steve,
    // 2026-07-31: "I cannot draw a line on that snap grid").
    //
    // CONTACT guides are the exception and must stay exact: an OnLine landing
    // is a point ON an existing edge, which is a topological claim — buildWires
    // splits that segment at the contact point to route a loop through it, and
    // a point rounded a few microns off the edge silently stops closing the
    // region. Those keep gridAlongLine's on-the-line result.
    auto onLattice = [&](glm::vec2 p) {
        if (!m_snapToGridEnabled || m_gridStep <= 0.0f) return p;
        return glm::vec2(std::round(p.x / m_gridStep) * m_gridStep,
                         std::round(p.y / m_gridStep) * m_gridStep);
    };
    auto isContact = [](InferenceGuide::Kind k) {
        return k == InferenceGuide::OnLine;
    };
    // A guide direction and the snap lattice can only both be honoured when
    // the ray is AXIS-ALIGNED: such a ray passes through lattice points, so
    // rounding both coordinates keeps the point on it. Any other direction
    // cannot — onLattice moves the point up to half a diagonal cell OFF the
    // ray, and near the anchor that is degrees of angular error (measured on
    // a 1 mm grid: 8.4 deg on a 3 mm leg off a 70 deg corner, 3.4 deg at
    // 5 mm, decaying as 1/length). The guide still highlights, because it
    // fired on DIRECTION, so the tool claims an exact bisector / tangent and
    // then draws something visibly off it — Steve, 2026-09-03. For a
    // genuinely diagonal ray the angle IS the user's intent, so stay exactly
    // on it and let gridAlongLine put the dominant coordinate on a step.
    // (A 45 deg ray is unaffected either way: gridAlongLine already lands
    // both coordinates on the lattice there.)
    auto axisAligned = [](glm::vec2 d) {
        return std::abs(d.x) < 1e-4f || std::abs(d.y) < 1e-4f;
    };

    // Intersection cap is WIDER than the single-line posCap: each cand only
    // enters the list after passing its own perpDist tolerance, so a two-cand
    // pair is already user-deliberate (both inferences fired cleanly). The
    // intersection then sits wherever geometry puts it — for a steep edge ×
    // charged vertical, that can be a few mm off the cursor's perpendicular
    // path. With posCap (1.5 mm) we'd silently fall through to single-line
    // OnLine even though "On Line + On Vertical Axis" both showed as fired,
    // which is what Steve hit in the 18.9 mm screenshot.
    //
    // Both caps are further bounded by the DISPLACEMENT BUDGET: how far any
    // inference may move the placement from where the cursor actually is.
    // Every cap above is absolute (or grid-relative), but the damage a pull
    // does is relative to the segment being drawn — 1.5 mm off a 100 mm line
    // is nothing, 1.5 mm off a 2 mm line is the line. That asymmetry is why
    // short segments came out quantized to whatever geometry happened to sit
    // near the anchor (Steve, 2026-09-03: a 2 mm line placed fine, 1 mm and
    // 3 mm were unreachable, the only alternatives being no line at all — an
    // intersection sitting ON the anchor, so the segment collapsed — or a
    // 19 mm jump to a farther intersection still inside the 5x pair cap).
    // Capping the pull at a quarter of the extent drawn so far lets an
    // inference REFINE a placement but never REDEFINE it, and scales itself
    // out of the way: past ~6 mm of draw the absolute caps bind again, so
    // nothing changes for normal-sized geometry. Only applies once there's a
    // drawn extent to measure against — before the first click the absolute
    // caps stand alone.
    float pullBudget = 1e30f;
    if (haveDirAnchor) pullBudget = 0.25f * glm::length(pos - dirAnchor);

    const float intersectCap = std::min(posCap * 5.0f, pullBudget);
    int bestI = -1, bestJ = -1;
    glm::vec2 bestIsect = pos;
    float bestIsectD = intersectCap;
    for (size_t i = 0; i < cands.size(); ++i) {
        for (size_t j = i + 1; j < cands.size(); ++j) {
            glm::vec2 isect;
            float t1, t2;
            if (!intersect2(cands[i].anchor, cands[i].dir,
                            cands[j].anchor, cands[j].dir, isect, t1, t2)) continue;
            if (cands[i].isSegment && (t1 < 0.0f || t1 > cands[i].segLen)) continue;
            if (cands[j].isSegment && (t2 < 0.0f || t2 > cands[j].segLen)) continue;
            float d = glm::length(isect - pos);
            if (d < bestIsectD) {
                bestIsectD = d;
                bestIsect = isect;
                bestI = static_cast<int>(i);
                bestJ = static_cast<int>(j);
            }
        }
    }
    // The renderer reads g.from for the OnLine diamond marker (a point-
    // marker, not a guide-line origin), so it must follow the snap point;
    // for dashed-line kinds (axis / perp / parallel / tangent) g.from is
    // the guide's anchor end and stays at the cand's reference. (Steve:
    // the preview diamond was tracking the cursor's perpendicular foot
    // instead of the grid-aligned snap.)
    auto emitWithSnap = [&](const LineCand& c, glm::vec2 snapPos) {
        glm::vec2 from = (c.kind == InferenceGuide::OnLine) ? snapPos : c.visFrom;
        m_activeInferences.push_back({c.kind, from, snapPos, c.refId});
    };
    // "Borrowed-direction" inferences copy their angle from an existing feature
    // (a parallel/perp reference, a tangent, the line you're snapping ONTO).
    // rectifyNearAxis must NOT flatten these to the axis: a Parallel guide off a
    // line that's 3° below horizontal IS a 3° line by intent — flattening it to
    // horizontal drew the orange line while the cyan "Parallel" guide still
    // showed (a guide/result mismatch). Only the free-drag / pure-axis / angle-
    // snap paths get rectified. (axis-from-point is already exact, so it's
    // unaffected either way.)
    auto isBorrowedDir = [](InferenceGuide::Kind k) {
        return k == InferenceGuide::ParallelToPrev ||
               k == InferenceGuide::PerpToPrev ||
               k == InferenceGuide::PerpToRef ||
               k == InferenceGuide::TangentToCircle ||
               k == InferenceGuide::OnLine ||
               k == InferenceGuide::OnLineExtension ||
               k == InferenceGuide::CornerBisector ||
               k == InferenceGuide::CornerTangent;
    };
    if (bestI >= 0) {
        // A pair of purely directional guides is quantized outright — both
        // relationships are about direction, so the lattice decides position.
        // With ONE contact guide in the pair the point has to stay on that
        // edge, so quantize ALONG it instead (the directional half then holds
        // approximately, which is the right way round: the edge landing is
        // topology, the direction is a hint). Two contacts crossing is a real
        // vertex — leave it exactly where the geometry puts it.
        const bool ci = isContact(cands[bestI].kind);
        const bool cj = isContact(cands[bestJ].kind);
        if (!ci && !cj) {
            // Only when BOTH rays are axis-aligned; otherwise rounding the
            // crossing breaks the very directions that defined it.
            if (axisAligned(cands[bestI].dir) && axisAligned(cands[bestJ].dir))
                bestIsect = onLattice(bestIsect);
        } else if (ci != cj) {
            const auto& con = ci ? cands[bestI] : cands[bestJ];
            bestIsect = gridAlongLine(con.anchor, con.dir, con.isSegment,
                                      con.segLen, bestIsect);
        }
        emitWithSnap(cands[bestI], bestIsect);
        emitWithSnap(cands[bestJ], bestIsect);
        // A pair intersection is a deliberate composite of two guides — never
        // flatten it; doing so would break BOTH relationships.
        return bestIsect;
    }

    int bestK = -1;
    float bestPerp = std::min(posCap, pullBudget);
    for (size_t i = 0; i < cands.size(); ++i) {
        if (!cands[i].standaloneAllowed) continue;
        if (cands[i].perpDist < bestPerp) {
            bestPerp = cands[i].perpDist;
            bestK = static_cast<int>(i);
        }
    }
    if (bestK >= 0) {
        const auto& c = cands[bestK];
        glm::vec2 snapped = gridAlongLine(c.anchor, c.dir, c.isSegment, c.segLen, c.proj);
        if (!isContact(c.kind) && axisAligned(c.dir)) snapped = onLattice(snapped);
        emitWithSnap(c, snapped);
        // Preserve a borrowed direction (parallel/perp/tangent/on-line) exactly;
        // only flatten genuinely free near-axis results.
        return isBorrowedDir(c.kind) ? snapped : rectifyNearAxis(snapped);
    }

    // Angle-snap fallback: cursor direction from anchor within ~3° of a
    // configurable degree increment (Settings). Only fires when nothing above
    // did — the perp / parallel inferences are stronger semantic intents.
    if (allowDirectional && haveDirAnchor && m_angleSnapDeg > 0) {
        glm::vec2 v = pos - dirAnchor;
        float len = glm::length(v);
        if (len > 0.5f) {
            float a = std::atan2(v.y, v.x);
            const float step =
                static_cast<float>(m_angleSnapDeg) * static_cast<float>(M_PI) / 180.0f;
            float snappedA = std::round(a / step) * step;
            float angDelta = std::abs(a - snappedA);
            const float TWO_PI = 2.0f * static_cast<float>(M_PI);
            if (angDelta > static_cast<float>(M_PI))
                angDelta = TWO_PI - angDelta;
            const float angTol = 3.0f * static_cast<float>(M_PI) / 180.0f * angleScale();
            if (angDelta < angTol) {
                glm::vec2 dir(std::cos(snappedA), std::sin(snappedA));
                glm::vec2 snappedPos = dirAnchor + dir * len;
                float posOff = glm::length(snappedPos - pos);
                // Capture window: with grid snap on, never wider than half a
                // grid cell (slightly over, so the exact half-cell boundary
                // still lands on the axis grid line). Otherwise a 1-cell rise
                // over a long run sits inside the horizontal snap band and gets
                // swallowed — "I can't rise 1mm and go over any amount."
                // (Steve.) Grid off keeps the original mm-based cap.
                const float angleCap =
                    (m_snapToGridEnabled && m_gridStep > 0.0f)
                        ? m_gridStep * 0.5f + 1e-3f : posCap;
                if (posOff < angleCap) {
                    // DIAG (sticky-angle report): cursor vs snapped angle, the
                    // line length and the positional gate — so we can see why a
                    // near-axis long line refuses an in-between angle.
                    static float s_lastAngLog = -999.0f;
                    float snDeg = snappedA * 180.0f / static_cast<float>(M_PI);
                    if (std::abs(snDeg - s_lastAngLog) > 0.4f) {
                        std::fprintf(stderr,
                            "[Infer] angle-snap FIRED step=%d cursor=%.2f° "
                            "snapped=%.2f° len=%.1fmm posOff=%.2f cap=%.2f\n",
                            m_angleSnapDeg, a * 180.0f / static_cast<float>(M_PI),
                            snDeg, len, posOff, angleCap);
                        s_lastAngLog = snDeg;
                    }
                    // Grid-along-line so the ray's endpoint lands on a lattice
                    // step instead of sub-grid drift.
                    snappedPos = gridAlongLine(dirAnchor, dir, false, 0.0f,
                                               snappedPos);
                    // A 15 / 30 / 60 deg increment is a diagonal ray: same
                    // rule as the guides above, or the chosen angle comes out
                    // wrong by however far the lattice pulled it.
                    if (axisAligned(dir)) snappedPos = onLattice(snappedPos);
                    m_activeInferences.push_back(
                        {InferenceGuide::AngleSnap, dirAnchor,
                         snappedPos, -1});
                    return rectifyNearAxis(snappedPos);
                }
            }
        }
    }

    // Grid snap: always round to the nearest grid increment when enabled. A
    // fractional-threshold band sounds reasonable but at any practical zoom
    // level is sub-pixel cursor precision on BOTH axes simultaneously, so
    // the snap silently misses and the user ends up with off-grid sketches
    // when they explicitly asked for 1 mm precision. The toolbar "Snap to
    // grid" checkbox is the explicit opt-out for free-form drawing.
    if (!m_snapToGridEnabled || m_gridStep <= 0.0f) return pos;
    glm::vec2 result;
    result.x = std::round(pos.x / m_gridStep) * m_gridStep;
    result.y = std::round(pos.y / m_gridStep) * m_gridStep;
    return result;
}

int SketchTool::findCoincidentPoint(glm::vec2 pos, int excludeId) const {
    if (!m_sketch) return -1;

    // A weld radius is a SCREEN distance. As a fixed 0.3 mm it shrank with the
    // zoom — 15 px in a millimetre view but a fifth of a pixel in a metre one —
    // and welding is the ONLY thing that closes a loop into an extrudable
    // region. Grid snap hid that for years: with a stable lattice the closing
    // click lands EXACTLY on the first vertex, so a radius of nearly zero still
    // welded. Change the lattice underfoot and it stops — switching feet -> mm
    // makes the new lattice incommensurable with a vertex placed on the old one
    // (304.8-based vs 1-based), the closing click snaps elsewhere, and 0.3 mm
    // cannot bridge the gap: "I can't close out a sketch to extrude."
    //
    // Same shape as tolStep()'s screen term, and the 0.3 mm stays as a floor so
    // a deeply zoomed-in view keeps its old precision (and so the radius is
    // still sane on the first frame, before the viewport has pushed a scale).
    const float threshold =
        std::max(0.3f, kWeldRadiusPx * m_mmPerPixel) * snapScale();
    // Return the NEAREST point within the radius, not the first one found. On
    // dense or small-scale geometry (e.g. an SVG imported small, whose spline
    // control points sit within the weld radius of each other) "first in range"
    // can weld an arc/line endpoint onto the wrong neighbour — the arc then
    // renders rotated off its latched endpoints with the wrong bulge. Scaling
    // the artwork up spread the points past the radius, which is why that made
    // the same operation behave; nearest makes it scale-independent.
    int best = -1;
    float bestD = threshold;
    const auto& points = m_sketch->getPoints();
    for (const auto& pt : points) {
        if (pt.id == excludeId) continue;
        if (pt.fromText) continue; // never weld user geometry onto glyphs
        float d = glm::length(pos - pt.pos);
        if (d < bestD) { bestD = d; best = pt.id; }
    }
    return best;
}

void SketchTool::selectAll() {
    if (!m_sketch) return;
    m_selectedPoints.clear();
    m_selectedLines.clear();
    m_selectedCircles.clear();
    m_selectedArcs.clear();
    for (const auto& p : m_sketch->getPoints())  m_selectedPoints.insert(p.id);
    for (const auto& l : m_sketch->getLines())   m_selectedLines.insert(l.id);
    for (const auto& c : m_sketch->getCircles()) m_selectedCircles.insert(c.id);
    for (const auto& a : m_sketch->getArcs())    m_selectedArcs.insert(a.id);
}

void SketchTool::handleSelectTool(glm::vec2 pos) {
    if (!m_sketch) return;

    // Hit-test priority: point first, then line, then circle/arc perimeter.
    // Points and line segments are usually the user's intended target; the
    // curve perimeters back them up when the click doesn't land on either.
    int nearPt = findCoincidentPoint(pos, -1);

    int nearLine = -1;
    int nearCircle = -1;
    int nearArc = -1;
    const float tol = std::max(tolStep() * 0.5f, 0.5f) * snapScale(); // sketch units (wider on touch)
    if (nearPt < 0) {
        // Line segments.
        float bestD = 0.0f;
        const auto& lines = m_sketch->getLines();
        for (const auto& l : lines) {
            const SketchPoint* a = m_sketch->getPoint(l.startPointId);
            const SketchPoint* b = m_sketch->getPoint(l.endPointId);
            if (!a || !b) continue;
            glm::vec2 ab = b->pos - a->pos;
            float len2 = glm::dot(ab, ab);
            if (len2 < 1e-12f) continue;
            float t = glm::clamp(glm::dot(pos - a->pos, ab) / len2, 0.0f, 1.0f);
            glm::vec2 proj = a->pos + ab * t;
            float d = glm::distance(proj, pos);
            if (d < tol && (nearLine < 0 || d < bestD)) {
                nearLine = l.id;
                bestD = d;
            }
        }
        // Circle / arc perimeters — only consulted when no line landed.
        if (nearLine < 0) {
            float bestCircleD = 0.0f;
            for (const auto& c : m_sketch->getCircles()) {
                const SketchPoint* center = m_sketch->getPoint(c.centerPointId);
                if (!center) continue;
                float d = std::abs(glm::distance(pos, center->pos) -
                                   static_cast<float>(c.radius));
                if (d < tol && (nearCircle < 0 || d < bestCircleD)) {
                    nearCircle = c.id;
                    bestCircleD = d;
                }
            }
            float bestArcD = 0.0f;
            for (const auto& a : m_sketch->getArcs()) {
                const SketchPoint* center = m_sketch->getPoint(a.centerPointId);
                if (!center) continue;
                // Approximate as full-circle perimeter — picking on the arc's
                // sweep range is overkill for selection feel.
                float d = std::abs(glm::distance(pos, center->pos) -
                                   static_cast<float>(a.radius));
                if (d < tol && (nearArc < 0 || d < bestArcD)) {
                    nearArc = a.id;
                    bestArcD = d;
                }
            }
        }
    }

    // If the user clicked on something that's ALREADY part of the selection,
    // start a drag of the whole selection in-place (don't replace the selection).
    bool reclickedSelected =
        (nearPt    >= 0 && m_selectedPoints.count(nearPt))   ||
        (nearLine  >= 0 && m_selectedLines.count(nearLine))  ||
        (nearCircle>= 0 && m_selectedCircles.count(nearCircle)) ||
        (nearArc   >= 0 && m_selectedArcs.count(nearArc));

    if (!m_lastDownAddedToSel && !reclickedSelected) {
        m_selectedPoints.clear();
        m_selectedLines.clear();
        m_selectedCircles.clear();
        m_selectedArcs.clear();
    }

    if (nearPt >= 0) {
        m_selectedPoints.insert(nearPt);
        m_dragPointId = nearPt;
        m_isDragging = true; // multi-point drag handled in onMouseMove
    } else if (nearLine >= 0) {
        if (m_lastDownAddedToSel) {
            // Ctrl+click on a line: toggle.
            if (m_selectedLines.count(nearLine)) m_selectedLines.erase(nearLine);
            else m_selectedLines.insert(nearLine);
        } else {
            m_selectedLines.insert(nearLine);
            m_isDragging = true; // dragging the line translates the whole selection
        }
    } else if (nearCircle >= 0) {
        if (m_lastDownAddedToSel) {
            if (m_selectedCircles.count(nearCircle)) m_selectedCircles.erase(nearCircle);
            else m_selectedCircles.insert(nearCircle);
        } else {
            m_selectedCircles.insert(nearCircle);
        }
    } else if (nearArc >= 0) {
        if (m_lastDownAddedToSel) {
            if (m_selectedArcs.count(nearArc)) m_selectedArcs.erase(nearArc);
            else m_selectedArcs.insert(nearArc);
        } else {
            m_selectedArcs.insert(nearArc);
        }
    }
    // Empty space + no modifier: selection already cleared above.
}

void SketchTool::handleLineTool(glm::vec2 pos) {
    if (!m_isPlacing) {
        // First click: set start point
        m_firstClick = pos;
        m_isPlacing = true;
        m_clickCount = 1;

        // Check if we're snapping to an existing point
        m_chainStartPointCreated = false;
        if (m_lastPointId == -1) {
            const auto& points = m_sketch->getPoints();
            for (const auto& pt : points) {
                if (glm::length(pos - pt.pos) < 1e-4f) {
                    m_lastPointId = pt.id;
                    break;
                }
            }
            if (m_lastPointId == -1) {
                m_lastPointId = m_sketch->addPoint(pos);
                m_chainStartPointCreated = true; // brand-new, drop on Esc
            }
        }
        // Remember where the chain started — closing back onto this point
        // auto-completes the loop and ends placement (see below).
        m_chainStartPointId = m_lastPointId;
        m_lineChain.clear();
        m_lineChain.push_back(m_lastPointId);
    } else {
        // Second click: create line and continue chain.
        // Reject a zero-length segment — a tap/release back onto the anchor (or
        // snapped onto it) commits nothing and keeps the chain placing (#25),
        // so press-drag-release / tap-tap can't drop a degenerate line.
        const SketchPoint* anchorPt = m_sketch->getPoint(m_lastPointId);
        if (anchorPt && glm::length(pos - anchorPt->pos) < 1e-4f)
            return;
        int endPointId = -1;

        // Check if snapping to existing point
        const auto& points = m_sketch->getPoints();
        for (const auto& pt : points) {
            if (pt.id != m_lastPointId && glm::length(pos - pt.pos) < 1e-4f) {
                endPointId = pt.id;
                break;
            }
        }

        if (endPointId == -1) {
            endPointId = m_sketch->addPoint(pos);
        }

        int newLineId = m_sketch->addLine(m_lastPointId, endPointId);
        // Constraints are entirely opt-in — no autoConstrain on creation. The
        // user applies Horizontal / Vertical / Coincident / etc. explicitly via
        // the toolbar when they want one.
        (void)newLineId;

        // Remember the direction of the segment we just committed — the
        // perpendicular- and parallel-to-previous inferences need it while the
        // user places the next vertex in the chain.
        const SketchPoint* spJustEnded = m_sketch->getPoint(m_lastPointId);
        const SketchPoint* epJustEnded = m_sketch->getPoint(endPointId);
        if (spJustEnded && epJustEnded) {
            glm::vec2 d = epJustEnded->pos - spJustEnded->pos;
            float dl = glm::length(d);
            if (dl > 1e-6f) {
                m_prevLineDir = d / dl;
                m_hasPrevLineDir = true;
            }
        }

        // Auto-close: if the chain has at least two committed segments and the
        // endpoint we just placed IS the chain's starting point, the loop is
        // closed — commit and end the chain so the user doesn't have to hit
        // Esc/Enter to escape line mode.
        if (m_clickCount >= 2 && endPointId == m_chainStartPointId) {
            m_isPlacing = false;
            m_clickCount = 0;
            m_lastPointId = -1;
            m_chainStartPointId = -1;
            m_hasPrevLineDir = false;
            m_activeInferences.clear();
            m_lineChain.clear();
            return;
        }

        // Continue chaining: the end becomes the new start
        m_lastPointId = endPointId;
        m_firstClick = pos;
        m_clickCount++;
        m_lineChain.push_back(endPointId);
    }
}

void SketchTool::handleCircleTool(glm::vec2 pos, bool exact) {
    if (!m_isPlacing) {
        // First click: set center
        m_firstClick = pos;
        m_isPlacing = true;
        m_clickCount = 1;
    } else {
        // Second click. Center mode: first click is the centre, drag is the
        // radius. TwoPoint mode: the two clicks are opposite ends of the
        // diameter, so the centre is their midpoint and the rim passes through
        // the first click.
        // Snap the radius/diameter onto the grid (same helper as the preview),
        // so a grid-enabled sketch commits clean whole-unit circles.
        //
        // NOT when the position came from a typed value. A number the user
        // typed is a stronger statement of intent than the grid, and rounding
        // it here produced a circle that disagreed with its own Radius
        // constraint: typing 7.3 on a 1mm grid built r=4.0 and then recorded
        // a Radius of 3.65 against it.
        if (m_snapToGridEnabled && !exact)
            pos = snapRadialToGrid(m_firstClick, pos);
        glm::vec2 center = (m_circleMode == CircleMode::TwoPoint)
                               ? 0.5f * (m_firstClick + pos) : m_firstClick;
        float radius = (m_circleMode == CircleMode::TwoPoint)
                           ? 0.5f * glm::length(pos - m_firstClick)
                           : glm::length(pos - m_firstClick);
        if (radius > 1e-6f) {
            // Reuse the existing point at this position if there is one
            // (this is how concentric circles share a center).
            int existing = findCoincidentPoint(center, -1);
            int centerId = (existing >= 0) ? existing : m_sketch->addPoint(center);
            m_sketch->addCircle(centerId, static_cast<double>(radius));
        }

        m_isPlacing = false;
        m_clickCount = 0;
    }
}

void SketchTool::handleRectangleTool(glm::vec2 pos) {
    if (!m_isPlacing) {
        // First click: set first corner
        m_firstClick = pos;
        m_isPlacing = true;
        m_clickCount = 1;
    } else {
        // Second click. Corner mode: first click + this click are opposite
        // corners. Center mode: first click is the centre, so the opposite
        // corner is mirrored through it.
        glm::vec2 c1 = (m_rectMode == RectMode::Center)
                           ? 2.0f * m_firstClick - pos : m_firstClick;
        if (std::abs(pos.x - c1.x) > 1e-6f &&
            std::abs(pos.y - c1.y) > 1e-6f) {
            m_sketch->addRectangle(c1, pos);
        }

        m_isPlacing = false;
        m_clickCount = 0;
    }
}

float SketchTool::rimBreakBand(bool wholeSelection) const {
    // Two bands, because the two gestures mean different things (Steve): moving
    // the LINE about the circle should keep its end on the circle, while taking
    // the POINT off the circle is how you say you no longer want it there.
    //
    // So dragging a whole selection gets a generous band — the end rides round
    // the rim through any normal repositioning and only lets go if the line is
    // hauled somewhere else entirely. Dragging the point alone gets a tight one,
    // so a deliberate pull detaches immediately.
    const float tight = std::max(tolStep() * 0.75f, 0.5f) * snapScale();
    return wholeSelection ? std::max(tight * 8.0f, 4.0f * tolStep()) : tight;
}

// Sticky, not locked. While the drag stays near the rim the point rides it —
// that is "move the line about the circle and the ends stay on it". Once the
// drag pulls clear, the attachment is dropped silently and the point goes
// where it was put. No dialog, no constraint to delete: the user walking away
// from the rim IS the instruction.
glm::vec2 SketchTool::slideOrRelease(int pointId, glm::vec2 target,
                                    bool wholeSelection) {
    if (!m_sketch) return target;
    const SketchPoint* p = m_sketch->getPoint(pointId);
    if (!p || p->onCurveId < 0) return target;

    glm::vec2 centre(0.0f);
    float radius = 0.0f;
    bool arcLimited = false;
    float aStart = 0.0f, aSweep = 0.0f;
    bool found = false;
    for (const auto& c : m_sketch->getCircles())
        if (c.id == p->onCurveId) {
            const SketchPoint* ctr = m_sketch->getPoint(c.centerPointId);
            if (ctr) { centre = ctr->pos; radius = float(c.radius); found = true; }
            break;
        }
    if (!found)
        for (const auto& a : m_sketch->getArcs())
            if (a.id == p->onCurveId) {
                const SketchPoint* ctr = m_sketch->getPoint(a.centerPointId);
                const SketchPoint* sp  = m_sketch->getPoint(a.startPointId);
                const SketchPoint* ep  = m_sketch->getPoint(a.endPointId);
                if (ctr && sp && ep) {
                    centre = ctr->pos; radius = float(a.radius);
                    aStart = std::atan2(sp->pos.y - ctr->pos.y, sp->pos.x - ctr->pos.x);
                    float e = std::atan2(ep->pos.y - ctr->pos.y, ep->pos.x - ctr->pos.x);
                    const float TWO_PI = 2.0f * float(M_PI);
                    aSweep = e - aStart;
                    while (aSweep < 0.0f) aSweep += TWO_PI;
                    arcLimited = true; found = true;
                }
                break;
            }
    // The rim was deleted out from under it: nothing to hold on to.
    if (!found || radius < 1e-6f) { m_sketch->setPointOnCurve(pointId, -1); return target; }

    const glm::vec2 v = target - centre;
    const float dc = glm::length(v);
    if (dc < 1e-6f) return target;                 // dead centre: no rim point
    if (std::abs(dc - radius) > rimBreakBand(wholeSelection)) {  // pulled clear
        m_sketch->setPointOnCurve(pointId, -1);
        return target;
    }
    glm::vec2 onRim = centre + (v / dc) * radius;
    if (arcLimited) {
        float a = std::atan2(onRim.y - centre.y, onRim.x - centre.x) - aStart;
        const float TWO_PI = 2.0f * float(M_PI);
        while (a < 0.0f) a += TWO_PI;
        while (a >= TWO_PI) a -= TWO_PI;
        if (a > aSweep) {           // slid off the end of the sweep
            m_sketch->setPointOnCurve(pointId, -1);
            return target;
        }
    }
    return onRim;
}

glm::vec2 SketchTool::snapRadialToGrid(glm::vec2 fixed, glm::vec2 moving) const {
    if (!m_snapToGridEnabled || m_gridStep <= 0.0f) return moving;
    glm::vec2 d = moving - fixed;
    float L = glm::length(d);
    if (L < 1e-6f) return moving;
    float snapped = std::round(L / m_gridStep) * m_gridStep;
    if (snapped < m_gridStep) snapped = m_gridStep;   // never collapse to a point
    return fixed + d * (snapped / L);
}

glm::vec2 SketchTool::snapArcApex(glm::vec2 A, glm::vec2 B, glm::vec2 apex) const {
    // Snap the swept apex so the sweep lands on the nearest 15° multiple when
    // within ±5° of one (e.g. a clean 180° semicircle). The apex sits on AB's
    // perpendicular bisector at signed distance d = (L/2)·tan(θ/4) from the
    // chord midpoint. Returns the apex unchanged when not near a snap target.
    glm::vec2 C = apex;
    const float ax = A.x, ay = A.y;
    const float bx = C.x, by = C.y;       // 'b' = the apex (3rd circumcircle pt)
    const float cx = B.x, cy = B.y;       // 'c' = arc end
    const float D = 2.0f * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if (std::abs(D) <= 1e-10f) return apex;
    const float ux = ((ax*ax+ay*ay)*(by-cy) +
                      (bx*bx+by*by)*(cy-ay) +
                      (cx*cx+cy*cy)*(ay-by)) / D;
    const float uy = ((ax*ax+ay*ay)*(cx-bx) +
                      (bx*bx+by*by)*(ax-cx) +
                      (cx*cx+cy*cy)*(bx-ax)) / D;
    glm::vec2 ctr(ux, uy);
    const float sA = std::atan2(A.y - ctr.y, A.x - ctr.x);
    const float eA = std::atan2(B.y - ctr.y, B.x - ctr.x);
    const float mA = std::atan2(C.y - ctr.y, C.x - ctr.x);
    const float TWO_PI = 2.0f * static_cast<float>(M_PI);
    auto norm = [TWO_PI](float a){ while (a < 0) a += TWO_PI;
                                    while (a >= TWO_PI) a -= TWO_PI;
                                    return a; };
    const float ds = norm(eA - sA);
    const float dm = norm(mA - sA);
    const float sweep = (dm < ds) ? ds : (ds - TWO_PI);
    const float deg = sweep * 180.0f / static_cast<float>(M_PI);
    const float step = 15.0f;
    const float target = std::round(deg / step) * step;
    if (std::abs(target) >= 0.5f && std::abs(deg - target) <= 7.0f) {
        glm::vec2 chord = B - A;
        float L = glm::length(chord);
        if (L > 1e-4f) {
            glm::vec2 chordDir = chord / L;
            glm::vec2 perp(-chordDir.y, chordDir.x);
            glm::vec2 M = 0.5f * (A + B);
            if (glm::dot(C - M, perp) < 0.0f) perp = -perp;  // keep cursor's side
            const float thetaAbs = std::abs(target) * static_cast<float>(M_PI) / 180.0f;
            const float d = (L * 0.5f) * std::tan(thetaAbs * 0.25f);
            return M + perp * d;
        }
    }
    return apex;
}

glm::vec2 SketchTool::arcApexSnap(glm::vec2 rawPos) const {
    if (!m_snapToGridEnabled) return rawPos;
    // Angle first, on the RAW cursor. snapArcApex returns its input unchanged
    // when the sweep isn't near a 15° multiple, so an unequal result means an
    // angle target (45/90/180/…) was hit — prefer it over the grid.
    glm::vec2 angled = snapArcApex(m_firstClick, m_secondClick, rawPos);
    if (angled != rawPos) return angled;
    return snap(rawPos);   // no clean angle nearby — fall back to the grid
}

void SketchTool::handleArcTool(glm::vec2 pos) {
    if (!m_isPlacing) {
        // First click: set start point
        m_firstClick = pos;
        m_isPlacing = true;
        m_clickCount = 1;
    } else if (m_clickCount == 1) {
        // Second click: set end point
        m_secondClick = pos;
        m_clickCount = 2;
    } else if (m_clickCount == 2) {
        // Third click: set midpoint on arc, compute center and radius
        glm::vec2 start = m_firstClick;
        glm::vec2 end = m_secondClick;
        // `pos` is already the final apex (onMouseDown ran it through
        // arcApexSnap — angle preferred over grid), the SAME value the preview
        // showed, so the placed arc's centre matches the previewed one.
        glm::vec2 mid = pos;

        // Compute the circumcenter of the three points (start, mid, end)
        // This gives us the arc center
        float ax = start.x, ay = start.y;
        float bx = mid.x, by = mid.y;
        float cx = end.x, cy = end.y;

        float D = 2.0f * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));

        if (std::abs(D) > 1e-10f) {
            float ux = ((ax * ax + ay * ay) * (by - cy) +
                        (bx * bx + by * by) * (cy - ay) +
                        (cx * cx + cy * cy) * (ay - by)) / D;
            float uy = ((ax * ax + ay * ay) * (cx - bx) +
                        (bx * bx + by * by) * (ax - cx) +
                        (cx * cx + cy * cy) * (bx - ax)) / D;

            glm::vec2 center(ux, uy);
            float radius = glm::length(start - center);

            auto reuseOrAdd = [&](glm::vec2 p) {
                int existing = findCoincidentPoint(p, -1);
                return (existing >= 0) ? existing : m_sketch->addPoint(p);
            };
            // ENDPOINTS weld — that is the point of clicking near existing
            // geometry, and both were snapped before they got here, so the weld
            // lands on the same point and moves nothing.
            int startId  = reuseOrAdd(start);
            int endId    = reuseOrAdd(end);

            // The CENTRE must not. It is a DERIVED point (the circumcentre),
            // not something the user aimed at, and welding it to whatever
            // happens to sit within 0.3mm silently breaks the invariant the
            // renderer draws with: it sweeps `arc.radius` about the centre
            // POINT between the endpoint ANGLES, so the curve only touches its
            // endpoints while |start-centre| == radius == |end-centre|. Welding
            // moved the centre and left radius computed from the old one, so
            // the arc placed itself a fraction off the ends it was anchored to
            // — "shifts over and is no longer anchored" (Steve, 2026-07-30).
            // Reuse only a point that IS the centre to within float noise,
            // which keeps concentric geometry sharing a point without ever
            // moving the arc.
            int centerId = -1;
            // `near` is a <windef.h> macro — see MoveHoleOp::classifyRimEdges.
            // This TU happens not to pull windows.h in today, so it compiles;
            // renamed anyway so an include change can't turn it into a
            // baffling MSVC syntax error later.
            if (const int nearId = findCoincidentPoint(center, -1); nearId >= 0) {
                if (const auto* np = m_sketch->getPoint(nearId))
                    if (glm::length(np->pos - center) < 1e-4f) centerId = nearId;
            }
            if (centerId < 0) centerId = m_sketch->addPoint(center);

            // Radius from the FINAL positions, not the pre-weld ones: if an
            // endpoint did move, the stored radius has to move with it or the
            // same detachment reappears from the other end.
            if (const auto* cp = m_sketch->getPoint(centerId)) {
                if (const auto* sp = m_sketch->getPoint(startId))
                    radius = glm::length(sp->pos - cp->pos);
            }

            // Store the endpoints in the order whose CCW sweep passes
            // through the user's MID click. addArc keeps only (center,
            // start, end) and everything downstream sweeps CCW start→end —
            // committing the wrong order flips the arc to the complementary
            // side ("comes out weird on first try, inverts after third
            // click... randomly" — random because it depended on which side
            // of the chord the bulge was clicked).
            auto ang = [&](glm::vec2 p) {
                float a = std::atan2(p.y - center.y, p.x - center.x);
                if (a < 0.0f) a += 2.0f * static_cast<float>(M_PI);
                return a;
            };
            float sA = ang(start), eA = ang(end), mA = ang(mid);
            float sweep = eA - sA;
            if (sweep <= 0.0f) sweep += 2.0f * static_cast<float>(M_PI);
            float rel = mA - sA;
            if (rel < 0.0f) rel += 2.0f * static_cast<float>(M_PI);
            if (rel > sweep) std::swap(startId, endId);

            m_sketch->addArc(centerId, startId, endId, static_cast<double>(radius));
        }

        m_isPlacing = false;
        m_clickCount = 0;
    }
}

void SketchTool::handleSplineTool(glm::vec2 pos) {
    if (!m_sketch) return;

    // Each click adds a control point
    int ptId = -1;

    // Check if snapping to an existing point (snap() has already pulled the
    // click exactly onto any nearby point, so an exact compare suffices)
    const auto& points = m_sketch->getPoints();
    for (const auto& pt : points) {
        if (glm::length(pos - pt.pos) < 1e-4f) {
            ptId = pt.id;
            break;
        }
    }

    // Finishing flow (this used to not exist — the spline just "kept
    // going" no matter what you clicked):
    // - clicking the FIRST control point closes the loop and commits
    // - clicking the LAST placed point again commits the open spline
    if (!m_splinePoints.empty() && ptId >= 0) {
        if (ptId == m_splinePoints.front() && m_splinePoints.size() >= 3) {
            // Close the loop: append the front id (front==back marks closed)
            // on the ALREADY-COMMITTED spline and end placement.
            if (m_activeSplineId >= 0)
                m_sketch->appendSplineControlPoint(m_activeSplineId, ptId);
            m_splinePoints.clear();
            m_activeSplineId = -1;
            m_isPlacing = false;
            m_clickCount = 0;
            return;
        }
        if (ptId == m_splinePoints.back()) {
            // Clicking the last point again ends placement — the spline has
            // been committed and growing since the 2nd point, so there is
            // nothing left to do but reset the tool.
            m_splinePoints.clear();
            m_activeSplineId = -1;
            m_isPlacing = false;
            m_clickCount = 0;
            return;
        }
    }

    if (ptId == -1) {
        ptId = m_sketch->addPoint(pos);
    }

    m_splinePoints.push_back(ptId);
    // COMMIT EARLY, GROW PER CLICK: the spline element exists from the 2nd
    // point on, so every undo snapshot holds the spline-so-far — undo then
    // shrinks it one control point at a time ("as though it was committed
    // earlier") instead of deleting the curve and stranding orphan dots.
    if (m_splinePoints.size() == 2)
        m_activeSplineId = m_sketch->addSpline(m_splinePoints);
    else if (m_splinePoints.size() > 2 && m_activeSplineId >= 0)
        m_sketch->appendSplineControlPoint(m_activeSplineId, ptId);

    if (!m_isPlacing) {
        m_firstClick = pos;
        m_isPlacing = true;
    }

    // Direction of the control leg just laid down, so the perpendicular- and
    // parallel-to-previous guides work while the NEXT control point is placed —
    // the same continuation the line chain gets. A spline's control polygon is
    // what the user is actually steering, so its last leg is the meaningful
    // "previous direction" even though the curve itself is smooth.
    if (m_splinePoints.size() >= 2) {
        const SketchPoint* a =
            m_sketch->getPoint(m_splinePoints[m_splinePoints.size() - 2]);
        const SketchPoint* b = m_sketch->getPoint(m_splinePoints.back());
        if (a && b) {
            const glm::vec2 d = b->pos - a->pos;
            const float dl = glm::length(d);
            if (dl > 1e-6f) {
                m_prevLineDir = d / dl;
                m_hasPrevLineDir = true;
            }
        }
    }

    m_clickCount = static_cast<int>(m_splinePoints.size());
    // Also finalized via onConfirm() (Enter key)
}

void SketchTool::removeLastSplinePoint() {
    if (!m_sketch || m_splinePoints.empty()) return;
    int id = m_splinePoints.back();
    m_splinePoints.pop_back();

    // Shrink the committed-and-growing spline too; below 2 points it stops
    // being a curve — remove the element entirely.
    if (m_activeSplineId >= 0) {
        if (!m_sketch->popSplineControlPoint(m_activeSplineId)) {
            m_sketch->removeElement(m_activeSplineId);
            m_activeSplineId = -1;
        }
    }

    // Only delete the sketch point if nothing references it: not this
    // spline-in-progress (duplicate snaps), not any committed element.
    bool referenced =
        std::count(m_splinePoints.begin(), m_splinePoints.end(), id) > 0;
    if (!referenced) {
        for (const auto& l : m_sketch->getLines())
            if (l.startPointId == id || l.endPointId == id) {
                referenced = true; break;
            }
        if (!referenced)
            for (const auto& c : m_sketch->getCircles())
                if (c.centerPointId == id) { referenced = true; break; }
        if (!referenced)
            for (const auto& a : m_sketch->getArcs())
                if (a.centerPointId == id || a.startPointId == id ||
                    a.endPointId == id) { referenced = true; break; }
        if (!referenced)
            for (const auto& sp : m_sketch->getSplines())
                for (int cid : sp.controlPointIds)
                    if (cid == id) { referenced = true; break; }
        if (!referenced)
            for (const auto& pg : m_sketch->getPolygons()) {
                if (pg.centerPointId == id) { referenced = true; break; }
                for (int vid : pg.vertexPointIds)
                    if (vid == id) { referenced = true; break; }
                if (referenced) break;
            }
    }
    if (!referenced) m_sketch->removeElement(id);

    m_clickCount = static_cast<int>(m_splinePoints.size());
    // The leg the perpendicular/parallel guides were following may have just
    // been removed; recompute it from what is left.
    m_hasPrevLineDir = false;
    if (m_splinePoints.size() >= 2) {
        const SketchPoint* a =
            m_sketch->getPoint(m_splinePoints[m_splinePoints.size() - 2]);
        const SketchPoint* b = m_sketch->getPoint(m_splinePoints.back());
        if (a && b) {
            const glm::vec2 d = b->pos - a->pos;
            const float dl = glm::length(d);
            if (dl > 1e-6f) { m_prevLineDir = d / dl; m_hasPrevLineDir = true; }
        }
    }
    if (m_splinePoints.empty()) m_isPlacing = false;
}

// ─── Trim tool ──────────────────────────────────────────────────────────────
//
// The trim tool removes the segment of a sketch element between its two nearest
// intersections with any other element. If the element has no intersections,
// the whole element is removed. Splines and polygons are not split — clicking
// one removes it entirely.

namespace {

// Wrap angle into [0, 2π).
float wrap2Pi(float a) {
    const float TWO_PI = 2.0f * static_cast<float>(M_PI);
    while (a < 0.0f) a += TWO_PI;
    while (a >= TWO_PI) a -= TWO_PI;
    return a;
}

// Closest point on segment p1→p2 to q, returns squared distance.
float distSqPointSegment(glm::vec2 q, glm::vec2 p1, glm::vec2 p2, float* outT = nullptr) {
    glm::vec2 d = p2 - p1;
    float len2 = glm::dot(d, d);
    if (len2 < 1e-12f) {
        if (outT) *outT = 0.0f;
        glm::vec2 v = q - p1;
        return glm::dot(v, v);
    }
    float t = glm::dot(q - p1, d) / len2;
    t = std::clamp(t, 0.0f, 1.0f);
    if (outT) *outT = t;
    glm::vec2 closest = p1 + t * d;
    glm::vec2 v = q - closest;
    return glm::dot(v, v);
}

// True if angle θ falls within arc going CCW from startAngle to endAngle.
bool angleInArc(float theta, float startAngle, float endAngle) {
    float s = wrap2Pi(startAngle);
    float e = wrap2Pi(endAngle);
    float t = wrap2Pi(theta);
    if (s <= e) return t >= s - 1e-5f && t <= e + 1e-5f;
    // arc crosses 0
    return t >= s - 1e-5f || t <= e + 1e-5f;
}

struct Hit { glm::vec2 pos; float param; };

// Intersect line p1-p2 with line p3-p4. param is t along p1→p2.
void intersectLineLine(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, glm::vec2 p4,
                       std::vector<Hit>& out) {
    glm::vec2 d1 = p2 - p1;
    glm::vec2 d2 = p4 - p3;
    float denom = d1.x * d2.y - d1.y * d2.x;
    if (std::abs(denom) < 1e-10f) return; // parallel
    float t = ((p3.x - p1.x) * d2.y - (p3.y - p1.y) * d2.x) / denom;
    float u = ((p3.x - p1.x) * d1.y - (p3.y - p1.y) * d1.x) / denom;
    const float eps = 1e-4f;
    if (t < -eps || t > 1.0f + eps) return;
    if (u < -eps || u > 1.0f + eps) return;
    out.push_back({p1 + t * d1, t});
}

// Intersect line p1-p2 with circle (center, radius). param = t along the line.
void intersectLineCircle(glm::vec2 p1, glm::vec2 p2, glm::vec2 center, float radius,
                         std::vector<Hit>& out) {
    glm::vec2 d = p2 - p1;
    glm::vec2 f = p1 - center;
    float a = glm::dot(d, d);
    float b = 2.0f * glm::dot(f, d);
    float c = glm::dot(f, f) - radius * radius;
    float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) return;
    disc = std::sqrt(disc);
    const float eps = 1e-4f;
    for (float t : { (-b - disc) / (2 * a), (-b + disc) / (2 * a) }) {
        if (t < -eps || t > 1.0f + eps) continue;
        out.push_back({p1 + t * d, t});
    }
}

// Intersect line with arc (filter line-circle by arc angle range).
void intersectLineArc(glm::vec2 p1, glm::vec2 p2, glm::vec2 center, float radius,
                      float startAngle, float endAngle, std::vector<Hit>& out) {
    std::vector<Hit> tmp;
    intersectLineCircle(p1, p2, center, radius, tmp);
    for (const auto& h : tmp) {
        glm::vec2 r = h.pos - center;
        float theta = std::atan2(r.y, r.x);
        if (angleInArc(theta, startAngle, endAngle)) out.push_back(h);
    }
}

// Intersect circle (c1,r1) with circle (c2,r2). Param = angle on circle 1.
void intersectCircleCircle(glm::vec2 c1, float r1, glm::vec2 c2, float r2,
                           std::vector<Hit>& out) {
    glm::vec2 d = c2 - c1;
    float dist = glm::length(d);
    if (dist < 1e-10f) return; // concentric
    if (dist > r1 + r2 + 1e-5f) return;
    if (dist < std::abs(r1 - r2) - 1e-5f) return;
    float a = (r1 * r1 - r2 * r2 + dist * dist) / (2.0f * dist);
    float h2 = r1 * r1 - a * a;
    float h = (h2 > 0.0f) ? std::sqrt(h2) : 0.0f;
    glm::vec2 mid = c1 + d * (a / dist);
    glm::vec2 perp(-d.y / dist, d.x / dist);
    for (int sign : { 1, -1 }) {
        glm::vec2 p = mid + perp * (h * static_cast<float>(sign));
        float theta = std::atan2(p.y - c1.y, p.x - c1.x);
        out.push_back({p, theta});
        if (h < 1e-6f) break; // tangent
    }
}

} // anonymous

// Returns elementId of the picked element, or -1 if none within threshold.
// Sets pickType to one of "line","circle","arc","spline","polygon".
static int pickSketchElement(const Sketch& sketch, glm::vec2 pos, float threshold,
                             std::string& pickType) {
    float bestDistSq = threshold * threshold;
    int bestId = -1;
    pickType.clear();

    // Lines
    for (const auto& ln : sketch.getLines()) {
        const SketchPoint* a = sketch.getPoint(ln.startPointId);
        const SketchPoint* b = sketch.getPoint(ln.endPointId);
        if (!a || !b) continue;
        float dsq = distSqPointSegment(pos, a->pos, b->pos);
        if (dsq < bestDistSq) { bestDistSq = dsq; bestId = ln.id; pickType = "line"; }
    }
    // Circles
    for (const auto& ci : sketch.getCircles()) {
        const SketchPoint* c = sketch.getPoint(ci.centerPointId);
        if (!c) continue;
        float d = glm::length(pos - c->pos);
        float dsq = (d - static_cast<float>(ci.radius)) * (d - static_cast<float>(ci.radius));
        if (dsq < bestDistSq) { bestDistSq = dsq; bestId = ci.id; pickType = "circle"; }
    }
    // Arcs
    for (const auto& ar : sketch.getArcs()) {
        const SketchPoint* c = sketch.getPoint(ar.centerPointId);
        const SketchPoint* s = sketch.getPoint(ar.startPointId);
        const SketchPoint* e = sketch.getPoint(ar.endPointId);
        if (!c || !s || !e) continue;
        glm::vec2 r = pos - c->pos;
        float distC = glm::length(r);
        float perimeterDist = std::abs(distC - static_cast<float>(ar.radius));
        float theta = std::atan2(r.y, r.x);
        float startA = std::atan2(s->pos.y - c->pos.y, s->pos.x - c->pos.x);
        float endA = std::atan2(e->pos.y - c->pos.y, e->pos.x - c->pos.x);
        if (angleInArc(theta, startA, endA)) {
            float dsq = perimeterDist * perimeterDist;
            if (dsq < bestDistSq) { bestDistSq = dsq; bestId = ar.id; pickType = "arc"; }
        }
    }
    // Splines: walk the densified curve so a click on the visible green stroke
    // picks the spline, not just clicks landing on a control-point square.
    // (Steve: couldn't trim/delete a spline by clicking on the curve — old
    // picker only checked proximity to control points, so clicking between
    // them missed entirely.)
    for (const auto& sp : sketch.getSplines()) {
        std::vector<glm::vec2> samp = sketch.sampleSpline2D(sp, 24);
        for (size_t i = 0; i + 1 < samp.size(); ++i) {
            float dsq = distSqPointSegment(pos, samp[i], samp[i + 1]);
            if (dsq < bestDistSq) {
                bestDistSq = dsq; bestId = sp.id; pickType = "spline";
            }
        }
    }
    // Polygons: walk the edges between consecutive vertices (closed loop) so a
    // click on a polygon side picks it, not only clicks at the vertex squares.
    for (const auto& po : sketch.getPolygons()) {
        const auto& ids = po.vertexPointIds;
        size_t n = ids.size();
        for (size_t i = 0; i < n; ++i) {
            const SketchPoint* a = sketch.getPoint(ids[i]);
            const SketchPoint* b = sketch.getPoint(ids[(i + 1) % n]);
            if (!a || !b) continue;
            float dsq = distSqPointSegment(pos, a->pos, b->pos);
            if (dsq < bestDistSq) {
                bestDistSq = dsq; bestId = po.id; pickType = "polygon";
            }
        }
    }
    return bestId;
}

// Collect intersection points + parameters along a given line element.
static void collectLineIntersections(const Sketch& sketch, const SketchLine& target,
                                     std::vector<Hit>& out) {
    const SketchPoint* a = sketch.getPoint(target.startPointId);
    const SketchPoint* b = sketch.getPoint(target.endPointId);
    if (!a || !b) return;
    glm::vec2 p1 = a->pos, p2 = b->pos;

    for (const auto& ln : sketch.getLines()) {
        if (ln.id == target.id) continue;
        const SketchPoint* c = sketch.getPoint(ln.startPointId);
        const SketchPoint* d = sketch.getPoint(ln.endPointId);
        if (!c || !d) continue;
        intersectLineLine(p1, p2, c->pos, d->pos, out);
    }
    for (const auto& ci : sketch.getCircles()) {
        const SketchPoint* c = sketch.getPoint(ci.centerPointId);
        if (!c) continue;
        intersectLineCircle(p1, p2, c->pos, static_cast<float>(ci.radius), out);
    }
    for (const auto& ar : sketch.getArcs()) {
        const SketchPoint* c = sketch.getPoint(ar.centerPointId);
        const SketchPoint* s = sketch.getPoint(ar.startPointId);
        const SketchPoint* e = sketch.getPoint(ar.endPointId);
        if (!c || !s || !e) continue;
        float startA = std::atan2(s->pos.y - c->pos.y, s->pos.x - c->pos.x);
        float endA = std::atan2(e->pos.y - c->pos.y, e->pos.x - c->pos.x);
        intersectLineArc(p1, p2, c->pos, static_cast<float>(ar.radius), startA, endA, out);
    }
}

// Collect intersection angles around a circle element (parameters are angles in [0,2π)).
static void collectCircleIntersections(const Sketch& sketch, glm::vec2 center, float radius,
                                       int skipId, std::vector<Hit>& out) {
    for (const auto& ln : sketch.getLines()) {
        if (ln.id == skipId) continue;
        const SketchPoint* a = sketch.getPoint(ln.startPointId);
        const SketchPoint* b = sketch.getPoint(ln.endPointId);
        if (!a || !b) continue;
        std::vector<Hit> tmp;
        intersectLineCircle(a->pos, b->pos, center, radius, tmp);
        for (auto& h : tmp) {
            float theta = std::atan2(h.pos.y - center.y, h.pos.x - center.x);
            out.push_back({h.pos, wrap2Pi(theta)});
        }
    }
    for (const auto& ci : sketch.getCircles()) {
        if (ci.id == skipId) continue;
        const SketchPoint* c = sketch.getPoint(ci.centerPointId);
        if (!c) continue;
        std::vector<Hit> tmp;
        intersectCircleCircle(center, radius, c->pos, static_cast<float>(ci.radius), tmp);
        for (auto& h : tmp) out.push_back({h.pos, wrap2Pi(h.param)});
    }
    for (const auto& ar : sketch.getArcs()) {
        if (ar.id == skipId) continue;
        const SketchPoint* c = sketch.getPoint(ar.centerPointId);
        const SketchPoint* s = sketch.getPoint(ar.startPointId);
        const SketchPoint* e = sketch.getPoint(ar.endPointId);
        if (!c || !s || !e) continue;
        std::vector<Hit> tmp;
        intersectCircleCircle(center, radius, c->pos, static_cast<float>(ar.radius), tmp);
        float startA = std::atan2(s->pos.y - c->pos.y, s->pos.x - c->pos.x);
        float endA = std::atan2(e->pos.y - c->pos.y, e->pos.x - c->pos.x);
        for (auto& h : tmp) {
            glm::vec2 r = h.pos - c->pos;
            float thetaOnOther = std::atan2(r.y, r.x);
            if (!angleInArc(thetaOnOther, startA, endA)) continue;
            float theta = std::atan2(h.pos.y - center.y, h.pos.x - center.x);
            out.push_back({h.pos, wrap2Pi(theta)});
        }
    }
}

namespace {

// Plan for a single trim click: what to delete, plus the data needed to
// regenerate the surviving sub-segments. Computed by planTrim() without any
// mutation so that both the hover preview and the actual click use the same
// geometric decision.
struct TrimAction {
    enum class Kind { None, FullDelete, Line, Circle, Arc };

    Kind kind = Kind::None;
    int targetId = -1;

    // Line
    glm::vec2 lineP1{0}, lineP2{0};
    int lineKillIndex = -1;
    std::vector<float> lineBounds;

    // Circle (whole circle being cut into arcs)
    glm::vec2 circCenter{0};
    float circRadius = 0.0f;
    int circCenterPointId = -1;
    int circKillIndex = -1;
    std::vector<float> circAngles; // CCW-sorted intersection angles in [0, 2π)

    // Arc (existing arc being cut into sub-arcs)
    glm::vec2 arcCenter{0};
    float arcRadius = 0.0f;
    float arcStartA = 0.0f;
    int arcCenterPointId = -1;
    int arcKillIndex = -1;
    std::vector<float> arcBounds; // CCW distances from arcStartA: [0, d1, ..., totalArc]

    bool valid() const { return kind != Kind::None; }
};

TrimAction planTrim(const Sketch& sketch, glm::vec2 pos, float threshold) {
    TrimAction action;
    std::string pickType;
    int id = pickSketchElement(sketch, pos, threshold, pickType);
    if (id < 0) return action;
    action.targetId = id;

    if (pickType == "spline" || pickType == "polygon") {
        action.kind = TrimAction::Kind::FullDelete;
        return action;
    }

    if (pickType == "line") {
        const SketchLine* line = nullptr;
        for (const auto& l : sketch.getLines()) if (l.id == id) { line = &l; break; }
        if (!line) return action;
        const SketchPoint* a = sketch.getPoint(line->startPointId);
        const SketchPoint* b = sketch.getPoint(line->endPointId);
        if (!a || !b) return action;
        action.lineP1 = a->pos;
        action.lineP2 = b->pos;

        std::vector<Hit> hits;
        collectLineIntersections(sketch, *line, hits);
        if (hits.empty()) { action.kind = TrimAction::Kind::FullDelete; return action; }

        std::sort(hits.begin(), hits.end(),
                  [](const Hit& x, const Hit& y) { return x.param < y.param; });
        std::vector<Hit> uniq;
        for (const auto& h : hits) {
            if (uniq.empty() || std::abs(h.param - uniq.back().param) > 1e-3f) uniq.push_back(h);
        }

        std::vector<float> bounds;
        bounds.push_back(0.0f);
        for (const auto& h : uniq) {
            if (h.param > 1e-3f && h.param < 1.0f - 1e-3f) bounds.push_back(h.param);
        }
        bounds.push_back(1.0f);

        float tClick;
        distSqPointSegment(pos, action.lineP1, action.lineP2, &tClick);

        int killIndex = -1;
        for (int i = 0; i + 1 < static_cast<int>(bounds.size()); ++i) {
            if (tClick >= bounds[i] - 1e-3f && tClick <= bounds[i + 1] + 1e-3f) {
                killIndex = i; break;
            }
        }
        if (killIndex < 0) { action.kind = TrimAction::Kind::FullDelete; return action; }

        action.kind = TrimAction::Kind::Line;
        action.lineBounds = std::move(bounds);
        action.lineKillIndex = killIndex;
        return action;
    }

    if (pickType == "circle") {
        const SketchCircle* circ = nullptr;
        for (const auto& c : sketch.getCircles()) if (c.id == id) { circ = &c; break; }
        if (!circ) return action;
        const SketchPoint* cp = sketch.getPoint(circ->centerPointId);
        if (!cp) return action;
        action.circCenter = cp->pos;
        action.circRadius = static_cast<float>(circ->radius);
        action.circCenterPointId = circ->centerPointId;

        std::vector<Hit> hits;
        collectCircleIntersections(sketch, action.circCenter, action.circRadius, id, hits);
        if (hits.empty()) { action.kind = TrimAction::Kind::FullDelete; return action; }

        std::sort(hits.begin(), hits.end(),
                  [](const Hit& x, const Hit& y) { return x.param < y.param; });
        std::vector<float> angles;
        for (const auto& h : hits) {
            if (angles.empty() || std::abs(h.param - angles.back()) > 1e-3f) angles.push_back(h.param);
        }

        glm::vec2 r = pos - action.circCenter;
        float thetaClick = wrap2Pi(std::atan2(r.y, r.x));
        int n = static_cast<int>(angles.size());
        int killIndex = -1;
        for (int i = 0; i < n; ++i) {
            float a = angles[i];
            float b = angles[(i + 1) % n];
            if (angleInArc(thetaClick, a, b)) { killIndex = i; break; }
        }
        if (killIndex < 0) { action.kind = TrimAction::Kind::FullDelete; return action; }

        action.kind = TrimAction::Kind::Circle;
        action.circAngles = std::move(angles);
        action.circKillIndex = killIndex;
        return action;
    }

    if (pickType == "arc") {
        const SketchArc* arc = nullptr;
        for (const auto& a : sketch.getArcs()) if (a.id == id) { arc = &a; break; }
        if (!arc) return action;
        const SketchPoint* cp = sketch.getPoint(arc->centerPointId);
        const SketchPoint* sp = sketch.getPoint(arc->startPointId);
        const SketchPoint* ep = sketch.getPoint(arc->endPointId);
        if (!cp || !sp || !ep) return action;
        action.arcCenter = cp->pos;
        action.arcRadius = static_cast<float>(arc->radius);
        action.arcStartA = std::atan2(sp->pos.y - cp->pos.y, sp->pos.x - cp->pos.x);
        action.arcCenterPointId = arc->centerPointId;
        float endA = std::atan2(ep->pos.y - cp->pos.y, ep->pos.x - cp->pos.x);
        float totalCCW = wrap2Pi(endA - action.arcStartA);

        std::vector<Hit> hits;
        collectCircleIntersections(sketch, action.arcCenter, action.arcRadius, id, hits);
        std::vector<float> inRangeD; // CCW distances of in-range intersections
        for (const auto& h : hits) {
            float d = wrap2Pi(h.param - action.arcStartA);
            if (d > 1e-3f && d < totalCCW - 1e-3f) inRangeD.push_back(d);
        }
        if (inRangeD.empty()) { action.kind = TrimAction::Kind::FullDelete; return action; }

        std::sort(inRangeD.begin(), inRangeD.end());
        std::vector<float> bounds;
        bounds.push_back(0.0f);
        for (float d : inRangeD) {
            if (bounds.empty() || std::abs(d - bounds.back()) > 1e-3f) bounds.push_back(d);
        }
        bounds.push_back(totalCCW);

        glm::vec2 r = pos - action.arcCenter;
        float dClick = wrap2Pi(std::atan2(r.y, r.x) - action.arcStartA);
        int killIndex = -1;
        for (int i = 0; i + 1 < static_cast<int>(bounds.size()); ++i) {
            if (dClick >= bounds[i] - 1e-3f && dClick <= bounds[i + 1] + 1e-3f) {
                killIndex = i; break;
            }
        }
        if (killIndex < 0) { action.kind = TrimAction::Kind::FullDelete; return action; }

        action.kind = TrimAction::Kind::Arc;
        action.arcBounds = std::move(bounds);
        action.arcKillIndex = killIndex;
        return action;
    }

    return action;
}

// Densify the segment that would be removed into 2D sketch-space points.
// For FullDelete the entire element is densified (so the renderer can highlight it).
void densifyTrimPreview(const Sketch& sketch, const TrimAction& a, std::vector<glm::vec2>& out) {
    out.clear();
    if (a.kind == TrimAction::Kind::FullDelete) {
        for (const auto& l : sketch.getLines()) if (l.id == a.targetId) {
            const SketchPoint* p1 = sketch.getPoint(l.startPointId);
            const SketchPoint* p2 = sketch.getPoint(l.endPointId);
            if (p1 && p2) { out.push_back(p1->pos); out.push_back(p2->pos); }
            return;
        }
        for (const auto& c : sketch.getCircles()) if (c.id == a.targetId) {
            const SketchPoint* cp = sketch.getPoint(c.centerPointId);
            if (!cp) return;
            float r = static_cast<float>(c.radius);
            const int samples = 64;
            for (int i = 0; i <= samples; ++i) {
                float t = 2.0f * static_cast<float>(M_PI) * i / samples;
                out.push_back(cp->pos + glm::vec2(std::cos(t), std::sin(t)) * r);
            }
            return;
        }
        for (const auto& ar : sketch.getArcs()) if (ar.id == a.targetId) {
            const SketchPoint* cp = sketch.getPoint(ar.centerPointId);
            const SketchPoint* sp = sketch.getPoint(ar.startPointId);
            const SketchPoint* ep = sketch.getPoint(ar.endPointId);
            if (!cp || !sp || !ep) return;
            float r = static_cast<float>(ar.radius);
            float startA = std::atan2(sp->pos.y - cp->pos.y, sp->pos.x - cp->pos.x);
            float endA = std::atan2(ep->pos.y - cp->pos.y, ep->pos.x - cp->pos.x);
            float total = wrap2Pi(endA - startA);
            const int samples = 32;
            for (int i = 0; i <= samples; ++i) {
                float t = total * i / samples;
                float th = startA + t;
                out.push_back(cp->pos + glm::vec2(std::cos(th), std::sin(th)) * r);
            }
            return;
        }
        for (const auto& spl : sketch.getSplines()) if (spl.id == a.targetId) {
            for (int pid : spl.controlPointIds) {
                const SketchPoint* p = sketch.getPoint(pid);
                if (p) out.push_back(p->pos);
            }
            return;
        }
        for (const auto& po : sketch.getPolygons()) if (po.id == a.targetId) {
            for (int pid : po.vertexPointIds) {
                const SketchPoint* p = sketch.getPoint(pid);
                if (p) out.push_back(p->pos);
            }
            if (!out.empty()) out.push_back(out.front());
            return;
        }
        return;
    }
    switch (a.kind) {
        case TrimAction::Kind::None: return;
        case TrimAction::Kind::FullDelete: return; // handled above
        case TrimAction::Kind::Line: {
            float t0 = a.lineBounds[a.lineKillIndex];
            float t1 = a.lineBounds[a.lineKillIndex + 1];
            out.push_back(a.lineP1 + t0 * (a.lineP2 - a.lineP1));
            out.push_back(a.lineP1 + t1 * (a.lineP2 - a.lineP1));
            return;
        }
        case TrimAction::Kind::Circle: {
            int n = static_cast<int>(a.circAngles.size());
            float a0 = a.circAngles[a.circKillIndex];
            float a1 = a.circAngles[(a.circKillIndex + 1) % n];
            float sweep = wrap2Pi(a1 - a0);
            const int samples = 32;
            for (int i = 0; i <= samples; ++i) {
                float t = static_cast<float>(i) / samples;
                float th = a0 + sweep * t;
                out.push_back(a.circCenter +
                              glm::vec2(std::cos(th), std::sin(th)) * a.circRadius);
            }
            return;
        }
        case TrimAction::Kind::Arc: {
            float d0 = a.arcBounds[a.arcKillIndex];
            float d1 = a.arcBounds[a.arcKillIndex + 1];
            const int samples = 32;
            for (int i = 0; i <= samples; ++i) {
                float t = static_cast<float>(i) / samples;
                float th = a.arcStartA + d0 + (d1 - d0) * t;
                out.push_back(a.arcCenter +
                              glm::vec2(std::cos(th), std::sin(th)) * a.arcRadius);
            }
            return;
        }
    }
}

// Reuse an existing sketch point if one already sits within `tol` of `pos`,
// otherwise add a new one. Critical for trim: new sub-segments must share their
// endpoint IDs with the adjacent geometry (the other element that defined the
// intersection point), otherwise buildWires() can't stitch them into a closed
// loop and buildRegions() never sees a manifold region for push/pull.
int findOrAddPoint(Sketch& sketch, glm::vec2 pos, float tol = 1e-2f) {
    for (const auto& p : sketch.getPoints()) {
        if (glm::length(pos - p.pos) < tol) return p.id;
    }
    return sketch.addPoint(pos);
}

void applyTrim(Sketch& sketch, const TrimAction& a) {
    if (a.kind == TrimAction::Kind::None) return;

    if (a.kind == TrimAction::Kind::FullDelete) {
        sketch.removeElement(a.targetId);
        return;
    }

    if (a.kind == TrimAction::Kind::Line) {
        sketch.removeElement(a.targetId);
        for (int i = 0; i + 1 < static_cast<int>(a.lineBounds.size()); ++i) {
            if (i == a.lineKillIndex) continue;
            glm::vec2 sp = a.lineP1 + a.lineBounds[i] * (a.lineP2 - a.lineP1);
            glm::vec2 ep = a.lineP1 + a.lineBounds[i + 1] * (a.lineP2 - a.lineP1);
            int sid = findOrAddPoint(sketch, sp);
            int eid = findOrAddPoint(sketch, ep);
            sketch.addLine(sid, eid);
        }
        return;
    }

    if (a.kind == TrimAction::Kind::Circle) {
        sketch.removeElement(a.targetId);
        int centerId = sketch.getPoint(a.circCenterPointId) ? a.circCenterPointId
                                                            : findOrAddPoint(sketch, a.circCenter);
        int n = static_cast<int>(a.circAngles.size());
        for (int i = 0; i < n; ++i) {
            if (i == a.circKillIndex) continue;
            float a0 = a.circAngles[i];
            float a1 = a.circAngles[(i + 1) % n];
            glm::vec2 sp = a.circCenter + glm::vec2(std::cos(a0), std::sin(a0)) * a.circRadius;
            glm::vec2 ep = a.circCenter + glm::vec2(std::cos(a1), std::sin(a1)) * a.circRadius;
            int sid = findOrAddPoint(sketch, sp);
            int eid = findOrAddPoint(sketch, ep);
            sketch.addArc(centerId, sid, eid, static_cast<double>(a.circRadius));
        }
        return;
    }

    if (a.kind == TrimAction::Kind::Arc) {
        sketch.removeElement(a.targetId);
        int centerId = sketch.getPoint(a.arcCenterPointId) ? a.arcCenterPointId
                                                           : findOrAddPoint(sketch, a.arcCenter);
        for (int i = 0; i + 1 < static_cast<int>(a.arcBounds.size()); ++i) {
            if (i == a.arcKillIndex) continue;
            float angA = a.arcStartA + a.arcBounds[i];
            float angB = a.arcStartA + a.arcBounds[i + 1];
            glm::vec2 sp = a.arcCenter + glm::vec2(std::cos(angA), std::sin(angA)) * a.arcRadius;
            glm::vec2 ep = a.arcCenter + glm::vec2(std::cos(angB), std::sin(angB)) * a.arcRadius;
            int sid = findOrAddPoint(sketch, sp);
            int eid = findOrAddPoint(sketch, ep);
            sketch.addArc(centerId, sid, eid, static_cast<double>(a.arcRadius));
        }
        return;
    }
}

} // anonymous

void SketchTool::handleTrimTool(glm::vec2 pos) {
    if (!m_sketch) return;
    float threshold = std::max(0.3f, tolStep() * 0.5f);
    TrimAction a = planTrim(*m_sketch, pos, threshold);
    if (!a.valid()) return;
    applyTrim(*m_sketch, a);
    // Trimming away a whole element (or an end segment) strands the original
    // endpoints — sweep them up so no orphan vertices remain.
    m_sketch->pruneOrphanPoints();
    m_trimHoverPoints.clear(); // stale after mutation; recomputed on next move
}

void SketchTool::computeTrimHover(glm::vec2 pos) {
    m_trimHoverPoints.clear();
    if (!m_sketch) return;
    float threshold = std::max(0.3f, tolStep() * 0.5f);
    TrimAction a = planTrim(*m_sketch, pos, threshold);
    if (a.valid()) densifyTrimPreview(*m_sketch, a, m_trimHoverPoints);
}

void SketchTool::handlePolygonTool(glm::vec2 pos) {
    if (!m_isPlacing) {
        // First click: set center
        m_firstClick = pos;
        m_isPlacing = true;
        m_clickCount = 1;
    } else {
        // Second click: set radius + rotation and create polygon. Cursor
        // direction from center defines vertex 0's angle, so the first
        // vertex lands exactly under the (grid-snapped) cursor — same
        // "corner snaps to grid" behaviour the user gets with circles.
        glm::vec2 delta = pos - m_firstClick;
        float radius = glm::length(delta);
        if (radius > 1e-6f) {
            int existing = findCoincidentPoint(m_firstClick, -1);
            int centerId = (existing >= 0) ? existing : m_sketch->addPoint(m_firstClick);
            double rotation = std::atan2(delta.y, delta.x);
            // Diagnostic for the "first polygon comes out scrambled in the
            // wrong place" dogfood bug — logs what the tool THINKS it is
            // committing so the discrepancy point is identifiable.
            std::fprintf(stderr, "[Polygon] commit: center=(%.3f, %.3f) "
                                 "reusedPt=%d r=%.3f rot=%.1fdeg sides=%d "
                                 "clickPos=(%.3f, %.3f)\n",
                         m_firstClick.x, m_firstClick.y, existing,
                         radius, rotation * 180.0 / M_PI, m_polygonSides,
                         pos.x, pos.y);
            m_sketch->addPolygon(centerId, static_cast<double>(radius),
                                 m_polygonSides, rotation);
        }

        m_isPlacing = false;
        m_clickCount = 0;
    }
}

void SketchTool::handleTextTool(glm::vec2 pos) {
    // The click/anchor is the box CENTER (matches the preview + the SVG tool),
    // so shift the baseline-left origin generate() expects by the rotated centre
    // of the measured text box. The tool stays active, so several labels can be
    // stamped in a row.
    const glm::vec2 center = (m_textPrevMin + m_textPrevMax) * 0.5f;
    const float ang = glm::radians(static_cast<float>(m_textAngle));
    const float ca = std::cos(ang), sa = std::sin(ang);
    const glm::vec2 rc(center.x * ca - center.y * sa,
                       center.x * sa + center.y * ca);
    const glm::vec2 anchorPos = pos - rc;
    size_t p0 = m_sketch->getPoints().size();
    size_t l0 = m_sketch->getLines().size();
    int loops = TextSketch::generate(m_sketch, m_textString, m_textFontPath,
                                     anchorPos, m_textHeight,
                                     static_cast<float>(m_textAngle));
    if (loops <= 0) {
        std::fprintf(stderr, "[Text] nothing placed (font='%s')\n",
                     m_textFontPath.c_str());
        return;
    }
    recordStamp(p0, l0);
}

void SketchTool::handleAirfoilTool(glm::vec2 pos) {
    // Single-click stamp like Text/SVG, but the click is the LEADING EDGE, not
    // a bounding-box centre: that is the datum a wing's chord and twist are
    // measured from, so stations stacked for a loft line up on it.
    if (m_airfoil.empty()) return;
    const size_t p0 = m_sketch->getPoints().size();
    const size_t l0 = m_sketch->getLines().size();
    const size_t s0 = m_sketch->getSplines().size();
    if (AirfoilImport::place(m_sketch, m_airfoil, pos, m_airfoilChord,
                             static_cast<float>(m_textAngle),
                             m_airfoilAnchor) > 0) {
        recordStamp(p0, l0, s0);
    }
}

void SketchTool::handleSvgTool(glm::vec2 pos) {
    // Same single-click stamp as Text; the click point is the artwork's
    // bounding-box centre.
    size_t p0 = m_sketch->getPoints().size();
    size_t l0 = m_sketch->getLines().size();
    if (SvgImport::place(m_sketch, m_svgPaths, pos, m_svgWidth,
                         static_cast<float>(m_textAngle)) > 0) {
        recordStamp(p0, l0);
    }
}

void SketchTool::recordStamp(size_t pointsBefore, size_t linesBefore,
                             size_t splinesBefore) {
    std::vector<int> ids;
    const auto& lns = m_sketch->getLines();
    const auto& pts = m_sketch->getPoints();
    const auto& spl = m_sketch->getSplines();
    // Splines are captured too: a stamp that lays down curves (an airfoil
    // section is two of them) would otherwise leave them behind when the
    // placement is undone, and only its stray points would disappear.
    // The default sentinel means "this stamp made no splines" -- Text and SVG
    // emit line loops only, and must not sweep up pre-existing curves.
    if (splinesBefore != static_cast<size_t>(-1))
        for (size_t i = splinesBefore; i < spl.size(); ++i) ids.push_back(spl[i].id);
    for (size_t i = linesBefore; i < lns.size(); ++i) ids.push_back(lns[i].id);
    for (size_t i = pointsBefore; i < pts.size(); ++i) ids.push_back(pts[i].id);
    if (!ids.empty()) m_stampStack.push_back(std::move(ids)); // push, don't overwrite
}

void SketchTool::commitStamp() {
    if (!m_sketch) return;
    // Stamp at the positioned anchor (m_currentPos). Driven by the touch "Place"
    // button; desktop stamps directly on click via onMouseDown.
    if (m_mode == SketchToolMode::Text)     handleTextTool(m_currentPos);
    else if (m_mode == SketchToolMode::Svg) handleSvgTool(m_currentPos);
    else if (m_mode == SketchToolMode::Airfoil) handleAirfoilTool(m_currentPos);
}

// --- Interactive Mirror ----------------------------------------------------

bool SketchTool::beginMirror() {
    if (!m_sketch) return false;
    m_mirrorPoints.clear();  m_mirrorLines.clear(); m_mirrorCircles.clear();
    m_mirrorArcs.clear();    m_mirrorSplines.clear();

    if (hasElementSelection()) {
        m_mirrorPoints  = m_selectedPoints;
        m_mirrorLines   = m_selectedLines;
        m_mirrorCircles = m_selectedCircles;
        m_mirrorArcs    = m_selectedArcs;
        m_mirrorSplines = m_selectedSplines;
    } else {
        for (const auto& p : m_sketch->getPoints())  m_mirrorPoints.insert(p.id);
        for (const auto& l : m_sketch->getLines())   m_mirrorLines.insert(l.id);
        for (const auto& c : m_sketch->getCircles()) m_mirrorCircles.insert(c.id);
        for (const auto& a : m_sketch->getArcs())    m_mirrorArcs.insert(a.id);
        for (const auto& s : m_sketch->getSplines()) m_mirrorSplines.insert(s.id);
    }
    if (m_mirrorPoints.empty() && m_mirrorLines.empty() && m_mirrorCircles.empty() &&
        m_mirrorArcs.empty() && m_mirrorSplines.empty())
        return false;

    // Seed the line at the centroid of every involved vertex.
    glm::vec2 c{0.0f}; int n = 0;
    auto addPt = [&](int id) { if (auto* p = m_sketch->getPoint(id)) { c += p->pos; ++n; } };
    for (int id : m_mirrorPoints) addPt(id);
    for (int id : m_mirrorLines)
        for (const auto& l : m_sketch->getLines()) if (l.id == id) { addPt(l.startPointId); addPt(l.endPointId); break; }
    for (int id : m_mirrorCircles)
        for (const auto& cc : m_sketch->getCircles()) if (cc.id == id) { addPt(cc.centerPointId); break; }
    for (int id : m_mirrorArcs)
        for (const auto& aa : m_sketch->getArcs()) if (aa.id == id) { addPt(aa.startPointId); addPt(aa.endPointId); break; }
    for (int id : m_mirrorSplines)
        for (const auto& sp : m_sketch->getSplines()) if (sp.id == id) { for (int cp : sp.controlPointIds) addPt(cp); break; }
    glm::vec2 anchor = (n > 0) ? c / static_cast<float>(n) : glm::vec2(0.0f);

    setMode(SketchToolMode::Mirror); // clears the live selection; sources captured above
    m_mirrorAnchor = anchor;
    m_mirrorAngleRad = static_cast<float>(M_PI) * 0.5f; // vertical line
    m_mirrorActive = true;
    return true;
}

void SketchTool::cancelMirror() {
    m_mirrorActive = false;
    m_mirrorPoints.clear(); m_mirrorLines.clear(); m_mirrorCircles.clear();
    m_mirrorArcs.clear();   m_mirrorSplines.clear();
}

glm::vec2 SketchTool::mirrorReflect(glm::vec2 p) const {
    glm::vec2 d(std::cos(m_mirrorAngleRad), std::sin(m_mirrorAngleRad)); // line dir
    glm::vec2 nrm(-d.y, d.x);                                            // line normal
    glm::vec2 to = p - m_mirrorAnchor;
    return m_mirrorAnchor + d * glm::dot(to, d) - nrm * glm::dot(to, nrm);
}

void SketchTool::getMirrorPreview(std::vector<std::vector<glm::vec2>>& polylines,
                                  std::vector<glm::vec2>& points) const {
    polylines.clear();
    points.clear();
    if (!m_sketch || !m_mirrorActive) return;
    auto R = [&](glm::vec2 p) { return mirrorReflect(p); };

    // Track which vertices are consumed by a curve so lone points stay sparse.
    std::set<int> referenced;
    for (int id : m_mirrorLines)
        for (const auto& l : m_sketch->getLines()) if (l.id == id) {
            const SketchPoint* a = m_sketch->getPoint(l.startPointId);
            const SketchPoint* b = m_sketch->getPoint(l.endPointId);
            if (a && b) polylines.push_back({R(a->pos), R(b->pos)});
            referenced.insert(l.startPointId); referenced.insert(l.endPointId);
            break;
        }
    for (int id : m_mirrorCircles)
        for (const auto& c : m_sketch->getCircles()) if (c.id == id) {
            const SketchPoint* ctr = m_sketch->getPoint(c.centerPointId);
            if (ctr) {
                std::vector<glm::vec2> loop;
                const int N = 48;
                for (int i = 0; i <= N; ++i) {
                    float t = 2.0f * static_cast<float>(M_PI) * i / N;
                    loop.push_back(R(ctr->pos + glm::vec2(std::cos(t), std::sin(t)) *
                                              static_cast<float>(c.radius)));
                }
                polylines.push_back(std::move(loop));
            }
            referenced.insert(c.centerPointId);
            break;
        }
    for (int id : m_mirrorArcs)
        for (const auto& a : m_sketch->getArcs()) if (a.id == id) {
            const SketchPoint* ctr = m_sketch->getPoint(a.centerPointId);
            const SketchPoint* sp  = m_sketch->getPoint(a.startPointId);
            const SketchPoint* ep  = m_sketch->getPoint(a.endPointId);
            if (ctr && sp && ep) {
                float a0 = std::atan2(sp->pos.y - ctr->pos.y, sp->pos.x - ctr->pos.x);
                float a1 = std::atan2(ep->pos.y - ctr->pos.y, ep->pos.x - ctr->pos.x);
                // Sweep CCW from a0 to a1 (the minor arc convention addArc uses).
                while (a1 < a0) a1 += 2.0f * static_cast<float>(M_PI);
                std::vector<glm::vec2> poly;
                const int N = 32;
                for (int i = 0; i <= N; ++i) {
                    float t = a0 + (a1 - a0) * i / N;
                    poly.push_back(R(ctr->pos + glm::vec2(std::cos(t), std::sin(t)) *
                                             static_cast<float>(a.radius)));
                }
                polylines.push_back(std::move(poly));
            }
            referenced.insert(a.startPointId); referenced.insert(a.endPointId);
            referenced.insert(a.centerPointId);
            break;
        }
    for (int id : m_mirrorSplines)
        for (const auto& sp : m_sketch->getSplines()) if (sp.id == id) {
            std::vector<glm::vec2> poly;
            for (glm::vec2 q : m_sketch->sampleSpline2D(sp, 16)) poly.push_back(R(q));
            if (poly.size() >= 2) polylines.push_back(std::move(poly));
            for (int cp : sp.controlPointIds) referenced.insert(cp);
            break;
        }
    // Lone vertices (not part of any mirrored curve).
    for (int id : m_mirrorPoints)
        if (!referenced.count(id))
            if (const SketchPoint* p = m_sketch->getPoint(id)) points.push_back(R(p->pos));
}

void SketchTool::commitMirror(std::set<int>& outPoints, std::set<int>& outLines) {
    if (!m_sketch || !m_mirrorActive) return;
    std::unordered_map<int, int> remap;
    auto remapPt = [&](int oldId) -> int {
        auto it = remap.find(oldId);
        if (it != remap.end()) return it->second;
        const SketchPoint* p = m_sketch->getPoint(oldId);
        if (!p) return -1;
        glm::vec2 np = mirrorReflect(p->pos);
        // Weld a reflected vertex onto an existing coincident one (a point on
        // the mirror line maps to itself) — same as the one-shot mirror did.
        int existing = findCoincidentPoint(np, -1);
        int nid = (existing >= 0) ? existing : m_sketch->addPoint(np);
        remap[oldId] = nid;
        return nid;
    };
    for (int id : m_mirrorPoints) { int nid = remapPt(id); if (nid >= 0) outPoints.insert(nid); }
    for (int lid : m_mirrorLines)
        for (const auto& l : m_sketch->getLines()) if (l.id == lid) {
            int s = remapPt(l.startPointId), e = remapPt(l.endPointId);
            if (s >= 0 && e >= 0 && s != e) {
                int nl = m_sketch->addLine(s, e);
                outLines.insert(nl); outPoints.insert(s); outPoints.insert(e);
            }
            break;
        }
    for (int cid : m_mirrorCircles)
        for (const auto& c : m_sketch->getCircles()) if (c.id == cid) {
            int ctr = remapPt(c.centerPointId);
            if (ctr >= 0) { m_sketch->addCircle(ctr, c.radius); outPoints.insert(ctr); }
            break;
        }
    for (int aid : m_mirrorArcs)
        for (const auto& a : m_sketch->getArcs()) if (a.id == aid) {
            int ctr = remapPt(a.centerPointId);
            int s = remapPt(a.startPointId), e = remapPt(a.endPointId);
            // Reflection reverses winding — swap start/end so the rebuilt arc
            // keeps the same swept (minor) span on the mirrored side.
            if (ctr >= 0 && s >= 0 && e >= 0) {
                m_sketch->addArc(ctr, e, s, a.radius);
                outPoints.insert(s); outPoints.insert(e); outPoints.insert(ctr);
            }
            break;
        }
    for (int sid : m_mirrorSplines)
        for (const auto& sp : m_sketch->getSplines()) if (sp.id == sid) {
            std::vector<int> cps;
            for (int cp : sp.controlPointIds) { int n2 = remapPt(cp); if (n2 >= 0) cps.push_back(n2); }
            if (cps.size() >= 2) m_sketch->addSpline(cps);
            for (int n2 : cps) outPoints.insert(n2);
            break;
        }
}

// --- Offset tool ----------------------------------------------------------

void SketchTool::updateOffsetHover(glm::vec2 pos) {
    m_offsetChainHover.clear();
    if (!m_sketch) return;
    if (m_offsetSuppressHover) {
        if (glm::length(pos - m_offsetCommitPos) < std::max(1.0f, tolStep() * 2.0f))
            return;
        m_offsetSuppressHover = false;
    }
    // Same catch range Trim picks with, so the two hover the same way.
    const float threshold = std::max(0.3f, tolStep() * 0.5f);
    OffsetChain ch = walkOffsetChain(*m_sketch, pos, threshold);
    if (ch.valid()) densifyChain(ch, m_offsetChainHover);
}

void SketchTool::updateOffsetDistance(glm::vec2 pos) {
    if (!m_offsetChain.valid()) return;
    m_offsetDistance = signedDistanceToChain(m_offsetChain, pos);
    recomputeOffsetPreview();
}

void SketchTool::recomputeOffsetPreview() {
    m_offsetPreview.clear();
    m_offsetResult = OffsetResult{};
    if (!m_offsetChain.valid() || std::abs(m_offsetDistance) < 1e-6f) return;
    m_offsetResult = offsetChain(m_offsetChain, m_offsetDistance, m_offsetCorners);
    pruneOffset(m_offsetResult, m_offsetChain, m_offsetDistance);
    if (m_offsetResult.valid) densifySegs(m_offsetResult.segs, m_offsetPreview);
}

void SketchTool::setOffsetDistance(float d) {
    m_offsetDistance = d;
    recomputeOffsetPreview();
}

void SketchTool::handleOffsetTool(glm::vec2 pos) {
    if (!m_sketch) return;

    if (m_offsetPhase == OffsetPhase::Pick) {
        const float threshold = std::max(0.3f, tolStep() * 0.5f);
        OffsetChain ch = walkOffsetChain(*m_sketch, pos, threshold);
        if (!ch.valid()) return; // missed, or landed on something unofferable
        m_offsetChain = std::move(ch);
        m_offsetChainHover.clear();
        m_offsetPhase = OffsetPhase::Distance;
        m_offsetDistance = 0.0f;
        m_offsetResult = OffsetResult{};
        m_offsetPreview.clear();
        // Marks a placement in progress so the host's two-step Escape applies.
        m_isPlacing = true;
        return;
    }

    // Distance phase: the click asks for the commit, it does not perform it —
    // the app wraps commitOffset in recordSketchMutation for one undo step.
    updateOffsetDistance(snap(pos));
    if (m_offsetResult.valid) m_offsetCommitRequested = true;
}

void SketchTool::commitOffset(std::set<int>& outPoints, std::set<int>& outElements) {
    m_offsetCommitRequested = false;
    if (!m_sketch || !m_offsetResult.valid) return;
    applyOffset(*m_sketch, m_offsetResult,
                [this](glm::vec2 p) { return findCoincidentPoint(p, -1); },
                outPoints, outElements);
    // Back to Pick, tool still active: offsetting several chains in a row is
    // the normal way to use this (Trim likewise stays active after a click).
    const glm::vec2 where = m_currentPos;
    cancelOffset();
    // The cursor is still sitting on the geometry we just made, so the
    // Pick-phase hover would light it up immediately — the orange ghost turns
    // cyan ON THE SPOT and the whole commit reads as "nothing happened" (it is
    // exactly what it looked like in testing). Stay quiet until the cursor
    // actually moves off what was just created.
    m_offsetSuppressHover = true;
    m_offsetCommitPos = where;
}

void SketchTool::cancelOffset() {
    m_offsetPhase = OffsetPhase::Pick;
    m_offsetChain = OffsetChain{};
    m_offsetResult = OffsetResult{};
    m_offsetDistance = 0.0f;
    m_offsetCommitRequested = false;
    m_offsetChainHover.clear();
    m_offsetPreview.clear();
    m_offsetSuppressHover = false;
    if (m_mode == SketchToolMode::Offset) m_isPlacing = false;
}

void SketchTool::undoLastStamp() {
    if (!m_sketch || m_stampStack.empty()) return;
    // Pop ONE stamp off the top — repeated calls walk back to the original.
    const std::vector<int>& ids = m_stampStack.back();
    for (int id : ids) m_sketch->removeElement(id);
    std::fprintf(stderr, "[Stamp] removed last placement (%zu elements, %zu stamp(s) left)\n",
                 ids.size(), m_stampStack.size() - 1);
    m_stampStack.pop_back();
}

// --- Dimension tool ---------------------------------------------------

DimPick SketchTool::hitTestDimEntity(glm::vec2 pos) const {
    DimPick out;
    if (!m_sketch) return out;
    int nearPt = findCoincidentPoint(pos, -1);
    if (nearPt >= 0) {
        // Text glyph geometry is not dimensionable — fall through and let a
        // real line/circle/arc at this position win instead (a fromText hit
        // must not abort the whole hit test).
        const SketchPoint* p = m_sketch->getPoint(nearPt);
        if (p && !p->fromText) {
            // A circle / arc CENTRE resolves to its circle, not the bare
            // point: dimensioning to a lone centre almost always means the
            // circle (diameter, or a gap to another circle). Picking the
            // point would instead make a centre-to-centre distance, which is
            // rarely what's wanted and blocks the rim-gap dim.
            for (const auto& c : m_sketch->getCircles())
                if (c.centerPointId == nearPt) return {DimEntityKind::Circle, c.id};
            for (const auto& a : m_sketch->getArcs())
                if (a.centerPointId == nearPt) return {DimEntityKind::Arc, a.id};
            return {DimEntityKind::Point, nearPt};
        }
    }
    const float tol = std::max(tolStep() * 0.5f, 0.5f) * snapScale();
    // Lines (segment distance), skipping fromText — same math as handleSelectTool.
    float bestD = 0.0f; int bestLine = -1;
    for (const auto& l : m_sketch->getLines()) {
        if (l.fromText) continue;
        const SketchPoint* a = m_sketch->getPoint(l.startPointId);
        const SketchPoint* b = m_sketch->getPoint(l.endPointId);
        if (!a || !b) continue;
        glm::vec2 ab = b->pos - a->pos;
        float len2 = glm::dot(ab, ab);
        if (len2 < 1e-12f) continue;
        float t = glm::clamp(glm::dot(pos - a->pos, ab) / len2, 0.0f, 1.0f);
        float d = glm::distance(a->pos + ab * t, pos);
        if (d < tol && (bestLine < 0 || d < bestD)) { bestLine = l.id; bestD = d; }
    }
    if (bestLine >= 0) return {DimEntityKind::Line, bestLine};
    // Circle then arc perimeters — same as handleSelectTool.
    float bestCd = 0.0f; int bestCircle = -1;
    for (const auto& c : m_sketch->getCircles()) {
        const SketchPoint* ctr = m_sketch->getPoint(c.centerPointId);
        if (!ctr) continue;
        float d = std::abs(glm::distance(pos, ctr->pos) - static_cast<float>(c.radius));
        if (d < tol && (bestCircle < 0 || d < bestCd)) { bestCircle = c.id; bestCd = d; }
    }
    if (bestCircle >= 0) return {DimEntityKind::Circle, bestCircle};
    float bestAd = 0.0f; int bestArc = -1;
    for (const auto& a : m_sketch->getArcs()) {
        const SketchPoint* ctr = m_sketch->getPoint(a.centerPointId);
        if (!ctr) continue;
        float d = std::abs(glm::distance(pos, ctr->pos) - static_cast<float>(a.radius));
        if (d < tol && (bestArc < 0 || d < bestAd)) { bestArc = a.id; bestAd = d; }
    }
    if (bestArc >= 0) return {DimEntityKind::Arc, bestArc};
    return out;
}

void SketchTool::handleDimensionTool(glm::vec2 pos) {
    if (!m_sketch) return;
    m_currentPos = pos;
    switch (m_dimPhase) {
        case DimPhase::PickFirst: {
            DimPick hit = hitTestDimEntity(pos);
            if (hit.kind == DimEntityKind::None) return;
            m_dimPickA = hit;
            if (hit.kind == DimEntityKind::Line ||
                hit.kind == DimEntityKind::Circle ||
                hit.kind == DimEntityKind::Arc)
                // Tentative single-entity dim (line length / circle diameter),
                // but stay open for a second pick: a second circle/arc turns
                // this into a centre-to-centre distance, a second line into a
                // distance/angle. Empty space commits the tentative dim.
                m_dimPending = resolveDimension(*m_sketch, hit, DimPick{});
            else
                m_dimPending = PendingDimension{}; // lone point: no dim yet
            m_dimPhase = DimPhase::PickSecondOrPlace;
            return;
        }
        case DimPhase::PickSecondOrPlace: {
            DimPick hit = hitTestDimEntity(pos);
            if (hit.kind != DimEntityKind::None) {
                if (hit.kind == m_dimPickA.kind && hit.id == m_dimPickA.id) {
                    m_dimRejectReason = "Same entity — pick a different one.";
                    return;
                }
                PendingDimension pair = resolveDimension(*m_sketch, m_dimPickA, hit);
                if (pair.valid) { m_dimPending = pair; m_dimPhase = DimPhase::PlaceLabel; return; }
                // Invalid combo: picks unchanged, but say why rather than
                // letting the click look like a miss.
                m_dimRejectReason =
                    (m_dimPickA.kind == DimEntityKind::Line && hit.kind == DimEntityKind::Line)
                        ? "These lines can't be dimensioned to each other "
                          "(they meet, or are collinear)."
                        : "That pair can't be dimensioned.";
                return;
            }
            // Empty space: places the tentative single-entity dim (line length).
            if (m_dimPending.valid) { m_dimLabelPos = pos; m_dimReady = true; return; }
            // Lone point + empty space: nothing to measure yet.
            m_dimRejectReason = "A single point has no dimension — "
                                "pick a second entity.";
            return;
        }
        case DimPhase::PlaceLabel: {
            m_dimLabelPos = pos;
            m_dimReady = true;
            return;
        }
    }
}

void SketchTool::clearDimState() {
    m_dimPhase = DimPhase::PickFirst;
    m_dimPickA = DimPick{};
    m_dimPending = PendingDimension{};
    m_dimReady = false;
}

PendingDimension SketchTool::resolveDimension(const Sketch& sk, DimPick a, DimPick b) {
    PendingDimension out;
    auto lineById = [&sk](int id) -> const SketchLine* {
        for (const auto& l : sk.getLines()) if (l.id == id) return &l;
        return nullptr;
    };
    auto lineEnds = [&sk, &lineById](int id, glm::vec2& s, glm::vec2& e) {
        const SketchLine* l = lineById(id);
        if (!l) return false;
        const SketchPoint* sp = sk.getPoint(l->startPointId);
        const SketchPoint* ep = sk.getPoint(l->endPointId);
        if (!sp || !ep) return false;
        s = sp->pos; e = ep->pos;
        return true;
    };
    auto perpDist = [](glm::vec2 p, glm::vec2 s, glm::vec2 e) -> double {
        glm::vec2 d = e - s;
        float len = glm::length(d);
        if (len < 1e-10f) return -1.0;
        glm::vec2 r = p - s;
        return std::abs(static_cast<double>(d.x) * r.y -
                        static_cast<double>(d.y) * r.x) / len;
    };
    // Centre point id of a picked circle or arc (both store centerPointId).
    auto centerPointId = [&sk](DimPick pk) -> int {
        if (pk.kind == DimEntityKind::Circle)
            for (const auto& c : sk.getCircles())
                if (c.id == pk.id) return c.centerPointId;
        if (pk.kind == DimEntityKind::Arc)
            for (const auto& a : sk.getArcs())
                if (a.id == pk.id) return a.centerPointId;
        return -1;
    };

    // Single-entity dims.
    if (b.kind == DimEntityKind::None) {
        if (a.kind == DimEntityKind::Circle) {
            for (const auto& c : sk.getCircles())
                if (c.id == a.id) { out = {ConstraintType::Radius, a.id, -1, c.radius, true}; break; }
            return out;
        }
        if (a.kind == DimEntityKind::Arc) {
            for (const auto& ar : sk.getArcs())
                if (ar.id == a.id) { out = {ConstraintType::Radius, a.id, -1, ar.radius, true}; break; }
            return out;
        }
        if (a.kind == DimEntityKind::Line) {
            const SketchLine* l = lineById(a.id);
            glm::vec2 s, e;
            if (l && lineEnds(a.id, s, e))
                out = {ConstraintType::Distance, l->startPointId, l->endPointId,
                       static_cast<double>(glm::distance(s, e)), true};
            return out;
        }
        return out; // lone point: invalid
    }

    auto isCurve = [](DimEntityKind k) {
        return k == DimEntityKind::Circle || k == DimEntityKind::Arc;
    };
    auto radiusOf = [&sk](DimPick pk) -> double {
        if (pk.kind == DimEntityKind::Circle)
            for (const auto& c : sk.getCircles())
                if (c.id == pk.id) return c.radius;
        if (pk.kind == DimEntityKind::Arc)
            for (const auto& a : sk.getArcs())
                if (a.id == pk.id) return a.radius;
        return -1.0;
    };
    // Two circles/arcs → rim-to-rim gap (centre distance minus both radii),
    // the clearance a machinist reads between the two circle edges — NOT the
    // centre distance. entityA/entityB stay the circle ids so the solver can
    // recompute the gap as radii change.
    if (isCurve(a.kind) && isCurve(b.kind)) {
        int ca = centerPointId(a), cb = centerPointId(b);
        const SketchPoint* pa = sk.getPoint(ca);
        const SketchPoint* pb = sk.getPoint(cb);
        double rA = radiusOf(a), rB = radiusOf(b);
        if (pa && pb && ca != cb && rA >= 0.0 && rB >= 0.0)
            out = {ConstraintType::CircleGap, a.id, b.id,
                   static_cast<double>(glm::distance(pa->pos, pb->pos)) - rA - rB,
                   true};
        return out;
    }

    // A circle/arc paired with a line or a point dimensions from its CENTRE.
    // hitTestDimEntity deliberately resolves a centre-point click to the
    // circle itself (so the rim-gap dim stays reachable), which left the
    // centre unpickable and made hole-centre-to-edge — the most common
    // dimension on a machining drawing — impossible to create at all.
    // Substituting the centre point here recovers it without a modifier key,
    // so it works the same on touch.
    if (isCurve(a.kind) && (b.kind == DimEntityKind::Line ||
                            b.kind == DimEntityKind::Point)) {
        int ca = centerPointId(a);
        if (ca < 0) return out;
        a = {DimEntityKind::Point, ca};
    } else if (isCurve(b.kind) && (a.kind == DimEntityKind::Line ||
                                   a.kind == DimEntityKind::Point)) {
        int cb = centerPointId(b);
        if (cb < 0) return out;
        b = {DimEntityKind::Point, cb};
    }

    // Normalize point-first for the mixed pair.
    if (a.kind == DimEntityKind::Line && b.kind == DimEntityKind::Point) std::swap(a, b);

    if (a.kind == DimEntityKind::Point && b.kind == DimEntityKind::Point) {
        const SketchPoint* pa = sk.getPoint(a.id);
        const SketchPoint* pb = sk.getPoint(b.id);
        if (pa && pb)
            out = {ConstraintType::Distance, a.id, b.id,
                   static_cast<double>(glm::distance(pa->pos, pb->pos)), true};
        return out;
    }
    if (a.kind == DimEntityKind::Point && b.kind == DimEntityKind::Line) {
        const SketchPoint* p = sk.getPoint(a.id);
        glm::vec2 s, e;
        if (p && lineEnds(b.id, s, e)) {
            // Reject a point that IS an endpoint of the target line: the
            // perpendicular distance is then 0 by construction, so a
            // DistancePointLine constraint pinned at 0 has no direction to
            // correct along — applyCorrection's ~value/2 nudge on the point
            // and the line's endpoints cancels itself out every iteration
            // and flings the other endpoint outward instead of converging.
            const SketchLine* bl = lineById(b.id);
            if (bl && (a.id == bl->startPointId || a.id == bl->endPointId)) return out;
            double d = perpDist(p->pos, s, e);
            if (d >= 0.0) out = {ConstraintType::DistancePointLine, a.id, b.id, d, true};
        }
        return out;
    }
    if (a.kind == DimEntityKind::Line && b.kind == DimEntityKind::Line) {
        glm::vec2 as, ae, bs, be;
        if (!lineEnds(a.id, as, ae) || !lineEnds(b.id, bs, be)) return out;
        glm::vec2 da = ae - as, db = be - bs;
        if (glm::length(da) < 1e-10f || glm::length(db) < 1e-10f) return out;
        // Signed angle of B relative to A, wrapped to [-π, π] — same
        // convention as the solver's Angle error term.
        double ang = std::atan2(db.y, db.x) - std::atan2(da.y, da.x);
        while (ang >  M_PI) ang -= 2.0 * M_PI;
        while (ang < -M_PI) ang += 2.0 * M_PI;
        // Parallel (or anti-parallel) within kDimParallelTolRad: distance
        // dim, pinned to one of the SECOND line's endpoints measured to the
        // FIRST line (the first pick stays the reference for both branches).
        // Endpoint choice matters visually: in chained sketches (rectangles)
        // an endpoint is a shared corner and its perpendicular foot can land
        // outside the first segment, hanging the label off to the side over
        // unrelated geometry. Prefer the endpoint whose foot falls INSIDE
        // the first segment, and skip endpoints the first line owns
        // (zero-distance pick in disguise).
        double folded = std::min(std::abs(ang), M_PI - std::abs(ang));
        if (folded <= kDimParallelTolRad) {
            const SketchLine* lb = lineById(b.id);
            const SketchLine* la = lineById(a.id);
            if (!lb || !la) return out;
            // Chained segments (sharing a vertex) that read as parallel are
            // near-collinear polyline continuations — a "distance between
            // these lines" dim is ill-defined there. Reject the pair.
            if (lb->startPointId == la->startPointId || lb->startPointId == la->endPointId ||
                lb->endPointId == la->startPointId || lb->endPointId == la->endPointId)
                return out;
            glm::vec2 dA = ae - as;
            float lenA2 = glm::dot(dA, dA); // >0: parallel branch already
                                            // rejected degenerate lines
            int bestPt = -1;
            double bestScore = -1.0;
            const int candIds[2] = {lb->startPointId, lb->endPointId};
            const glm::vec2 candPos[2] = {bs, be};
            for (int i = 0; i < 2; ++i) {
                if (candIds[i] == la->startPointId || candIds[i] == la->endPointId)
                    continue;
                float t = glm::dot(candPos[i] - as, dA) / lenA2;
                // Score: distance of the foot parameter from the segment
                // interior — 0 while inside [0,1], grows outside. Lower wins.
                double outside = (t < 0.0f) ? -t : (t > 1.0f ? t - 1.0f : 0.0);
                if (bestPt < 0 || outside < bestScore) {
                    bestPt = i;
                    bestScore = outside;
                }
            }
            if (bestPt < 0) return out; // both endpoints belong to line A
            double d = perpDist(candPos[bestPt], as, ae);
            // d≈0 means the lines are collinear (chained polyline segments):
            // a zero-distance dim is meaningless and destabilises the solver.
            if (d >= 1e-6)
                out = {ConstraintType::DistancePointLine, candIds[bestPt], a.id, d, true};
        } else {
            out = {ConstraintType::Angle, a.id, b.id, ang, true};
        }
        return out;
    }
    return out; // circle/arc pairs and circle+point: out of scope
}

bool SketchTool::linesParallelWithinDimTol(const Sketch& sk, int lineIdA, int lineIdB) {
    auto lineEnds = [&sk](int id, glm::vec2& s, glm::vec2& e) {
        for (const auto& l : sk.getLines()) {
            if (l.id != id) continue;
            const SketchPoint* sp = sk.getPoint(l.startPointId);
            const SketchPoint* ep = sk.getPoint(l.endPointId);
            if (!sp || !ep) return false;
            s = sp->pos; e = ep->pos;
            return true;
        }
        return false;
    };
    glm::vec2 as, ae, bs, be;
    if (!lineEnds(lineIdA, as, ae) || !lineEnds(lineIdB, bs, be)) return false;
    glm::vec2 da = ae - as, db = be - bs;
    if (glm::length(da) < 1e-10f || glm::length(db) < 1e-10f) return false;
    // Same signed-angle-then-fold test as resolveDimension's line-line
    // branch, so "parallel enough" means the same thing everywhere.
    double ang = std::atan2(db.y, db.x) - std::atan2(da.y, da.x);
    while (ang >  M_PI) ang -= 2.0 * M_PI;
    while (ang < -M_PI) ang += 2.0 * M_PI;
    double folded = std::min(std::abs(ang), M_PI - std::abs(ang));
    return folded <= kDimParallelTolRad;
}

} // namespace materializr
