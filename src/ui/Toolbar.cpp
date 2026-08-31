#include "UiTheme.h"
#include "Toolbar.h"
#include "TouchIcons.h"
#include "../core/SelectionManager.h"
#include "../core/History.h"
#include "../core/Operation.h"
#include "../plugin/PluginRegistry.h"
#include "../plugin/PluginContext.h"
#include "../plugin/Contributions.h"
#include <imgui.h>
#include <cmath>
#include <BRep_Tool.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <Geom_Surface.hxx>
#include <Geom_Plane.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <GeomAbs_CurveType.hxx>
#include <TopoDS.hxx>
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"

namespace materializr {

Toolbar::Toolbar() = default;

// Tooltip helper. Wraps long descriptions across multiple lines instead of
// the single-line behaviour ImGui::SetItemTooltip gives by default — tooltip
// strings can run to a couple of sentences and used to truncate awkwardly.
// BeginItemTooltip handles the hover-delay; PushTextWrapPos gives us the
// width cap (in pixels, roughly 28em at the current font size).
void Toolbar::tip(const char* text) const {
    if (!m_showTooltips) return;
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void Toolbar::setSelectionManager(const SelectionManager* sel) {
    m_selection = sel;
}

bool Toolbar::catalogOffers(ToolAction a) const {
    for (const auto& t : m_catalog)
        if (t.pluginIndex < 0 && t.action == a) return true;
    return false;
}

ToolAction Toolbar::renderCatalogRemainder(
        std::initializer_list<ToolAction> handled) {
    ToolAction action = ToolAction::None;
    for (const auto& t : m_catalog) {
        if (t.pluginIndex >= 0 || t.action == ToolAction::None) continue;
        bool done = false;
        for (ToolAction h : handled) if (h == t.action) { done = true; break; }
        if (done) continue;
        if (ImGui::Button(materializr::tr(t.label), ImVec2(-1, bh(30)))) action = t.action;
        if (t.tip) tip(materializr::tr(t.tip));
    }
    return action;
}

ToolAction Toolbar::render() {
    ToolAction action = ToolAction::None;

    // Snapshot the catalogue for this frame — see catalogOffers() in the
    // header for why classic reads it at all.
    m_catalog = railTools();

    ImGui::Begin("Tools", nullptr, ImGuiWindowFlags_NoCollapse);

    if (m_sketchMode) {
        action = renderSketchTools();
    } else if (!m_selection || !m_selection->hasSelection()) {
        action = renderNoSelectionTools();
    } else if (m_selection->hasSelectedSketchRegions()) {
        action = renderSketchRegionTools();
    } else if (m_selection->primaryType() == SelectionType::Plane) {
        action = renderPlaneSelectedTools();
    } else if (m_selection->primaryType() == SelectionType::Axis) {
        action = renderAxisSelectedTools();
    } else if (m_selection->hasSelectedSketches()) {
        action = renderSketchSelectedTools();
    } else if (m_selection->hasSelectedFaces()) {
        action = renderFaceTools();
        if (action == ToolAction::None) {
            // Body tools (gizmos + Mirror) stay available when a face is
            // selected so the user can move/rotate/scale the whole body, but
            // the whole-body plugin contributions (Split / Duplicate / Pattern)
            // are skipped — they don't apply in face-selection context.
            action = renderBodyTools(/*primaryContext=*/false);
        }
    } else if (m_selection->hasSelectedBodies()) {
        action = renderBodyTools();
    } else if (m_selection->hasSelectedEdges()) {
        action = renderEdgeTools();
    } else {
        action = renderNoSelectionTools();
    }

    ImGui::End();
    return action;
}

// THE tool catalogue: which tools the current selection context offers.
// All three layouts read this — the rails render it directly, and classic
// gates its own buttons on catalogOffers() (see Toolbar.h). Add a tool HERE
// and every layout gets it. The dispatch below mirrors render()'s.
std::vector<Toolbar::RailTool> Toolbar::railTools() const {
    std::vector<RailTool> t;
    auto add = [&](const char* icon, const char* label, ToolAction a,
                   bool active = false, const char* tip = nullptr) {
        t.push_back({icon, label, a, active, tip});
    };

    // Plugin toolbar contributions for the given context mask — the rail twin
    // of renderPluginButtons, so Boolean / Delete / Duplicate / Pattern / Loft
    // / Split / Construction reach every layout, and future plugins land here
    // automatically. Icons/short labels are matched by contribution name
    // (registry strings are long-lived, so the c_str()s stay valid).
    auto addPlugins = [&](int contextMask) {
        const auto& contribs = PluginRegistry::instance().toolbarContributions();
        for (size_t i = 0; i < contribs.size(); ++i) {
            const auto& c = contribs[i];
            if (!((1 << static_cast<int>(c.context)) & contextMask)) continue;
            // Base construction creation is covered by the shared Construct
            // menu ("New Plane/Axis…", renderConstructionMenuItems) which the
            // rail's Construct group and the im-touch + FAB both host — rail
            // buttons for them just duplicated the + menu (Steve).
            if (c.name == "Construction Plane" ||
                c.name == "Construction Axis") continue;
            const char* icon  = MZ_ICON_EDIT;
            const char* label = c.name.c_str();
            if      (c.name == "Union")              icon = MZ_ICON_UNION;
            else if (c.name == "Subtract")           icon = MZ_ICON_SUBTRACT;
            else if (c.name == "Intersect")          icon = MZ_ICON_INTERSECT;
            else if (c.name == "Delete")             icon = MZ_ICON_DELETE;
            else if (c.name == "Duplicate")          icon = MZ_ICON_COPY;
            else if (c.name == "Loft")               icon = MZ_ICON_SPLINE;
            else if (c.name == "Linear Pattern")   { icon = MZ_ICON_PATTERN_LINEAR;   label = "Linear"; }
            else if (c.name == "Circular Pattern") { icon = MZ_ICON_PATTERN_CIRCULAR; label = "Circular"; }
            else if (c.name.rfind("Split", 0) == 0)  icon = MZ_ICON_SPLIT;
            t.push_back({icon, label, ToolAction::None, false,
                         c.tooltip.empty() ? nullptr : c.tooltip.c_str(),
                         static_cast<int>(i)});
        }
    };

    if (m_sketchMode) {
        // SketchToolMode ints per setActiveSketchMode(): 1=Select … 8=Trim, 12=Dimension.
        add(MZ_ICON_SELECT,  "Select",  ToolAction::SelectSketch, m_activeSketchMode == 1,
            "Pick sketch elements (points, lines, regions). Drag a selection to move it.");
        add(MZ_ICON_LINE,    "Line",    ToolAction::Line,         m_activeSketchMode == 2,
            "Draw straight line segments. Tap to add vertices; Finish ends the chain.");
        add(MZ_ICON_CIRCLE,  "Circle",  ToolAction::Circle,       m_activeSketchMode == 3,
            "Draw a circle: press the centre, drag to the radius.");
        // Draw-origin toggle directly under the ACTIVE circle/rect tool, same
        // as the classic toolbar — label shows the current mode.
        if (m_activeSketchMode == 3)
            add(MZ_ICON_CIRCLE, m_circleMode == 0 ? "Center" : "2-Point",
                ToolAction::SketchToggleDrawOrigin, false,
                "Circle origin: Center = tap the centre then drag the radius; "
                "2-Point = the two taps are opposite ends of the diameter. "
                "Tap to toggle.");
        add(MZ_ICON_RECT,    "Rectangle", ToolAction::Rectangle,  m_activeSketchMode == 4,
            "Draw a rectangle: press one corner, drag to the opposite.");
        if (m_activeSketchMode == 4)
            add(MZ_ICON_RECT, m_rectMode == 0 ? "Corner" : "Center",
                ToolAction::SketchToggleDrawOrigin, false,
                "Rectangle origin: Corner = tap a corner, drag to the opposite; "
                "Center = tap the centre, drag to a corner. Tap to toggle.");
        add(MZ_ICON_ARC,     "Arc",     ToolAction::Arc,          m_activeSketchMode == 5,
            "Three-point arc: tap start, end, then a point on the curve.");
        add(MZ_ICON_SPLINE,  "Spline",  ToolAction::Spline,       m_activeSketchMode == 6,
            "Multi-point spline. Tap control points; Finish ends the curve.");
        add(MZ_ICON_POLYGON, "Polygon", ToolAction::Polygon,      m_activeSketchMode == 7,
            "Regular polygon: tap the centre, drag to size. Side count in properties.");
        add(MZ_ICON_TEXT,    "Text",    ToolAction::SketchText,   m_activeSketchMode == 9,
            "Insert text as real outline geometry: set string, font and letter "
            "height in the popup, then tap to place.");
        add(MZ_ICON_SVG,     "SVG",     ToolAction::SketchSvg,    m_activeSketchMode == 10,
            "Import an SVG file as sketch outlines: pick the file, set the "
            "width in the popup, tap to place.");
        add(MZ_ICON_SPLINE,  "Airfoil", ToolAction::SketchAirfoil, m_activeSketchMode == 13,
            "Import an aerofoil section (Selig or Lednicer coordinates from "
            "airfoiltools.com): pick the file, set the chord in the popup, tap "
            "to place the leading edge.");
        add(MZ_ICON_TRIM,    "Trim",    ToolAction::Trim,         m_activeSketchMode == 8,
            "Trim a sketch segment at its nearest intersections.");
        add(MZ_ICON_MEASURE, "Dimension", ToolAction::SketchDimension,
            m_activeSketchMode == 12,
            "Dimension: tap entities, tap to place the label, type the value.");
        // Sketch-element transforms — mirror the classic sketch toolbar so all
        // three layouts behave identically. Like classic, they're always here
        // in a sketch and simply no-op if nothing is selected. (The rail
        // renderer opens a sides popout for Polygon above; these fire directly.)
        add(MZ_ICON_COPY,    "Copy",     ToolAction::SketchCopy, false,
            "Duplicate the selected sketch elements at an offset.");
        add(MZ_ICON_MIRROR,  "Mirror",   ToolAction::SketchMirror, false,
            "Mirror selected elements across a sketch line you'll pick next.");
        add(MZ_ICON_OFFSET,  "Offset",   ToolAction::SketchOffset, false,
            "Offset a selected closed loop: drag to set the side and distance.");
        add(MZ_ICON_PATTERN_LINEAR,   "Linear",   ToolAction::SketchLinearPattern, false,
            "Linear pattern: copy the selected sketch elements N times along the sketch X axis.");
        add(MZ_ICON_PATTERN_CIRCULAR, "Circular", ToolAction::SketchRadialPattern, false,
            "Circular pattern: copy the selected sketch elements around an origin you specify.");
        // Live inference-level cycle (Full → Reduced → Off → Max) — the label
        // shows the CURRENT level. Same Settings gate as the classic toolbar.
        if (m_showInferenceToggle) {
            const char* lvl = m_inferenceLevel == 0 ? "Full"
                            : m_inferenceLevel == 1 ? "Reduced"
                            : m_inferenceLevel == 2 ? "Off" : "Max";
            add(MZ_ICON_GUIDES, lvl, ToolAction::SketchCycleInference, false,
                "How many drawing guides to show, and how eagerly they snap. "
                "Max = Full plus wider catch ranges tuned for fingertips. "
                "Full = classic guides plus hover-to-charge references. "
                "Reduced = classic guides only. Off = grid + endpoint only. "
                "Tap to cycle.");
        }
        add(MZ_ICON_MEASURE, "Measure", ToolAction::Measure, false,
            "Measure distance / length between picked sketch elements.");
        if (!m_cameraOrtho)
            add(MZ_ICON_LOOK, "Look At", ToolAction::LookAtSketch, false,
                "Snap the camera to look straight down the sketch plane "
                "(orthographic).");
        addPlugins(1 << static_cast<int>(SelectionContext::InSketchMode));
    } else if (!m_selection || !m_selection->hasSelection()) {
        // No bare "Sketch" here: with nothing selected it just duplicated
        // "Sketch on… > XY plane". Sketch appears once a face or construction
        // plane is picked (the branches below); world-plane starts live in
        // the rail's "Sketch on…" group.
        add(MZ_ICON_MEASURE, "Measure", ToolAction::Measure, false,
            "Measure distance, length, or angle between picked features.");
        addPlugins((1 << static_cast<int>(SelectionContext::NoSelection)) |
                   (1 << static_cast<int>(SelectionContext::Always)));
    } else if (m_selection->hasSelectedSketchRegions()) {
        // BOTH, always. Attachment picks the ORDER, not the menu: a region
        // whose sketch still drives a body leads with Push (modify in place),
        // a standalone one leads with Extrude (new body). It used to be
        // either/or, which hid Extrude from every sketch drawn on a face —
        // exactly when "make this a separate body instead of fusing it" is
        // wanted (Steve, 2026-08-05). The face branch below and the classic
        // layout already offered both; only this rail didn't.
        if (m_selSketchAttached) {
            add(MZ_ICON_PUSHPULL, "Push",     ToolAction::PushPull, false,
                "Push/pull the region into or out of the host body.");
            add(MZ_ICON_EXTRUDE,  "Extrude",  ToolAction::ExtrudeSketch, false,
                "Extrude the region into a NEW body, leaving the host "
                "body unchanged.");
        } else {
            add(MZ_ICON_EXTRUDE,  "Extrude",  ToolAction::ExtrudeSketch, false,
                "Extrude the region into a new solid body.");
            add(MZ_ICON_PUSHPULL, "Push",     ToolAction::PushPull, false,
                "Push/pull the region into or out of the body beneath it.");
        }
        add(MZ_ICON_SUBTRACT, "Subtract", ToolAction::SubtractSketch, false,
            "Extrude the region and cut it out of the body it runs into.");
        add(MZ_ICON_EDIT,     "Edit",     ToolAction::EditSketch, false,
            "Reopen the sketch this region belongs to.");
        add(MZ_ICON_MOVE,     "Move",     ToolAction::Move, false,
            "Show the translate gizmo: drag axes or planes to move.");
        add(MZ_ICON_ROTATE,   "Rotate",   ToolAction::Rotate, false,
            "Show the rotate gizmo: drag rings to rotate around each axis.");
        addPlugins(1 << static_cast<int>(SelectionContext::HasSketchRegions));
    } else if (m_selection->primaryType() == SelectionType::Plane) {
        add(MZ_ICON_SKETCH, "Sketch", ToolAction::SketchOnFace, false,
            "Start a sketch on the selected construction plane.");
        add(MZ_ICON_MOVE,   "Move",   ToolAction::Move, false,
            "Move the construction plane along its normal.");
        add(MZ_ICON_ROTATE, "Rotate", ToolAction::Rotate, false,
            "Rotate the construction plane.");
    } else if (m_selection->primaryType() == SelectionType::Axis) {
        add(MZ_ICON_MOVE, "Move", ToolAction::Move, false,
            "Move the construction axis.");
    } else if (m_selection->hasSelectedSketches()) {
        add(MZ_ICON_EDIT,     "Edit",     ToolAction::EditSketch, false,
            "Reopen the selected sketch for editing.");
        // Both, ordered by attachment — mirrors the region case above.
        if (m_selSketchAttached) {
            add(MZ_ICON_PUSHPULL, "Push",    ToolAction::PushPull, false,
                "Push/pull the sketch's regions into or out of the host body.");
            add(MZ_ICON_EXTRUDE,  "Extrude", ToolAction::ExtrudeSketch, false,
                "Extrude the sketch's regions into a NEW body, leaving the "
                "host body unchanged.");
        } else {
            add(MZ_ICON_EXTRUDE,  "Extrude", ToolAction::ExtrudeSketch, false,
                "Extrude the sketch's regions into a solid body.");
            add(MZ_ICON_PUSHPULL, "Push",    ToolAction::PushPull, false,
                "Push/pull the sketch's regions into or out of the body "
                "beneath them.");
        }
        add(MZ_ICON_SUBTRACT, "Subtract", ToolAction::SubtractSketch, false,
            "Extrude the sketch's regions and cut them out of the body they run into.");
        add(MZ_ICON_LATHE,    "Lathe",    ToolAction::Revolve, false,
            "Spin the sketch profile around an axis into a solid.");
        add(MZ_ICON_MOVE,     "Move",     ToolAction::Move, false,
            "Show the translate gizmo: drag axes or planes to move.");
        add(MZ_ICON_ROTATE,   "Rotate",   ToolAction::Rotate, false,
            "Show the rotate gizmo: drag rings to rotate around each axis.");
        addPlugins(1 << static_cast<int>(SelectionContext::HasSketches));
    } else if (m_selection->hasSelectedFaces()) {
        add(MZ_ICON_SKETCH,   "Sketch",  ToolAction::SketchOnFace, false,
            "Start a sketch on the selected face.");
        // These only make sense on a FLAT face: Push/Pull and Extrude freak the
        // boolean out on a curved/fillet face, and Shell is a no-op there. Hide
        // them (like Diameter only on rounds) rather than fail on use. #28
        if (m_selFacePlanar) {
            add(MZ_ICON_PUSHPULL, "Push",    ToolAction::PushPull, false,
                "Push/pull the face along its normal.");
            add(MZ_ICON_EXTRUDE,  "Extrude", ToolAction::ExtrudeSketch, false,
                "Extrude the face into new material.");
            add(MZ_ICON_SHELL,    "Shell",   ToolAction::Shell, false,
                "Hollow the body, leaving this face open.");
            // THE face-scale tool. There used to be a second one — the
            // Transform "Scale" button, which ran MoveFaceOp::Scale — and the
            // two were not merely similar: measured on a 20mm box scaled to
            // 50%, both return the identical 4666.667 frustum, because Scale
            // is exactly this op with the blend length pinned to the full
            // depth (which is already the default here). One button now; see
            // probe_scale_vs_scaleface.
            add(MZ_ICON_SCALE, "Scale Face", ToolAction::ScaleFace, false,
                "Scale this face; the side walls re-slope to follow. Under 100% "
                "tapers it in, over 100% flares it out. Shorten the length to "
                "blend near the face instead of from the base.");
        }
        // Taper takes cylindrical and conical faces too (it drafts along their
        // own axis), so it is NOT gated on the face being flat.
        add(MZ_ICON_SPLIT, "Draft", ToolAction::Taper, false,
            "Draft angle for moulding or printing: tilt the selected face(s) "
            "by an angle about a fixed neutral plane. Unlike Rotate it takes "
            "SEVERAL faces at one angle (all four walls of a box) and works on "
            "curved faces too \xE2\x80\x94 a cylinder drafts into a cone.");
        add(MZ_ICON_REPAIR,   "Repair",  ToolAction::RemoveFace, false,
            "Delete the face and heal the body over it.");
        add(MZ_ICON_PROJECT,  "Project", ToolAction::ProjectSketch, false,
            "Project a sketch onto this face along the sketch's normal, then "
            "engrave (cut in) or emboss (raise out) to a depth.");
        if (m_canEditDiameter) {
            add(MZ_ICON_CIRCLE, "Diameter", ToolAction::EditDiameter, false,
                "Set the hole or boss to an exact diameter.");
            add(MZ_ICON_THREAD, "Thread", ToolAction::Thread, false,
                "Cut a helical screw thread into the picked cylindrical face — "
                "external on a boss, internal in a hole.");
        }
        // "Edit Fillet / Chamfer" when the picked face was produced by one —
        // same ownsFace() probe as the classic Face Operations section.
        if (m_selection && m_history) {
            TopoDS_Shape pickedFace;
            for (const auto& e : m_selection->getSelection())
                if (e.type == SelectionType::Face && !e.shape.IsNull()) {
                    pickedFace = e.shape; break;
                }
            if (!pickedFace.IsNull()) {
                // Pick the op that BEST owns the face — highest ownsFaceScore
                // (exact IsSame beats the geometric fallback), latest on ties.
                // The old first-match loop let an earlier fuzzy over-match (a
                // big fillet) win over the actual chamfer (#49).
                const Operation* best = nullptr; int bestScore = 0;
                for (const auto& op : m_history->operations()) {
                    if (!op || !op->isEnabled()) continue;
                    if (op->kind() != Operation::Kind::Fillet && op->kind() != Operation::Kind::Chamfer) continue;
                    int sc = op->ownsFaceScore(pickedFace);
                    if (sc > 0 && sc >= bestScore) { bestScore = sc; best = op.get(); }
                }
                if (best && best->kind() == Operation::Kind::Fillet)
                    add(MZ_ICON_FILLET, "Edit Fillet",
                        ToolAction::EditFilletChamfer, false,
                        "Change this fillet's radius without re-picking edges.");
                else if (best && best->kind() == Operation::Kind::Chamfer)
                    add(MZ_ICON_CHAMFER, "Edit Chamfer",
                        ToolAction::EditFilletChamfer, false,
                        "Change this chamfer's distance without re-picking edges.");
            }
        }
        // Move/Rotate/Scale transform a flat face (its feature follows); on a
        // curved/fillet face they freak out or do nothing — hide them. #28
        if (m_selFacePlanar) {
            add(MZ_ICON_MOVE,   "Move",   ToolAction::Move, false,
                "Move the face (its feature follows).");
            add(MZ_ICON_ROTATE, "Rotate", ToolAction::Rotate, false,
                "Tilt the face — or twist it with the ring about its normal.");
            // No "Scale" here: it did the same thing as Scale Face above.
        } else if (m_selFaceIsHoleWall) {
            // The one curved face #28 shouldn't hide Move on: a hole's bore.
            // Clicking the inside wall means the whole hole (both rims travel
            // together); grabbing a rim EDGE is how you move just one side.
            add(MZ_ICON_MOVE,   "Move",   ToolAction::Move, false,
                "Slide the whole hole across the face it pierces \xE2\x80\x94 "
                "select a rim edge instead to tilt or reshape one end.");
        }
        // Two or more picked faces is the user asserting "these are one face",
        // which is what lets this merge try harder than the whole-body one.
        if (m_selection->selectedFaceCount() >= 2)
            add(MZ_ICON_REPAIR, "Merge Faces", ToolAction::MergeFaces, false,
                "Merge the selected faces into one. Use this on the seam lines "
                "across a flat face that an imported STEP part arrives with \xE2\x80\x94 "
                "they also confuse Unfold and sketch-on-face. Refuses if the "
                "faces aren't really one surface.");
        add(MZ_ICON_UNFOLD, "Unfold", ToolAction::Unfold, false,
            "Flatten the selected faces into a 2D cut pattern (SVG / tiled PDF).");
        addPlugins(1 << static_cast<int>(SelectionContext::HasFaces));
    } else if (m_selection->hasSelectedBodies()) {
        add(MZ_ICON_MOVE,   "Move",   ToolAction::Move, false,
            "Show the translate gizmo: drag axes or planes to move.");
        add(MZ_ICON_ROTATE, "Rotate", ToolAction::Rotate, false,
            "Show the rotate gizmo: drag rings to rotate around each axis.");
        add(MZ_ICON_SCALE,  "Scale",  ToolAction::Scale, false,
            "Scale the body: uniform, or per-axis in properties.");
        add(MZ_ICON_MIRROR, "Mirror", ToolAction::Mirror, false,
            "Mirror the body across a plane you'll place next.");
        add(MZ_ICON_SPLIT, "Split", ToolAction::Split, false,
            "Cut the body in two. Pick the axis and slide the cut off centre — "
            "a ghost plane shows where it lands before you commit.");
        add(MZ_ICON_LATHE,  "Revolve", ToolAction::Revolve, false,
            "Rotate the body around an axis (watch a fan spin or a hinge open).");
        if (m_selection->selectedBodyCount() == 1)
            add(MZ_ICON_UNFOLD, "Unfold", ToolAction::Unfold, false,
                "Flatten the body into a 2D cut pattern (SVG / tiled PDF).");
        add(MZ_ICON_REPAIR, "Merge Faces", ToolAction::MergeFaces, false,
            "Sweep the body for faces that are exactly coplanar and merge them. "
            "Imported STEP parts arrive with flat surfaces split into pieces "
            "\xE2\x80\x94 the seam lines across an otherwise flat face, which also "
            "confuse Unfold and sketch-on-face. For a seam this leaves behind, "
            "select the two faces either side of it and merge those instead.");
        add(MZ_ICON_MEASURE, "Measure", ToolAction::Measure, false,
            "Measure distance, length, or angle between picked features.");
        // Same gating as the classic body section: MultipleBodies plugins
        // (Union / Subtract / Intersect) only once 2+ bodies are picked.
        {
            int mask = 1 << static_cast<int>(SelectionContext::HasBodies);
            if (m_selection->selectedBodyCount() >= 2)
                mask |= 1 << static_cast<int>(SelectionContext::MultipleBodies);
            addPlugins(mask);
        }
    } else if (m_selection->hasSelectedEdges()) {
        add(MZ_ICON_FILLET,  "Fillet",  ToolAction::Fillet, false,
            "Round the selected edges with a radius.");
        add(MZ_ICON_CHAMFER, "Chamfer", ToolAction::Chamfer, false,
            "Cut the selected edges to a flat bevel.");
        // Move on an EDGE selection means the hole that rim belongs to. #28
        // hides Move on curved FACES, which is why a round hole's wall isn't
        // clickable — but its rim is a single circular EDGE, so selecting that
        // is unambiguous. Only offered when the edges resolve to exactly one
        // hole; an ordinary edge gets nothing rather than a surprise body move.
        // Sits UNDER Fillet/Chamfer (Steve, 2026-08-03): those two are what an
        // edge selection is usually for, and they're always present, so a
        // conditional button above them made the pair jump position.
        if (m_selEdgeIsHoleRim)
            add(MZ_ICON_MOVE, "Move", ToolAction::Move, false,
                "Move this hole: drag one rim to tilt the bore, one straight "
                "side to reshape it, or select both rims to slide the whole "
                "hole.");
        if (m_canEditDiameter)
            add(MZ_ICON_CIRCLE, "Diameter", ToolAction::EditDiameter, false,
                "Set the hole or boss to an exact diameter.");
        addPlugins(1 << static_cast<int>(SelectionContext::HasEdges));
    } else {
        // Fallback (vertex or other selection): same rule as no-selection —
        // no bare "Sketch" (it duplicated Sketch on… > world plane).
        add(MZ_ICON_MEASURE, "Measure", ToolAction::Measure, false,
            "Measure distance, length, or angle between picked features.");
        addPlugins((1 << static_cast<int>(SelectionContext::NoSelection)) |
                   (1 << static_cast<int>(SelectionContext::Always)));
    }
    return t;
}

void Toolbar::fireRailPlugin(int index) {
    if (!m_pluginCtx) return;
    auto& contribs = PluginRegistry::instance().toolbarContributions();
    if (index < 0 || index >= static_cast<int>(contribs.size())) return;
    auto& c = contribs[index];
    if (c.toolFactory)
        PluginRegistry::instance().activateTool(c.toolFactory(), *m_pluginCtx);
    else if (c.action)
        c.action(*m_pluginCtx);
}

void Toolbar::setSketchMode(bool active) {
    m_sketchMode = active;
}

bool Toolbar::isSketchMode() const {
    return m_sketchMode;
}

// Render plugin-contributed buttons matching any of the given contexts.
// contextMask is a bitmask: bit N = SelectionContext(N).
void Toolbar::renderPluginButtons(int contextMask) {
    if (!m_pluginCtx) return;
    auto& contribs = PluginRegistry::instance().toolbarContributions();
    std::string lastSection;
    for (size_t i = 0; i < contribs.size(); ++i) {
        auto& c = contribs[i];
        if (!((1 << static_cast<int>(c.context)) & contextMask)) continue;
        if (c.section != lastSection) {
            if (!lastSection.empty()) ImGui::Separator();
            ImGui::TextColored(materializr::accentText(), "%s", c.section.c_str());
            ImGui::Separator();
            lastSection = c.section;
        }
        ImGui::PushID(static_cast<int>(i + 10000));
        if (ImGui::Button(materializr::tr(c.name.c_str()), ImVec2(-1, bh(30)))) {
            if (c.toolFactory) {
                PluginRegistry::instance().activateTool(c.toolFactory(), *m_pluginCtx);
            } else if (c.action) {
                c.action(*m_pluginCtx);
            }
        }
        if (!c.tooltip.empty()) tip(materializr::tr(c.tooltip.c_str()));
        ImGui::PopID();
    }
}

void Toolbar::renderPrimitivesMenu() {
    if (!m_pluginCtx) return;
    if (ImGui::Button(materializr::tr("Primitives..."), ImVec2(-1, bh(30))))
        ImGui::OpenPopup("PrimitivesMenu");
    tip(materializr::tr("Create a stock OCCT primitive (box / cylinder / sphere / cone / torus). Picking one opens its parameter popup — defaults land a 10 mm / R5 mm shape at the world origin."));
    if (ImGui::BeginPopup("PrimitivesMenu")) {
        if (ImGui::MenuItem(materializr::tr("Box")))
            m_pluginCtx->requestInteractiveOp(InteractiveOp::PrimitiveBox);
        if (ImGui::MenuItem(materializr::tr("Cylinder")))
            m_pluginCtx->requestInteractiveOp(InteractiveOp::PrimitiveCylinder);
        if (ImGui::MenuItem(materializr::tr("Sphere")))
            m_pluginCtx->requestInteractiveOp(InteractiveOp::PrimitiveSphere);
        if (ImGui::MenuItem(materializr::tr("Cone")))
            m_pluginCtx->requestInteractiveOp(InteractiveOp::PrimitiveCone);
        if (ImGui::MenuItem(materializr::tr("Torus")))
            m_pluginCtx->requestInteractiveOp(InteractiveOp::PrimitiveTorus);
        ImGui::EndPopup();
    }
}

void Toolbar::renderAddPlaneMenu() {
    if (!m_selection || !m_pluginCtx) return;

    // Detect which construction-plane modes the current selection supports.
    int  planarFaces = 0, planeCount = 0;
    bool haveCyl = false, straightEdge = false, haveAxis = false;
    for (const auto& e : m_selection->getSelection()) {
        if (e.type == SelectionType::Plane) { ++planeCount; continue; }
        if (e.type == SelectionType::Axis)  { haveAxis = true; continue; }
        if (e.shape.IsNull()) continue;
        try {
            if (e.type == SelectionType::Face) {
                Handle(Geom_Surface) s = BRep_Tool::Surface(TopoDS::Face(e.shape));
                if (!s.IsNull()) {
                    if (s->IsKind(STANDARD_TYPE(Geom_Plane))) ++planarFaces;
                    else if (!Handle(Geom_CylindricalSurface)::DownCast(s).IsNull())
                        haveCyl = true;
                }
            } else if (e.type == SelectionType::Edge) {
                BRepAdaptor_Curve ad(TopoDS::Edge(e.shape));
                if (ad.GetType() == GeomAbs_Line) straightEdge = true;
            }
        } catch (...) {}
    }

    const bool midplane = (planarFaces >= 2) || (planeCount >= 2);
    if (!(midplane || haveCyl || haveAxis || straightEdge)) return;

    ImGui::Separator();
    if (ImGui::Button(materializr::tr("Add Plane..."), ImVec2(-1, bh(30))))
        ImGui::OpenPopup("AddPlaneMenu");
    tip(materializr::tr("Create a construction plane derived from the current selection."));
    if (ImGui::BeginPopup("AddPlaneMenu")) {
        if (midplane && ImGui::MenuItem(materializr::tr("Midplane (between the 2 selected)")))
            m_pluginCtx->requestInteractiveOp(InteractiveOp::Midplane);
        if (haveCyl) {
            if (ImGui::MenuItem(materializr::tr("Tangent to cylinder")))
                m_pluginCtx->requestInteractiveOp(InteractiveOp::TangentPlane);
            if (ImGui::MenuItem(materializr::tr("Perpendicular to cylinder axis")))
                m_pluginCtx->requestInteractiveOp(InteractiveOp::PlaneNormalToAxis);
            if (ImGui::MenuItem(materializr::tr("Through cylinder axis (longitudinal)")))
                m_pluginCtx->requestInteractiveOp(InteractiveOp::PlaneThroughAxis);
        } else if (haveAxis || straightEdge) {
            if (ImGui::MenuItem(straightEdge ? "Normal to edge" : "Normal to axis"))
                m_pluginCtx->requestInteractiveOp(InteractiveOp::PlaneNormalToAxis);
        }
        ImGui::EndPopup();
    }
}

void Toolbar::renderAddAxisMenu() {
    if (!m_selection || !m_pluginCtx) return;

    int  planarFaces = 0, planeCount = 0, vertexCount = 0;
    bool haveCyl = false, straightEdge = false;
    for (const auto& e : m_selection->getSelection()) {
        if (e.type == SelectionType::Plane)  { ++planeCount;  continue; }
        if (e.type == SelectionType::Vertex) { ++vertexCount; continue; }
        if (e.shape.IsNull()) continue;
        try {
            if (e.type == SelectionType::Face) {
                Handle(Geom_Surface) s = BRep_Tool::Surface(TopoDS::Face(e.shape));
                if (!s.IsNull()) {
                    if (s->IsKind(STANDARD_TYPE(Geom_Plane))) ++planarFaces;
                    else if (!Handle(Geom_CylindricalSurface)::DownCast(s).IsNull())
                        haveCyl = true;
                }
            } else if (e.type == SelectionType::Edge) {
                BRepAdaptor_Curve ad(TopoDS::Edge(e.shape));
                if (ad.GetType() == GeomAbs_Line) straightEdge = true;
            }
        } catch (...) {}
    }

    const bool twoPlanes  = (planeCount >= 2) || (planarFaces >= 2);
    const bool twoVerts   = (vertexCount >= 2);
    const bool faceNormal = (planarFaces >= 1);
    if (!(haveCyl || straightEdge || twoVerts || faceNormal || twoPlanes)) return;

    ImGui::Separator();
    if (ImGui::Button(materializr::tr("Add Axis..."), ImVec2(-1, bh(30))))
        ImGui::OpenPopup("AddAxisMenu");
    tip(materializr::tr("Create a construction axis derived from the current selection."));
    if (ImGui::BeginPopup("AddAxisMenu")) {
        if (haveCyl && ImGui::MenuItem(materializr::tr("From cylinder axis")))
            m_pluginCtx->requestInteractiveOp(InteractiveOp::AxisFromCylinder);
        if (straightEdge && ImGui::MenuItem(materializr::tr("Along edge")))
            m_pluginCtx->requestInteractiveOp(InteractiveOp::AxisAlongEdge);
        if (twoVerts && ImGui::MenuItem(materializr::tr("Through two vertices")))
            m_pluginCtx->requestInteractiveOp(InteractiveOp::AxisTwoPoints);
        if (faceNormal && ImGui::MenuItem(materializr::tr("Normal to face")))
            m_pluginCtx->requestInteractiveOp(InteractiveOp::AxisNormalToFace);
        if (twoPlanes && ImGui::MenuItem(materializr::tr("Intersection of two planes")))
            m_pluginCtx->requestInteractiveOp(InteractiveOp::AxisTwoPlanes);
        ImGui::EndPopup();
    }
}

ToolAction Toolbar::renderSketchTools() {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Sketch Tools"));
    // Constraint status badge — only appears once the sketch has constraints.
    // Green = Fully constrained, blue = Under (free DOF), red = Over
    // (contradictory). Hover shows the precise degree-of-freedom count.
    if (m_sketchSolverState >= 0) {
        ImVec4 col;
        const char* label = "";
        switch (m_sketchSolverState) {
            case 0: col = ImVec4(0.20f, 0.85f, 0.35f, 1.0f); label = "Fully constrained"; break;
            case 1: col = ImVec4(0.30f, 0.65f, 1.00f, 1.0f); label = "Under-constrained"; break;
            case 2: col = ImVec4(0.95f, 0.30f, 0.30f, 1.0f); label = "Over-constrained";   break;
            default: col = ImVec4(0.7f,0.7f,0.7f,1.0f);      label = "";                    break;
        }
        ImGui::TextColored(col, "● %s", label);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(materializr::tr("Degrees of freedom: %d\nNegative = contradictory constraints, zero = uniquely determined, positive = free to drag."),
                              m_sketchSolverDof);
        }
    }
    ImGui::Separator();

    // Snap on/off + step both live in the corner widget next to the ViewCube
    // now — single source of truth. The duplicate grid-step row used to sit
    // here but was removed once the corner widget proved sufficient.

    // Render a sketch-tool button with a thick light-grey border when it's
    // the currently active mode. Caller checks the return value to set
    // `action`. Mode id matches SketchToolMode enum (see Toolbar.h).
    auto skBtn = [&](const char* label, int modeId) -> bool {
        bool active = (m_activeSketchMode == modeId);
        if (active) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        }
        bool clicked = ImGui::Button(label, ImVec2(-1, bh(30)));
        if (active) {
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }
        return clicked;
    };

