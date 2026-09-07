#include "ui/LengthField.h"
#include "core/Units.h"
#include "SketchEditOp.h"
#include "SketchSolver.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <sstream>
#include <vector>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"

namespace materializr {

SketchEditOp::SketchEditOp(std::shared_ptr<Sketch> liveSketch,
                           std::shared_ptr<Sketch> beforeSnapshot,
                           std::shared_ptr<Sketch> afterSnapshot)
    : m_target(std::move(liveSketch)),
      m_before(std::move(beforeSnapshot)),
      m_after(std::move(afterSnapshot)) {}

bool SketchEditOp::execute(Document& /*doc*/) {
    if (!m_target || !m_after) return false;
    *m_target = *m_after;
    return true;
}

bool SketchEditOp::undo(Document& /*doc*/) {
    if (!m_target || !m_before) return false;
    *m_target = *m_before;
    return true;
}

// Helper: human-friendly name for a ConstraintType. Used in descriptions.
static const char* constraintName(ConstraintType t) {
    switch (t) {
        case ConstraintType::Coincident:    return "Coincident";
        case ConstraintType::Horizontal:    return "Horizontal";
        case ConstraintType::Vertical:      return "Vertical";
        case ConstraintType::Distance:      return "Distance";
        case ConstraintType::Radius:        return "Ø";
        case ConstraintType::Parallel:      return "Parallel";
        case ConstraintType::Perpendicular: return "Perpendicular";
        case ConstraintType::Fixed:         return "Fix Position";
        case ConstraintType::Tangent:       return "Tangent";
        case ConstraintType::Equal:         return "Equal";
        case ConstraintType::Concentric:    return "Concentric";
        case ConstraintType::Angle:         return "Angle";
        case ConstraintType::DistancePointLine: return "Distance to Line";
        case ConstraintType::CircleGap: return "Circle Gap";
    }
    return "Constraint";
}

std::string SketchEditOp::description() const {
    if (!m_before || !m_after) return "Sketch edit";

    // Constraint diff first — these read more specifically than the generic
    // geometry-count descriptions below.
    const auto& cBefore = m_before->getConstraints();
    const auto& cAfter  = m_after->getConstraints();
    if (cBefore.size() != cAfter.size()) {
        // Added or removed. Look at the difference set (by id).
        char buf[80];
        if (cAfter.size() > cBefore.size()) {
            // Find the first id present in after but not before.
            for (const auto& c : cAfter) {
                bool wasThere = false;
                for (const auto& b : cBefore) if (b.id == c.id) { wasThere = true; break; }
                if (wasThere) continue;
                const char* name = constraintName(c.type);
                if (c.type == ConstraintType::Distance) {
                    std::snprintf(buf, sizeof(buf), "Add Distance %s", materializr::fmtLength(c.value).c_str());
                } else if (c.type == ConstraintType::Radius) {
                    std::snprintf(buf, sizeof(buf), "Add \xC3\x98 %s", materializr::fmtLength(c.value * 2.0).c_str());
                } else if (c.type == ConstraintType::Angle) {
                    std::snprintf(buf, sizeof(buf), "Add Angle %.1f\xC2\xB0",
                                  c.value * 180.0 / M_PI);
                } else if (c.type == ConstraintType::DistancePointLine) {
                    std::snprintf(buf, sizeof(buf), "Add Distance %s", materializr::fmtLength(c.value).c_str());
                } else if (c.type == ConstraintType::CircleGap) {
                    std::snprintf(buf, sizeof(buf), "Add Gap %s", materializr::fmtLength(c.value).c_str());
                } else {
                    std::snprintf(buf, sizeof(buf), "Add %s", name);
                }
                return buf;
            }
            return "Add constraint";
        } else {
            // Removed — name the removed type if we can identify it.
            for (const auto& b : cBefore) {
                bool stillThere = false;
                for (const auto& a : cAfter) if (a.id == b.id) { stillThere = true; break; }
                if (stillThere) continue;
                std::snprintf(buf, sizeof(buf), "Remove %s", constraintName(b.type));
                return buf;
            }
            return "Remove constraint";
        }
    } else {
        // Same count — check for a value edit on the same id.
        for (size_t i = 0; i < cAfter.size(); ++i) {
            // Find matching id in before.
            const Constraint* bMatch = nullptr;
            for (const auto& b : cBefore) if (b.id == cAfter[i].id) { bMatch = &b; break; }
            if (!bMatch) continue;
            // isDriving too: promoting a reference dimension to driving (or
            // back) changes nothing numeric, so a value-only comparison would
            // classify it as "no edit" and the toggle would get no history
            // step to undo.
            if (std::abs(bMatch->value - cAfter[i].value) > 1e-9 ||
                std::abs(bMatch->valueY - cAfter[i].valueY) > 1e-9 ||
                bMatch->isDriving != cAfter[i].isDriving) {
                char buf[100];
                if (cAfter[i].type == ConstraintType::Angle) {
                    std::snprintf(buf, sizeof(buf), "Edit Angle %.1f\xC2\xB0 \xE2\x86\x92 %.1f\xC2\xB0",
                                  bMatch->value * 180.0 / M_PI,
                                  cAfter[i].value * 180.0 / M_PI);
                } else if (cAfter[i].type == ConstraintType::Radius) {
                    std::snprintf(buf, sizeof(buf), "Edit \xC3\x98 %s \xE2\x86\x92 %s", materializr::fmtLength(bMatch->value * 2.0).c_str(), materializr::fmtLength(cAfter[i].value * 2.0).c_str());
                } else if (cAfter[i].type == ConstraintType::Distance ||
                           cAfter[i].type == ConstraintType::DistancePointLine ||
                           cAfter[i].type == ConstraintType::CircleGap) {
                    std::snprintf(buf, sizeof(buf), "Edit Distance %s \xE2\x86\x92 %s", materializr::fmtLength(bMatch->value).c_str(), materializr::fmtLength(cAfter[i].value).c_str());
                } else {
                    std::snprintf(buf, sizeof(buf), "Edit %s",
                                  constraintName(cAfter[i].type));
                }
                return buf;
            }
        }
    }

    // No constraint diff — describe the GEOMETRY that was added, measured
    // directly off the snapshot (no constraint required). Turns the generic
    // "Add sketch element" into "Rectangle 80 × 45 mm", "Circle Ø20 mm", etc.,
    // so the history reads meaningfully. (A "reference dimension" for display
    // only — it drives nothing, so there's no over-constraint risk.)
    {
        auto posOf = [](const Sketch& sk, int ptId) -> glm::vec2 {
            for (const auto& p : sk.getPoints()) if (p.id == ptId) return p.pos;
            return glm::vec2(0.0f, 0.0f);
        };
        auto isNew = [](int id, const auto& beforeVec) {
            for (const auto& b : beforeVec) if (b.id == id) return false;
            return true;
        };
        std::vector<const SketchLine*> nl;
        for (const auto& l : m_after->getLines())
            if (isNew(l.id, m_before->getLines())) nl.push_back(&l);
        std::vector<const SketchCircle*> nc;
        for (const auto& c : m_after->getCircles())
            if (isNew(c.id, m_before->getCircles())) nc.push_back(&c);
        std::vector<const SketchArc*> na;
        for (const auto& a : m_after->getArcs())
            if (isNew(a.id, m_before->getArcs())) na.push_back(&a);

        auto lineLen = [&](const SketchLine* l) {
            glm::vec2 a = posOf(*m_after, l->startPointId);
            glm::vec2 b = posOf(*m_after, l->endPointId);
            double dx = b.x - a.x, dy = b.y - a.y;
            return std::sqrt(dx * dx + dy * dy);
        };
        char buf[96];
        if (nc.size() == 1 && nl.empty() && na.empty()) {
            std::snprintf(buf, sizeof(buf), "Circle \xC3\x98%s", materializr::fmtLength(nc[0]->radius * 2.0).c_str());
            return buf;
        }
        if (na.size() == 1 && nl.empty() && nc.empty()) {
            std::snprintf(buf, sizeof(buf), "Arc R%s", materializr::fmtLength(na[0]->radius).c_str());
            return buf;
        }
        if (nl.size() == 4 && nc.empty() && na.empty()) {
            // Rectangle iff the four sides form two equal pairs (W,W,H,H).
            double L[4]; for (int i = 0; i < 4; ++i) L[i] = lineLen(nl[i]);
            std::sort(L, L + 4);
            const double tol = 1e-3 + 0.01 * L[3];   // 1% of the longest side
            if (std::abs(L[0] - L[1]) < tol && std::abs(L[2] - L[3]) < tol) {
                // Report width × height as the sketch-plane bounding box, so an
                // axis-aligned rectangle reads "80 × 45" (x-extent × y-extent)
                // the way it was drawn rather than sorted side lengths.
                float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
                for (const auto* l : nl)
                    for (int pid : {l->startPointId, l->endPointId}) {
                        glm::vec2 p = posOf(*m_after, pid);
                        minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
                        miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
                    }
                std::snprintf(buf, sizeof(buf), "Rectangle %s \xC3\x97 %s", materializr::fmtLength(maxx - minx).c_str(), materializr::fmtLength(maxy - miny).c_str());
                return buf;
            }
        }
        if (nl.size() == 1 && nc.empty() && na.empty()) {
            std::snprintf(buf, sizeof(buf), "Line %s", materializr::fmtLength(lineLen(nl[0])).c_str());
            return buf;
        }
    }

    // Anything else — fall back to the generic element-count diff.
    int delta = m_after->elementCount() - m_before->elementCount();
    if (delta > 0) return "Add sketch element";
    if (delta < 0) return "Remove sketch element";
    return "Modify sketch";
}

// Writes one Sketch's contents in the project file's SKETCH_START / SKETCH_END
// format. Matches the schema that ProjectIO::parseSketchBody reads, so the
// snapshots inside a SketchEditOp can be rehydrated on load by the same
// parser the top-level sketches use.
static void writeSketchBody(std::ostream& os, const Sketch& sk, int sketchId,
                            const std::string& name, bool visible, int sourceBody) {
    os << "SKETCH_START " << sketchId << " \"" << name << "\" "
       << (visible ? 1 : 0) << " " << sourceBody << "\n";

    // Plane.
    const auto& ax = sk.getPlane().Position();
    auto o = ax.Location();
    auto n = ax.Direction();
    auto x = ax.XDirection();
    auto y = ax.YDirection();
    os << "PLANE "
       << o.X() << " " << o.Y() << " " << o.Z() << " "
       << n.X() << " " << n.Y() << " " << n.Z() << " "
       << x.X() << " " << x.Y() << " " << x.Z() << " "
       << y.X() << " " << y.Y() << " " << y.Z() << "\n";

    // Points.
    const auto& pts = sk.getPoints();
    os << "POINT_COUNT " << pts.size() << "\n";
    for (const auto& p : pts) {
        os << "POINT " << p.id << " " << p.pos.x << " " << p.pos.y
           << " " << (p.isConstruction ? 1 : 0)
           << " " << (p.fromText ? 1 : 0) << "\n";
    }

    // Lines.
    const auto& lines = sk.getLines();
    os << "LINE_COUNT " << lines.size() << "\n";
    for (const auto& l : lines) {
        os << "LINE " << l.id << " " << l.startPointId << " " << l.endPointId
           << " " << (l.isConstruction ? 1 : 0)
           << " " << (l.fromText ? 1 : 0) << "\n";
    }

    // Circles.
    const auto& circs = sk.getCircles();
    os << "CIRCLE_COUNT " << circs.size() << "\n";
    for (const auto& c : circs) {
        os << "CIRCLE " << c.id << " " << c.centerPointId << " " << c.radius
           << " " << (c.isConstruction ? 1 : 0) << "\n";
    }

    // Arcs.
    const auto& arcs = sk.getArcs();
    os << "ARC_COUNT " << arcs.size() << "\n";
    for (const auto& a : arcs) {
        os << "ARC " << a.id << " " << a.centerPointId << " " << a.startPointId
           << " " << a.endPointId << " " << a.radius
           << " " << (a.isConstruction ? 1 : 0) << "\n";
    }

    // Splines.
    const auto& splines = sk.getSplines();
    os << "SPLINE_COUNT " << splines.size() << "\n";
    for (const auto& sp : splines) {
        os << "SPLINE " << sp.id << " " << (sp.isConstruction ? 1 : 0)
           << " " << sp.controlPointIds.size();
        for (int id : sp.controlPointIds) os << " " << id;
        os << "\n";
    }

    // Polygons.
    const auto& polys = sk.getPolygons();
    os << "POLYGON_COUNT " << polys.size() << "\n";
    for (const auto& g : polys) {
        os << "POLYGON " << g.id << " " << g.centerPointId << " " << g.radius
           << " " << g.sides << " " << (g.isConstruction ? 1 : 0)
           << " " << g.vertexPointIds.size();
        for (int id : g.vertexPointIds) os << " " << id;
        os << " " << g.lineIds.size();
        for (int id : g.lineIds) os << " " << id;
        os << "\n";
    }

    // Constraints.
    const auto& cs = sk.getConstraints();
    os << "CONSTRAINT_COUNT " << cs.size() << "\n";
    for (const auto& c : cs) {
        os << "CONSTRAINT " << c.id << " " << static_cast<int>(c.type)
           << " " << c.entityA << " " << c.entityB
           << " " << c.value << " " << c.valueY
           << " " << c.labelOffX << " " << c.labelOffY
           << " " << (c.isDriving ? 1 : 0)
           << " " << c.orientX << " " << c.orientY << "\n";
    }

    os << "SKETCH_END\n";
}

std::string SketchEditOp::serializeWithDocument(const Document& doc) const {
    if (!m_target || !m_before || !m_after) return "";

    // The sketch this op edits — used as the rebind anchor at load time. Prefer
    // the live-pointer lookup, but fall back to the id cached at creation time
    // (setSketchId) if the target pointer no longer resolves in the document.
    // Without the fallback a stale/replaced m_target made this return "", which
    // saved the step with NO params — so it reloaded as a frozen ReplayOp and
    // raised the "frozen steps" warning for what is really a normal edit.
    int sketchId = doc.findSketchId(m_target.get());
    if (sketchId < 0) sketchId = m_sketchId;
    if (sketchId < 0) return ""; // truly unknown; nothing to bind on load

    std::string name = doc.getSketchName(sketchId);
    bool visible = doc.isSketchVisible(sketchId);
    // SourceBody travels with the snapshots themselves (each Sketch carries it
    // via getSourceBody()), but we also stash it on the SKETCH_START line so
    // the loader doesn't need a second hop through the live sketch.
    int sourceBody = m_target->getSourceBody();

    std::ostringstream os;
    writeSketchBody(os, *m_before, sketchId, name, visible, sourceBody);
    writeSketchBody(os, *m_after,  sketchId, name, visible, sourceBody);
    return os.str();
}

void SketchEditOp::getEditedElements(std::set<int>& lines, std::set<int>& circles,
                                     std::set<int>& arcs) const {
    lines.clear(); circles.clear(); arcs.clear();
    if (!m_after) return;
    auto wasPresent = [](const auto& vec, int id) {
        for (const auto& e : vec) if (e.id == id) return true;
        return false;
    };
    // No before-snapshot (e.g. a reloaded op) — highlight everything in after,
    // so the user still sees which sketch the step touches.
    const bool haveBefore = static_cast<bool>(m_before);
    for (const auto& l : m_after->getLines())
        if (!haveBefore || !wasPresent(m_before->getLines(), l.id)) lines.insert(l.id);
    for (const auto& c : m_after->getCircles())
        if (!haveBefore || !wasPresent(m_before->getCircles(), c.id)) circles.insert(c.id);
    for (const auto& a : m_after->getArcs())
        if (!haveBefore || !wasPresent(m_before->getArcs(), a.id)) arcs.insert(a.id);
}

void SketchEditOp::applyCircleRadiusToSnapshots(int circleId, double radius) {
    auto setIn = [&](const std::shared_ptr<Sketch>& s) {
        if (!s) return;
        for (const auto& c : s->getCircles())
            if (c.id == circleId) { s->setCircleRadius(circleId, radius); return; }
    };
    setIn(m_before);
    setIn(m_after);
}

void SketchEditOp::renderProperties() {
    if (!m_after) {
        ImGui::TextDisabled("%s", materializr::tr("No snapshot"));
        return;
    }
    // Edit dimensional values inline. For each change we re-solve `m_after`
    // so dependent geometry catches up — Apply Changes then copies the
    // solved snapshot onto the live sketch via editStep / execute().
    auto resolveAfter = [&]() {
        SketchSolver solver;
        solver.solve(*m_after);
    };

    bool anyDim = false;
    auto& cs = m_after->getMutableConstraints();
    for (size_t i = 0; i < cs.size(); ++i) {
        Constraint& c = cs[i];
        // Show only what THIS step introduced or changed. m_after is a FULL
        // snapshot, so iterating it raw re-lists every dimension the sketch has
        // ever gained — every later step then showed all four circle diameters
        // regardless of whether it drew a line, a rectangle, or removed
        // something. Same before/after delta the row description() already
        // uses: skip a constraint that existed UNCHANGED before this step.
        if (m_before) {
            const Constraint* prev = nullptr;
            for (const auto& b : m_before->getConstraints())
                if (b.id == c.id) { prev = &b; break; }
            if (prev && std::abs(prev->value - c.value) < 1e-9 &&
                        std::abs(prev->valueY - c.valueY) < 1e-9 &&
                        prev->isDriving == c.isDriving)
                continue;
        }
        ImGui::PushID(static_cast<int>(i));
        switch (c.type) {
            case ConstraintType::Distance: {
                anyDim = true;
                double v = c.value;
                if (materializr::lengthField(materializr::trFormat("Distance (%s)", materializr::unitSuffix()).c_str(), &v,
                                       ImGuiInputTextFlags_EnterReturnsTrue)) {
                    c.value = v;
                    resolveAfter();
                }
                break;
            }
            case ConstraintType::Radius: {
                anyDim = true;
                // Stored as radius; show as diameter to match the in-sketch
                // popup ("Ø ..." in descriptions and dimensions).
                double dia = c.value * 2.0;
                if (materializr::lengthField(materializr::trFormat("\xC3\x98 (%s)", materializr::unitSuffix()).c_str(), &dia,
                                       ImGuiInputTextFlags_EnterReturnsTrue)) {
                    c.value = std::max(dia, 1e-6) * 0.5;
                    resolveAfter();
                }
                break;
            }
            case ConstraintType::Angle: {
                anyDim = true;
                double deg = c.value * 180.0 / M_PI;
                if (materializr::inputNumber(materializr::tr("Angle (\xC2\xB0)"), &deg, 0.0, 0.0, "%.2f",
                                       ImGuiInputTextFlags_EnterReturnsTrue)) {
                    c.value = deg * M_PI / 180.0;
                    resolveAfter();
                }
                break;
            }
            default: {
                // Non-dimensional constraints have nothing to tune. Show the
                // name as a read-only row so the user can confirm what's in
                // the step, then move on.
                const char* name = constraintName(c.type);
                ImGui::TextDisabled("• %s", name);
                break;
            }
        }
        ImGui::PopID();
    }

    // Constraint-less geometry: let a newly-added circle's DIAMETER be edited
    // directly here (its centre stays put — the unambiguous case). Editing
    // writes straight to m_after's circle; Apply Changes (execute) copies the
    // resized sketch onto the live one. Lines/rectangles/arcs are intentionally
    // left out — which point/side stays fixed is ambiguous without constraints.
    if (m_before) {
        std::vector<int> newCircleIds;
        for (const auto& c : m_after->getCircles()) {
            bool wasThere = false;
            for (const auto& b : m_before->getCircles())
                if (b.id == c.id) { wasThere = true; break; }
            if (wasThere) continue;
            bool governed = false;   // a Radius constraint already edits it above
            for (const auto& cn : m_after->getConstraints())
                if (cn.type == ConstraintType::Radius && cn.entityA == c.id) {
                    governed = true; break;
                }
            if (!governed) newCircleIds.push_back(c.id);
        }
        for (int cid : newCircleIds) {
            double r = 0.0;
            for (const auto& c : m_after->getCircles())
                if (c.id == cid) { r = c.radius; break; }
            double dia = r * 2.0;
            ImGui::PushID(cid + 1000000);   // keep clear of the constraint-row ids
            if (materializr::lengthField(materializr::trFormat("Diameter (%s)", materializr::unitSuffix()).c_str(), &dia,
                                   ImGuiInputTextFlags_EnterReturnsTrue)) {
                // Writes the after-snapshot AND records the edit so Apply can
                // carry the new radius into later snapshots — otherwise the next
                // step's full snapshot overwrites it before any extrude reads it.
                editCircleRadius(cid, std::max(dia, 1e-6) * 0.5);
            }
            ImGui::PopID();
            anyDim = true;
        }
    }

    // NOTE: line / rectangle / arc *size* edits are intentionally NOT offered
    // here. They're edited live in the sketch's Properties panel (select the
    // element while in the sketch) — editing them through a history step meant
    // replaying full per-step snapshots, which clobbered edits to anything but
    // the latest step. The Properties-panel path mutates the live sketch
    // directly and is undoable, so that's the single home for resizing.

    if (!anyDim) {
        ImGui::TextWrapped("%s", materializr::tr("Nothing editable in this step. Resize lines, rectangles and arcs from the Properties panel while editing the sketch."));
    } else {
        ImGui::TextDisabled("%s", materializr::tr("Press Enter to commit a value, then Apply Changes."));
    }
}

} // namespace materializr
