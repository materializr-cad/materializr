#include "ui/LengthField.h"
#include "ui/StepperRow.h"
#include "FaceOpControllers.h"
#include "../ui/UiTheme.h"      // viewportBanner
#include "../ui/NumField.h"     // inputNumber, btnConfirm/btnCancel
#include "../ui/OpDialogGrip.h"
#include "../touch_mode.h"
#include "UserAxes.h"
#include "../core/Document.h"
#include "../core/SelectionManager.h"
#include "../core/NumParse.h"
#include "../ui/TouchWidgets.h" // im-touch number-pad amount fields
#include "../modeling/ShellOp.h"
#include "../modeling/TaperOp.h"
#include "../modeling/ScaleFaceOp.h"
#include "../modeling/ProjectSketchOp.h"
#include "../modeling/DefeatureOp.h"
#include "../modeling/ResizeCylindricalOp.h"
#include "../modeling/FaceTweak.h"
#include "../modeling/FaceTweakOp.h"
#include "../modeling/MoveFaceOp.h"
#include "../core/PlaneAxes.h"
#include <BRepTools_WireExplorer.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepTools.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <GeomLib_IsPlanarSurface.hxx>
#include "../core/History.h"
#include "../modeling/Sketch.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <imgui.h>
#include <BRep_Tool.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <GProp_GProps.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <Geom_Surface.hxx>
#include <Geom_Plane.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_ConicalSurface.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"

namespace materializr {

namespace {
// Same test as Application::faceIsPlanar — a plane, or close enough that OCCT's
// planarity check accepts it (a face can be planar without a Geom_Plane).
bool faceIsPlanar(const TopoDS_Face& face) {
    Handle(Geom_Surface) s = BRep_Tool::Surface(face);
    if (s.IsNull()) return false;
    if (s->IsKind(STANDARD_TYPE(Geom_Plane))) return true;
    GeomLib_IsPlanarSurface tester(s, 1.0e-7);
    return tester.IsPlanar();
}

// True if `face` shares an edge with a rounded (cylinder/torus = fillet) face of
// `body`. That's the exact condition OCCT's offset can't open, so it's what the
// Shell warning should key on — NOT merely "the body has fillets somewhere"
// (which mis-blamed fillets on a plain side face that failed for another reason).
bool faceBordersRounded(const TopoDS_Shape& body, const TopoDS_Face& face) {
    if (body.IsNull() || face.IsNull()) return false;
    TopTools_IndexedDataMapOfShapeListOfShape edgeToFaces;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, edgeToFaces);
    for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
        int idx = edgeToFaces.FindIndex(ex.Current());
        if (idx == 0) continue;
        for (const TopoDS_Shape& nb : edgeToFaces.FindFromIndex(idx)) {
            if (nb.IsSame(face)) continue;
            BRepAdaptor_Surface sa(TopoDS::Face(nb));
            if (sa.GetType() == GeomAbs_Cylinder || sa.GetType() == GeomAbs_Torus)
                return true;
        }
    }
    return false;
}
} // namespace

// ─── Shell ───────────────────────────────────────────────────────────────────

int ShellController::onBegin(const IopContext& ctx) {
    for (const auto& e : ctx.selection.getSelection()) {
        if (e.type == SelectionType::Face && e.bodyId >= 0 &&
            !e.shape.IsNull()) {
            m_face = TopoDS::Face(e.shape);
            m_thickness = 1.0f;
            materializr::formatLengthDigits(m_inputBuf, sizeof(m_inputBuf), m_thickness);
            m_inputFocus = true;
            return e.bodyId;
        }
    }
    return -1;
}

std::unique_ptr<Operation> ShellController::buildOp(const IopContext&) {
    if (m_thickness <= 0.0f) return nullptr;
    auto op = std::make_unique<ShellOp>();
    op->setBody(bodyId());
    op->setThickness(static_cast<double>(m_thickness));
    op->addFaceToRemove(m_face);
    return op;
}

void ShellController::panelBody(const IopContext& ctx, bool& changed) {
    ImGui::TextDisabled("%s", materializr::tr("Hollows the body, opening a face."));

    if (ctx.cornerCommitUi) {
        // im-touch: number-pad amount field — no InputText, no native
        // keyboard (which froze the app on iOS).
        if (materializr::amountLengthField("shellAmt", nullptr, &m_thickness, /*allowSign=*/false, 0.1f, 20.0f)) {
            materializr::formatLengthDigits(m_inputBuf, sizeof(m_inputBuf), m_thickness);
            changed = true;
        }
    } else {
    if (m_inputFocus) {
        ImGui::SetKeyboardFocusHere();
        m_inputFocus = false;
    }
    ImGui::SetNextItemWidth(140);
    // parseFinite: non-finite input keeps the previous thickness rather
    // than feeding inf into MakeThickSolid.
    // The member is the truth; the buffer follows it unless being typed in.
    materializr::reseedLengthBufferIfIdle("##shellThickness", m_inputBuf, sizeof(m_inputBuf), m_thickness);
    if (ImGui::InputText("##shellThickness", m_inputBuf, sizeof(m_inputBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        (void)materializr::parseLength(m_inputBuf, m_thickness);
        requestCommit();
    } else if (materializr::lengthBufferIsActive("##shellThickness")) {
        // Only while the user is typing. Parsing an IDLE buffer wrote the
        // buffer's rounded text back over a more precise member — a value was
        // truncated to the display decimals just by opening the tool.
        float parsed = m_thickness;
        if (materializr::parseLength(m_inputBuf, parsed) &&
            std::abs(parsed - m_thickness) > 0.001f) {
            m_thickness = parsed;
            changed = true;
        }
    }
    ImGui::SameLine();
    ImGui::Text("%s", materializr::unitSuffix());
    }

    if (materializr::lengthStepperRow("shellStep", &m_thickness,
                                /*allowNegative=*/false, 0.1f, 20.0f)) {
        // Snap to 0.1 mm — wall thicknesses are almost always in tenths, and a
        // free-floating 3.47 mm slider value is just noise.
        m_thickness = std::round(m_thickness * 10.0f) / 10.0f;
        materializr::formatLengthDigits(m_inputBuf, sizeof(m_inputBuf), m_thickness);
        changed = true;
    }

    if (!previewOk()) {
        const ImVec4 warn(1.0f, 0.6f, 0.3f, 1.0f);
        // Only blame fillets when THIS face actually borders one — OCCT can't
        // open a fillet-bordered face (it seals the cavity), and no thickness
        // fixes it; the answer is order-of-operations: shell first, fillet after.
        // A plain side face that failed for another reason gets the generic hint.
        const TopoDS_Shape& body = ctx.doc.getBody(bodyId());
        if (faceBordersRounded(body, m_face)) {
            ImGui::TextColored(warn, "%s", materializr::tr("Shell failed: OCCT can't open a fillet-bordered face.\nShell the body FIRST, then add the fillets."));
        } else {
            ImGui::TextColored(warn, "%s", materializr::tr("Shell failed - try a thinner wall, or\nthis body's faces can't be shelled."));
        }
    }
}

void ShellController::onCleanup() {
    m_face.Nullify();
}

// ─── Taper ───────────────────────────────────────────────────────────────────

int TaperController::onBegin(const IopContext& ctx) {
    // Collect every selected face on ONE body — multi-select all four
    // sides of a box to pyramid it in one go.
    m_faces.clear();
    int body = -1;
    for (const auto& e : ctx.selection.getSelection()) {
        if (e.type != SelectionType::Face || e.bodyId < 0 ||
            e.shape.IsNull())
            continue;
        if (body < 0) body = e.bodyId;
        if (e.bodyId != body) continue; // one body per op
        m_faces.push_back(TopoDS::Face(e.shape));
    }
    if (m_faces.empty()) return -1;
    m_angle = 10.0f;
    m_axisIdx = 0;
    m_flipBase = false;
    return body;
}

bool TaperController::resolveFrame(const IopContext& ctx, glm::vec3& dirOut,
                                   glm::vec3& neutralOut) const {
    if (bodyId() < 0 || m_faces.empty()) return false;

    // Pull direction. Auto: a cylindrical/conical face drafts along its own
    // axis; a planar face drafts along the world axis most PERPENDICULAR to
    // its normal (preferring up). Manual: the user-convention X/Y/Z radios.
    glm::vec3 dir(0.0f, 1.0f, 0.0f);
    if (m_axisIdx == 0) {
        try {
            const TopoDS_Face& f = m_faces.front();
            Handle(Geom_Surface) s = BRep_Tool::Surface(f);
            Handle(Geom_CylindricalSurface) cyl =
                Handle(Geom_CylindricalSurface)::DownCast(s);
            Handle(Geom_ConicalSurface) cone =
                Handle(Geom_ConicalSurface)::DownCast(s);
            if (!cyl.IsNull() || !cone.IsNull()) {
                gp_Dir a = !cyl.IsNull()
                               ? cyl->Cylinder().Position().Direction()
                               : cone->Cone().Position().Direction();
                dir = glm::vec3(static_cast<float>(a.X()),
                                static_cast<float>(a.Y()),
                                static_cast<float>(a.Z()));
            } else {
                BRepGProp_Face prop(f);
                double u1, u2, v1, v2;
                prop.Bounds(u1, u2, v1, v2);
                gp_Pnt c;
                gp_Vec nv;
                prop.Normal(0.5 * (u1 + u2), 0.5 * (v1 + v2), c, nv);
                glm::vec3 n(static_cast<float>(nv.X()),
                            static_cast<float>(nv.Y()),
                            static_cast<float>(nv.Z()));
                if (glm::length(n) > 1e-6f) n = glm::normalize(n);
                const glm::vec3 axes[3] = {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}};
                float best = 2.0f;
                for (const auto& a : axes) {
                    float d = std::abs(glm::dot(n, a));
                    if (d < best - 1e-4f) { best = d; dir = a; }
                }
            }
        } catch (...) {}
    } else {
        dir = userAxisToWorldVec(m_axisIdx - 1);
    }
    if (glm::length(dir) < 1e-6f) return false;
    dir = glm::normalize(dir);

    // Neutral plane: perpendicular to the pull direction, through the
    // body's extreme along it — the BASE stays fixed and the far end
    // tilts. Flip moves the fixed plane to the other extreme.
    try {
        Bnd_Box bb;
        BRepBndLib::Add(ctx.doc.getBody(bodyId()), bb);
        if (bb.IsVoid()) return false;
        double x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        glm::vec3 corners[8] = {
            {(float)x0, (float)y0, (float)z0}, {(float)x1, (float)y0, (float)z0},
            {(float)x0, (float)y1, (float)z0}, {(float)x1, (float)y1, (float)z0},
            {(float)x0, (float)y0, (float)z1}, {(float)x1, (float)y0, (float)z1},
            {(float)x0, (float)y1, (float)z1}, {(float)x1, (float)y1, (float)z1}};
        float lo = 1e30f, hi = -1e30f;
        for (const auto& c : corners) {
            float p = glm::dot(c, dir);
            lo = std::min(lo, p);
            hi = std::max(hi, p);
        }
        glm::vec3 center(0.5f * (float)(x0 + x1), 0.5f * (float)(y0 + y1),
                         0.5f * (float)(z0 + z1));
        float proj = m_flipBase ? hi : lo;
        neutralOut = center + dir * (proj - glm::dot(center, dir));
        dirOut = dir;
        return true;
    } catch (...) { return false; }
}

std::unique_ptr<Operation> TaperController::buildOp(const IopContext& ctx) {
    if (std::abs(m_angle) < 0.1f) return nullptr;
    glm::vec3 dir, np;
    if (!resolveFrame(ctx, dir, np)) return nullptr;
    auto op = std::make_unique<TaperOp>();
    op->setBody(bodyId());
    for (const auto& f : m_faces) op->addFace(f);
    op->setDirection(dir.x, dir.y, dir.z);
    op->setNeutralPoint(np.x, np.y, np.z);
    op->setAngleDeg(static_cast<double>(m_angle));
    return op;
}

void TaperController::panelBody(const IopContext& ctx, bool& changed) {
    ImGui::TextDisabled(materializr::tr("%zu face(s) tilt about the body's base."),
                        m_faces.size());
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
    ImGui::TextDisabled("%s", materializr::tr("Tip: pick SIDE walls — a cylinder wall becomes a cone, box sides become a pyramid."));
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    if (previewOk()) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
                           materializr::tr("Previewing %.1f deg"), m_angle);
    } else if (std::abs(m_angle) < 0.1f) {
        // buildOp() short-circuits at ~0° so no preview is computed —
        // but the face is fine. Don't flash the "can't taper" warning
        // when the user is just sitting on the slider's zero stop.
        ImGui::TextDisabled("%s", materializr::tr("Move the angle slider to preview."));
    } else {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", materializr::tr("No preview: this face can't draft along the current Pull axis. Try another axis, Flip base, or pick a side face."));
        ImGui::TextDisabled("%s", materializr::tr("Note: only flat / cylindrical / conical walls can be drafted (a kernel limit) - for freeform shapes like wing skins, use Scale Face on the END face instead."));
        ImGui::PopTextWrapPos();
    }

    ImGui::TextDisabled(materializr::tr("Angle: %.1f deg"), m_angle);
    if (materializr::stepperRow("taperStep", &m_angle,
                                /*allowNegative=*/true, -45.0f, 45.0f))
        changed = true;
    if (ctx.cornerCommitUi &&
        touchui::amountField("taperAmt", nullptr, &m_angle, "deg", 1,
                             /*allowSign=*/true, -45.0f, 45.0f))
        changed = true;

    ImGui::Text("%s", materializr::tr("Pull axis"));
    ImGui::SameLine();
    const char* axisNames[4] = {"Auto", "X", "Y", "Z"};
    for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::RadioButton(axisNames[i], m_axisIdx == i)) {
            m_axisIdx = i;
            changed = true;
        }
    }
    if (ImGui::Checkbox(materializr::tr("Flip base (fixed end)"), &m_flipBase))
        changed = true;
}

void TaperController::onCleanup() { m_faces.clear(); }

// ─── Remove Face (defeature) ─────────────────────────────────────────────────