    // Draw-origin toggle, rendered directly beneath whichever of Circle /
    // Rectangle is active so it reads as part of that tool (not stranded at the
    // bottom of the list).
    auto drawOriginToggle = [&](bool isRect) {
        const char* label = isRect
            ? (m_rectMode == 0 ? "Draw from: Corner" : "Draw from: Center")
            : (m_circleMode == 0 ? "Draw from: Center" : "Draw from: 2-Point");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.38f, 0.55f, 1.0f));
        if (ImGui::Button(label, ImVec2(-1, bh(22))))
            action = ToolAction::SketchToggleDrawOrigin;
        ImGui::PopStyleColor();
        tip(isRect
            ? "Rectangle origin: Corner = click a corner, drag to the opposite; "
              "Center = click the centre, drag to a corner. Click to toggle."
            : "Circle origin: Center = click the centre, drag the radius; "
              "2-Point = the two clicks are opposite ends of the diameter "
              "(rim passes through the first click). Click to toggle.");
    };

    if (skBtn("Select / Move", 1)) action = ToolAction::SelectSketch;
    tip(materializr::tr("Pick sketch elements (points, lines, regions). Drag selection to move."));
    if (skBtn("Line",      2))     action = ToolAction::Line;
    tip(materializr::tr("Draw straight line segments. Click to add vertices, Esc to finish."));
    if (skBtn("Circle",    3))     action = ToolAction::Circle;
    tip(materializr::tr("Draw a circle: click centre, drag to radius."));
    if (m_activeSketchMode == 3)   drawOriginToggle(false);
    if (skBtn("Rectangle", 4))     action = ToolAction::Rectangle;
    tip(materializr::tr("Draw an axis-aligned rectangle: click one corner, drag to the opposite."));
    if (m_activeSketchMode == 4)   drawOriginToggle(true);
    if (skBtn("Arc",       5))     action = ToolAction::Arc;
    tip(materializr::tr("Three-point arc: click start, end, then a point on the curve."));
    if (skBtn("Spline",    6))     action = ToolAction::Spline;
    tip(materializr::tr("Multi-point spline. Click control points, Enter to finish."));
    // Polygon: a popout to pick the regular-polygon side count by name (like
    // the Primitives menu), instead of typing it into a dialog. The chosen
    // count sets the tool's sides and activates polygon placement.
    {
        bool active = (m_activeSketchMode == 7);
        if (active) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        }
        if (ImGui::Button(materializr::tr("Polygon"), ImVec2(-1, bh(30))))
            ImGui::OpenPopup("PolygonSidesMenu");
        if (active) { ImGui::PopStyleColor(); ImGui::PopStyleVar(); }
        tip(materializr::tr("Regular polygon: pick the number of sides, then click the centre and drag for size / rotation."));
        if (ImGui::BeginPopup("PolygonSidesMenu")) {
            struct PolyChoice { const char* name; int sides; };
            static const PolyChoice choices[] = {
                {"Triangle (3)", 3}, {"Square (4)", 4}, {"Pentagon (5)", 5},
                {"Hexagon (6)", 6}, {"Heptagon (7)", 7}, {"Octagon (8)", 8}};
            for (const auto& ch : choices)
                if (ImGui::MenuItem(ch.name)) {
                    m_requestedPolygonSides = ch.sides;
                    action = ToolAction::Polygon;
                }
            ImGui::EndPopup();
        }
    }
    tip(materializr::tr("Regular polygon: click centre, drag to size. Side count in properties."));
    if (skBtn("Text",      9))     action = ToolAction::SketchText;
    tip(materializr::tr("Insert text as real outline geometry: set string, font and letter height in the popup, then click to place. Letters become closed regions - extrude them or engrave them onto a face."));
    if (skBtn("Import SVG", 10))   action = ToolAction::SketchSvg;
    tip(materializr::tr("Import an SVG file as sketch outlines: pick the file, set the width in the popup, click to place. Paths become closed regions - extrude a logo or engrave it onto a face."));
    if (skBtn("Airfoil",   13))    action = ToolAction::SketchAirfoil;
    tip(materializr::tr("Import an aerofoil section (Selig or Lednicer coordinates, e.g. from airfoiltools.com): pick the file, set the chord in the popup, click to place the leading edge. Stack sections on planes and Loft for a wing."));
    if (skBtn("Trim",      8))     action = ToolAction::Trim;
    tip(materializr::tr("Trim a sketch segment at the nearest intersections."));
    if (skBtn("Dimension", 12))    action = ToolAction::SketchDimension;
    tip(materializr::tr("Dimension tool (D): click a line, circle, point pair, or two lines, place the label, then type the value."));

    // Transforms operate on the current sketch-element selection (Select tool +
    // click/Ctrl+click on points and lines). No-op if nothing is selected.
    // Rotate is the gizmo's ring handle (drag = 15° snap, popup for exact value),
    // so it doesn't get its own button.
    ImGui::Separator();
    if (ImGui::Button(materializr::tr("Copy"),   ImVec2(-1, bh(28)))) action = ToolAction::SketchCopy;
    tip(materializr::tr("Duplicate the selected sketch elements at an offset."));
    if (ImGui::Button(materializr::tr("Mirror"), ImVec2(-1, bh(28)))) action = ToolAction::SketchMirror;
    tip(materializr::tr("Mirror selected elements across a sketch line you'll pick next."));
    if (ImGui::Button(materializr::tr("Offset"), ImVec2(-1, bh(28)))) action = ToolAction::SketchOffset;
    tip(materializr::tr("Offset a selected closed loop: drag to set the side and distance."));
    if (ImGui::Button(materializr::tr("Linear Pattern"), ImVec2(-1, bh(28)))) action = ToolAction::SketchLinearPattern;
    tip(materializr::tr("Copy the selected sketch elements N times along the sketch X axis."));
    if (ImGui::Button(materializr::tr("Circular Pattern"), ImVec2(-1, bh(28)))) action = ToolAction::SketchRadialPattern;
    tip(materializr::tr("Copy the selected sketch elements around an origin you specify."));

    // Drawing-inference level — a live Full → Reduced → Off toggle. Lets the
    // user calm the ghost guides (and the hover-charged references) in a busy
    // area without leaving the sketch. Constraints now live exclusively on the
    // sketch-viewport right-click "Add Constraint" menu. Hidden when the user
    // has set the level once in Settings and prefers not to see the live
    // toggle (Settings → Sketch → "Show level toggle in sketch toolbar").
    if (m_showInferenceToggle) {
        ImGui::Separator();
        const char* lbl = m_inferenceLevel == 0 ? "Inferences: Full"
                        : m_inferenceLevel == 1 ? "Inferences: Reduced"
                        : m_inferenceLevel == 2 ? "Inferences: Off"
                                                : "Inferences: Max";
        ImVec4 col = m_inferenceLevel == 0 ? ImVec4(0.20f, 0.45f, 0.65f, 1.0f)
                   : m_inferenceLevel == 1 ? ImVec4(0.60f, 0.42f, 0.15f, 1.0f)
                   : m_inferenceLevel == 2 ? ImVec4(0.34f, 0.34f, 0.34f, 1.0f)
                                           : ImVec4(0.16f, 0.52f, 0.48f, 1.0f); // Max = teal (strongest)
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        if (ImGui::Button(lbl, ImVec2(-1, bh(26)))) action = ToolAction::SketchCycleInference;
        ImGui::PopStyleColor();
        tip(materializr::tr("How many drawing guides to show, and how eagerly they snap. Max = Full plus wider catch ranges tuned for fingertips (touch). Full = the classic guides PLUS hover-to-charge references (dwell on a point to align from it). Reduced = the classic guides only, no hover-charging. Off = grid + endpoint only. Click to cycle."));
    }

    // Measure moved to the View menu (unified across layouts).

    if (!m_cameraOrtho) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.85f, 1.0f));
        if (ImGui::Button(materializr::tr("Look at Sketch"), ImVec2(-1, bh(30))))
            action = ToolAction::LookAtSketch;
        tip(materializr::tr("Snap the camera to look straight down the sketch plane (orthographic)."));
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    if (ImGui::Button(materializr::tr("Finish Sketch"), ImVec2(-1, bh(30))))
        action = ToolAction::FinishSketch;
    tip(materializr::tr("Leave sketch mode and return to the 3D viewport. Keeps the sketch."));
    if (ImGui::Button(materializr::tr("Exit Sketch"), ImVec2(-1, bh(30))))
        action = ToolAction::ExitSketchDiscard;
    tip(materializr::tr("Discard the current sketch entirely and leave sketch mode. Rewinds history to before the sketch was entered; the body returns to its pre-sketch state. Useful when you've started a sketch you don't want to keep — Esc-while-placing only cancels the in-progress shape, this clears everything."));

    // Plugin buttons for InSketchMode context
    renderPluginButtons(1 << static_cast<int>(SelectionContext::InSketchMode));

    return action;
}

