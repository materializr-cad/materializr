#include "SketchSolver.h"
#include <cmath>
#include <algorithm>

namespace materializr {

SketchSolver::SketchSolver() = default;

// Thin wrappers around the active sketch's constraint storage. The Sketch owns
// the data; the solver only caches state/DOF from the last solve().
int SketchSolver::addConstraint(const Constraint& c) {
    if (!m_sketch) return -1;
    return m_sketch->addConstraint(c);
}

void SketchSolver::removeConstraint(int id) {
    if (!m_sketch) return;
    m_sketch->removeConstraint(id);
}

static const std::vector<Constraint> s_emptyConstraints;
const std::vector<Constraint>& SketchSolver::getConstraints() const {
    if (!m_sketch) return s_emptyConstraints;
    return m_sketch->getConstraints();
}

bool SketchSolver::solve(Sketch& sketch, int maxIterations, double tolerance) {
    m_sketch = &sketch;
    auto& constraints = sketch.getMutableConstraints();
    // Compute degrees of freedom
    int numPoints = sketch.pointCount();
    int numEquations = 0;
    for (const auto& c : constraints) {
        // Reference (non-driving) dimensions annotate only - they enforce
        // nothing, so they must not consume a degree of freedom. Counting
        // them would report a freely-movable sketch as Fully/Over-constrained
        // the moment a measurement was placed on it.
        if (!c.isDriving) continue;
        switch (c.type) {
            case ConstraintType::Coincident:
                numEquations += 2; // x and y must match
                break;
            case ConstraintType::Horizontal:
            case ConstraintType::Vertical:
                numEquations += 1;
                break;
            case ConstraintType::Distance:
                numEquations += 1;
                break;
            case ConstraintType::Radius:
                numEquations += 1;
                break;
            case ConstraintType::Parallel:
                numEquations += 1;
                break;
            case ConstraintType::Perpendicular:
                numEquations += 1;
                break;
            case ConstraintType::Fixed:
                numEquations += 2; // x and y fixed
                break;
            case ConstraintType::Tangent:
                numEquations += 1;
                break;
            case ConstraintType::Equal:
                numEquations += 1;
                break;
            case ConstraintType::Concentric:
                numEquations += 2; // x and y must match (same as coincident)
                break;
            case ConstraintType::Angle:
                numEquations += 1; // one angle equation between two lines
                break;
            case ConstraintType::DistancePointLine:
                numEquations += 1;
                break;
            case ConstraintType::CircleGap:
                numEquations += 1;
                break;
        }
    }

    // Not every freedom in a sketch is a point coordinate, and not every
    // stored value is a freedom.
    //
    // A CIRCLE stores a centre point and a radius: three values for the three
    // freedoms a circle actually has. Nothing else determines its radius, and
    // applyCorrection drives it directly through setCircleRadius, so it earns
    // a +1. Counting only points while still subtracting the Radius/CircleGap
    // equation that pins it put every circle one degree low - a circle with a
    // locked centre and a driven radius (3 equations against 3 freedoms) read
    // Over-constrained.
    //
    // An ARC stores a centre, a start point, an end point AND a radius: seven
    // values for a shape that geometrically has only five freedoms (centre
    // x/y, radius, two endpoint bearings). The other two are the intrinsic
    // relations |start - centre| == |end - centre| == radius.
    //
    // Whether those two may be SUBTRACTED depends on whether anything actually
    // holds them. A driving Radius does: its correction goes through
    // setArcRadius -> resizeArc, which slides both endpoints onto the radius.
    // With no driving Radius nothing enforces either relation - every other
    // correction moves arc endpoints through the generic point mover - so all
    // seven stored values are independently reachable and the arc really does
    // have seven freedoms, incoherent ones included. Subtracting the relations
    // there would claim a coherence the solver does not maintain, and report a
    // still-movable sketch as fully constrained.
    int numCircles = static_cast<int>(sketch.getCircles().size());
    int arcsWithDrivenRadius = 0, arcsWithout = 0;
    for (const auto& arc : sketch.getArcs()) {
        bool driven = false;
        for (const auto& c : constraints) {
            if (c.isDriving && c.type == ConstraintType::Radius &&
                c.entityA == arc.id) { driven = true; break; }
        }
        (driven ? arcsWithDrivenRadius : arcsWithout) += 1;
    }

    // +1 radius per circle. Per arc: +1 for the radius, and -2 for the
    // intrinsic relations only where a driving Radius keeps them true.
    m_dof = 2 * numPoints + numCircles
            + arcsWithout - arcsWithDrivenRadius
            - numEquations;

    if (m_dof < 0) {
        m_state = SketchState::OverConstrained;
    } else if (m_dof == 0) {
        m_state = SketchState::FullyConstrained;
    } else {
        m_state = SketchState::UnderConstrained;
    }

    // Refresh every reference dimension's stored value from the geometry it
    // measures. Every computeError branch is defined as (current − target),
    // so adding the residual back onto the target yields the current reading.
    // Degenerate branches return 0.0, which leaves the last good value in
    // place rather than collapsing the label to zero.
    //
    // Run at both exits below so a label always shows the post-solve geometry
    // - a reference dim on a shape that a DRIVING constraint just moved has
    // to follow it, which is the whole point of an annotation.
    auto refreshReferenceValues = [&] {
        for (auto& c : constraints) {
            if (c.isDriving) continue;
            c.value += computeError(c, sketch);
        }
    };

    // Record which way round each unsigned dimension was placed, once, from
    // the geometry as it stands the first time it is solved - which is the
    // arrangement the user drew. Everything after this reads the stored value,
    // so a dimension driven through zero is restored to the side it came from
    // rather than whichever side the last correction happened to leave it on.
    for (auto& c : constraints) {
        if (!c.isDriving) continue;
        if (c.orientX != 0.0 || c.orientY != 0.0) continue; // already recorded
        if (c.type == ConstraintType::DistancePointLine) {
            const SketchPoint* p = sketch.getPoint(c.entityA);
            if (!p) continue;
            for (const auto& line : sketch.getLines()) {
                if (line.id != c.entityB) continue;
                const SketchPoint* a = sketch.getPoint(line.startPointId);
                const SketchPoint* b = sketch.getPoint(line.endPointId);
                if (!a || !b) break;
                glm::vec2 dir = b->pos - a->pos, rel = p->pos - a->pos;
                double cross = static_cast<double>(dir.x) * rel.y -
                               static_cast<double>(dir.y) * rel.x;
                if (cross != 0.0) c.orientX = (cross > 0.0) ? 1.0 : -1.0;
                break;
            }
        }
    }
    // The Distance hint is NOT sticky, and must not be: it is only ever a
    // tiebreaker for the moment the two points coincide and the geometry
    // offers no direction of its own. Frozen at first solve it goes stale the
    // same way the old hardcoded +x did - drag a horizontal pair upright, run
    // the value through zero, and it separates horizontally again. Re-record
    // it every solve while it is still knowable, so the fallback always
    // describes where the pair last actually was.
    //
    // The DPL side above is the opposite case and is deliberately written
    // once: re-deriving it from live geometry is exactly what let a crossing
    // be adopted instead of corrected.
    for (auto& c : constraints) {
        if (!c.isDriving || c.type != ConstraintType::Distance) continue;
        const SketchPoint* pa = sketch.getPoint(c.entityA);
        const SketchPoint* pb = sketch.getPoint(c.entityB);
        if (!pa || !pb) continue;
        glm::vec2 d = pb->pos - pa->pos;
        float len = glm::length(d);
        if (len > 1e-10f) { c.orientX = d.x / len; c.orientY = d.y / len; }
    }

    // Iterative relaxation
    for (int iter = 0; iter < maxIterations; ++iter) {
        double maxError = 0.0;

        for (auto& constraint : constraints) {
            // Reference dimensions are pure annotation: never corrected, and
            // never allowed to hold up convergence. They are reported
            // satisfied so the UI doesn't paint them as violated - a
            // measurement cannot be "unsatisfied".
            if (!constraint.isDriving) {
                constraint.isSatisfied = true;
                continue;
            }
            double error = computeError(constraint, sketch);
            maxError = std::max(maxError, std::abs(error));

            if (std::abs(error) > tolerance) {
                applyCorrection(constraint, sketch, error);
                constraint.isSatisfied = false;
            } else {
                constraint.isSatisfied = true;
            }
        }

        if (maxError <= tolerance) {
            // Mark all as satisfied
            for (auto& c : constraints) {
                c.isSatisfied = true;
            }
            refreshReferenceValues();
            return true;
        }
    }

    refreshReferenceValues();
    return false;
}

SketchState SketchSolver::getState() const {
    return m_state;
}

int SketchSolver::degreesOfFreedom() const {
    return m_dof;
}

void SketchSolver::clear() {
    if (m_sketch) {
        m_sketch->getMutableConstraints().clear();
        m_sketch->setNextConstraintId(1);
    }
    m_state = SketchState::UnderConstrained;
    m_dof = 0;
}

double SketchSolver::computeError(const Constraint& c, const Sketch& sketch) const {
    switch (c.type) {
        case ConstraintType::Coincident: {
            const SketchPoint* pa = sketch.getPoint(c.entityA);
            const SketchPoint* pb = sketch.getPoint(c.entityB);
            if (!pa || !pb) return 0.0;
            glm::vec2 diff = pa->pos - pb->pos;
            return glm::length(diff);
        }

        case ConstraintType::Horizontal: {
            // entityA is a line id - find the line
            const auto& lines = sketch.getLines();
            for (const auto& line : lines) {
                if (line.id == c.entityA) {
                    const SketchPoint* sp = sketch.getPoint(line.startPointId);
                    const SketchPoint* ep = sketch.getPoint(line.endPointId);
                    if (!sp || !ep) return 0.0;
                    return ep->pos.y - sp->pos.y;
                }
            }
            return 0.0;
        }

        case ConstraintType::Vertical: {
            const auto& lines = sketch.getLines();
            for (const auto& line : lines) {
                if (line.id == c.entityA) {
                    const SketchPoint* sp = sketch.getPoint(line.startPointId);
                    const SketchPoint* ep = sketch.getPoint(line.endPointId);
                    if (!sp || !ep) return 0.0;
                    return ep->pos.x - sp->pos.x;
                }
            }
            return 0.0;
        }

        case ConstraintType::Distance: {
            const SketchPoint* pa = sketch.getPoint(c.entityA);
            const SketchPoint* pb = sketch.getPoint(c.entityB);
            if (!pa || !pb) return 0.0;
            double dist = glm::length(pa->pos - pb->pos);
            return dist - c.value;
        }

        case ConstraintType::Radius: {
            // entityA is a circle OR an arc id - the Dimension tool creates
            // this type for both (see resolveDimension's Circle/Arc branches),
            // and applyCorrection below already writes back through
            // setCircleRadius / setArcRadius. Scanning circles only made an
            // arc id return 0.0 ("already satisfied"), so applyCorrection's
            // arc branch was never reached: the radius never moved while
            // solve() reported success and the label rendered the typed value
            // over unchanged geometry.
            for (const auto& circle : sketch.getCircles()) {
                if (circle.id == c.entityA) {
                    return circle.radius - c.value;
                }
            }
            for (const auto& arc : sketch.getArcs()) {
                if (arc.id != c.entityA) continue;
                // An arc's radius is not just the stored number: the arc is
                // only that radius if both endpoints actually sit on it, and
                // buildWires reads all of centre, endpoints and radius. Every
                // OTHER correction branch moves endpoints through plain
                // movePoint without touching the stored radius, so measuring
                // the field alone reported "satisfied" the instant the number
                // matched while a neighbour had already dragged the geometry
                // off it - the setter ran once and was then silently undone.
                // Measure the worst of the three so the solver keeps pulling
                // until the arc really holds its dimension.
                //
                // DRIVING only. refreshReferenceValues re-reads an annotation
                // as value += computeError, so handing it the worst of three
                // disagreeing residuals made a reference label bounce between
                // the two endpoint distances forever instead of settling on a
                // reading.
                if (!c.isDriving) return arc.radius - c.value;
                double worst = arc.radius - c.value;
                const SketchPoint* ctr = sketch.getPoint(arc.centerPointId);
                if (ctr) {
                    for (int ptId : {arc.startPointId, arc.endPointId}) {
                        const SketchPoint* p = sketch.getPoint(ptId);
                        if (!p) continue;
                        double d = glm::length(p->pos - ctr->pos) - c.value;
                        if (std::abs(d) > std::abs(worst)) worst = d;
                    }
                }
                return worst;
            }
            return 0.0;
        }

        case ConstraintType::Fixed: {
            const SketchPoint* pa = sketch.getPoint(c.entityA);
            if (!pa) return 0.0;
            // Fixed = pin this point at the (value, valueY) it had when the
            // constraint was added.
            glm::vec2 target(static_cast<float>(c.value),
                             static_cast<float>(c.valueY));
            return glm::length(pa->pos - target);
        }

        case ConstraintType::Parallel: {
            // entityA and entityB are line ids
            const auto& lines = sketch.getLines();
            glm::vec2 dirA(0), dirB(0);
            for (const auto& line : lines) {
                if (line.id == c.entityA) {
                    const SketchPoint* sp = sketch.getPoint(line.startPointId);
                    const SketchPoint* ep = sketch.getPoint(line.endPointId);
                    if (sp && ep) dirA = ep->pos - sp->pos;
                }
                if (line.id == c.entityB) {
                    const SketchPoint* sp = sketch.getPoint(line.startPointId);
                    const SketchPoint* ep = sketch.getPoint(line.endPointId);
                    if (sp && ep) dirB = ep->pos - sp->pos;
                }
            }
            float lenA = glm::length(dirA);
            float lenB = glm::length(dirB);
            if (lenA < 1e-10f || lenB < 1e-10f) return 0.0;
            // Cross product (should be 0 for parallel)
            dirA /= lenA;
            dirB /= lenB;
            return static_cast<double>(dirA.x * dirB.y - dirA.y * dirB.x);
        }

        case ConstraintType::Perpendicular: {
            const auto& lines = sketch.getLines();
            glm::vec2 dirA(0), dirB(0);
            for (const auto& line : lines) {
                if (line.id == c.entityA) {
                    const SketchPoint* sp = sketch.getPoint(line.startPointId);
                    const SketchPoint* ep = sketch.getPoint(line.endPointId);
                    if (sp && ep) dirA = ep->pos - sp->pos;
                }
                if (line.id == c.entityB) {
                    const SketchPoint* sp = sketch.getPoint(line.startPointId);
                    const SketchPoint* ep = sketch.getPoint(line.endPointId);
                    if (sp && ep) dirB = ep->pos - sp->pos;
                }
            }
            float lenA = glm::length(dirA);
            float lenB = glm::length(dirB);
            if (lenA < 1e-10f || lenB < 1e-10f) return 0.0;
            // Dot product (should be 0 for perpendicular)
            dirA /= lenA;
            dirB /= lenB;
            return static_cast<double>(dirA.x * dirB.x + dirA.y * dirB.y);
        }

        case ConstraintType::Tangent: {
            // Tangent between arc (entityA) and line (entityB)
            // The arc's endpoint should lie on the line, and the line direction
            // should be perpendicular to the radius at that point
            const auto& arcs = sketch.getArcs();
            const auto& lines = sketch.getLines();

            const SketchArc* arc = nullptr;
            const SketchLine* line = nullptr;

            for (const auto& a : arcs) {
                if (a.id == c.entityA) { arc = &a; break; }
            }
            for (const auto& l : lines) {
                if (l.id == c.entityB) { line = &l; break; }
            }

            if (!arc || !line) return 0.0;

            const SketchPoint* arcCenter = sketch.getPoint(arc->centerPointId);
            const SketchPoint* lineStart = sketch.getPoint(line->startPointId);
            const SketchPoint* lineEnd = sketch.getPoint(line->endPointId);
            if (!arcCenter || !lineStart || !lineEnd) return 0.0;

            // Distance from arc center to line should equal arc radius
            glm::vec2 lineDir = lineEnd->pos - lineStart->pos;
            float lineLen = glm::length(lineDir);
            if (lineLen < 1e-10f) return 0.0;
            lineDir /= lineLen;

            // Signed distance from center to line (using perpendicular)
            glm::vec2 toCenter = arcCenter->pos - lineStart->pos;
            float dist = std::abs(toCenter.x * (-lineDir.y) + toCenter.y * lineDir.x);

            return static_cast<double>(dist - static_cast<float>(arc->radius));
        }

        case ConstraintType::Equal: {
            // entityA/entityB are EITHER two line ids (equal length) OR two
            // circle/arc ids (equal radius). Try lengths first, fall back to
            // radii so one constraint type covers both.
            auto lineLen = [&](int id, double& out) -> bool {
                for (const auto& line : sketch.getLines())
                    if (line.id == id) {
                        const SketchPoint* sp = sketch.getPoint(line.startPointId);
                        const SketchPoint* ep = sketch.getPoint(line.endPointId);
                        if (!sp || !ep) return false;
                        out = glm::length(ep->pos - sp->pos);
                        return true;
                    }
                return false;
            };
            auto curveRad = [&](int id, double& out) -> bool {
                for (const auto& ci : sketch.getCircles())
                    if (ci.id == id) { out = ci.radius; return true; }
                for (const auto& ar : sketch.getArcs())
                    if (ar.id == id) { out = ar.radius; return true; }
                return false;
            };
            double a = 0.0, b = 0.0;
            if (lineLen(c.entityA, a) && lineLen(c.entityB, b)) return a - b;
            if (curveRad(c.entityA, a) && curveRad(c.entityB, b)) return a - b;
            return 0.0; // mixed / missing: inert
        }

        case ConstraintType::Concentric: {
            // entityA and entityB are circle or arc ids; their centers should coincide
            // Look up center points for both entities
            int centerA = -1, centerB = -1;

            const auto& circles = sketch.getCircles();
            const auto& arcs = sketch.getArcs();

            for (const auto& circ : circles) {
                if (circ.id == c.entityA) centerA = circ.centerPointId;
                if (circ.id == c.entityB) centerB = circ.centerPointId;
            }
            for (const auto& arc : arcs) {
                if (arc.id == c.entityA) centerA = arc.centerPointId;
                if (arc.id == c.entityB) centerB = arc.centerPointId;
            }

            if (centerA < 0 || centerB < 0) return 0.0;

            const SketchPoint* pa = sketch.getPoint(centerA);
            const SketchPoint* pb = sketch.getPoint(centerB);
            if (!pa || !pb) return 0.0;

            return static_cast<double>(glm::length(pa->pos - pb->pos));
        }

        case ConstraintType::Angle: {
            // entityA and entityB are line ids. c.value is the target angle in
            // radians (signed, line B relative to line A). Error = current
            // signed angle minus target, wrapped to [-π, π].
            const auto& lines = sketch.getLines();
            glm::vec2 dirA(0), dirB(0);
            for (const auto& line : lines) {
                if (line.id == c.entityA) {
                    const SketchPoint* sp = sketch.getPoint(line.startPointId);
                    const SketchPoint* ep = sketch.getPoint(line.endPointId);
                    if (sp && ep) dirA = ep->pos - sp->pos;
                }
                if (line.id == c.entityB) {
                    const SketchPoint* sp = sketch.getPoint(line.startPointId);
                    const SketchPoint* ep = sketch.getPoint(line.endPointId);
                    if (sp && ep) dirB = ep->pos - sp->pos;
                }
            }
            if (glm::length(dirA) < 1e-10f || glm::length(dirB) < 1e-10f) return 0.0;
            float angA = std::atan2(dirA.y, dirA.x);
            float angB = std::atan2(dirB.y, dirB.x);
            double diff = static_cast<double>(angB - angA) - c.value;
            const double TWO_PI = 2.0 * M_PI;
            while (diff >  M_PI) diff -= TWO_PI;
            while (diff < -M_PI) diff += TWO_PI;
            return diff;
        }

        case ConstraintType::DistancePointLine: {
            // entityA = point id, entityB = line id. Distance is to the
            // line's INFINITE carrier, matching how CAD dimensions read.
            const SketchPoint* p = sketch.getPoint(c.entityA);
            if (!p) return 0.0;
            for (const auto& line : sketch.getLines()) {
                if (line.id != c.entityB) continue;
                const SketchPoint* a = sketch.getPoint(line.startPointId);
                const SketchPoint* b = sketch.getPoint(line.endPointId);
                if (!a || !b) return 0.0;
                glm::vec2 dir = b->pos - a->pos;
                float len = glm::length(dir);
                if (len < 1e-10f) return 0.0; // degenerate line: inert, no NaN
                glm::vec2 rel = p->pos - a->pos;
                // SIGNED, against the side the dimension was placed on. An
                // unsigned |distance| - value reads zero for a point sitting
                // on the WRONG side at the right distance, so no correction
                // ever ran: the sketch sat mirrored and reported satisfied,
                // then jumped back across the line the next time an unrelated
                // edit happened to disturb the value. Recording the side is
                // only worth anything if the error function enforces it.
                double sgn = (static_cast<double>(dir.x) * rel.y -
                              static_cast<double>(dir.y) * rel.x) / len;
                double side = (c.orientX != 0.0) ? ((c.orientX > 0.0) ? 1.0 : -1.0)
                                                 : ((sgn < 0.0) ? -1.0 : 1.0);
                return side * sgn - c.value;
            }
            return 0.0;
        }

        case ConstraintType::CircleGap: {
            // entityA / entityB = circle (or arc) ids. Gap is the rim-to-rim
            // clearance: |centreA - centreB| - rA - rB.
            int caPt = -1, cbPt = -1;
            double rA = 0.0, rB = 0.0;
            for (const auto& ci : sketch.getCircles()) {
                if (ci.id == c.entityA) { caPt = ci.centerPointId; rA = ci.radius; }
                if (ci.id == c.entityB) { cbPt = ci.centerPointId; rB = ci.radius; }
            }
            for (const auto& ar : sketch.getArcs()) {
                if (ar.id == c.entityA) { caPt = ar.centerPointId; rA = ar.radius; }
                if (ar.id == c.entityB) { cbPt = ar.centerPointId; rB = ar.radius; }
            }
            const SketchPoint* pa = sketch.getPoint(caPt);
            const SketchPoint* pb = sketch.getPoint(cbPt);
            if (!pa || !pb) return 0.0;
            double centreDist = glm::length(pa->pos - pb->pos);
            return (centreDist - rA - rB) - c.value;
        }
    }

    return 0.0;
}

void SketchSolver::applyCorrection(const Constraint& c, Sketch& sketch, double error) const {
    switch (c.type) {
        case ConstraintType::Coincident: {
            const SketchPoint* pa = sketch.getPoint(c.entityA);
            const SketchPoint* pb = sketch.getPoint(c.entityB);
            if (!pa || !pb) return;
            glm::vec2 mid = (pa->pos + pb->pos) * 0.5f;
            sketch.movePoint(c.entityA, mid);
            sketch.movePoint(c.entityB, mid);
            break;
        }

        case ConstraintType::Horizontal: {
            const auto& lines = sketch.getLines();
            for (const auto& line : lines) {
                if (line.id == c.entityA) {
                    const SketchPoint* sp = sketch.getPoint(line.startPointId);
                    const SketchPoint* ep = sketch.getPoint(line.endPointId);
                    if (!sp || !ep) return;
                    float avgY = (sp->pos.y + ep->pos.y) * 0.5f;
                    sketch.movePoint(line.startPointId, glm::vec2(sp->pos.x, avgY));
                    sketch.movePoint(line.endPointId, glm::vec2(ep->pos.x, avgY));
                    break;
                }
            }
            break;
        }

        case ConstraintType::Vertical: {
            const auto& lines = sketch.getLines();
            for (const auto& line : lines) {
                if (line.id == c.entityA) {
                    const SketchPoint* sp = sketch.getPoint(line.startPointId);
                    const SketchPoint* ep = sketch.getPoint(line.endPointId);
                    if (!sp || !ep) return;
                    float avgX = (sp->pos.x + ep->pos.x) * 0.5f;
                    sketch.movePoint(line.startPointId, glm::vec2(avgX, sp->pos.y));
                    sketch.movePoint(line.endPointId, glm::vec2(avgX, ep->pos.y));
                    break;
                }
            }
            break;
        }

        case ConstraintType::Distance: {
            const SketchPoint* pa = sketch.getPoint(c.entityA);
            const SketchPoint* pb = sketch.getPoint(c.entityB);
            if (!pa || !pb) return;
            glm::vec2 diff = pb->pos - pa->pos;
            float currentDist = glm::length(diff);
            if (currentDist < 1e-10f) {
                // Coincident points carry no direction of their own. Pushing
                // apart along a hardcoded +x rotated the pair onto that axis:
                // a vertical pair driven 10 -> 0 -> 10 came back horizontal.
                // Separate along the direction the dimension was placed on
                // when there is one, and only fall back to +x when there is
                // not.
                glm::vec2 hint(static_cast<float>(c.orientX),
                               static_cast<float>(c.orientY));
                // Normalise rather than assume: the pair round-trips through
                // the project file at the stream's default precision.
                diff = (glm::length(hint) > 1e-6f) ? glm::normalize(hint)
                                                   : glm::vec2(1.0f, 0.0f);
                currentDist = 1.0f;
            }
            glm::vec2 dir = diff / currentDist;
            float targetDist = static_cast<float>(c.value);
            float correction = (targetDist - currentDist) * 0.5f;
            sketch.movePoint(c.entityA, pa->pos - dir * correction);
            sketch.movePoint(c.entityB, pb->pos + dir * correction);
            break;
        }

        case ConstraintType::Radius: {
            // entityA is the circle's (or arc's) id. The radius lives in
            // the circle / arc struct itself, so we write it back through
            // the dedicated setter rather than the generic point-mover.
            // We try circles first (the common case) and fall back to arcs.
            //
            // A non-positive radius is not reachable geometry. Both setters
            // clamp it to 1e-6, and for an arc that clamp now drags both
            // endpoints in with it, so a value of 0 or less would grind the
            // arc down onto its own centre and take the profile around it with
            // it - worse than the value simply not applying. Leave the geometry
            // alone; computeError still reports the constraint unsatisfied.
            if (c.value <= 0.0) return;
            for (const auto& circle : sketch.getCircles()) {
                if (circle.id == c.entityA) {
                    sketch.setCircleRadius(c.entityA, c.value);
                    return;
                }
            }
            for (const auto& arc : sketch.getArcs()) {
                if (arc.id == c.entityA) {
                    sketch.setArcRadius(c.entityA, c.value);
                    return;
                }
            }
            break;
        }

        case ConstraintType::Fixed: {
            glm::vec2 target(static_cast<float>(c.value),
                             static_cast<float>(c.valueY));
            sketch.movePoint(c.entityA, target);
            break;
        }

        case ConstraintType::Parallel: {
            const auto& lines = sketch.getLines();
            const SketchLine* lineA = nullptr;
            const SketchLine* lineB = nullptr;
            for (const auto& line : lines) {
                if (line.id == c.entityA) lineA = &line;
                if (line.id == c.entityB) lineB = &line;
            }
            if (!lineA || !lineB) return;

            const SketchPoint* spB = sketch.getPoint(lineB->startPointId);
            const SketchPoint* epB = sketch.getPoint(lineB->endPointId);
            const SketchPoint* spA = sketch.getPoint(lineA->startPointId);
            const SketchPoint* epA = sketch.getPoint(lineA->endPointId);
            if (!spA || !epA || !spB || !epB) return;

            // Rotate line B's endpoint to be parallel to line A
            glm::vec2 dirA = epA->pos - spA->pos;
            float lenA = glm::length(dirA);
            if (lenA < 1e-10f) return;
            dirA /= lenA;

            float lenB = glm::length(epB->pos - spB->pos);
            glm::vec2 newEndB = spB->pos + dirA * lenB;
            sketch.movePoint(lineB->endPointId, newEndB);
            break;
        }

        case ConstraintType::Perpendicular: {
            const auto& lines = sketch.getLines();
            const SketchLine* lineA = nullptr;
            const SketchLine* lineB = nullptr;
            for (const auto& line : lines) {
                if (line.id == c.entityA) lineA = &line;
                if (line.id == c.entityB) lineB = &line;
            }
            if (!lineA || !lineB) return;

            const SketchPoint* spB = sketch.getPoint(lineB->startPointId);
            const SketchPoint* epB = sketch.getPoint(lineB->endPointId);
            const SketchPoint* spA = sketch.getPoint(lineA->startPointId);
            const SketchPoint* epA = sketch.getPoint(lineA->endPointId);
            if (!spA || !epA || !spB || !epB) return;

            // Rotate line B's endpoint to be perpendicular to line A
            glm::vec2 dirA = epA->pos - spA->pos;
            float lenA = glm::length(dirA);
            if (lenA < 1e-10f) return;
            dirA /= lenA;

            // Perpendicular direction: rotate 90 degrees
            glm::vec2 perpDir(-dirA.y, dirA.x);
            float lenB = glm::length(epB->pos - spB->pos);
            glm::vec2 newEndB = spB->pos + perpDir * lenB;
            sketch.movePoint(lineB->endPointId, newEndB);
            break;
        }

        case ConstraintType::Tangent: {
            // Move the arc center so its distance to the line equals the radius
            const auto& arcs = sketch.getArcs();
            const auto& lines = sketch.getLines();

            const SketchArc* arc = nullptr;
            const SketchLine* line = nullptr;

            for (const auto& a : arcs) {
                if (a.id == c.entityA) { arc = &a; break; }
            }
            for (const auto& l : lines) {
                if (l.id == c.entityB) { line = &l; break; }
            }

            if (!arc || !line) return;

            const SketchPoint* arcCenter = sketch.getPoint(arc->centerPointId);
            const SketchPoint* lineStart = sketch.getPoint(line->startPointId);
            const SketchPoint* lineEnd = sketch.getPoint(line->endPointId);
            if (!arcCenter || !lineStart || !lineEnd) return;

            glm::vec2 lineDir = lineEnd->pos - lineStart->pos;
            float lineLen = glm::length(lineDir);
            if (lineLen < 1e-10f) return;
            lineDir /= lineLen;

            // Normal to the line (pointing toward center)
            glm::vec2 normal(-lineDir.y, lineDir.x);
            glm::vec2 toCenter = arcCenter->pos - lineStart->pos;
            float signedDist = toCenter.x * normal.x + toCenter.y * normal.y;

            // We want |signedDist| == radius. Preserve the sign.
            float targetDist = static_cast<float>(arc->radius);
            if (signedDist < 0.0f) targetDist = -targetDist;

            float correction = targetDist - signedDist;
            glm::vec2 newCenter = arcCenter->pos + normal * correction;
            sketch.movePoint(arc->centerPointId, newCenter);
            break;
        }

        case ConstraintType::Equal: {
            // Circle/arc pair → equalise radii to their average (radii live in
            // the shape structs, written through the dedicated setters). Falls
            // through to the line-length path below when the ids are lines.
            {
                auto curveRad = [&](int id, double& out) -> bool {
                    for (const auto& ci : sketch.getCircles())
                        if (ci.id == id) { out = ci.radius; return true; }
                    for (const auto& ar : sketch.getArcs())
                        if (ar.id == id) { out = ar.radius; return true; }
                    return false;
                };
                auto setRad = [&](int id, double r) {
                    for (const auto& ci : sketch.getCircles())
                        if (ci.id == id) { sketch.setCircleRadius(id, r); return; }
                    for (const auto& ar : sketch.getArcs())
                        if (ar.id == id) { sketch.setArcRadius(id, r); return; }
                };
                double rA = 0.0, rB = 0.0;
                if (curveRad(c.entityA, rA) && curveRad(c.entityB, rB)) {
                    double avg = 0.5 * (rA + rB);
                    setRad(c.entityA, avg);
                    setRad(c.entityB, avg);
                    return;
                }
            }
            // Make both lines the same length by adjusting their endpoints
            const auto& lines = sketch.getLines();
            const SketchLine* lineA = nullptr;
            const SketchLine* lineB = nullptr;

            for (const auto& line : lines) {
                if (line.id == c.entityA) lineA = &line;
                if (line.id == c.entityB) lineB = &line;
            }
            if (!lineA || !lineB) return;

            const SketchPoint* spA = sketch.getPoint(lineA->startPointId);
            const SketchPoint* epA = sketch.getPoint(lineA->endPointId);
            const SketchPoint* spB = sketch.getPoint(lineB->startPointId);
            const SketchPoint* epB = sketch.getPoint(lineB->endPointId);
            if (!spA || !epA || !spB || !epB) return;

            float lenA = glm::length(epA->pos - spA->pos);
            float lenB = glm::length(epB->pos - spB->pos);

            if (lenA < 1e-10f && lenB < 1e-10f) return;

            float avgLen = (lenA + lenB) * 0.5f;

            // Scale line B's endpoint to match average length
            if (lenB > 1e-10f) {
                glm::vec2 dirB = (epB->pos - spB->pos) / lenB;
                glm::vec2 newEndB = spB->pos + dirB * avgLen;
                sketch.movePoint(lineB->endPointId, newEndB);
            }

            // Scale line A's endpoint to match average length
            if (lenA > 1e-10f) {
                glm::vec2 dirA = (epA->pos - spA->pos) / lenA;
                glm::vec2 newEndA = spA->pos + dirA * avgLen;
                sketch.movePoint(lineA->endPointId, newEndA);
            }
            break;
        }

        case ConstraintType::Concentric: {
            // Same as Coincident but for circle/arc centers
            int centerA = -1, centerB = -1;

            const auto& circles = sketch.getCircles();
            const auto& arcs = sketch.getArcs();

            for (const auto& circ : circles) {
                if (circ.id == c.entityA) centerA = circ.centerPointId;
                if (circ.id == c.entityB) centerB = circ.centerPointId;
            }
            for (const auto& arc : arcs) {
                if (arc.id == c.entityA) centerA = arc.centerPointId;
                if (arc.id == c.entityB) centerB = arc.centerPointId;
            }

            if (centerA < 0 || centerB < 0) return;

            const SketchPoint* pa = sketch.getPoint(centerA);
            const SketchPoint* pb = sketch.getPoint(centerB);
            if (!pa || !pb) return;

            glm::vec2 mid = (pa->pos + pb->pos) * 0.5f;
            sketch.movePoint(centerA, mid);
            sketch.movePoint(centerB, mid);
            break;
        }

        case ConstraintType::Angle: {
            // Rotate line B around its start point so its direction makes the
            // target signed angle (c.value) with line A. Line A is left alone.
            const auto& lines = sketch.getLines();
            const SketchLine* lineA = nullptr;
            const SketchLine* lineB = nullptr;
            for (const auto& line : lines) {
                if (line.id == c.entityA) lineA = &line;
                if (line.id == c.entityB) lineB = &line;
            }
            if (!lineA || !lineB) return;
            const SketchPoint* spA = sketch.getPoint(lineA->startPointId);
            const SketchPoint* epA = sketch.getPoint(lineA->endPointId);
            const SketchPoint* spB = sketch.getPoint(lineB->startPointId);
            const SketchPoint* epB = sketch.getPoint(lineB->endPointId);
            if (!spA || !epA || !spB || !epB) return;
            glm::vec2 dirA = epA->pos - spA->pos;
            if (glm::length(dirA) < 1e-10f) return;
            float angA = std::atan2(dirA.y, dirA.x);
            float targetAng = angA + static_cast<float>(c.value);
            float lenB = glm::length(epB->pos - spB->pos);
            glm::vec2 newDir(std::cos(targetAng), std::sin(targetAng));
            sketch.movePoint(lineB->endPointId, spB->pos + newDir * lenB);
            break;
        }

        case ConstraintType::DistancePointLine: {
            const SketchPoint* p = sketch.getPoint(c.entityA);
            if (!p) return;
            for (const auto& line : sketch.getLines()) {
                if (line.id != c.entityB) continue;
                // Defensive identity guard: resolveDimension rejects picking
                // a point that IS an endpoint of the target line, but a
                // constraint reaching the solver this way (e.g. loaded from
                // an older project file, or injected directly) has zero
                // ACTUAL perpendicular distance (the point sits on the line
                // by construction) with no correction direction to separate
                // them along - the ~value/2 nudge below would cancel itself
                // for the point (it's also one of the endpoints being
                // corrected) but not for the OTHER endpoint, which gets
                // flung outward every iteration instead of settling. Leave
                // it alone; computeError() above returns 0-value harmlessly.
                if (c.entityA == line.startPointId || c.entityA == line.endPointId) return;
                const SketchPoint* a = sketch.getPoint(line.startPointId);
                const SketchPoint* b = sketch.getPoint(line.endPointId);
                if (!a || !b) return;
                glm::vec2 dir = b->pos - a->pos;
                float len = glm::length(dir);
                if (len < 1e-10f) return;
                dir /= len;
                glm::vec2 n(-dir.y, dir.x); // unit normal
                float s = glm::dot(p->pos - a->pos, n); // signed distance
                // Which side the dimension was placed on. Re-deriving it from
                // the live sign (flip n whenever s < 0) meant that once
                // another correction - or a pass through zero - carried the
                // point across the line, the constraint cheerfully pinned it
                // on the WRONG side and called it satisfied. Drive towards the
                // recorded side instead, so crossing over is corrected rather
                // than adopted. Falls back to the old behaviour when no side
                // was recorded.
                float side = (c.orientX != 0.0)
                                 ? static_cast<float>(c.orientX > 0.0 ? 1.0 : -1.0)
                                 : (s < 0.0f ? -1.0f : 1.0f);
                // Move ONLY the measured point, not the reference line. The
                // line is what we're dimensioning AGAINST (e.g. an already-
                // placed box's edge); dragging its endpoints too would deform
                // that box every time a neighbour is dimensioned to it. The
                // point's own geometry (if part of a constrained rectangle)
                // gets re-squared by that rectangle's H/V constraints in the
                // following relaxation passes.
                //
                // n is the +1 side's normal by construction, so the target
                // signed distance is side * value.
                float corr = side * static_cast<float>(c.value) - s;
                sketch.movePoint(c.entityA, p->pos + n * corr);
                return;
            }
            break;
        }

        case ConstraintType::CircleGap: {
            // Drive the centres apart/together so the rim gap reaches target;
            // radii are left to their own Radius constraints. Target centre
            // distance = value + rA + rB.
            int caPt = -1, cbPt = -1;
            double rA = 0.0, rB = 0.0;
            for (const auto& ci : sketch.getCircles()) {
                if (ci.id == c.entityA) { caPt = ci.centerPointId; rA = ci.radius; }
                if (ci.id == c.entityB) { cbPt = ci.centerPointId; rB = ci.radius; }
            }
            for (const auto& ar : sketch.getArcs()) {
                if (ar.id == c.entityA) { caPt = ar.centerPointId; rA = ar.radius; }
                if (ar.id == c.entityB) { cbPt = ar.centerPointId; rB = ar.radius; }
            }
            const SketchPoint* pa = sketch.getPoint(caPt);
            const SketchPoint* pb = sketch.getPoint(cbPt);
            if (!pa || !pb) return;
            // The rim gap bottoms out at -(rA + rB): that is concentric, the
            // deepest two circles can overlap. A value below it implies a
            // NEGATIVE centre distance, which no arrangement of two points can
            // satisfy - the correction then drove the centres past each other,
            // dir flipped, and the next pass drove them back, so the solver
            // oscillated for the full iteration budget and left the geometry
            // wherever the last pass dropped it. Clamp to the closest
            // achievable arrangement instead; the constraint still reports
            // unsatisfied (computeError is unchanged), so the UI can flag the
            // value as impossible rather than the sketch quietly thrashing.
            float targetCentre = std::max(0.0f, static_cast<float>(c.value + rA + rB));

            glm::vec2 diff = pb->pos - pa->pos;
            float centreDist = glm::length(diff);
            if (centreDist < 1e-10f) {
                // Coincident centres carry no direction. Only invent one when
                // the target actually wants them apart - inventing it while
                // the target is concentric shoved them back off each other
                // every pass, which is the same thrash by another route.
                if (targetCentre < 1e-10f) return;
                diff = glm::vec2(1.0f, 0.0f);
                centreDist = 1.0f;
            }
            glm::vec2 dir = diff / centreDist;
            float corr = (targetCentre - centreDist) * 0.5f;
            sketch.movePoint(caPt, pa->pos - dir * corr);
            sketch.movePoint(cbPt, pb->pos + dir * corr);
            break;
        }
    }
}

} // namespace materializr
