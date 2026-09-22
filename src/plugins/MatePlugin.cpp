#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../core/SelectionManager.h"
#include "../core/Document.h"
#include "../modeling/Mate.h"
#include "../modeling/MateSolver.h"
#include "../modeling/FaceAnchor.h"
#include "../ui/NumField.h"
#include "../ui/LengthField.h"

#include <imgui.h>
#include <BRepAdaptor_Surface.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <cmath>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <vector>
#include <string>

// Mates - position a body by a persistent relationship to another body,
// instead of baking a one-shot transform. Moving the reference carries
// everything mated to it.
//
// The plugin owns the UI only. The mate itself lives on the Document and is
// placed by MateSolver, the same split sketch constraints have: the data and
// the solver are core, the panel is not.
namespace {

using materializr::Mate;
using materializr::MateType;
using materializr::MateSolver;

const char* typeName(MateType t) {
    switch (t) {
        case MateType::Fasten:     return "Fasten";
        case MateType::Concentric: return "Concentric";
        case MateType::Planar:     return "Planar";
    }
    return "Fasten";
}

// Confirmation popup after a mate is created. Clicking "Mate" used to have no
// feedback at all - a body-to-body pick with no faces always falls back to an
// anchorless Fasten, which captures the CURRENT arrangement (offset 0 means
// "keep what I have", see createMate below), so nothing visibly moves and the
// button looked broken. This tells the user what got created either way, and
// for the no-visible-effect case, where to go to actually see or change it.
bool g_mateConfirmOpenRequested = false;
std::string g_mateConfirmText;

std::string bodyLabel(materializr::PluginContext& ctx, int id) {
    std::string name = ctx.document().getBodyName(id);
    return name.empty() ? ("Body " + std::to_string(id)) : name;
}

// The two bodies a mate would join, in selection order: the first pick is the
// reference that stays put, the second is the one that moves. That ordering is
// the whole mental model, so the panel states it rather than leaving the user
// to discover which body jumps.
bool selectedPair(materializr::PluginContext& ctx, int& refBody, int& moveBody) {
    std::vector<int> ids;
    for (const auto& e : ctx.selection().getSelection()) {
        if (e.type != SelectionType::Body && e.type != SelectionType::Face)
            continue;
        if (e.bodyId < 0) continue;
        bool dup = false;
        for (int id : ids) dup = dup || (id == e.bodyId);
        if (!dup) ids.push_back(e.bodyId);
    }
    if (ids.size() < 2) return false;
    refBody = ids[0];
    moveBody = ids[1];
    return true;
}

// Infer the mate from what was picked, the way resolveDimension infers a
// constraint from a picked pair: two cylinders read as Concentric, two planes
// as Planar, anything else as Fasten. Always overridable in the panel - an
// inference that cannot be corrected is worse than no inference.
MateType inferType(materializr::PluginContext& ctx) {
    int cyl = 0, planar = 0, faces = 0;
    for (const auto& e : ctx.selection().getSelection()) {
        if (e.type != SelectionType::Face || e.shape.IsNull()) continue;
        if (e.shape.ShapeType() != TopAbs_FACE) continue;
        ++faces;
        BRepAdaptor_Surface surf(TopoDS::Face(e.shape));
        if (surf.GetType() == GeomAbs_Cylinder) ++cyl;
        else if (surf.GetType() == GeomAbs_Plane) ++planar;
    }
    if (faces >= 2 && cyl >= 2) return MateType::Concentric;
    if (faces >= 2 && planar >= 2) return MateType::Planar;
    return MateType::Fasten;
}

// Re-place everything after any edit. Cheap: the solve is a graph walk, not an
// iterative solver, and it only runs on an edit rather than per frame.
//
// The solver holds no placement state - both the body bases and the sketch
// plane bases live on the Document - so a local instance here shares one truth
// with the one History owns. It did not always, and the half-migrated version
// was worse than either: bodies were shared while sketch planes were not, so
// the body held still and its sketch drifted one offset per click.
// Every mate mutation (create, edit, delete) routes through here, so this is
// also the one place that marks the change: nothing else does. Without it,
// the solver moves a body in the Document but the viewport keeps the stale
// mesh (nothing marks meshes dirty outside History) and the project reports
// no unsaved changes (nothing marks it dirty outside History either) - a
// mate created here would silently vanish on quit/reopen.
void resolve(materializr::PluginContext& ctx) {
    Document& doc = ctx.document();
    MateSolver solver;
    const auto res = solver.solve(doc);
    doc.setMateSolveError(res.ok ? std::string() : res.error);
    ctx.markMeshesDirty();
    ctx.markDocumentDirty();
}

// Anchors for the faces picked on one body. Without these a Concentric or
// Planar mate has no frame, so frameFor refuses it and the mate is marked
// broken on its very first solve - a mate created one second ago rendering as
// "reference lost". FaceAnchor is the same machinery that lets a fillet or a
// dimension survive a regeneration renaming faces.
std::vector<FaceAnchor::Anchor> anchorsForBody(materializr::PluginContext& ctx,
                                               int bodyId) {
    std::vector<TopoDS_Face> faces;
    for (const auto& e : ctx.selection().getSelection()) {
        if (e.type != SelectionType::Face || e.bodyId != bodyId) continue;
        if (e.shape.IsNull() || e.shape.ShapeType() != TopAbs_FACE) continue;
        faces.push_back(TopoDS::Face(e.shape));
    }
    if (faces.empty()) return {};

    Document& doc = ctx.document();
    std::vector<FaceAnchor::SketchRef> sketches;
    for (int sid : doc.getAllSketchIds())
        if (auto sk = doc.getSketch(sid))
            sketches.push_back({sid, sk.get()});

    // compute() returns ONE anchor per face, Kind::None for a face no sketch
    // can attribute - so the vector is never empty and testing empty() was
    // testing nothing. resolve() is all-or-nothing ("unless EVERY anchor is
    // non-None"), so a single None makes the whole set useless. Return nothing
    // rather than something that cannot resolve.
    auto anchors = FaceAnchor::compute(faces, sketches);
    for (const auto& a : anchors)
        if (a.kind == FaceAnchor::Anchor::None) return {};
    return anchors;
}

void createMate(materializr::PluginContext& ctx) {
    Document& doc = ctx.document();
    int refBody = -1, moveBody = -1;
    if (!selectedPair(ctx, refBody, moveBody)) return;

    // A body can only be placed by one mate. Refusing here, at creation, is
    // what keeps the solver a topological walk instead of a constraint system
    // that has to reconcile two answers.
    for (const auto& m : doc.getMates())
        if (!m.suppressed && m.bodyB == moveBody) return;

    // The grounded body anchors the whole graph. If nothing is grounded yet,
    // the first reference picked becomes it - otherwise the first mate a user
    // creates would place nothing.
    if (doc.getGroundedBody() < 0) doc.setGroundedBody(refBody);

    Mate m{};
    m.type = inferType(ctx);
    m.bodyA = refBody;
    m.bodyB = moveBody;
    m.anchorsA = anchorsForBody(ctx, refBody);
    m.anchorsB = anchorsForBody(ctx, moveBody);

    // Concentric and Planar are defined BY the faces picked; with no anchors
    // they have no frame and would be born broken. Fall back to Fasten, which
    // works from body geometry, rather than creating a mate that can never
    // resolve.
    if ((m.type != MateType::Fasten) &&
        (m.anchorsA.empty() || m.anchorsB.empty()))
        m.type = MateType::Fasten;

    // Capture the arrangement as it stands, so creating the mate does not move
    // anything: offset 0 means "hold what I have".
    {
        Bnd_Box ba, bb;
        // From the BASE where one exists - the solver measures from bases, and
        // measuring the pose from the placed shape instead made the body jump
        // by the old placement delta when a mate was deleted and remade.
        BRepBndLib::Add(doc.hasMateBase(refBody) ? doc.getMateBase(refBody)
                                                 : doc.getBody(refBody), ba);
        BRepBndLib::Add(doc.hasMateBase(moveBody) ? doc.getMateBase(moveBody)
                                                  : doc.getBody(moveBody), bb);
        if (!ba.IsVoid() && !bb.IsVoid()) {
            double ax, ay, az, ax2, ay2, az2, bx, by, bz, bx2, by2, bz2;
            ba.Get(ax, ay, az, ax2, ay2, az2);
            bb.Get(bx, by, bz, bx2, by2, bz2);
            m.relX = bx - ax;
            m.relY = by - ay;
            m.relZ = bz - az;
            m.hasRelPose = true;
        }
    }

    doc.addMate(m);
    resolve(ctx);

    std::string ref = bodyLabel(ctx, refBody);
    std::string mv  = bodyLabel(ctx, moveBody);
    if (m.type == MateType::Fasten) {
        g_mateConfirmText = mv + " is now mated to " + ref +
            " (Fasten). It's holding its current position - nothing moved. "
            "Open the Mates section below to set an offset, or pick faces on "
            "both bodies first for a Planar / Concentric mate that aligns them.";
    } else {
        g_mateConfirmText = mv + " is now mated to " + ref + " (" +
            typeName(m.type) + "), aligned to the picked faces. Adjust "
            "offset, roll or Flip in the Mates section below.";
    }
    g_mateConfirmOpenRequested = true;
}

bool renderPanel(materializr::PluginContext& ctx) {
    Document& doc = ctx.document();
    auto& mates = doc.getMutableMates();
    if (mates.empty()) return false;

    ImGui::TextUnformatted("Mates");

    // The solver's Result used to be discarded everywhere, so a cycle or an
    // over-constraint produced an assembly that simply stopped moving with no
    // explanation anywhere in the app.
    if (!doc.mateSolveError().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.35f, 1.0f));
        ImGui::TextWrapped("Mates not applied: %s", doc.mateSolveError().c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();

    int toDelete = -1;
    for (auto& m : mates) {
        ImGui::PushID(m.id);

        if (m.broken) {
            // A lost face reference is not a crash and not a silent drop; say
            // so where the user is already looking.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.35f, 1.0f));
            ImGui::Text("%s (reference lost)", typeName(m.type));
            ImGui::PopStyleColor();
        } else {
            ImGui::Text("%s: body %d -> body %d", typeName(m.type),
                        m.bodyA, m.bodyB);
        }

        bool dirty = false;

        // Concentric and Planar are defined by the picked faces. With no
        // anchors they have no frame, so offering them here would let the user
        // turn a working mate into a permanently broken one from a dropdown.
        // Anchors exist only for faces a sketch can attribute - a primitive or
        // an imported STEP body yields none - so this is the common case, not
        // an edge case.
        // A broken mate is broken BECAUSE its anchors were lost, so locking the
        // dropdown on "no anchors" closed the one guided repair - switching to
        // Fasten, which needs none. Keep it open when broken.
        const bool anchored =
            m.broken || (!m.anchorsA.empty() && !m.anchorsB.empty());
        int typeIdx = static_cast<int>(m.type);
        const char* kinds[] = {"Fasten", "Concentric", "Planar"};
        if (anchored) {
            if (ImGui::Combo("Type", &typeIdx, kinds, 3)) {
                m.type = static_cast<MateType>(typeIdx);
                dirty = true;
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Combo("Type", &typeIdx, kinds, 3);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Only Fasten is available: these bodies have no "
                    "sketch-derived faces to align to. Primitive and imported "
                    "bodies cannot carry face references.");
        }

        double offset = m.offset;
        if (materializr::lengthField("Offset", &offset)) {
            m.offset = offset;
            dirty = true;
        }

        // Angle and Flip need a frame that reflects the body's ACTUAL current
        // orientation to stay correct across a base recapture (undo/redo,
        // Suppress toggle, any History op that clears cached mate bases) -
        // frameFor() (Concentric/Planar) reads that from the resolved face's
        // real geometry, so it self-corrects. The anchorless-Fasten fallback
        // (bboxFrame) cannot: it derives only a fixed axis convention at the
        // bbox's min corner, blind to how the shape is actually rotated. A
        // nonzero angle recaptured from an already-rotated base compounds the
        // rotation instead of reproducing it - a silent, unreported drift.
        //
        // NOT gated on `anchored`: that flag is deliberately true for a
        // BROKEN mate too (so its Type dropdown stays open for repair), but a
        // broken mate can be Fasten with genuinely empty anchors - exactly the
        // bboxFrame case. Gate on whether real anchors exist right now.
        const bool hasRealAnchors = !m.anchorsA.empty() && !m.anchorsB.empty();
        if (hasRealAnchors) {
            double angleDeg = m.angle * 180.0 / M_PI;
            if (materializr::inputNumber("Angle (deg)", &angleDeg)) {
                m.angle = angleDeg * M_PI / 180.0;
                dirty = true;
            }
            if (ImGui::Checkbox("Flip", &m.flipped)) dirty = true;
        } else {
            double angleDeg = 0.0;
            ImGui::BeginDisabled();
            materializr::inputNumber("Angle (deg)", &angleDeg);
            bool flip = false;
            ImGui::Checkbox("Flip", &flip);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Only available with anchored faces: a rotation or flip "
                    "recaptured from this body's current (already-rotated) "
                    "position, instead of a real face's orientation, would "
                    "silently compound on the next solve.");
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Suppress", &m.suppressed)) dirty = true;

        ImGui::SameLine();
        if (ImGui::Button("Delete")) toDelete = m.id;

        ImGui::Separator();
        ImGui::PopID();

        if (dirty) resolve(ctx);
    }

    if (toDelete >= 0) {
        // Deleting leaves the body where the solve last put it rather than
        // snapping it back to where history built it - un-mating should not
        // look like the part jumping away.
        doc.removeMate(toDelete);
        resolve(ctx);
    }
    return true;
}

} // namespace