ToolAction Toolbar::renderNoSelectionTools() {
    ToolAction action = ToolAction::None;

    // Start a sketch on a base plane — lets you model from scratch with no body.
    ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Create"));
    ImGui::Separator();
    if (ImGui::Button(materializr::tr("Sketch on XY"), ImVec2(-1, bh(30)))) action = ToolAction::StartSketchXY;
    tip(materializr::tr("Start a new sketch on the world XY (floor) plane."));
    if (ImGui::Button(materializr::tr("Sketch on XZ"), ImVec2(-1, bh(30)))) action = ToolAction::StartSketchXZ;
    tip(materializr::tr("Start a new sketch on the world XZ (front) plane."));
    if (ImGui::Button(materializr::tr("Sketch on YZ"), ImVec2(-1, bh(30)))) action = ToolAction::StartSketchYZ;
    tip(materializr::tr("Start a new sketch on the world YZ (side) plane."));

    // OCCT primitives (Box / Cylinder / Sphere / Cone / Torus) under a
    // single fold-out button so the empty-canvas toolbar stays compact.
    // Each menu item fires a requestInteractiveOp the PrimitivesPlugin
    // wired up; Application opens the per-kind parameter popup.
    // (Steve: "Primitives button, pop-out side menu, then continue as
    //  normal — keeps the Create section uncluttered".)
    renderPrimitivesMenu();

    // Axis from a vertex selection (two vertices → through-points axis). This
    // is the fallback context vertices land in; renders nothing otherwise.
    renderAddAxisMenu();

    // Measure moved to the View menu (unified across layouts).

    // Plugin buttons: NoSelection + Always
    int mask = (1 << static_cast<int>(SelectionContext::NoSelection))
             | (1 << static_cast<int>(SelectionContext::Always));
    renderPluginButtons(mask);

    return action;
}