int DefeatureController::onBegin(const IopContext& ctx) {
    // Gather every selected face on ONE body — multi-select a few faces to
    // remove them together.
    m_faces.clear();
    int body = -1;
    for (const auto& e : ctx.selection.getSelection()) {
        if (e.type != SelectionType::Face || e.bodyId < 0 || e.shape.IsNull())
            continue;
        if (body < 0) body = e.bodyId;
        if (e.bodyId != body) continue; // one body per op
        m_faces.push_back(TopoDS::Face(e.shape));
    }
    if (m_faces.empty()) return -1;
    return body;
}

std::unique_ptr<Operation> DefeatureController::buildOp(const IopContext&) {
    if (m_faces.empty()) return nullptr;
    auto op = std::make_unique<DefeatureOp>();
    op->setBody(bodyId());
    for (const auto& f : m_faces) op->addFace(f);
    return op;
}

void DefeatureController::panelBody(const IopContext&, bool&) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
    ImGui::TextDisabled("%s", materializr::tr("Removes the selected face(s) and heals the surrounding faces back together — e.g. take a baked fillet back to a sharp edge so you can re-fillet it."));
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    ImGui::Text(materializr::tr("%zu face(s) selected"), m_faces.size());

    if (previewOk()) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "%s", materializr::tr("Previewing removal"));
    } else {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", materializr::tr("Can't remove: the neighbouring faces can't be extended to close the gap. Try a different face — a single fillet / round usually works."));
        ImGui::PopTextWrapPos();
    }
}

void DefeatureController::onCleanup() { m_faces.clear(); }

// ─── Project Sketch ──────────────────────────────────────────────────────────

int ProjectSketchController::onBegin(const IopContext& ctx) {
    m_face.Nullify();
    m_sketchIds.clear();
    m_regionFilter.clear();
    m_depth = 1.0f;
    m_mode = 0;

    int body = -1;
    int pickedSketch = -1;
    for (const auto& e : ctx.selection.getSelection()) {
        if (e.type == SelectionType::Face && e.bodyId >= 0 &&
            !e.shape.IsNull() && body < 0) {
            m_face = TopoDS::Face(e.shape);
            body = e.bodyId;
        }
        // Regions Ctrl+clicked beforehand narrow the projection to just
        // those; all of them must come from one sketch.
        if (e.type == SelectionType::SketchRegion && e.sketchId >= 0) {
            if (pickedSketch < 0) pickedSketch = e.sketchId;
            if (e.sketchId == pickedSketch)
                m_regionFilter.push_back(e.subShapeIndex);
        }
    }
    if (body < 0) return -1;

    m_sketchIds = ctx.doc.getAllSketchIds();
    if (m_sketchIds.empty()) {
        std::fprintf(stderr, "[ProjectSketch] no sketches in document\n");
        return -1;
    }
    // Default to the selection's sketch, else the newest one.
    m_sketchPick = static_cast<int>(m_sketchIds.size()) - 1;
    if (pickedSketch >= 0) {
        for (size_t i = 0; i < m_sketchIds.size(); ++i)
            if (m_sketchIds[i] == pickedSketch)
                m_sketchPick = static_cast<int>(i);
    }
    return body;
}

std::unique_ptr<Operation> ProjectSketchController::buildOp(
    const IopContext&) {
    if (m_face.IsNull() || m_sketchIds.empty() || m_depth < 0.01f)
        return nullptr;
    auto op = std::make_unique<ProjectSketchOp>();
    op->setBody(bodyId());
    op->setTargetFace(m_face);
    op->setSketchId(m_sketchIds[m_sketchPick]);
    op->setRegionFilter(m_regionFilter);
    op->setDepth(static_cast<double>(m_depth));
    op->setMode(m_mode == 1 ? ProjectSketchOp::Mode::Emboss
                            : ProjectSketchOp::Mode::Engrave);
    return op;
}

void ProjectSketchController::panelBody(const IopContext& ctx,
                                        bool& changed) {
    ImGui::TextDisabled("%s", materializr::tr("Projects the sketch onto this face along the\nsketch's normal, then cuts in or raises out."));
    ImGui::TextWrapped("%s", materializr::tr("Click the sketch elements you want projected — click each to add or remove. Use Select all / Clear below."));

    // Live region scoping: clicking sketch regions in the viewport while this
    // panel is open narrows the projection to just those (each click toggles —
    // no modifier needed while this step is active); clicking empty space goes
    // back to the whole sketch. A clicked region also drives the sketch choice,
    // so picking "the relevant sketch" is literally clicking it.
    {
        int selSketch = -1;
        std::vector<int> live;
        for (const auto& e : ctx.selection.getSelection()) {
            if (e.type != SelectionType::SketchRegion || e.sketchId < 0)
                continue;
            if (selSketch < 0) selSketch = e.sketchId;
            if (e.sketchId == selSketch)
                live.push_back(e.subShapeIndex);
        }
        if (selSketch >= 0 &&
            selSketch != m_sketchIds[m_sketchPick]) {
            for (size_t i = 0; i < m_sketchIds.size(); ++i) {
                if (m_sketchIds[i] == selSketch) {
                    m_sketchPick = static_cast<int>(i);
                    changed = true;
                }
            }
        }
        std::sort(live.begin(), live.end());
        std::vector<int> cur = m_regionFilter;
        std::sort(cur.begin(), cur.end());
        if (live != cur) {
            m_regionFilter = live;
            changed = true;
        }
    }

    std::string current =
        ctx.doc.getSketchName(m_sketchIds[m_sketchPick]);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##projSketch", current.c_str())) {
        for (size_t i = 0; i < m_sketchIds.size(); ++i) {
            ImGui::PushID(static_cast<int>(i)); // names may repeat
            bool sel = static_cast<int>(i) == m_sketchPick;
            std::string label = ctx.doc.getSketchName(m_sketchIds[i]);
            if (ImGui::Selectable(label.c_str(), sel)) {
                if (static_cast<int>(i) != m_sketchPick) {
                    m_sketchPick = static_cast<int>(i);
                    m_regionFilter.clear(); // filter was for the old sketch
                    changed = true;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    // Select all → then click the few you DON'T want to drop them (easier than
    // hand-picking every letter of a long inscription). Clear → back to none.
    if (ImGui::SmallButton(materializr::tr("Select all"))) {
        if (auto sk = ctx.doc.getSketch(m_sketchIds[m_sketchPick])) {
            const int sid = m_sketchIds[m_sketchPick];
            const int n = static_cast<int>(sk->buildRegions().size());
            for (int i = 0; i < n; ++i) {
                bool already = false;
                for (const auto& s : ctx.selection.getSelection())
                    if (s.type == SelectionType::SketchRegion &&
                        s.sketchId == sid && s.subShapeIndex == i) {
                        already = true;
                        break;
                    }
                if (already) continue;
                SelectionEntry e;
                e.type = SelectionType::SketchRegion;
                e.sketchId = sid;
                e.subShapeIndex = i;
                ctx.selection.toggleSelection(e); // adds (absent after the check)
            }
            changed = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(materializr::tr("Clear"))) {
        ctx.selection.clear();
        changed = true;
    }
    // Smart guess: auto-nesting of a dense logo is imperfect, so cycle the
    // selection through loops-only / islands-only / all. One press usually
    // lands close to what you want (loops-only = letters, counters hollow);
    // fix the stragglers by clicking. An "island" is a region whose interior
    // sits inside another region's solid (a counter that should be a hole).
    if (ImGui::SmallButton(materializr::tr("Cycle loops/islands"))) {
        if (auto sk = ctx.doc.getSketch(m_sketchIds[m_sketchPick])) {
            auto regions = sk->buildRegions();
            const int sid = m_sketchIds[m_sketchPick];
            std::vector<bool> island(regions.size(), false);
            for (size_t i = 0; i < regions.size(); ++i)
                for (size_t j = 0; j < regions.size(); ++j)
                    if (i != j && sk->isPointInRegion(
                                      regions[j], regions[i].representativePoint)) {
                        island[i] = true;
                        break;
                    }
            m_cycleMode = (m_cycleMode + 1) % 3; // press 1=loops, 2=islands, 3=all
            ctx.selection.clear();
            for (size_t i = 0; i < regions.size(); ++i) {
                const bool want = m_cycleMode == 0 ||
                                  (m_cycleMode == 1 && !island[i]) ||
                                  (m_cycleMode == 2 && island[i]);
                if (!want) continue;
                SelectionEntry e;
                e.type = SelectionType::SketchRegion;
                e.sketchId = sid;
                e.subShapeIndex = static_cast<int>(i);
                ctx.selection.toggleSelection(e);
            }
            changed = true;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", m_cycleMode == 0 ? "all"
                              : m_cycleMode == 1 ? "loops" : "islands");
    if (!m_regionFilter.empty()) {
        ImGui::TextDisabled(materializr::tr("%d region(s) selected - click any to add or\nremove. Use Clear to reset."),
                            static_cast<int>(m_regionFilter.size()));
    } else {
        ImGui::TextDisabled("%s", materializr::tr("All regions. Click elements to project only\nthose (click each to add or remove)."));
    }

    if (!wantsLivePreview(ctx)) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                           materializr::tr("%d regions - live preview is off here.\nConfirm to apply (may take a moment)."),
                           effectiveRegionCount(ctx));
    }

    if (ImGui::RadioButton(materializr::tr("Engrave"), &m_mode, 0)) changed = true;
    ImGui::SameLine();
    if (ImGui::RadioButton(materializr::tr("Emboss"), &m_mode, 1)) changed = true;

    ImGui::TextDisabled("%s", materializr::trFormat("Depth: %s", materializr::fmtLength(m_depth)).c_str());
    if (materializr::lengthStepperRow("projDepthStep", &m_depth,
                                /*allowNegative=*/false, 0.1f, 10.0f)) {
        changed = true;
    }
    if (ctx.cornerCommitUi &&
        materializr::amountLengthField("projAmt", nullptr, &m_depth, /*allowSign=*/false, 0.1f, 10.0f))
        changed = true;

    if (!previewOk()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", materializr::tr("Projection failed - the selected region(s) couldn't\nbe applied (off the face, or too thin/degenerate)."));
    }
}

void ProjectSketchController::onCleanup() {
    m_face.Nullify();
    m_sketchIds.clear();
    m_regionFilter.clear();
}

int ProjectSketchController::effectiveRegionCount(const IopContext& ctx) const {
    if (m_sketchIds.empty()) return 0;
    if (!m_regionFilter.empty()) return static_cast<int>(m_regionFilter.size());
    if (auto sk = ctx.doc.getSketch(m_sketchIds[m_sketchPick]))
        return static_cast<int>(sk->buildRegions().size());
    return 0;
}

bool ProjectSketchController::wantsLivePreview(const IopContext& ctx) const {
    return effectiveRegionCount(ctx) <= kPreviewRegionCap;
}

// ─── Scale Face ──────────────────────────────────────────────────────────────

int ScaleFaceController::onBegin(const IopContext& ctx) {
    int body = -1;
    m_face.Nullify();
    for (const auto& e : ctx.selection.getSelection()) {
        if (e.type == SelectionType::Face && e.bodyId >= 0 &&
            !e.shape.IsNull()) {
            m_face = TopoDS::Face(e.shape);
            body = e.bodyId;
            break;
        }
    }
    if (body < 0 || m_face.IsNull()) return -1;

    m_pctU = m_pctV = 30.0f;
    m_uniform = true;
    m_dragAxis = -1;
    m_len = 10.0f;
    m_lenMax = 100.0f;
    try {
        TopoDS_Shape bodyShape = ctx.doc.getBody(body);

        BRepGProp_Face gpf(m_face);
        double u1, u2, v1, v2;
        gpf.Bounds(u1, u2, v1, v2);
        gp_Pnt onFace;
        gp_Vec nv;
        gpf.Normal(0.5 * (u1 + u2), 0.5 * (v1 + v2), onFace, nv);
        Bnd_Box bb;
        BRepBndLib::Add(bodyShape, bb);
        if (nv.Magnitude() > 1e-9 && !bb.IsVoid()) {
            gp_Dir n(nv);
            double x0, y0, z0, x1, y1, z1;
            bb.Get(x0, y0, z0, x1, y1, z1);
            gp_Pnt corners[8] = {
                gp_Pnt(x0, y0, z0), gp_Pnt(x1, y0, z0),
                gp_Pnt(x0, y1, z0), gp_Pnt(x1, y1, z0),
                gp_Pnt(x0, y0, z1), gp_Pnt(x1, y0, z1),
                gp_Pnt(x0, y1, z1), gp_Pnt(x1, y1, z1)};
            double depth = 0.0;
            for (const auto& c : corners) {
                double d = gp_Vec(c, onFace).Dot(gp_Vec(n));
                depth = std::max(depth, d);
            }
            if (depth > 1e-3) {
                // Default = the FULL depth of the body behind the face, so
                // scaling a box top re-slopes the sides from the BASE.
                m_lenMax = static_cast<float>(depth);
                m_len = m_lenMax;
            }
        }
        // Gizmo frame: the face plane's own axes + the face's half-extents
        // along them. COPY the plane — Pln() returns a temporary, and a
        // reference into it dangles (the red-line-to-infinity bug).
        Handle(Geom_Plane) gpl =
            Handle(Geom_Plane)::DownCast(BRep_Tool::Surface(m_face));
        if (!gpl.IsNull()) {
            const gp_Pln fpln = gpl->Pln();
            const gp_Ax3& fax = fpln.Position();
            gp_Dir ud = fax.XDirection(), vd2 = fax.YDirection();
            m_axisU = glm::vec3((float)ud.X(), (float)ud.Y(), (float)ud.Z());
            m_axisV = glm::vec3((float)vd2.X(), (float)vd2.Y(),
                                (float)vd2.Z());
            GProp_GProps fpr;
            BRepGProp::SurfaceProperties(m_face, fpr);
            gp_Pnt fc = fpr.CentreOfMass();
            m_center = glm::vec3((float)fc.X(), (float)fc.Y(), (float)fc.Z());
            Bnd_Box fbb;
            BRepBndLib::Add(m_face, fbb);
            if (!fbb.IsVoid()) {
                double fx0, fy0, fz0, fx1, fy1, fz1;
                fbb.Get(fx0, fy0, fz0, fx1, fy1, fz1);
                gp_Pnt fcs[8] = {
                    gp_Pnt(fx0, fy0, fz0), gp_Pnt(fx1, fy0, fz0),
                    gp_Pnt(fx0, fy1, fz0), gp_Pnt(fx1, fy1, fz0),
                    gp_Pnt(fx0, fy0, fz1), gp_Pnt(fx1, fy0, fz1),
                    gp_Pnt(fx0, fy1, fz1), gp_Pnt(fx1, fy1, fz1)};
                float hu = 1.0f, hv = 1.0f;
                for (const auto& cpt : fcs) {
                    glm::vec3 d((float)cpt.X() - m_center.x,
                                (float)cpt.Y() - m_center.y,
                                (float)cpt.Z() - m_center.z);
                    hu = std::max(hu, std::abs(glm::dot(d, m_axisU)));
                    hv = std::max(hv, std::abs(glm::dot(d, m_axisV)));
                }
                m_halfU = hu;
                m_halfV = hv;
            }
        }
    } catch (...) {}
    return body;
}

std::unique_ptr<Operation> ScaleFaceController::buildOp(const IopContext&) {
    auto op = std::make_unique<ScaleFaceOp>();
    op->setBody(bodyId());
    op->setFace(m_face);
    op->setScaleUV(static_cast<double>(m_pctU), static_cast<double>(m_pctV));
    op->setLength(static_cast<double>(m_len));
    // Always Pinch — it re-slopes the EXISTING walls and, since the >100%
    // union landed, does it in both directions. The old Extend/Pinch radio
    // asked the user to pick a boolean before they knew what either did, and
    // Extend answered a different question anyway (bolt a new tapered section
    // on top). The op keeps both modes so old projects replay unchanged.
    op->setMode(ScaleFaceOp::Mode::Pinch);
    return op;
}

void ScaleFaceController::applyHandleDrag(int axis, float dPct,
                                          const IopContext& ctx) {
    float& pct = (axis == 0) ? m_pctU : m_pctV;
    pct = std::min(maxPct(), std::max(5.0f, pct + dPct));
    if (m_uniform) {
        m_pctU = pct;
        m_pctV = pct;
    }
    update(ctx);
}

// Hit-test + drag, moved here from Application_Viewport verbatim in behaviour.
// Click anywhere along an arrow SHAFT (centre → tip), not just a disc at the
// tip: the old 16-px tip target made the visible arrow look clickable when it
// wasn't (Steve: "the gizmo is not clickable").
void ScaleFaceController::onViewportInput(const IopViewport& vp,
                                          const IopContext& ctx) {
    if (vp.clicked && m_dragAxis < 0) {
        const glm::vec3 tipU = m_center + m_axisU * (m_halfU * m_pctU / 100.0f);
        const glm::vec3 tipV = m_center + m_axisV * (m_halfV * m_pctV / 100.0f);
        glm::vec2 cs, tu, tv;
        const bool gotC = vp.toScreen(m_center, cs);
        const bool gotU = vp.toScreen(tipU, tu);
        const bool gotV = vp.toScreen(tipV, tv);
        auto distToSeg = [&](glm::vec2 a, glm::vec2 b) {
            const glm::vec2 d = b - a;
            const float len2 = glm::dot(d, d);
            glm::vec2 q;
            if (len2 < 1e-6f) {
                q = vp.mouse - a;
            } else {
                float t = glm::dot(vp.mouse - a, d) / len2;
                t = std::max(0.0f, std::min(1.0f, t));
                q = vp.mouse - (a + t * d);
            }
            return std::sqrt(glm::dot(q, q));
        };
        const float du = (gotC && gotU) ? distToSeg(cs, tu) : 1e9f;
        const float dv = (gotC && gotV) ? distToSeg(cs, tv) : 1e9f;
        // Hit radius in PHYSICAL pixels: ImGui runs at device resolution
        // here (fonts are 15 * uiScale), so a bare 12 px shrank the grab
        // zone to half the arrow's apparent size on a 2x display (Steve:
        // "the grab zone on the arrow could be a little bigger").
        const float pick = (ctx.panel.imTouch ? 30.0f : 20.0f) *
                           ctx.panel.uiScale;
        if      (du < pick && du <= dv) m_dragAxis = 0;
        else if (dv < pick)             m_dragAxis = 1;
        setDraggingHandle(m_dragAxis >= 0);
    }

    if (m_dragAxis >= 0 && vp.dragging) {
        const glm::vec3 axis = (m_dragAxis == 0) ? m_axisU : m_axisV;
        const float half     = (m_dragAxis == 0) ? m_halfU : m_halfV;
        const float dW = vp.dragAlongAxis(m_center, axis, vp.mouseDelta);
        applyHandleDrag(m_dragAxis, dW / std::max(half, 1e-3f) * 100.0f, ctx);
    }

    if (vp.released) {
        m_dragAxis = -1;
        setDraggingHandle(false);
    }
}

void ScaleFaceController::drawOverlay(const IopOverlay& ov) const {
    auto handle = [&](const glm::vec3& axis, float halfExt, float pct,
                      unsigned col) {
        const glm::vec3 tipW = m_center + axis * (halfExt * pct / 100.0f);
        glm::vec2 a, b;
        if (!ov.toScreen(m_center, a) || !ov.toScreen(tipW, b)) return;
        ov.line(a, b, col, 3.0f);
        ov.disc(b, 7.0f, col);
        char hl[16];
        std::snprintf(hl, sizeof(hl), "%.0f%%", pct);
        ov.label(b, hl, col);
    };
    handle(m_axisU, m_halfU, m_pctU, 0xFF5A5AEBu); // red   (0xAABBGGRR)
    handle(m_axisV, m_halfV, m_pctV, 0xFFEB965Au); // blue
}

void ScaleFaceController::panelBody(const IopContext& ctx, bool& changed) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
    ImGui::TextDisabled("%s", materializr::tr("Scale this face; the side walls re-slope to follow. Under 100%% shrinks it, over 100%% grows it. Full length = walls follow from the base; shorter = blend only near the face."));
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    if (previewOk()) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "%s", materializr::trFormat("Previewing %.0f%% x %.0f%% over %s", m_pctU, m_pctV, materializr::fmtLength(m_len)).c_str());
    } else {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", materializr::tr("No preview: needs a FLAT end face (and 100%% is a no-op). Try another face or tweak values."));
        ImGui::PopTextWrapPos();
    }

    if (ImGui::Checkbox(materializr::tr("Uniform"), &m_uniform) && m_uniform) {
        m_pctV = m_pctU;
        changed = true;
    }
    if (m_uniform) {
        ImGui::TextDisabled(materializr::tr("Scale: %.0f %%"), m_pctU);
        if (materializr::stepperRow("scaleStep", &m_pctU,
                                    /*allowNegative=*/true, 5.0f, maxPct(),
                                    /*zeroValue=*/100.0f)) {
            m_pctV = m_pctU;
            changed = true;
        }
        if (ctx.cornerCommitUi &&
            touchui::amountField("scaleAmt", nullptr, &m_pctU, "%", 0,
                                 /*allowSign=*/false, 5.0f, maxPct())) {
            m_pctV = m_pctU;
            changed = true;
        }
    } else {
        // Each slider's text label is shown ABOVE the bar in the colour of
        // the matching face-gizmo arrow, with a hidden "##" slider label.
        // (Steve: "U" and "V" mean nothing here, and the narrow panel was
        //  truncating "Scale U" / "Scale V" to just "S" at the right edge.)
        const ImVec4 redCol (0.92f, 0.35f, 0.35f, 1.0f); // matches the red arrow
        const ImVec4 blueCol(0.35f, 0.59f, 0.92f, 1.0f); // matches the blue arrow
        ImGui::TextColored(redCol, "%s", materializr::tr("Red line"));
        ImGui::SameLine(); ImGui::TextDisabled("%.0f %%", m_pctU);
        if (materializr::stepperRow("scaleUStep", &m_pctU,
                                    /*allowNegative=*/true, 5.0f, maxPct(),
                                    /*zeroValue=*/100.0f))
            changed = true;
        if (ctx.cornerCommitUi &&
            touchui::amountField("scaleUAmt", nullptr, &m_pctU, "%", 0,
                                 /*allowSign=*/false, 5.0f, maxPct()))
            changed = true;
        ImGui::TextColored(blueCol, "%s", materializr::tr("Blue line"));
        ImGui::SameLine(); ImGui::TextDisabled("%.0f %%", m_pctV);
        if (materializr::stepperRow("scaleVStep", &m_pctV,
                                    /*allowNegative=*/true, 5.0f, maxPct(),
                                    /*zeroValue=*/100.0f))
            changed = true;
        if (ctx.cornerCommitUi &&
            touchui::amountField("scaleVAmt", nullptr, &m_pctV, "%", 0,
                                 /*allowSign=*/false, 5.0f, maxPct()))
            changed = true;
    }
    ImGui::TextDisabled("%s", materializr::tr("Or drag the two arrows on the face."));
    ImGui::TextDisabled("%s", materializr::trFormat("Length: %s", materializr::fmtLength(m_len)).c_str());
    if (materializr::lengthStepperRow("lenStep", &m_len,
                                /*allowNegative=*/false, 0.5f,
                                std::max(m_lenMax, 1.0f)))
        changed = true;
    if (ctx.cornerCommitUi &&
        materializr::amountLengthField("lenAmt", nullptr, &m_len, /*allowSign=*/false, 0.5f, std::max(m_lenMax, 1.0f)))
        changed = true;
}