REGISTER_PLUGIN(Mate, [](materializr::PluginContext& ctx) {
    ctx.registerToolbarButton({
        "Mate", "Assembly",
        materializr::SelectionContext::MultipleBodies, 420,
        createMate, nullptr,
        "Position one body against another and keep the relationship. Pick the "
        "body that stays put first, then the one that moves.\n\n"
        "The mate type is inferred from what you picked - two round faces give "
        "Concentric, two flat faces give Planar, anything else Fasten - and you "
        "can change it afterwards. Editing the reference body carries "
        "everything mated to it along."});

    ctx.registerPropertySection({
        "Mates", materializr::SelectionContext::Always, 420, renderPanel});

    materializr::OverlayContribution confirm;
    confirm.name = "MateConfirm";
    confirm.render = [](materializr::PluginContext&) {
        if (g_mateConfirmOpenRequested) {
            ImGui::OpenPopup("Mate created##mateConfirm");
            g_mateConfirmOpenRequested = false;
        }
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Mate created##mateConfirm", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 320.0f);
            // Body names are baked in, so this can't route through tr() like a
            // static string - a translated catalogue lookup on this exact
            // concatenation would never hit.
            ImGui::TextUnformatted(g_mateConfirmText.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Separator();
            if (ImGui::Button(materializr::tr("OK"))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    };
    ctx.registerOverlay(std::move(confirm));
})