ToolAction Toolbar::renderBodyTools(bool primaryContext) {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Transform"));
    ImGui::Separator();

    // Gizmo modes side by side, then Mirror.
    //
    // Under a FACE selection these three become the face verbs (Move Face,
    // tilt, scale) — the selection picks body-vs-face, not the button. Scale is
    // dropped there because Face Operations already offers Scale Face, and they
    // are the same operation: measured, a 20mm box top at 50% comes back as the
    // identical frustum either way. Two buttons, one behaviour, is the thing
    // being fixed.
    const int cols = primaryContext ? 3 : 2;
    float cw = (ImGui::GetContentRegionAvail().x -
                (cols - 1) * ImGui::GetStyle().ItemSpacing.x) / static_cast<float>(cols);
    if (ImGui::Button(materializr::tr("Move"), ImVec2(cw, bh(30))))   action = ToolAction::Move;
    tip(materializr::tr("Show the translate gizmo. Drag axes / planes to move. (W)"));
    ImGui::SameLine();
    if (ImGui::Button(materializr::tr("Rotate"), ImVec2(cw, bh(30)))) action = ToolAction::Rotate;
    tip(materializr::tr("Show the rotate gizmo. Drag rings to rotate around each axis. (E)"));
    if (primaryContext) {
        ImGui::SameLine();
        if (ImGui::Button(materializr::tr("Scale"), ImVec2(cw, bh(30))))  action = ToolAction::Scale;
        tip(materializr::tr("Show the scale gizmo. Drag handles to resize. (R)"));
    }
    // Mirror + Revolve share the row so they read as the "uses an
    // already-created primitive" pair (mirror plane / construction axis).
    float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button(materializr::tr("Mirror"), ImVec2(half, bh(30))))    action = ToolAction::Mirror;
    tip(materializr::tr("Mirror the selected bodies across a plane you pick next."));
    ImGui::SameLine();
    // Context-sensitive: a selected sketch lathes (spin its profile into a
    // solid); otherwise the same button revolves the selected body around an
    // axis (a fan, a hinge). beginRevolve() picks the matching mode from the
    // selection, so both share one action.
    bool sketchSel = m_selection && m_selection->hasSelectedSketches();
    if (ImGui::Button(sketchSel ? "Lathe" : "Revolve", ImVec2(half, bh(30))))
        action = ToolAction::Revolve;
    if (sketchSel)
        tip(materializr::tr("Lathe: spin the selected sketch's profile around a Construction Axis into a new solid. Pick the axis next."));
    else
        tip(materializr::tr("Revolve the selected body/bodies around a Construction Axis (a fan, a hinge). Pick the axis next; multi-body selection rotates as a group."));

    // Plugin buttons: always include HasBodies (1+ bodies), and only include
    // MultipleBodies (2+ bodies, e.g. Union / Subtract / Intersect) when at
    // least two bodies are actually selected. Previously OR-ing them both
    // unconditionally meant boolean ops appeared with a single body picked,
    // which can't do anything.
    if (primaryContext) {
        int mask = (1 << static_cast<int>(SelectionContext::HasBodies));
        if (m_selection && m_selection->selectedBodyCount() >= 2) {
            mask |= (1 << static_cast<int>(SelectionContext::MultipleBodies));
        }
        renderPluginButtons(mask);
    }

    // Fabrication: flatten the selected body into a 2D pattern (laser / CNC /
    // cut-out templates). Body context only — under a FACE selection this
    // renderer is a fall-through for the Transform row, and the face's own
    // "Unfold Faces" button already covers it.
    if (primaryContext && m_selection && m_selection->selectedBodyCount() == 1) {
        ImGui::Spacing();
        ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Fabrication"));
        ImGui::Separator();
        if (ImGui::Button(materializr::tr("Unfold / Flatten"), ImVec2(-1, bh(30))))
            action = ToolAction::Unfold;
        tip(materializr::tr("Lay the body flat into a 2D pattern (cut + fold lines) for a laser cutter, CNC, or printed template. Mark it as foam board / sheet metal / wood to set how folds are processed."));
    }

    // Measure is deliberately NOT rendered here: it lives in the View menu in
    // every layout (the rail keeps a button because it has no menu bar).
    //
    // Only when this IS the body context. Under a face selection render()
    // falls through to here purely for the Transform row, and the catalogue
    // is then full of FACE tools that renderFaceTools has already placed —
    // running the net would render every one of them a second time.
    if (primaryContext && action == ToolAction::None)
        action = renderCatalogRemainder({ToolAction::Move, ToolAction::Rotate,
                                         ToolAction::Scale, ToolAction::Mirror,
                                         ToolAction::Revolve, ToolAction::Unfold,
                                         ToolAction::Measure});

    return action;
}