void ScaleFaceController::onCleanup() {
    m_face.Nullify();
    m_dragAxis = -1;
}

// ─── Resize Cylindrical (Edit Diameter) ──────────────────────────────────────
// Was ~17 members on Application plus begin/update/commit/cancel and a
// hand-rolled panel in Application_Dialogs. The base already models all of it:
// the snapshot, the live preview, Confirm/Cancel/Enter/Esc, and — via
// wantsLivePreview — the threaded-body case that has to skip the preview.

int ResizeCylindricalController::onBegin(const IopContext& ctx) {
    // Resolve our own target rather than being handed one. detectCylindricalPick
    // needs only the document and the selection, both of which are right here.
    m_pick = detectCylindricalPick(ctx.doc, ctx.selection);
    if (!m_pick.ok || m_pick.bodyId < 0) return -1;

    m_deferred = ctx.history.isBodyThreaded(m_pick.bodyId);
    m_newBottomDiameter = m_pick.bottomR * 2.0;
    m_newTopDiameter    = m_pick.topR    * 2.0;
    materializr::formatLengthDigits(m_botBuf, sizeof(m_botBuf), m_newBottomDiameter);
    materializr::formatLengthDigits(m_topBuf, sizeof(m_topBuf), m_newTopDiameter);
    m_inputFocus = true;
    return m_pick.bodyId;
}

bool ResizeCylindricalController::wantsLivePreview(const IopContext&) const {
    return !m_deferred;
}

std::unique_ptr<Operation> ResizeCylindricalController::buildOp(
    const IopContext&) {
    const double newBot = m_pick.editBottom ? m_newBottomDiameter * 0.5
                                            : m_pick.bottomR;
    const double newTop = m_pick.editTop    ? m_newTopDiameter    * 0.5
                                            : m_pick.topR;
    // Degenerate or unchanged: no op. The base treats a null op as "nothing to
    // push" and cleans up, which is what the old commit's cancel() branch did.
    const bool unchanged = std::abs(newBot - m_pick.bottomR) < 1e-5 &&
                           std::abs(newTop - m_pick.topR)    < 1e-5;
    if (newBot < 1e-4 || newTop < 1e-4 || unchanged) return nullptr;

    auto op = std::make_unique<ResizeCylindricalOp>();
    op->setBody(bodyId());
    op->setAxis(m_pick.axis);
    op->setHeight(m_pick.height);
    op->setOldRadii(m_pick.bottomR, m_pick.topR);
    op->setNewRadii(newBot, newTop);
    op->setIsHole(m_pick.isHole);
    return op;
}

void ResizeCylindricalController::panelBody(const IopContext& ctx,
                                            bool& changed) {
    // The base already titles the panel "Edit Diameter"; this line carries the
    // part that varies — which end, and whether it's a hole or an outer face.
    const bool bothEnds = both();
    const char* what = bothEnds       ? "Both ends"
                     : m_pick.editBottom ? "Bottom end"
                                         : "Top end";
    ImGui::TextDisabled("%s \xE2\x80\x94 %s", what,
                        m_pick.isHole ? "hole" : "outer face");

    if (bothEnds) {
        ImGui::TextUnformatted(materializr::trFormat("Original: %s", materializr::fmtLength(m_pick.topR * 2.0)).c_str());
    } else if (m_pick.editBottom) {
        ImGui::TextUnformatted(materializr::trFormat("Original: %s", materializr::fmtLength(m_pick.bottomR * 2.0)).c_str());
        ImGui::TextDisabled("%s", materializr::trFormat("Top stays at %s — drag this end to make a cone.", materializr::fmtLength(m_pick.topR * 2.0)).c_str());
    } else {
        ImGui::TextUnformatted(materializr::trFormat("Original: %s", materializr::fmtLength(m_pick.topR * 2.0)).c_str());
        ImGui::TextDisabled("%s", materializr::trFormat("Bottom stays at %s — drag this end to make a cone.", materializr::fmtLength(m_pick.bottomR * 2.0)).c_str());
    }

    if (m_inputFocus) {
        ImGui::SetKeyboardFocusHere();
        m_inputFocus = false;
    }

    // Drive one buffer; mirror into the other when face-editing both ends.
    char*   buf = m_pick.editBottom ? m_botBuf : m_topBuf;
    double* val = m_pick.editBottom ? &m_newBottomDiameter : &m_newTopDiameter;

    double parsed = *val;
    bool edited = false;
    if (ctx.cornerCommitUi) {
        // im-touch: number-pad amount field — no InputText, no native keyboard
        // (which froze the app on iOS).
        double v = *val;
        if (materializr::amountLengthField("rcylAmt", nullptr, &v, /*allowSign=*/false)) {
            parsed = v;
            edited = std::abs(parsed - *val) > 0.001;
            // v is millimetres; this buffer is read back with parseLength, i.e.
            // in DISPLAY units. "%.2f" wrote mm into it and also fixed the
            // precision at two decimals, quantising metres to 10 mm. Its
            // sibling ten lines down already used formatLengthDigits.
            materializr::formatLengthDigits(buf, 32, v);
        }
    } else {
        ImGui::SetNextItemWidth(140);
        // The member is the truth; the buffer follows unless being typed in.
        materializr::reseedLengthBufferIfIdle("##rcyldia", buf, 32, *val);
        if (ImGui::InputText("##rcyldia", buf, 32,
                             ImGuiInputTextFlags_EnterReturnsTrue))
            requestCommit();   // Enter in the field = Confirm
        // Only re-read while typing: an idle re-parse rewrote the model from
        // the buffer's rounded text, and reinterpreted stale text after a unit
        // switch. CharsDecimal dropped so a typed "2in" can reach parseLength.
        edited = materializr::lengthBufferIsActive("##rcyldia") &&
                 materializr::parseLength(buf, parsed) &&
                 std::abs(parsed - *val) > 0.001;
        ImGui::SameLine();
        ImGui::Text("%s", materializr::unitSuffix());
    }
    if (edited) {
        *val = parsed;
        if (bothEnds) {
            m_newBottomDiameter = parsed;
            m_newTopDiameter    = parsed;
            materializr::formatLengthDigits(m_pick.editBottom ? m_topBuf : m_botBuf, 32, parsed);
        }
        changed = true;
    }

    // Only complain once the user has actually asked for a different size.
    // buildOp returns nullptr for "unchanged", which the base reports as a
    // failed preview — so at the untouched original this warned about an
    // invalid diameter before anything had been typed.
    if (!previewOk() && !m_deferred && changedFromOriginal()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.35f, 1.0f), "%s", materializr::tr("Invalid diameter for this feature —\na hole can't exceed the surrounding wall."));
    }
    if (m_deferred) {
        ImGui::TextDisabled("%s", materializr::tr("Threaded body — applies on OK,\nthen the thread re-cuts in background."));
    }
}