ToolAction Toolbar::renderFaceTools() {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Face Operations"));
    ImGui::Separator();

    // Every gate below is the catalogue's (railTools()); the wording, order
    // and the Transform row are classic's own. The face branch is where the
    // two lists diverged most — Push/Pull, Extrude and Shell are flat-face
    // only, Taper is not, and each rule used to be written out twice.
    auto btn = [&](ToolAction a, const char* label, const char* t) {
        if (!catalogOffers(a)) return;
        if (ImGui::Button(label, ImVec2(-1, bh(30)))) action = a;
        tip(t);
    };

    btn(ToolAction::SketchOnFace, "Sketch on Face",
        "Start a new sketch lying on the picked face.");
    btn(ToolAction::PushPull, "Push / Pull",
        "Drag the face along its normal to extrude (+) or cut (\xE2\x88\x92) into the body.");
    // Extrude From a face = a new body that is the face's silhouette swept
    // along its normal. Push/Pull modifies the source body; Extrude always
    // creates a separate one. (Move Face / Taper / Scale Face are their own
    // entries; Move/Rotate/Scale below transform the face itself.)
    btn(ToolAction::ExtrudeSketch, "Extrude From",
        "Make a NEW body by extruding this face's silhouette (source body unchanged).");
    btn(ToolAction::Shell, "Shell",
        "Hollow the body, removing the picked face. Wall thickness in the popup.");
    btn(ToolAction::ScaleFace, "Scale Face",
        "Re-slope the side walls toward a scaled copy of this face \xE2\x80\x94 under "
        "100% tapers it in, over 100% flares it out. Blend length in the popup.");
    btn(ToolAction::Taper, "Draft",
        "Draft angle for moulding or printing: tilt the picked face(s) by an "
        "angle about a fixed neutral plane. Unlike Rotate it takes SEVERAL "
        "faces at one angle (all four walls of a box) and works on cylindrical "
        "and conical faces too \xE2\x80\x94 a cylinder drafts into a cone.");
    btn(ToolAction::RemoveFace, "Repair Geometry",
        "Delete the picked face(s) and heal the surrounding faces back together "
        "\xE2\x80\x94 take a baked fillet/chamfer back to a sharp edge so it can be "
        "re-applied, or clean a round/hole off an imported part.");
    btn(ToolAction::ProjectSketch, "Projection",
        "Project a sketch onto this face along the sketch's normal, then "
        "engrave (cut in) or emboss (raise out) to a depth - wrap a logo "
        "or text onto a cylinder. Sketch, mode and depth in the popup; "
        "click sketch regions in the viewport to project only those.");
    btn(ToolAction::EditDiameter, "Edit Diameter",
        "Resize a cylindrical hole / pin to an exact diameter.");
    btn(ToolAction::Thread, "Thread",
        "Cut a helical screw thread into the picked cylindrical face \xE2\x80\x94 "
        "external on a boss, internal in a hole. Pitch / depth / handedness "
        "in the popup.");
    btn(ToolAction::Unfold, "Unfold Faces",
        "Flatten the SELECTED faces into a 2D pattern (cut + fold lines) for a "
        "laser/CNC/printed template. Pick the faces of one panel (e.g. a skin) "
        "\xE2\x80\x94 unfolding a whole closed body rarely makes sense.");
    // "Edit Fillet / Chamfer" - the catalogue already ran the ownsFaceScore
    // probe that decides whether the picked face came from one, and which.
    // Classic used to run a second, identical probe of its own.
    if (catalogOffers(ToolAction::EditFilletChamfer)) {
        const RailTool* e = nullptr;
        for (const auto& t : m_catalog)
            if (t.pluginIndex < 0 && t.action == ToolAction::EditFilletChamfer) { e = &t; break; }
        if (e) {
            if (ImGui::Button(e->label, ImVec2(-1, bh(30))))
                action = ToolAction::EditFilletChamfer;
            if (e->tip) tip(e->tip);
        }
    }

    if (action == ToolAction::None)
        action = renderCatalogRemainder({
            ToolAction::SketchOnFace, ToolAction::PushPull, ToolAction::ExtrudeSketch,
            ToolAction::Shell, ToolAction::ScaleFace, ToolAction::Taper,
            ToolAction::RemoveFace, ToolAction::ProjectSketch, ToolAction::EditDiameter,
            ToolAction::Thread, ToolAction::Unfold, ToolAction::EditFilletChamfer,
            // Move/Rotate come from the shared Transform row that render()
            // falls through to (renderBodyTools). Scale is listed too so the
            // net stays quiet if it ever returns to the face catalogue.
            ToolAction::Move, ToolAction::Rotate, ToolAction::Scale});

    // Frozen-round hint: a fillet-shaped face with no editable op behind it
    // (an older save's baked geometry). "Edit Fillet" can't appear for it, so
    // point the user at Repair Geometry — restore the edge, then re-fillet.
    if (m_selFrozenRound) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
        ImGui::TextColored(materializr::dimText(), "%s", materializr::tr("This round is frozen (saved before edit support). Use Repair Geometry above to restore the sharp edge, then re-fillet."));
        ImGui::PopTextWrapPos();
    }

    // Construction-plane / -axis creation from the selected face(s).
    renderAddPlaneMenu();
    renderAddAxisMenu();

    // Plugin buttons for HasFaces context
    renderPluginButtons(1 << static_cast<int>(SelectionContext::HasFaces));

    return action;
}