void ResizeCylindricalController::onCleanup() {
    m_pick = CylindricalPick{};
    m_deferred = false;
    m_inputFocus = true;
}

// ─── Move Face ───────────────────────────────────────────────────────────────
// Slice 2: the gesture maths moves first — these four read nothing but the
// state, so they port with a rename and no behaviour change. The lifecycle
// (begin/update/commit) still runs on Application and calls back through
// the accessors until slice 3.

bool MoveFaceController::faceXformNontrivial() const {
    switch (m_st.faceXformKind) {
        case FaceXform::Translate: return glm::length(m_st.moveFaceVec) > 1e-4f;
        case FaceXform::Rotate:
            return m_st.moveFaceIsTwist
                ? std::abs(m_st.moveFaceTwist) > 1e-4f
                : (std::abs(m_st.moveFaceAngle) > 1e-4f || m_st.moveFaceRotHasAccum);
        case FaceXform::Scale:
            return m_st.moveFaceScaleUniform
                ? std::abs(m_st.moveFaceScale - 1.0f) > 1e-4f
                : (std::abs(m_st.moveFaceScaleA - 1.0f) > 1e-4f ||
                   std::abs(m_st.moveFaceScaleB - 1.0f) > 1e-4f);
    }
    return false;
}

glm::mat3 MoveFaceController::faceRotTotal() const {
    return rodrigues(m_st.moveFaceRotAxis, m_st.moveFaceAngle) * m_st.moveFaceRotAccum;
}

void MoveFaceController::bakeFaceRotationDrag() {
    // Twist isn't a tilt-matrix accumulation — nothing to bake for it.
    if (m_st.moveFaceIsTwist) return;
    if (m_st.faceXformKind != FaceXform::Rotate || std::abs(m_st.moveFaceAngle) < 1e-5f)
        return;
    m_st.moveFaceRotAccum = rodrigues(m_st.moveFaceRotAxis, m_st.moveFaceAngle) * m_st.moveFaceRotAccum;
    m_st.moveFaceRotHasAccum = true;
    m_st.moveFaceAngle = 0.0f;
    m_st.moveFaceAngleBase = 0.0f;
}

bool MoveFaceController::localTweakApplies() const {
    // Tilt only. A slide lands the plane back on itself, so the local rebuild
    // has literally nothing to solve; scale and twist aren't rigid transforms
    // of the face at all. The panel only offers the box under Rotate, but the
    // gesture can switch to a twist mid-session, so re-check it here.
    return m_st.moveFaceLocal && m_st.faceXformKind == FaceXform::Rotate &&
           !m_st.moveFaceIsTwist;
}

gp_Trsf MoveFaceController::faceTweakTrsf() const {
    // The same composed rotation configureFaceOp hands MoveFaceOp — live drag
    // stacked on the tilts already banked this session, about the face centre.
    const glm::mat3 R = faceRotTotal();
    const glm::vec3 t = m_st.moveFacePivot - R * m_st.moveFacePivot;
    gp_Trsf trsf;
    trsf.SetValues(R[0][0], R[1][0], R[2][0], t.x,
                   R[0][1], R[1][1], R[2][1], t.y,
                   R[0][2], R[1][2], R[2][2], t.z);
    return trsf;
}

bool MoveFaceController::applyLocalTweak(const IopContext& ctx) {
    m_st.moveFaceLocalRefusal = nullptr;
    if (m_st.moveFaceBodyId < 0 || m_st.moveFaceFace.IsNull()) return false;
    const auto r = materializr::tweak::moveFace(
        ctx.doc.getBody(m_st.moveFaceBodyId), m_st.moveFaceFace, faceTweakTrsf());
    if (!r.ok()) {
        m_st.moveFaceLocalRefusal = materializr::tweak::refusalText(r.refusal);
        return false;
    }
    ctx.doc.updateBody(m_st.moveFaceBodyId, r.shape);
    ctx.markMeshesDirty();
    return true;
}

void MoveFaceController::configureFaceOp(MoveFaceOp& op) const {
    switch (m_st.faceXformKind) {
        case FaceXform::Translate:
            op.setKind(MoveFaceOp::Kind::Translate);
            op.setMoveVector(gp_Vec(m_st.moveFaceVec.x, m_st.moveFaceVec.y, m_st.moveFaceVec.z));
            break;
        case FaceXform::Rotate: {
            if (m_st.moveFaceIsTwist) { // third ring = twist about the normal
                op.setKind(MoveFaceOp::Kind::Twist);
                op.setTwist(m_st.moveFaceTwist);
                break;
            }
            op.setKind(MoveFaceOp::Kind::Rotate);
            // Composed rotation (live drag ∘ accumulated tilts) as a gp_Trsf
            // about the pivot, so stacked tilts about both axes apply at once.
            glm::mat3 R = faceRotTotal();
            glm::vec3 Tt = m_st.moveFacePivot - R * m_st.moveFacePivot;
            gp_Trsf trsf;
            trsf.SetValues(R[0][0], R[1][0], R[2][0], Tt.x,
                           R[0][1], R[1][1], R[2][1], Tt.y,
                           R[0][2], R[1][2], R[2][2], Tt.z);
            op.setRotationExplicit(trsf);
            break;
        }
        case FaceXform::Scale:
            op.setKind(MoveFaceOp::Kind::Scale);
            if (m_st.moveFaceScaleUniform) {
                op.setScaleFactor(m_st.moveFaceScale);
            } else {
                op.setScaleNonUniform(
                    gp_Dir(m_st.moveFaceAxisA.x, m_st.moveFaceAxisA.y, m_st.moveFaceAxisA.z),
                    gp_Dir(m_st.moveFaceAxisB.x, m_st.moveFaceAxisB.y, m_st.moveFaceAxisB.z),
                    m_st.moveFaceScaleA, m_st.moveFaceScaleB);
            }
            break;
    }
    op.setLoopMotion(m_st.moveFaceMoveOuter, m_st.moveFaceHoleSlant, m_st.moveFaceHoleVertical);
}

// ─── Move Face: lifecycle (slice 2b) ────────────────────────────────────────
// Moved wholesale from Application_InteractiveOps. Everything these needed
// from the app — document, selection, history, toast, mesh refusal, grid
// snap — now arrives through IopContext, so the tool no longer reaches into
// a 28k-line class. They are still plain methods rather than base overrides:
// Move Face slides on-face sketches during the preview and RE-SELECTS the
// moved face on commit instead of clearing, and the base offers no hook for
// either. Adopting the base lifecycle is its own step.

void MoveFaceController::beginMoveFace(const IopContext& ctx, FaceXform kind) {
    if (ctx.refuseMesh("Move Face")) return;
    
    m_st.moveFaceActive = false;
    setActive(false); // keep the base flag — what the generic loops gate on — in step
    m_st.moveFaceBodyId = -1;
    m_st.moveFaceFace.Nullify();
    m_st.faceXformKind = kind;
    m_st.moveFaceVec = glm::vec3(0.0f);
    m_st.moveFaceBase = glm::vec3(0.0f);
    m_st.moveFaceAngle = m_st.moveFaceAngleBase = 0.0f;
    m_st.moveFaceRotAccum = glm::mat3(1.0f);
    m_st.moveFaceRotHasAccum = false;
    m_st.moveFaceTwist = m_st.moveFaceTwistBase = 0.0f;
    m_st.moveFaceIsTwist = false;
    m_st.moveFaceScale = m_st.moveFaceScaleBase = 1.0f;
    m_st.moveFaceScaleA = m_st.moveFaceScaleABase = 1.0f;
    m_st.moveFaceScaleB = m_st.moveFaceScaleBBase = 1.0f;
    m_st.moveFaceDragging = false;
    m_st.moveHoleMode = false;
    m_st.moveHoleWall.Nullify();

    // Hole move: if the Move selection is a recognizable THROUGH-HOLE wall, slide
    // the whole hole (MoveHoleOp) instead of shearing a face. buildVoid succeeds
    // only on a real hole wall (an outer face / block side fails it), so this
    // doesn't hijack ordinary Move Face. Translate only; pockets are refused.
    if (kind == FaceXform::Translate) {
        for (const auto& e : ctx.selection.getSelection()) {
            if (e.type != SelectionType::Face || e.shape.IsNull()) continue;
            TopoDS_Shape body;
            try { body = ctx.doc.getBody(e.bodyId); } catch (...) { continue; }
            if (body.IsNull()) continue;
            TopoDS_Face wall = TopoDS::Face(e.shape);
            TopoDS_Shape voidSolid; gp_Vec entryN; bool pocket = false;
            TopoDS_Wire rim;
            if (MoveHoleOp::buildVoid(body, wall, voidSolid, entryN, pocket, &rim)) {
                // Gizmo set-up at the hole: plane = entry face, translate only.
                m_st.moveHoleMode = true;
                m_st.moveHoleOpMode = MoveHoleOp::Mode::Slide;
                m_st.moveHoleRimEdge = TopoDS_Edge();
                m_st.moveHoleWall = wall;
                m_st.moveFaceBodyId = e.bodyId;
                m_st.moveFacePreviousShape = body;
                m_st.moveFaceN = glm::normalize(glm::vec3(entryN.X(), entryN.Y(), entryN.Z()));
                try {
                    GProp_GProps gp; BRepGProp::SurfaceProperties(wall, gp);
                    gp_Pnt c = gp.CentreOfMass();
                    m_st.moveFaceP0 = m_st.moveFacePivot = glm::vec3(c.X(), c.Y(), c.Z());
                } catch (...) { m_st.moveFaceP0 = m_st.moveFacePivot = glm::vec3(0.0f); }
                // Same canonical basis as the rim-edge path (PlaneAxes.h). The
                // old cross(N, A) construction flips with the ENTRY NORMAL'S
                // SIGN, and buildVoid's walk order decides that sign — a
                // top-facing hole (N = world +Y) got its blue arrow along -Z,
                // i.e. pointing the opposite way to the main gizmo's blue.
                // Which of the two hole paths ran depended on whether the
                // click landed on the rim EDGE or the WALL face, which is why
                // the reversal seemed to come and go per selection. This
                // branch is translate-only, so no rotate ring needs the
                // handedness the canonical basis gives up.
                materializr::inPlaneAxes(m_st.moveFaceN, m_st.moveFaceAxisA,
                                         m_st.moveFaceAxisB);
                m_st.moveFaceGrab = -1;
                m_st.moveFaceHalfExtent = 1.0f;
                // Move highlight: the hole's top rim, sampled as a world-space
                // polyline in loop[0] so the existing yellow-silhouette renderer
                // draws it following the drag (m_st.moveFaceMoveOuter → loop[0]
                // translates by the move vector). No hole sub-loops.
                m_st.moveFaceSilhouetteLoops.clear();
                m_st.moveFaceHoleSlant.clear();
                m_st.moveFaceHoleVertical.clear();
                m_st.moveFaceMoveOuter = true;
                m_st.moveFacePendingRebuild = false;
                if (!rim.IsNull()) {
                    std::vector<glm::vec3> pts;
                    for (BRepTools_WireExplorer we(rim); we.More(); we.Next()) {
                        BRepAdaptor_Curve crv(we.Current());
                        double f = crv.FirstParameter(), l = crv.LastParameter();
                        if (we.Current().Orientation() == TopAbs_REVERSED) std::swap(f, l);
                        const int Nseg = 16;
                        for (int i = 0; i < Nseg; ++i) {
                            gp_Pnt p = crv.Value(f + (l - f) * (double(i) / Nseg));
                            pts.emplace_back(p.X(), p.Y(), p.Z());
                        }
                    }
                    if (!pts.empty()) m_st.moveFaceSilhouetteLoops.push_back(pts);
                }
                m_st.moveFaceActive = true;
                setActive(true);
                return;
            }
            // Refuse ONLY when the pick is plausibly a bore wall. buildVoid
            // reports "one mouth" for plenty of ordinary faces — a solid
            // cylinder's flat top cap among them — and this used to toast and
            // RETURN on all of them, so selecting a cylinder's top face and
            // pressing Move refused with a message about holes instead of
            // moving the face (Steve, 2026-08-04). A bore wall is curved; a
            // cap is planar, so planar picks fall through to the ordinary face
            // move. Square-hole walls are planar too and still reach the
            // whole-hole slide above, because for them buildVoid SUCCEEDS.
            if (pocket && !faceIsPlanar(wall)) {
                ctx.toast("Only simple through-holes can be moved for now "
                          "\xE2\x80\x94 not pockets, countersunk, or stepped holes.");
                return;
            }
        }
    }

    // Sort the selection: the first PLANAR face slides (the moving face); every
    // OTHER selected face is a candidate hole WALL (move that hole as a straight
    // tube); selected EDGES are hole top rings (slant). Walls are matched to
    // hole loops by shared edges below — NOT by surface type, because after any
    // face op the wall is a ruled loft surface, not an analytic cylinder.
    std::vector<TopoDS_Face> selectedFaces;
    std::vector<TopoDS_Edge> selectedEdges;
    for (const auto& e : ctx.selection.getSelection()) {
        if (e.shape.IsNull()) continue;
        if (e.type == SelectionType::Face) {
            TopoDS_Face f = TopoDS::Face(e.shape);
            Handle(Geom_Surface) s = BRep_Tool::Surface(f);
            if (m_st.moveFaceFace.IsNull() && !s.IsNull() &&
                s->IsKind(STANDARD_TYPE(Geom_Plane))) {
                m_st.moveFaceBodyId = e.bodyId;
                m_st.moveFaceFace = f;
            } else {
                selectedFaces.push_back(f); // potential hole wall
            }
        } else if (e.type == SelectionType::Edge) {
            selectedEdges.push_back(TopoDS::Edge(e.shape));
        }
    }
    if (m_st.moveFaceBodyId < 0 || m_st.moveFaceFace.IsNull()) return;
    // Drop the chosen moving face from the wall candidates if it slipped in.
    std::vector<TopoDS_Face> selectedCylinders;
    for (const auto& f : selectedFaces)
        if (!f.IsSame(m_st.moveFaceFace)) selectedCylinders.push_back(f);

    // Move Face only makes sense on a FLAT face (the shear pins one plane and
    // slides another). A curved face (cylinder side, fillet, sphere) has no
    // single plane to slide, so refuse with guidance instead of shearing junk.
    {
        Handle(Geom_Surface) surf = BRep_Tool::Surface(m_st.moveFaceFace);
        if (surf.IsNull() || !surf->IsKind(STANDARD_TYPE(Geom_Plane))) {
            std::fprintf(stderr, "[MoveFace] declined: select a FLAT face\n");
            ctx.toast("Move Face needs a flat face - pick a planar face.");
            return;
        }
    }

    try { m_st.moveFacePreviousShape = ctx.doc.getBody(m_st.moveFaceBodyId); }
    catch (...) { return; }

    // (The loft rebuild now lofts the outer loop AND subtracts a loft of each
    // hole loop, so holed faces are allowed. Freeform / boolean bodies that
    // crashed the old shear are handled safely too: the op only lofts local
    // wires and refuses gracefully on release if the body isn't a clean prism —
    // no crash.)

    // Face plane (orientation-corrected outward normal + a point on it).
    try {
        BRepGProp_Face prop(m_st.moveFaceFace);
        double u1, u2, v1, v2;
        prop.Bounds(u1, u2, v1, v2);
        gp_Pnt c; gp_Vec n;
        prop.Normal((u1 + u2) * 0.5, (v1 + v2) * 0.5, c, n);
        if (n.Magnitude() < 1e-9) return;
        n.Normalize();
        m_st.moveFaceP0 = glm::vec3(c.X(), c.Y(), c.Z());
        m_st.moveFaceN  = glm::vec3(n.X(), n.Y(), n.Z());
        // Pivot for Rotate/Scale = the face's area centroid (its "middle").
        GProp_GProps gp; BRepGProp::SurfaceProperties(m_st.moveFaceFace, gp);
        gp_Pnt ctr = gp.CentreOfMass();
        m_st.moveFacePivot = glm::vec3(ctr.X(), ctr.Y(), ctr.Z());
    } catch (...) { return; }

    // Two in-plane arrow axes: project the world axis least aligned with N into
    // the face plane → A; B = N × A. A box top gets clean world-aligned arrows.
    {
        const glm::vec3 N = m_st.moveFaceN;
        if (kind == FaceXform::Translate) {
            // Slide: canonical basis (core/PlaneAxes.h). B = N x A is HANDED,
            // so it flips with the face normal's sign — and a normal's sign is
            // incidental. That put one arrow along a NEGATIVE world axis on
            // half the orientations, which is what "the arrow does nothing / is
            // reversed" kept meaning. Same fix the hole path got in 64a0c7f.
            inPlaneAxes(N, m_st.moveFaceAxisA, m_st.moveFaceAxisB);
        } else {
            // Rotate and Scale KEEP the handed basis: the ring sweep is
            // computed so that rotAxis x u = +N for both rings, so A and B
            // must stay right-handed about the normal or the red ring's
            // direction reads inverted against the green one.
            glm::vec3 ref = (std::abs(N.x) < 0.9f) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            glm::vec3 A = ref - glm::dot(ref, N) * N;
            if (glm::length(A) < 1e-5f) {
                ref = glm::vec3(0, 0, 1);
                A = ref - glm::dot(ref, N) * N;
            }
            m_st.moveFaceAxisA = glm::normalize(A);
            m_st.moveFaceAxisB = glm::normalize(glm::cross(N, m_st.moveFaceAxisA));
        }
    }
    m_st.moveFaceGrab = -1;
    m_st.moveFaceRotAxis = m_st.moveFaceAxisB; // default tilt axis until a ring is grabbed

    // Sketches sitting ON this face slide along with it. Coincident = plane
    // parallel to the face AND lying on it (same offset). Snapshot their planes
    // so the live preview / cancel can restore them.
    m_st.moveFaceSketchIds.clear();
    m_st.moveFaceSketchPlanes0.clear();
    {
        gp_Vec fN(m_st.moveFaceN.x, m_st.moveFaceN.y, m_st.moveFaceN.z);
        gp_Pnt fP(m_st.moveFaceP0.x, m_st.moveFaceP0.y, m_st.moveFaceP0.z);
        for (int sid : ctx.doc.getAllSketchIds()) {
            auto sk = ctx.doc.getSketch(sid);
            if (!sk) continue;
            const gp_Pln& sp = sk->getPlane();
            gp_Vec sN(sp.Axis().Direction());
            if (std::abs(sN.Dot(fN)) < 0.999) continue; // not parallel
            gp_Vec d(sp.Location().X() - fP.X(), sp.Location().Y() - fP.Y(),
                     sp.Location().Z() - fP.Z());
            if (std::abs(d.Dot(fN)) > 0.05) continue;   // not on the face plane
            m_st.moveFaceSketchIds.push_back(sid);
            m_st.moveFaceSketchPlanes0.push_back(sp);
        }
    }

    // Each loop of the face (outer outline first, then hole loops) captured as a
    // world-space polyline for the drag-time ghost, plus a per-hole "vertical"
    // flag (default false = slants). Loop order MUST match the op's enumeration
    // (OuterWire, then TopExp wires) so flags + ghost line up.
    m_st.moveFaceSilhouetteLoops.clear();
    m_st.moveFaceHoleSlant.clear();
    m_st.moveFaceHoleVertical.clear();
    m_st.moveFaceMoveOuter = true; // a planar face is selected → the outline slides
    m_st.moveFacePendingRebuild = false;
    std::vector<TopoDS_Wire> innerWires;
    try {
        // Walk edges in CONNECTED order (WireExplorer) so the polyline doesn't
        // zig-zag into a bowtie the way TopExp_Explorer's arbitrary order did.
        auto sampleWire = [](const TopoDS_Wire& w) {
            std::vector<glm::vec3> pts;
            for (BRepTools_WireExplorer we(w); we.More(); we.Next()) {
                const TopoDS_Edge& e = we.Current();
                BRepAdaptor_Curve crv(e);
                double f = crv.FirstParameter(), l = crv.LastParameter();
                if (e.Orientation() == TopAbs_REVERSED) std::swap(f, l);
                const int Nseg = 12;
                for (int i = 0; i < Nseg; ++i) {
                    gp_Pnt p = crv.Value(f + (l - f) * (double(i) / Nseg));
                    pts.emplace_back(p.X(), p.Y(), p.Z());
                }
            }
            return pts;
        };
        TopoDS_Wire outer = BRepTools::OuterWire(m_st.moveFaceFace);
        if (!outer.IsNull())
            m_st.moveFaceSilhouetteLoops.push_back(sampleWire(outer));
        for (TopExp_Explorer wx(m_st.moveFaceFace, TopAbs_WIRE); wx.More(); wx.Next()) {
            TopoDS_Wire w = TopoDS::Wire(wx.Current());
            if (w.IsSame(outer)) continue;
            m_st.moveFaceSilhouetteLoops.push_back(sampleWire(w));
            m_st.moveFaceHoleSlant.push_back(false);    // stays put until opted in
            m_st.moveFaceHoleVertical.push_back(false);
            innerWires.push_back(w);
        }

        // hole index whose inner wire contains the given edge, else -1.
        auto holeOfEdge = [&](const TopoDS_Edge& edge) -> int {
            for (size_t hi = 0; hi < innerWires.size(); ++hi)
                for (TopExp_Explorer we(innerWires[hi], TopAbs_EDGE); we.More(); we.Next())
                    if (edge.IsSame(we.Current())) return static_cast<int>(hi);
            return -1;
        };
        // Cylinder wall picked → that hole moves as a straight tube (vertical).
        for (const TopoDS_Face& cyl : selectedCylinders) {
            for (TopExp_Explorer ce(cyl, TopAbs_EDGE); ce.More(); ce.Next()) {
                int hi = holeOfEdge(TopoDS::Edge(ce.Current()));
                if (hi >= 0) { m_st.moveFaceHoleVertical[hi] = true; break; }
            }
        }
        // Hole top edge picked → that hole slants (top ring follows).
        for (const TopoDS_Edge& edge : selectedEdges) {
            int hi = holeOfEdge(edge);
            if (hi >= 0) m_st.moveFaceHoleSlant[hi] = true;
        }
    } catch (...) {
        m_st.moveFaceSilhouetteLoops.clear();
        m_st.moveFaceHoleSlant.clear();
        m_st.moveFaceHoleVertical.clear();
    }

    // Face half-extent (max distance pivot→outline) so a drag of ~that length
    // maps to ≈1 rad of tilt / a unit of scale — a size-independent feel.
    m_st.moveFaceHalfExtent = 1.0f;
    if (!m_st.moveFaceSilhouetteLoops.empty()) {
        float mx = 0.0f;
        for (const auto& p : m_st.moveFaceSilhouetteLoops[0])
            mx = std::max(mx, glm::length(p - m_st.moveFacePivot));
        if (mx > 1e-3f) m_st.moveFaceHalfExtent = mx;
    }

    // Hollow (shelled) body: the per-frame preview refuses (the loft engine
    // can't shear a cavity), so the body won't follow the drag — but the
    // commit reflows beneath the Shell and lands correctly. Say so up front
    // instead of looking broken.
    if (ctx.history.isBodyShelled(m_st.moveFaceBodyId))
        ctx.toast("Hollow body: the preview stays put \xE2\x80\x94 the change "
                  "applies when you release (re-shelled automatically).");

    m_st.moveFaceActive = true;
    setActive(true);
}

bool MoveFaceController::beginMoveHoleFromEdges(const IopContext& ctx) {
    std::vector<TopoDS_Edge> picked;
    int bodyId = -1;
    for (const auto& e : ctx.selection.getSelection()) {
        if (e.type != SelectionType::Edge || e.shape.IsNull()) continue;
        if (bodyId >= 0 && e.bodyId != bodyId) return false;  // one hole only
        bodyId = e.bodyId;
        picked.push_back(TopoDS::Edge(e.shape));
    }
    if (picked.empty() || bodyId < 0) return false;
    if (ctx.refuseMesh("Move")) return true;   // handled: refused

    TopoDS_Shape body;
    try { body = ctx.doc.getBody(bodyId); } catch (...) { return false; }
    if (body.IsNull()) return false;

    const MoveHoleOp::EdgePick pick = MoveHoleOp::classifyRimEdges(body, picked);
    if (!pick.ok) return false;      // not one hole's rim → caller falls through

    
    m_st.faceXformKind = FaceXform::Translate;
    m_st.moveFaceVec = m_st.moveFaceBase = glm::vec3(0.0f);
    m_st.moveFaceDragging = false;
    m_st.moveFaceActive = true;
    setActive(true);
    m_st.moveHoleMode = true;
    m_st.moveHoleOpMode = pick.mode;
    m_st.moveHoleRimEdge = pick.rimEdge;
    m_st.moveHoleNearIsEntry = pick.nearIsEntry;
    m_st.moveHoleWall = pick.wall;
    m_st.moveFaceBodyId = bodyId;
    m_st.moveFacePreviousShape = body;

    // Sample a wire into a world-space polyline: used both to place the gizmo
    // (its centroid) and to draw the yellow move highlight.
    auto sampleEdge = [](const TopoDS_Edge& e, std::vector<glm::vec3>& out) {
        BRepAdaptor_Curve crv(e);
        double f = crv.FirstParameter(), l = crv.LastParameter();
        if (e.Orientation() == TopAbs_REVERSED) std::swap(f, l);
        const int Nseg = 16;
        for (int i = 0; i < Nseg; ++i) {
            gp_Pnt p = crv.Value(f + (l - f) * (double(i) / Nseg));
            out.emplace_back(p.X(), p.Y(), p.Z());
        }
    };
    // Wires must be walked in CONNECTION order (BRepTools_WireExplorer), not
    // TopExp order, or the highlight polyline zig-zags across the rim.
    auto sampleWire = [&](const TopoDS_Wire& w, std::vector<glm::vec3>& out) {
        for (BRepTools_WireExplorer we(w); we.More(); we.Next())
            sampleEdge(we.Current(), out);
    };

    // Slide the rim IN ITS OWN PLANE — the entry normal from buildVoid is the
    // plane the drag lives in, exactly as the face-driven path uses it.
    TopoDS_Shape v; gp_Vec n; bool pocket = false;
    TopoDS_Wire entryRim, exitRim;
    if (MoveHoleOp::buildVoid(body, pick.wall, v, n, pocket, &entryRim, &exitRim)) {
        m_st.moveFaceN = glm::normalize(glm::vec3(n.X(), n.Y(), n.Z()));
    }

    // Anchor the gizmo on what is actually being dragged. Without this the
    // whole block below never ran and the gizmo drew at the world origin, far
    // from the hole — P0/pivot/axes kept their defaults.
    const TopoDS_Wire& nearRim = pick.nearIsEntry ? entryRim : exitRim;
    std::vector<glm::vec3> handle;   // the thing the user grabbed
    if (pick.mode == MoveHoleOp::Mode::EdgeMove && !pick.rimEdge.IsNull())
        sampleEdge(pick.rimEdge, handle);       // that one side
    else if (!nearRim.IsNull())
        sampleWire(nearRim, handle);            // that rim
    else if (!entryRim.IsNull())
        sampleWire(entryRim, handle);           // Slide: either mouth will do

    if (!handle.empty()) {
        glm::vec3 c(0.0f);
        for (const glm::vec3& p : handle) c += p;
        c /= float(handle.size());
        m_st.moveFaceP0 = m_st.moveFacePivot = c;
        // Gizmo scale: half the grabbed loop's own extent, so it reads as part
        // of the hole rather than swamping a 3 mm bore.
        float r = 0.0f;
        for (const glm::vec3& p : handle) r = std::max(r, glm::length(p - c));
        m_st.moveFaceHalfExtent = std::max(0.5f, r);
    } else {
        m_st.moveFaceP0 = m_st.moveFacePivot = glm::vec3(0.0f);
        m_st.moveFaceHalfExtent = 1.0f;
    }

    // In-plane axes, CANONICAL. The face path derives axis B as cross(N, A),
    // which flips sign with N — and buildVoid's entry normal points whichever
    // way its walk happened to go, so an identical hole gave a green arrow
    // along +Y or -Y depending on which mouth it called the entry. That reads
    // as reversed controls (it bit x/y holes, where the entry resolved to the
    // underside). Instead take the two WORLD axes most perpendicular to the
    // rim plane, in X→Y→Z order, always positively oriented: the arrows then
    // point along +X/+Y/+Z whichever rim you grab, matching the red/green/blue
    // the gizmo colours them. Translate-only, so handedness doesn't matter.
    materializr::inPlaneAxes(m_st.moveFaceN, m_st.moveFaceAxisA, m_st.moveFaceAxisB);
    m_st.moveFaceGrab = -1;

    // Highlight exactly what moves: the grabbed side for EdgeMove, the near rim
    // for Tilt, so the drag doesn't lie about which end is pinned.
    m_st.moveFaceSilhouetteLoops.clear();
    m_st.moveFaceHoleSlant.clear();
    m_st.moveFaceHoleVertical.clear();
    m_st.moveFaceMoveOuter = true;
    m_st.moveFacePendingRebuild = false;
    if (!handle.empty()) m_st.moveFaceSilhouetteLoops.push_back(handle);

    std::fprintf(stdout, "Hole move armed from rim edges: %s\n",
                 pick.mode == MoveHoleOp::Mode::Tilt     ? "tilt" :
                 pick.mode == MoveHoleOp::Mode::EdgeMove ? "edge" : "slide");
    return true;
}