ToolAction Toolbar::renderSketchSelectedTools() {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Sketch"));
    ImGui::Separator();
    ImGui::TextWrapped("%s", materializr::tr("Tip: hover a sketch region to highlight it, click to select, Ctrl+click to add to selection."));
    ImGui::Separator();

    if (catalogOffers(ToolAction::EditSketch)) {
        if (ImGui::Button(materializr::tr("Edit Sketch"), ImVec2(-1, bh(30))))
            action = ToolAction::EditSketch;
        tip(materializr::tr("Re-enter sketch mode to revise this sketch's geometry."));
    }
    // Push/Pull before Extrude From, matching this layout's Region and Face
    // panels. It was missing here entirely — the mirror of the rail's old
    // "attached sketch gets no Extrude" gap (see railTools) — so a whole
    // sketch could only ever spawn a new body, never modify its host in
    // place. Both tools now exist in every layout for every sketch selection.
    if (catalogOffers(ToolAction::PushPull)) {
        if (ImGui::Button(materializr::tr("Push / Pull"), ImVec2(-1, bh(30))))
            action = ToolAction::PushPull;
        tip(materializr::tr("Drag the arrow to push the sketch's regions into the body beneath them, or pull them out of it. Modifies that body in place — use Extrude From for a separate body."));
    }
    if (catalogOffers(ToolAction::ExtrudeSketch) &&
        ImGui::Button(materializr::tr("Extrude From"), ImVec2(-1, bh(30))))
        action = ToolAction::ExtrudeSketch;
    tip(materializr::tr("Make a NEW body by extruding the sketch's closed regions, leaving any host body unchanged. Whole-sketch extrude assumes ONE outer boundary - for multi-shape sketches (SVG imports, text), click individual regions in the viewport (Ctrl+click for several) and extrude those instead."));
    if (catalogOffers(ToolAction::SubtractSketch)) {
        if (ImGui::Button(materializr::tr("Subtract Sketch"), ImVec2(-1, bh(30))))
            action = ToolAction::SubtractSketch;
        tip(materializr::tr("Extrude the sketch's regions and cut the result out of the body they\nrun into \xE2\x80\x94 the host body when the sketch sits on one, otherwise\nwhichever body the sweep reaches."));
    }
    ImGui::TextWrapped("%s", materializr::tr("Subtract sweeps the profile like Extrude, then cuts that volume out of the body it reaches."));

    // Move / Rotate gizmo modes — appear here so a selected sketch behaves
    // like a movable construction plane. Bodies have these in renderBodyTools;
    // sketches need their own entry point. The Transform header matches the
    // "Sketch" / "Loft" section-label convention so the toolbar reads as a
    // sequence of clearly-titled groups.
    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Transform"));
    ImGui::Separator();
    if (ImGui::Button(materializr::tr("Move"), ImVec2(-1, bh(30))))
        action = ToolAction::Move;
    tip(materializr::tr("Show the Move gizmo on the selected sketch. Drag an axis to reposition the sketch in 3D - its geometry rides along, so this effectively turns the sketch into a movable construction plane. Only available outside ortho view and sketch-edit mode."));
    if (ImGui::Button(materializr::tr("Rotate"), ImVec2(-1, bh(30))))
        action = ToolAction::Rotate;
    tip(materializr::tr("Show the Rotate gizmo on the selected sketch. Drag a ring to spin the sketch around its centroid."));

    // Whatever else the catalogue offers here — Lathe (Revolve) lived in the
    // rails only until this net existed.
    if (action == ToolAction::None)
        action = renderCatalogRemainder({ToolAction::EditSketch, ToolAction::PushPull,
                                         ToolAction::ExtrudeSketch, ToolAction::SubtractSketch,
                                         ToolAction::Move, ToolAction::Rotate});

    // Plugin buttons for HasSketches context
    renderPluginButtons(1 << static_cast<int>(SelectionContext::HasSketches));

    return action;
}