void MoveFaceController::updateMoveFace(const IopContext& ctx) {
    if (!m_st.moveFaceActive || m_st.moveFaceBodyId < 0) return;

    // Hole-move preview: re-cut the hole at the dragged position each frame.
    if (m_st.moveHoleMode) {
        ctx.doc.updateBody(m_st.moveFaceBodyId, m_st.moveFacePreviousShape);
        ctx.markMeshesDirty();
        gp_Vec mv(m_st.moveFaceVec.x, m_st.moveFaceVec.y, m_st.moveFaceVec.z);
        if (mv.Magnitude() < 1e-9) return;
        try {
            MoveHoleOp op;
            op.setBody(m_st.moveFaceBodyId);
            op.setSeedWall(m_st.moveHoleWall);
            // The PREVIEW has to run the same verb as the commit. It used to
            // build a bare op, which defaults to Slide, so every drag showed the
            // whole hole moving no matter what the selection picked — and then
            // the result jumped to a tilt/reshape on release.
            op.setMode(m_st.moveHoleOpMode);
            op.setNearIsEntry(m_st.moveHoleNearIsEntry);
            if (m_st.moveHoleOpMode == MoveHoleOp::Mode::EdgeMove)
                op.setRimEdge(m_st.moveHoleRimEdge);
            op.setMoveVector(mv);
            if (!op.execute(ctx.doc))
                ctx.doc.updateBody(m_st.moveFaceBodyId, m_st.moveFacePreviousShape);
            ctx.markMeshesDirty();
        } catch (...) {
            ctx.doc.updateBody(m_st.moveFaceBodyId, m_st.moveFacePreviousShape);
        }
        return;
    }

    // Snap an in-plane face SLIDE to the grid step (issue #24): decompose the
    // translation onto the face's in-plane axes and round each to the step, so
    // the face moves in grid increments (like Extrude/Push-Pull). Only for a
    // Translate — Rotate has its own degree snap and Scale is a percentage.
    // m_st.moveFaceVec is recomputed absolutely from the drag each frame, so this
    // never compounds.
    if (m_st.faceXformKind == FaceXform::Translate && ctx.snapToGrid &&
        ctx.gridStep > 0.0f) {
        const float step = ctx.gridStep;
        const float a = std::round(glm::dot(m_st.moveFaceVec, m_st.moveFaceAxisA) / step) * step;
        const float b = std::round(glm::dot(m_st.moveFaceVec, m_st.moveFaceAxisB) / step) * step;
        m_st.moveFaceVec = a * m_st.moveFaceAxisA + b * m_st.moveFaceAxisB;
    }

    // Always preview from the original snapshot so transforms don't compound.
    ctx.doc.updateBody(m_st.moveFaceBodyId, m_st.moveFacePreviousShape);
    ctx.markMeshesDirty();
    if (!faceXformNontrivial()) { moveFaceSlideSketches(ctx, glm::vec3(0.0f)); return; }
    // Local tilt previews through the FaceTweak engine directly (no op needed —
    // the preview only has to put a shape on the document). A refusal leaves the
    // body on its snapshot and the reason on the state for the panel; it is not
    // quietly retried as a shear, because the two produce different bodies and
    // the user picked one.
    if (localTweakApplies()) {
        if (!applyLocalTweak(ctx))
            ctx.doc.updateBody(m_st.moveFaceBodyId, m_st.moveFacePreviousShape);
        ctx.markMeshesDirty();
        return;
    }
    try {
        auto op = std::make_unique<MoveFaceOp>();
        op->setBody(m_st.moveFaceBodyId);
        op->setFace(m_st.moveFaceFace);
        configureFaceOp(*op);
        if (!op->execute(ctx.doc))
            ctx.doc.updateBody(m_st.moveFaceBodyId, m_st.moveFacePreviousShape);
        // Sketch follow in the preview is translate-only for now (rotate/scale
        // sketches still follow on commit via the op's own transform).
        if (m_st.faceXformKind == FaceXform::Translate) moveFaceSlideSketches(ctx, m_st.moveFaceVec);
        ctx.markMeshesDirty();
    } catch (...) {
        ctx.doc.updateBody(m_st.moveFaceBodyId, m_st.moveFacePreviousShape);
    }
}

void MoveFaceController::commitMoveFace(const IopContext& ctx) {
    if (!m_st.moveFaceActive) { return; }

    // Hole-move commit: restore the snapshot, then push one MoveHoleOp.
    if (m_st.moveHoleMode) {
        if (m_st.moveFaceBodyId >= 0 && !m_st.moveFacePreviousShape.IsNull())
            ctx.doc.updateBody(m_st.moveFaceBodyId, m_st.moveFacePreviousShape);
        gp_Vec mv(m_st.moveFaceVec.x, m_st.moveFaceVec.y, m_st.moveFaceVec.z);
        if (mv.Magnitude() > 1e-9 && m_st.moveFaceBodyId >= 0 && !m_st.moveHoleWall.IsNull()) {
            auto op = std::make_unique<MoveHoleOp>();
            op->setBody(m_st.moveFaceBodyId);
            op->setSeedWall(m_st.moveHoleWall);
            op->setMode(m_st.moveHoleOpMode);
            op->setNearIsEntry(m_st.moveHoleNearIsEntry);
            if (m_st.moveHoleOpMode == MoveHoleOp::Mode::EdgeMove)
                op->setRimEdge(m_st.moveHoleRimEdge);
            op->setMoveVector(mv);
            if (ctx.history.pushOperation(std::move(op), ctx.doc))
                std::fprintf(stdout, "Hole move committed\n");
        }
        // The wall face identity changed; clear selection rather than chase it.
        ctx.selection.clear();
        m_st.moveHoleMode = false;
        m_st.moveFaceActive = false;
        setActive(false);
        m_st.moveHoleWall.Nullify();
        ctx.markMeshesDirty();
        return;
    }

    // Restore the original body + sketch planes before the real op runs (it
    // snapshots the body from the doc and re-applies the slide atomically).
    if (m_st.moveFaceBodyId >= 0 && !m_st.moveFacePreviousShape.IsNull())
        ctx.doc.updateBody(m_st.moveFaceBodyId, m_st.moveFacePreviousShape);
    moveFaceSlideSketches(ctx, glm::vec3(0.0f)); // restore sketches to snapshot

    bool committed = false;
    if (localTweakApplies() && faceXformNontrivial() && m_st.moveFaceBodyId >= 0 &&
        !m_st.moveFaceFace.IsNull()) {
        auto op = std::make_unique<FaceTweakOp>();
        op->setBody(m_st.moveFaceBodyId);
        op->setFace(m_st.moveFaceFace);
        op->setTransform(faceTweakTrsf());
        committed = ctx.history.pushOperation(std::move(op), ctx.doc);
        std::fprintf(stdout, committed ? "Local face tilt committed\n"
                                       : "Local face tilt refused\n");
    } else if (faceXformNontrivial() && m_st.moveFaceBodyId >= 0 && !m_st.moveFaceFace.IsNull()) {
        auto op = std::make_unique<MoveFaceOp>();
        op->setBody(m_st.moveFaceBodyId);
        op->setFace(m_st.moveFaceFace);
        configureFaceOp(*op);
        op->setSketchIds(m_st.moveFaceSketchIds); // on-face sketches ride along
        committed = ctx.history.pushOperation(std::move(op), ctx.doc);
        if (committed)
            std::fprintf(stdout, "Face %s committed\n",
                         (m_st.faceXformKind == FaceXform::Rotate && m_st.moveFaceIsTwist) ? "twist"
                         : m_st.faceXformKind == FaceXform::Rotate ? "tilt"
                         : m_st.faceXformKind == FaceXform::Scale ? "scale" : "move");
    }

    // Re-select the moved face in the REBUILT body, so the highlight + the next
    // op use live geometry. Without this the selection keeps the stale old-
    // position face: the highlight lingers there, and a chained op lofts from
    // that old wire (lands the body back where it started = "the op got undone").
    if (committed) {
        glm::vec3 want = m_st.moveFacePivot; // where the face centre ends up
        if (m_st.faceXformKind == FaceXform::Translate) want += m_st.moveFaceVec;
        TopoDS_Shape nb = ctx.doc.getBody(m_st.moveFaceBodyId);
        TopoDS_Face best; double bestD = 1e300;
        for (TopExp_Explorer fx(nb, TopAbs_FACE); fx.More(); fx.Next()) {
            TopoDS_Face f = TopoDS::Face(fx.Current());
            Handle(Geom_Surface) s = BRep_Tool::Surface(f);
            if (s.IsNull() || !s->IsKind(STANDARD_TYPE(Geom_Plane))) continue;
            try {
                GProp_GProps gp; BRepGProp::SurfaceProperties(f, gp);
                gp_Pnt c = gp.CentreOfMass();
                double d = glm::length(glm::vec3(c.X(), c.Y(), c.Z()) - want);
                if (d < bestD) { bestD = d; best = f; }
            } catch (...) {}
        }
        if (!best.IsNull()) {
            SelectionEntry entry;
            entry.type = SelectionType::Face;
            entry.bodyId = m_st.moveFaceBodyId;
            entry.shape = best;
            ctx.selection.select(entry);
        } else {
            ctx.selection.clear(); // fall back to clearing if we can't re-find it
        }
    }
    m_st.moveFaceSketchIds.clear();
    m_st.moveFaceSketchPlanes0.clear();
    m_st.moveFaceActive = false;
    setActive(false);
    m_st.moveHoleMode = false;
    m_st.moveHoleWall.Nullify();
    m_st.moveFaceBodyId = -1;
    m_st.moveFaceFace.Nullify();
    m_st.moveFacePreviousShape.Nullify();
    m_st.moveFaceVec = glm::vec3(0.0f);
    m_st.moveFaceBase = glm::vec3(0.0f);
    m_st.moveFaceDragging = false;
    m_st.moveFaceSilhouetteLoops.clear();
    m_st.moveFaceHoleSlant.clear();
    m_st.moveFaceHoleVertical.clear();
    m_st.moveFaceMoveOuter = true;
    m_st.moveFacePendingRebuild = false;
    ctx.markMeshesDirty();
}

void MoveFaceController::cancelMoveFace(const IopContext& ctx) {
    if (!m_st.moveFaceActive) return;
    if (m_st.moveFaceBodyId >= 0 && !m_st.moveFacePreviousShape.IsNull())
        ctx.doc.updateBody(m_st.moveFaceBodyId, m_st.moveFacePreviousShape);
    moveFaceSlideSketches(ctx, glm::vec3(0.0f)); // restore sketches to snapshot
    m_st.moveFaceSketchIds.clear();
    m_st.moveFaceSketchPlanes0.clear();
    m_st.moveFaceActive = false;
    setActive(false);
    m_st.moveHoleMode = false;
    m_st.moveHoleWall.Nullify();
    m_st.moveFaceBodyId = -1;
    m_st.moveFaceFace.Nullify();
    m_st.moveFacePreviousShape.Nullify();
    m_st.moveFaceVec = glm::vec3(0.0f);
    m_st.moveFaceBase = glm::vec3(0.0f);
    m_st.moveFaceDragging = false;
    m_st.moveFaceSilhouetteLoops.clear();
    m_st.moveFaceHoleSlant.clear();
    m_st.moveFaceHoleVertical.clear();
    m_st.moveFaceMoveOuter = true;
    m_st.moveFacePendingRebuild = false;
    ctx.markMeshesDirty();
}

void MoveFaceController::moveFaceSlideSketches(const IopContext& ctx, const glm::vec3& v) {
    gp_Trsf t;
    t.SetTranslation(gp_Vec(v.x, v.y, v.z));
    for (size_t i = 0; i < m_st.moveFaceSketchIds.size(); ++i) {
        if (auto sk = ctx.doc.getSketch(m_st.moveFaceSketchIds[i])) {
            gp_Pln p = m_st.moveFaceSketchPlanes0[i];
            if (v.x != 0.0f || v.y != 0.0f || v.z != 0.0f) p.Transform(t);
            sk->setPlane(p);
        }
    }
}