ToolAction Toolbar::renderPlaneSelectedTools() {
    ToolAction action = ToolAction::None;
    ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Construction Plane"));
    ImGui::Separator();
    if (ImGui::Button(materializr::tr("Sketch on this Plane"), ImVec2(-1, bh(30))))
        action = ToolAction::SketchOnFace; // dispatched on Plane in handler
    tip(materializr::tr("Start a new sketch lying on this construction plane — same workflow as Sketch on Face, just with the plane as the host."));

    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Transform"));
    ImGui::Separator();
    if (ImGui::Button(materializr::tr("Move"), ImVec2(-1, bh(30))))   action = ToolAction::Move;
    tip(materializr::tr("Show the Move gizmo on this plane. Drag an axis arrow to nudge it; the live readout pinned to the cursor shows the offset along the plane's own normal."));
    if (ImGui::Button(materializr::tr("Rotate"), ImVec2(-1, bh(30)))) action = ToolAction::Rotate;
    tip(materializr::tr("Show the Rotate gizmo. Drag a ring to spin the plane around its origin; snap is 5° increments when snap-to-grid is on."));

    if (action == ToolAction::None)
        action = renderCatalogRemainder({ToolAction::SketchOnFace, ToolAction::Move,
                                         ToolAction::Rotate});

    // Midplane between two selected construction planes; axis from their
    // intersection.
    renderAddPlaneMenu();
    renderAddAxisMenu();
    return action;
}