// The drag. Intersect the cursor ray with the face's plane, latch the
// nearest handle at drag start (ring-aware for Rotate), then track the
// gesture: slide along the latched axis, sweep a ring, twist, or scale.
// The body does NOT rebuild mid-drag — only the ghost silhouette moves
// (drawOverlay below); the rebuild runs once on release.
void MoveFaceController::onViewportInput(const IopViewport& vp,
                                         const IopContext& ctx) {
    if (!m_st.moveFaceActive) return;
    if (vp.dragging) {
        const glm::vec3& ro = vp.rayOrigin;
        const glm::vec3& rd = vp.rayDir;
        float denom = glm::dot(rd, m_st.moveFaceN);
        if (std::abs(denom) > 1e-6f) {
            float t = glm::dot(m_st.moveFaceP0 - ro, m_st.moveFaceN) / denom;
            glm::vec3 hit = ro + rd * t;
            // Cursor angle around the pivot in a ring's rotation plane
            // (normal = rotAxis): intersect the ray with that plane and
            // measure atan2 in the (u, N) basis. Used for ring-sweep tilt.
            auto ringCursorAngle = [&](glm::vec3 rotAxis, glm::vec3 u) -> float {
                float dn = glm::dot(rd, rotAxis);
                if (std::abs(dn) < 1e-5f) return m_st.moveFaceRotStartAngle;
                float tt = glm::dot(m_st.moveFacePivot - ro, rotAxis) / dn;
                glm::vec3 d = (ro + rd * tt) - m_st.moveFacePivot;
                return std::atan2(glm::dot(d, m_st.moveFaceN), glm::dot(d, u));
            };
            // Twist ring lies IN the face plane (about the normal): the
            // cursor's angle is measured in the (axisA, axisB) basis of
            // the point where the ray meets the face plane.
            auto twistCursorAngle = [&]() -> float {
                float dn = glm::dot(rd, m_st.moveFaceN);
                if (std::abs(dn) < 1e-5f) return m_st.moveFaceTwistStart;
                float tt = glm::dot(m_st.moveFacePivot - ro, m_st.moveFaceN) / dn;
                glm::vec3 d = (ro + rd * tt) - m_st.moveFacePivot;
                return std::atan2(glm::dot(d, m_st.moveFaceAxisB),
                                  glm::dot(d, m_st.moveFaceAxisA));
            };
            if (!m_st.moveFaceDragging) {
                if (m_st.faceXformKind == FaceXform::Rotate) {
                    // Ring-aware latch: sample each ring's actual circle
                    // and grab the nearer one (the arrow-tip proxy used
                    // before biased toward one ring). Ring radius mirrors
                    // the gizmo: camDist * 0.15 * kRingRadius(0.75).
                    float ringR = 0.1125f *
                        glm::length(vp.camPos - m_st.moveFacePivot);
                    auto ringDist = [&](glm::vec3 u, glm::vec3 v) -> float {
                        float best = 1e18f;
                        for (int i = 0; i < 32; ++i) {
                            float a = 6.2831853f * i / 32.0f;
                            glm::vec3 p = m_st.moveFacePivot +
                                ringR * (std::cos(a) * u + std::sin(a) * v);
                            glm::vec2 s; if (!vp.toScreen(p, s)) continue;
                            float dx = s.x - vp.mouse.x, dy = s.y - vp.mouse.y;
                            best = std::min(best, dx * dx + dy * dy);
                        }
                        return best;
                    };
                    // grab 0 = ring about axis B (plane A,N); 1 = about A;
                    // 2 = ring about the NORMAL (plane A,B) = the twist.
                    float dRingB = ringDist(m_st.moveFaceAxisA, m_st.moveFaceN);
                    float dRingA = ringDist(m_st.moveFaceAxisB, m_st.moveFaceN);
                    float dRingN = ringDist(m_st.moveFaceAxisA, m_st.moveFaceAxisB);
                    m_st.moveFaceGrab = 0; float bestRing = dRingB;
                    if (dRingA < bestRing) { bestRing = dRingA; m_st.moveFaceGrab = 1; }
                    if (dRingN < bestRing) { bestRing = dRingN; m_st.moveFaceGrab = 2; }
                } else {
                    // Latch the arrow/cube whose shaft is nearest the cursor.
                    float armLen = 0.25f * glm::length(vp.camTarget - vp.camPos);
                    if (armLen < 1.0f) armLen = 8.0f;
                    auto sd = [&](glm::vec3 axis) -> float {
                        glm::vec2 s;
                        if (!vp.toScreen(m_st.moveFaceP0 + axis * armLen * 0.6f, s))
                            return 1e18f;
                        float dx = s.x - vp.mouse.x, dy = s.y - vp.mouse.y;
                        return dx * dx + dy * dy;
                    };
                    float dA = std::min(sd(m_st.moveFaceAxisA), sd(-m_st.moveFaceAxisA));
                    float dB = std::min(sd(m_st.moveFaceAxisB), sd(-m_st.moveFaceAxisB));
                    m_st.moveFaceGrab = (dA <= dB) ? 0 : 1;
                    // Once per gesture: which arrow latched and the
                    // frame it moves in. Left in on purpose — arrow
                    // no-ops are order-dependent and impossible to
                    // reconstruct after the fact without this.
                    std::fprintf(stdout,
                        "Move drag latch: grab=%c hole=%d A=(%.2f,%.2f,%.2f) "
                        "B=(%.2f,%.2f,%.2f) N=(%.2f,%.2f,%.2f) P0=(%.1f,%.1f,%.1f)\n",
                        m_st.moveFaceGrab == 0 ? 'A' : 'B', (int)m_st.moveHoleMode,
                        m_st.moveFaceAxisA.x, m_st.moveFaceAxisA.y, m_st.moveFaceAxisA.z,
                        m_st.moveFaceAxisB.x, m_st.moveFaceAxisB.y, m_st.moveFaceAxisB.z,
                        m_st.moveFaceN.x, m_st.moveFaceN.y, m_st.moveFaceN.z,
                        m_st.moveFaceP0.x, m_st.moveFaceP0.y, m_st.moveFaceP0.z);
                }
                m_st.moveFaceDragStart = hit;
                m_st.moveFaceBase = m_st.moveFaceVec;
                m_st.moveFaceAngleBase = m_st.moveFaceAngle;
                m_st.moveFaceScaleBase = m_st.moveFaceScale;
                m_st.moveFaceScaleABase = m_st.moveFaceScaleA;
                m_st.moveFaceScaleBBase = m_st.moveFaceScaleB;
                if (m_st.faceXformKind == FaceXform::Rotate) {
                    if (m_st.moveFaceGrab == 2) {
                        // Twist ring: latch the cursor's angle in the
                        // face plane; the twist tracks the sweep.
                        m_st.moveFaceIsTwist = true;
                        m_st.moveFaceTwistBase = m_st.moveFaceTwist;
                        m_st.moveFaceTwistStart = twistCursorAngle();
                    } else {
                        m_st.moveFaceIsTwist = false;
                        // Latch the tilt axis + the cursor's starting
                        // angle around the ring; the tilt tracks the sweep.
                        m_st.moveFaceRotAxis = (m_st.moveFaceGrab == 0)
                            ? m_st.moveFaceAxisB : m_st.moveFaceAxisA;
                        // u chosen so rotAxis × u = +N for BOTH rings (else
                        // the red ring's sweep reads inverted vs the green).
                        glm::vec3 u = (m_st.moveFaceGrab == 0)
                            ? -m_st.moveFaceAxisA : m_st.moveFaceAxisB;
                        m_st.moveFaceRotStartAngle =
                            ringCursorAngle(m_st.moveFaceRotAxis, u);
                    }
                }
                m_st.moveFaceDragging = true;
                setDraggingHandle(true);
            }
            glm::vec3 axis = (m_st.moveFaceGrab == 0) ? m_st.moveFaceAxisA
                                                      : m_st.moveFaceAxisB;
            float along = glm::dot(hit - m_st.moveFaceDragStart, axis);
            if (m_st.faceXformKind == FaceXform::Translate) {
                // Snap the slide to whole grid steps when the grid is on.
                if (ctx.snapToGrid && ctx.gridStep > 0.0f)
                    along = std::round(along / ctx.gridStep) * ctx.gridStep;
                m_st.moveFaceVec = m_st.moveFaceBase + axis * along;
            } else if (m_st.faceXformKind == FaceXform::Rotate && m_st.moveFaceIsTwist) {
                // Twist: sweep the cursor around the normal ring (in the
                // face plane). The change in its angle since drag-start
                // IS the twist about the normal.
                float cur = twistCursorAngle();
                float delta = cur - m_st.moveFaceTwistStart;
                delta = std::atan2(std::sin(delta), std::cos(delta)); // wrap ±π
                m_st.moveFaceTwist = m_st.moveFaceTwistBase + delta;
                if (m_st.moveFaceRotSnap) {
                    float step = 1.0f / 57.2957795f;
                    m_st.moveFaceTwist = std::round(m_st.moveFaceTwist / step) * step;
                }
            } else if (m_st.faceXformKind == FaceXform::Rotate) {
                // Sweep the cursor AROUND the ring: the tilt = the change
                // in the cursor's angle in the ring plane since the drag
                // started (a real rotation gizmo, not a linear pull).
                glm::vec3 u = (m_st.moveFaceGrab == 0) ? -m_st.moveFaceAxisA
                                                       : m_st.moveFaceAxisB;
                float cur = ringCursorAngle(m_st.moveFaceRotAxis, u);
                float delta = cur - m_st.moveFaceRotStartAngle;
                delta = std::atan2(std::sin(delta), std::cos(delta)); // wrap to ±π
                m_st.moveFaceAngle = m_st.moveFaceAngleBase + delta;
                if (m_st.moveFaceRotSnap) { // snap to whole degrees
                    float step = 1.0f / 57.2957795f;
                    m_st.moveFaceAngle = std::round(m_st.moveFaceAngle / step) * step;
                }
            } else { // Scale
                float ext = std::max(m_st.moveFaceHalfExtent, 1e-3f);
                if (m_st.moveFaceScaleUniform) {
                    m_st.moveFaceScale = std::max(0.1f,
                        m_st.moveFaceScaleBase + along / ext);
                } else if (m_st.moveFaceGrab == 0) { // axis A handle
                    m_st.moveFaceScaleA = std::max(0.1f,
                        m_st.moveFaceScaleABase + along / ext);
                } else {                          // axis B handle
                    m_st.moveFaceScaleB = std::max(0.1f,
                        m_st.moveFaceScaleBBase + along / ext);
                }
            }
            // Deferred: don't rebuild the body mid-drag — only the ghost
            // silhouette moves (drawOverlay). Flag a rebuild for release.
            m_st.moveFacePendingRebuild = true;
        }
    } else if (!vp.down) {
        // Released: now run the (single) rebuild so the body catches up
        // to where the silhouette was dragged.
        if (m_st.moveFacePendingRebuild) {
            bakeFaceRotationDrag(); // fold a ring drag into the accumulator
            updateMoveFace(ctx);
            m_st.moveFacePendingRebuild = false;
        }
        if (m_st.moveFaceDragging)
            std::fprintf(stdout, "Move drag release: vec=(%.2f,%.2f,%.2f)\n",
                         m_st.moveFaceVec.x, m_st.moveFaceVec.y, m_st.moveFaceVec.z);
        m_st.moveFaceDragging = false; // released — next drag re-latches
        m_st.moveFaceGrab = -1;
        setDraggingHandle(false);
    }
}

// Ghost silhouette: each moving face loop drawn as a yellow outline,
// transformed by the current gesture (slide / tilt / scale). During a drag
// this is the only thing that moves; the body rebuilds on release.
void MoveFaceController::drawOverlay(const IopOverlay& ov) const {
    if (!m_st.moveFaceActive || m_st.moveFaceSilhouetteLoops.empty() ||
        !faceXformNontrivial()) return;
    // Apply the current gesture transform to a ghost point.
    auto xf = [&](const glm::vec3& p) -> glm::vec3 {
        if (m_st.faceXformKind == FaceXform::Translate)
            return p + m_st.moveFaceVec;
        if (m_st.faceXformKind == FaceXform::Scale) {
            glm::vec3 d = p - m_st.moveFacePivot;
            if (m_st.moveFaceScaleUniform)
                return m_st.moveFacePivot + d * m_st.moveFaceScale;
            // Non-uniform: scale along each in-plane axis.
            float dA = glm::dot(d, m_st.moveFaceAxisA);
            float dB = glm::dot(d, m_st.moveFaceAxisB);
            float dN = glm::dot(d, m_st.moveFaceN);
            return m_st.moveFacePivot + m_st.moveFaceAxisA * (dA * m_st.moveFaceScaleA)
                                      + m_st.moveFaceAxisB * (dB * m_st.moveFaceScaleB)
                                      + m_st.moveFaceN * dN;
        }
        if (m_st.moveFaceIsTwist) {
            // Twist: spin the top loop about the face normal through
            // the pivot (Rodrigues) — shows the final top orientation.
            glm::vec3 d = p - m_st.moveFacePivot;
            float c = std::cos(m_st.moveFaceTwist), s = std::sin(m_st.moveFaceTwist);
            const glm::vec3& k = m_st.moveFaceN;
            glm::vec3 r = d * c + glm::cross(k, d) * s +
                          k * glm::dot(k, d) * (1.0f - c);
            return m_st.moveFacePivot + r;
        }
        // Composed tilt (live ring ∘ accumulated tilts) about pivot.
        return m_st.moveFacePivot + faceRotTotal() * (p - m_st.moveFacePivot);
    };
    const unsigned col = 0xE640EBFFu; // yellow, 0xAABBGGRR
    for (size_t k = 0; k < m_st.moveFaceSilhouetteLoops.size(); ++k) {
        // These are the TOP rings. Outline moves with the face; a
        // hole's top ring moves only if that hole slants or is a
        // vertical tube (else it stays put, undrawn).
        bool holeRides = m_st.moveFaceMoveOuter &&
                         m_st.faceXformKind == FaceXform::Rotate;
        bool moves = (k == 0)
            ? m_st.moveFaceMoveOuter
            : (holeRides ||
               (k - 1 < m_st.moveFaceHoleSlant.size() && m_st.moveFaceHoleSlant[k - 1]) ||
               (k - 1 < m_st.moveFaceHoleVertical.size() && m_st.moveFaceHoleVertical[k - 1]));
        if (!moves) continue; // a static loop stays at rest, undrawn
        glm::vec2 prev{}, first{}; bool havePrev = false, haveFirst = false;
        for (const auto& p : m_st.moveFaceSilhouetteLoops[k]) {
            glm::vec2 s;
            if (!ov.toScreen(xf(p), s)) { havePrev = false; continue; }
            if (!haveFirst) { first = s; haveFirst = true; }
            if (havePrev) ov.line(prev, s, col, 2.0f);
            prev = s; havePrev = true;
        }
        if (haveFirst && havePrev) ov.line(prev, first, col, 2.0f);
    }
}

// Move / Tilt / Scale Face: instructions + value wells + commit/cancel. The
// body follows (loft) on release; this panel just dials exact values or bails.
// NOT the scaffold panel: the banner + the well anchor to the VIEWPORT window,
// so renderViewport calls this while that window is current.
void MoveFaceController::renderMoveFacePanel(const IopContext& ctx,
                                             float uiScale) {
    if (!m_st.moveFaceActive) return;
    const bool isRot = m_st.faceXformKind == FaceXform::Rotate;
    const bool isScl = m_st.faceXformKind == FaceXform::Scale;
    materializr::viewportBanner(
        ImVec4(0.2f, 1.0f, 0.5f, 1.0f),
        materializr::touchMode()
            ? "%s - drag a handle, then Confirm / Cancel."
            : "%s - drag a handle. Enter to confirm, Escape to cancel.",
        isRot ? "TILT / TWIST FACE (rings about its centre)"
              : isScl ? "SCALE FACE (about its centre)"
                      : "MOVE FACE (slide in plane)");

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 250.0f * uiScale,
                                   ImGui::GetWindowPos().y + 50), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f * uiScale, 0.0f),
                                        ImVec2(240.0f * uiScale, 100000.0f));
    ImGui::Begin("##MoveFaceInput", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_AlwaysAutoResize);
    opDialogDragGrip(uiScale);
    if (isRot) {
        // Colour the label to the active ring (red = axis B, green = A).
        ImVec4 lc = (m_st.moveFaceGrab == 1) ? ImVec4(0.4f, 0.95f, 0.45f, 1.0f)
                                             : ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
        ImGui::TextColored(lc, "%s", materializr::tr("Tilt (deg)")); ImGui::Separator();
        float deg = m_st.moveFaceAngle * 57.2957795f;
        bool ch = false;
        ImGui::SetNextItemWidth(150);
        ImGui::TextDisabled(materializr::tr("%.1f deg"), deg);
        if (materializr::stepperRow("tiltStep", &deg,
                                    /*allowNegative=*/true, -90.0f, 90.0f))
            ch = true;
        ImGui::SetNextItemWidth(90);
        if (materializr::inputNumber(materializr::tr("deg"), &deg, 1.0f, 5.0f, "%.1f")) ch = true;
        ImGui::Checkbox(materializr::tr("Snap 1 deg"), &m_st.moveFaceRotSnap);
        if (ch) {
            if (m_st.moveFaceRotSnap) deg = std::round(deg);
            m_st.moveFaceAngle = deg / 57.2957795f;
            m_st.moveFaceIsTwist = false; // editing tilt switches the gesture to tilt
            if (glm::length(m_st.moveFaceRotAxis) < 0.5f)
                m_st.moveFaceRotAxis = m_st.moveFaceAxisB;
            updateMoveFace(ctx);
        }
        // Twist = the third (blue) ring, about the face normal. Editable
        // here too so an exact angle can be dialled without dragging.
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.55f, 0.68f, 1.0f, 1.0f), "%s", materializr::tr("Twist (deg)"));
        ImGui::Separator();
        float twdeg = m_st.moveFaceTwist * 57.2957795f;
        bool twch = false;
        ImGui::SetNextItemWidth(150);
        ImGui::TextDisabled(materializr::tr("%.1f deg"), twdeg);
        if (materializr::stepperRow("twistStep", &twdeg,
                                    /*allowNegative=*/true, -180.0f, 180.0f))
            twch = true;
        ImGui::SetNextItemWidth(90);
        if (materializr::inputNumber(materializr::tr("deg##tw"), &twdeg, 1.0f, 5.0f, "%.1f")) twch = true;
        if (twch) {
            if (m_st.moveFaceRotSnap) twdeg = std::round(twdeg);
            m_st.moveFaceTwist = twdeg / 57.2957795f;
            m_st.moveFaceIsTwist = true; // editing twist switches the gesture to twist
            updateMoveFace(ctx);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.80f, 0.35f, 1.0f));
        ImGui::PushTextWrapPos(230.0f);
        ImGui::TextWrapped("%s", materializr::tr("Tilt and Twist are separate ops — one gesture does either a tilt OR a twist, not both. For a tapered-and-twisted face, commit one then the other."));
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        // The two readings of a tilt, offered as a choice rather than one being
        // silently right. Off: the body shears, so every feature inside it leans
        // to match. On: only the faces meeting this one are rebuilt and the rest
        // of the part stays exactly where it is.
        ImGui::Separator();
        if (ImGui::Checkbox(materializr::tr("Local (rebuild neighbours)"),
                            &m_st.moveFaceLocal)) {
            m_st.moveFaceLocalRefusal = nullptr;
            updateMoveFace(ctx);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", materializr::tr(
                "Off: the whole body shears, so holes and features inside it lean with the face.\n"
                "On: only the faces touching this one are rebuilt - everything else stays put.\n"
                "The face you tilt has to be flat; what it meets can curve."));
        if (m_st.moveFaceLocal && m_st.moveFaceLocalRefusal &&
            m_st.moveFaceLocalRefusal[0] != '\0') {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
            ImGui::PushTextWrapPos(230.0f);
            ImGui::TextWrapped("%s", m_st.moveFaceLocalRefusal);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    } else if (isScl) {
        ImGui::Text("%s", materializr::tr("Scale (%%)")); ImGui::Separator();
        bool ch = false;
        if (ImGui::Checkbox(materializr::tr("Uniform"), &m_st.moveFaceScaleUniform)) {
            if (m_st.moveFaceScaleUniform)
                m_st.moveFaceScale = 0.5f * (m_st.moveFaceScaleA + m_st.moveFaceScaleB);
            else
                m_st.moveFaceScaleA = m_st.moveFaceScaleB = m_st.moveFaceScale;
            ch = true;
        }
        if (m_st.moveFaceScaleUniform) {
            float pct = m_st.moveFaceScale * 100.0f;
            ImGui::SetNextItemWidth(150);
            ImGui::TextDisabled("%.0f %%", pct);
            if (materializr::stepperRow("sclStep", &pct,
                                        /*allowNegative=*/true, 10.0f,
                                        400.0f, /*zeroValue=*/100.0f))
                ch = true;
            ImGui::SetNextItemWidth(90);
            if (materializr::inputNumber("%", &pct, 5.0f, 25.0f, "%.0f")) ch = true;
            if (ch) m_st.moveFaceScale = std::max(0.1f, pct / 100.0f);
        } else {
            float a = m_st.moveFaceScaleA * 100.0f, b = m_st.moveFaceScaleB * 100.0f;
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", materializr::tr("Axis A (red)"));
            ImGui::SetNextItemWidth(150);
            ImGui::TextDisabled("%.0f %%", a);
            if (materializr::stepperRow("sclAStep", &a,
                                        /*allowNegative=*/true, 10.0f,
                                        400.0f, /*zeroValue=*/100.0f))
                ch = true;
            ImGui::SetNextItemWidth(90);
            if (materializr::inputNumber("% A", &a, 5.0f, 25.0f, "%.0f")) ch = true;
            ImGui::TextColored(ImVec4(0.4f, 0.95f, 0.45f, 1.0f), "%s", materializr::tr("Axis B (green)"));
            ImGui::SetNextItemWidth(150);
            ImGui::TextDisabled("%.0f %%", b);
            if (materializr::stepperRow("sclBStep", &b,
                                        /*allowNegative=*/true, 10.0f,
                                        400.0f, /*zeroValue=*/100.0f))
                ch = true;
            ImGui::SetNextItemWidth(90);
            if (materializr::inputNumber("% B", &b, 5.0f, 25.0f, "%.0f")) ch = true;
            if (ch) {
                m_st.moveFaceScaleA = std::max(0.1f, a / 100.0f);
                m_st.moveFaceScaleB = std::max(0.1f, b / 100.0f);
            }
        }
        if (ch) updateMoveFace(ctx);
    } else {
        ImGui::Text("%s", materializr::trFormat("Slide (%s)", materializr::unitSuffix()).c_str()); ImGui::Separator();
        ImGui::Text("(%.1f, %.1f, %.1f)  |%.1f|",
                    m_st.moveFaceVec.x, m_st.moveFaceVec.y, m_st.moveFaceVec.z,
                    glm::length(m_st.moveFaceVec));
    }

    // Read-out of what the SELECTION will do (the selection IS the control
    // now). A hole stays put unless you also pick its top edge (slants) or
    // its wall (vertical tube).
    if (!m_st.moveFaceHoleVertical.empty()) {
        ImGui::Separator();
        int nvert = 0, nslant = 0;
        for (bool v : m_st.moveFaceHoleVertical) if (v) ++nvert;
        for (bool s : m_st.moveFaceHoleSlant)    if (s) ++nslant;
        int nstatic = static_cast<int>(m_st.moveFaceHoleVertical.size()) - nvert - nslant;
        ImGui::TextWrapped(materializr::tr("Holes: %d stay, %d slant, %d vertical."),
                           nstatic, nslant, nvert);
        ImGui::TextDisabled("%s", materializr::tr("Pick a hole's top edge to slant it, its wall to keep it a vertical tube."));
    }

    if (!ctx.cornerCommitUi) {   // im-touch: corner ✓/✗ FABs instead
        ImGui::Spacing();
        if (ImGui::Button(materializr::btnConfirm(), ImVec2(110, 0))) commitMoveFace(ctx);
        ImGui::SameLine();
        if (ImGui::Button(materializr::btnCancel(), ImVec2(110, 0))) cancelMoveFace(ctx);
    }
    ImGui::End();
}

// Face gizmo: Move/Scale show two in-plane arrows; Rotate shows two rings
// (about the face centre) so a tilt reads as a rotation. The latched handle
// brightens.
void MoveFaceController::drawGizmos3D(const IopGizmo3D& g) const {
    if (!m_st.moveFaceActive) return;
    auto pack = [](const glm::vec3& c) -> unsigned {
        auto b = [](float v) {
            return static_cast<unsigned>(
                glm::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return 0xFF000000u | (b(c.b) << 16) | (b(c.g) << 8) | b(c.r);
    };
    // Translate arrows take the colour of the axis each in-plane direction
    // most aligns with — coloured by the USER's axis, not the world's. The
    // world is Y-up internally while everything the user reads is Z-up
    // (user X = world X, user Y = world Z, user Z = world Y — see
    // UserAxes.h), and this used to colour straight off the world axis. So a
    // top face's in-plane directions came out red + BLUE, when in the user's
    // own axes they are X and Y and should read red + GREEN (Steve,
    // 2026-08-04). Green and blue are therefore swapped relative to the
    // world mapping. The grabbed arrow brightens; the other dims.
    auto axisColor = [&](const glm::vec3& d, bool grabbed) {
        const glm::vec3 a = glm::abs(d);
        glm::vec3 c = (a.x >= a.y && a.x >= a.z) ? glm::vec3(0.90f, 0.20f, 0.20f)  // world X = user X
                    : (a.y >= a.z)               ? glm::vec3(0.30f, 0.40f, 0.95f)  // world Y = user Z
                                                 : glm::vec3(0.20f, 0.90f, 0.20f); // world Z = user Y
        return pack(grabbed ? glm::clamp(c * 1.7f, glm::vec3(0.0f), glm::vec3(1.0f))
                            : c * 0.6f);
    };
    if (m_st.faceXformKind == FaceXform::Rotate) {
        // grab 0 tilts about axis B (RED ring), grab 1 about axis A
        // (GREEN ring) — matched to the colored controls in the panel.
        const unsigned red0 = pack(m_st.moveFaceGrab == 0
                                       ? glm::vec3(1.0f, 0.32f, 0.32f)
                                       : glm::vec3(0.72f, 0.22f, 0.22f));
        const unsigned grn1 = pack(m_st.moveFaceGrab == 1
                                       ? glm::vec3(0.35f, 0.95f, 0.40f)
                                       : glm::vec3(0.24f, 0.66f, 0.28f));
        g.ring(m_st.moveFacePivot, m_st.moveFaceAxisB, red0);
        g.ring(m_st.moveFacePivot, m_st.moveFaceAxisA, grn1);
        // Third ring: about the face NORMAL (lies IN the face plane) —
        // grabbing it TWISTS the face rather than tilting it. Blue, the
        // "third axis" colour; brightens when latched (grab 2).
        const unsigned blu2 = pack(m_st.moveFaceGrab == 2
                                       ? glm::vec3(0.45f, 0.62f, 1.0f)
                                       : glm::vec3(0.28f, 0.40f, 0.78f));
        g.ring(m_st.moveFacePivot, m_st.moveFaceN, blu2);
    } else if (m_st.faceXformKind == FaceXform::Scale) {
        // Scale: cube handles (the regular scale-gizmo look). Axis A =
        // red, axis B = green, matched to the non-uniform controls.
        const unsigned rA = pack(m_st.moveFaceGrab == 0
                                     ? glm::vec3(1.0f, 0.32f, 0.32f)
                                     : glm::vec3(0.72f, 0.22f, 0.22f));
        const unsigned gB = pack(m_st.moveFaceGrab == 1
                                     ? glm::vec3(0.35f, 0.95f, 0.40f)
                                     : glm::vec3(0.24f, 0.66f, 0.28f));
        g.cube(m_st.moveFacePivot, m_st.moveFaceAxisA, rA);
        g.cube(m_st.moveFacePivot, m_st.moveFaceAxisB, gB);
    } else {
        g.arrow(m_st.moveFaceP0, m_st.moveFaceAxisA,
                axisColor(m_st.moveFaceAxisA, m_st.moveFaceGrab == 0));
        g.arrow(m_st.moveFaceP0, m_st.moveFaceAxisB,
                axisColor(m_st.moveFaceAxisB, m_st.moveFaceGrab == 1));
    }
}

} // namespace materializr