ToolAction Toolbar::renderAxisSelectedTools() {
    ToolAction action = ToolAction::None;
    ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Construction Axis"));
    ImGui::Separator();
    ImGui::TextWrapped("%s", materializr::tr("Axes are 1-D primitives — they'll feed Revolve and future Pattern-Around-Axis ops. For now you can move them; rotate isn't meaningful on a line."));

    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Transform"));
    ImGui::Separator();
    if (ImGui::Button(materializr::tr("Move"), ImVec2(-1, bh(30)))) action = ToolAction::Move;
    tip(materializr::tr("Show the Move gizmo on this axis. Drag an arrow to translate the axis origin; the direction is preserved."));

    if (action == ToolAction::None)
        action = renderCatalogRemainder({ToolAction::Move});

    renderAddPlaneMenu();
    return action;
}

ToolAction Toolbar::renderSketchRegionTools() {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Region"));
    ImGui::Separator();
    int n = m_selection ? m_selection->selectedSketchRegionCount() : 0;
    ImGui::Text(materializr::tr("%d region%s selected"), n, n == 1 ? "" : "s");
    ImGui::Spacing();

    // Push/Pull routes through the app's interactive arrow gizmo (default 0,
    // drag to extrude/cut) — same as a body face.
    if (catalogOffers(ToolAction::PushPull)) {
        if (ImGui::Button(materializr::tr("Push / Pull"), ImVec2(-1, bh(30))))
            action = ToolAction::PushPull;
        tip(materializr::tr("Drag the arrow to extrude this region into a body, or cut it into the parent."));
    }
    if (catalogOffers(ToolAction::ExtrudeSketch)) {
        if (ImGui::Button(materializr::tr("Extrude From"), ImVec2(-1, bh(30))))
            action = ToolAction::ExtrudeSketch;
        tip(materializr::tr("Make a NEW body from this region (Ctrl+click several regions to extrude them together). The source sketch/body is left unchanged."));
    }
    // Subtract: cut this region out of the body the sketch sits on, with a red
    // preview of the removed volume.
    if (catalogOffers(ToolAction::SubtractSketch)) {
        if (ImGui::Button(materializr::tr("Subtract"), ImVec2(-1, bh(30))))
            action = ToolAction::SubtractSketch;
        tip(materializr::tr("Cut this region into the body the sketch was drawn on (preview in red)."));
    }

    // Any remaining HasSketchRegions plugin buttons.
    renderPluginButtons(1 << static_cast<int>(SelectionContext::HasSketchRegions));

    // Edit the sketch this region belongs to — re-enter sketch mode to revise it.
    if (catalogOffers(ToolAction::EditSketch)) {
        if (ImGui::Button(materializr::tr("Edit Sketch"), ImVec2(-1, bh(30))))
            action = ToolAction::EditSketch;
        tip(materializr::tr("Re-enter sketch mode to revise this region's parent sketch."));
    }

    // Move / Rotate the region's PARENT sketch in 3D — same gizmo path as
    // the whole-sketch case. A region selection is just a finger pointing at
    // its sketch for these ops. Hidden in ortho view (gizmo's own rule) but
    // the buttons stay visible so the user understands the action exists.
    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Transform"));
    ImGui::Separator();
    if (ImGui::Button(materializr::tr("Move"), ImVec2(-1, bh(30))))
        action = ToolAction::Move;
    tip(materializr::tr("Show the Move gizmo on the parent sketch. Drag an axis to reposition the sketch in 3D - geometry follows, so the sketch becomes a movable construction plane. Outside ortho view only."));
    if (ImGui::Button(materializr::tr("Rotate"), ImVec2(-1, bh(30))))
        action = ToolAction::Rotate;
    tip(materializr::tr("Show the Rotate gizmo on the parent sketch. Drag a ring to spin the sketch around its centroid."));

    if (action == ToolAction::None)
        action = renderCatalogRemainder({ToolAction::PushPull, ToolAction::ExtrudeSketch,
                                         ToolAction::SubtractSketch, ToolAction::EditSketch,
                                         ToolAction::Move, ToolAction::Rotate});

    ImGui::Spacing();
    ImGui::TextWrapped("%s", materializr::tr("Drag positive distance to extrude, negative to cut into the body the sketch sits on."));

    return action;
}

ToolAction Toolbar::renderEdgeTools() {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Edge Ops"));
    ImGui::Separator();
    // Availability comes from the catalogue (railTools()); the wording and
    // placement below are classic's own. "Move Hole" used to be gated on a
    // duplicate copy of the rim test here and shipped to the rails only.
    if (catalogOffers(ToolAction::Fillet)) {
        if (ImGui::Button(materializr::tr("Fillet"), ImVec2(-1, bh(30)))) action = ToolAction::Fillet;
        tip(materializr::tr("Round the picked edge(s). Set radius in the popup."));
    }
    if (catalogOffers(ToolAction::Chamfer)) {
        if (ImGui::Button(materializr::tr("Chamfer"), ImVec2(-1, bh(30)))) action = ToolAction::Chamfer;
        tip(materializr::tr("Bevel the picked edge(s). Set distance in the popup."));
    }
    if (catalogOffers(ToolAction::Move)) {
        if (ImGui::Button(materializr::tr("Move Hole"), ImVec2(-1, bh(30)))) action = ToolAction::Move;
        tip(materializr::tr("Move the hole this rim belongs to: drag one rim to tilt the bore, one straight side to reshape it, or select both rims to slide the whole hole."));
    }
    if (catalogOffers(ToolAction::EditDiameter)) {
        if (ImGui::Button(materializr::tr("Edit Diameter"), ImVec2(-1, bh(30))))
            action = ToolAction::EditDiameter;
        tip(materializr::tr("Resize the cylindrical face this edge belongs to."));
    }
    // Anything the catalogue gained that classic has no bespoke button for.
    if (action == ToolAction::None)
        action = renderCatalogRemainder({ToolAction::Fillet, ToolAction::Chamfer,
                                         ToolAction::Move, ToolAction::EditDiameter});

    // Construction plane / axis from this edge.
    renderAddPlaneMenu();
    renderAddAxisMenu();

    // Plugin buttons for HasEdges context
    renderPluginButtons(1 << static_cast<int>(SelectionContext::HasEdges));

    return action;
}

} // namespace materializr
