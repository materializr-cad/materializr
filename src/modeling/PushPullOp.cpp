#include "ui/LengthField.h"
#include "core/Units.h"
#include "PushPullOp.h"
#include "SubShapeIndex.h"
#include "Sketch.h"
#include <cstdio>
#include <cstdlib>

#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepBndLib.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRepTools.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <gp_Pnt2d.hxx>
#include <Bnd_Box.hxx>
#include <BRep_Tool.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_ToroidalSurface.hxx>
#include <Geom_SurfaceOfRevolution.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>

#include "UnifyTolerance.h"
#include <BRepTools_History.hxx>
#include <TopoDS.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <TopAbs.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <imgui.h>
#include <cmath>
#include <unordered_set>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"
#include "ParamParse.h"

// A point that genuinely lies on the face's MATERIAL. Returns `center` when it's
// already inside the trimmed face; otherwise samples a UV grid (rejecting points
// in holes via the face classifier) — so a holed/annular face whose parametric
// centre falls in a hole still yields a usable probe point. False if none found.
static bool faceMaterialPoint(const TopoDS_Face& face, const gp_Pnt& center,
                              gp_Pnt& out) {
    if (face.IsNull()) return false;
    Handle(Geom_Surface) s = BRep_Tool::Surface(face);
    if (s.IsNull()) return false;
    double u1, u2, v1, v2;
    BRepTools::UVBounds(face, u1, u2, v1, v2);
    BRepTopAdaptor_FClass2d cls(face, 1e-7);
    // Prefer the supplied centre if it projects onto material.
    GeomAPI_ProjectPointOnSurf proj(center, s, u1, u2, v1, v2);
    if (proj.NbPoints() > 0) {
        double pu, pv; proj.LowerDistanceParameters(pu, pv);
        if (proj.LowerDistance() < 1e-6 &&
            cls.Perform(gp_Pnt2d(pu, pv)) == TopAbs_IN) { out = center; return true; }
    }
    const int N = 9;
    for (int i = 1; i < N; ++i)
        for (int j = 1; j < N; ++j) {
            double u = u1 + (u2 - u1) * i / N;
            double v = v1 + (v2 - v1) * j / N;
            if (cls.Perform(gp_Pnt2d(u, v)) == TopAbs_IN) {
                out = s->Value(u, v);
                return true;
            }
        }
    return false;
}

gp_Vec correctedOutwardNormal(const TopoDS_Shape& solid, const TopoDS_Face& face,
                              const gp_Pnt& center, const gp_Vec& normal) {
    if (solid.IsNull() || normal.Magnitude() < 1e-10) return normal;
    try {
        // Probe distance scaled to the body so a fixed absolute ε can't
        // overshoot a thin feature (the misfire that broke a prior classifier
        // fix). Floor/ceiling keep it sane across unit scales.
        double eps = 0.05;
        Bnd_Box box;
        BRepBndLib::Add(solid, box);
        if (!box.IsVoid()) {
            double xmin, ymin, zmin, xmax, ymax, zmax;
            box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
            double diag = gp_Vec(xmax - xmin, ymax - ymin, zmax - zmin).Magnitude();
            eps = std::min(0.05, std::max(1e-4, diag * 5e-4));
        }
        // Probe from a point ON the face material — NOT the parametric centre,
        // which for a holed/annular face lands in a hole and makes both ±ε
        // probes read OUTSIDE (ambiguous → a reversed face stays inverted).
        gp_Pnt probe = center;
        faceMaterialPoint(face, center, probe);
        gp_Vec u = normal.Normalized();
        BRepClass3d_SolidClassifier cPlus (solid, probe.Translated(u *  eps), 1e-7);
        BRepClass3d_SolidClassifier cMinus(solid, probe.Translated(u * -eps), 1e-7);
        // Unambiguous inverted pair: +normal lands INSIDE material, −normal
        // OUTSIDE. Only then is the trusted normal genuinely backwards.
        if (cPlus.State() == TopAbs_IN && cMinus.State() == TopAbs_OUT) {
            gp_Vec flipped = normal;
            flipped.Reverse();
            return flipped;
        }
    } catch (...) {}
    return normal; // correct pair / ambiguous / ON / throw → trust BRepGProp
}

PushPullOp::PushPullOp() = default;

void PushPullOp::setTargets(std::vector<Target> targets) {
    m_targets = std::move(targets);
    // Keep the cascade-source arrays sized in lockstep with m_targets. -1
    // entries mean "no sketch source" — face-driven push/pulls stay opaque
    // to the cascade walker.
    m_sketchSourceIds.assign(m_targets.size(), -1);
    m_sketchSourceRegions.assign(m_targets.size(), -1);
}

void PushPullOp::setDistance(double d) {
    m_distance = d;
}

void PushPullOp::setSketchSource(int targetIndex, int sketchId, int regionIndex) {
    if (targetIndex < 0 ||
        targetIndex >= static_cast<int>(m_sketchSourceIds.size())) return;
    m_sketchSourceIds[targetIndex]     = sketchId;
    m_sketchSourceRegions[targetIndex] = regionIndex;
}

bool PushPullOp::hasAnySketchSource() const {
    for (int id : m_sketchSourceIds) if (id >= 0) return true;
    return false;
}

int PushPullOp::getSketchIdAt(int targetIndex) const {
    if (targetIndex < 0 ||
        targetIndex >= static_cast<int>(m_sketchSourceIds.size())) return -1;
    return m_sketchSourceIds[targetIndex];
}

// Rebuild any target.profile that was originally produced by the given
// sketch. Used by the cascade walker after a constraint commit. Returns
// true if at least one profile was rebuilt — caller should then re-execute
// the op so the body shape catches up.
bool PushPullOp::rebuildProfileFromSketch(Document& doc, int sketchId) {
    if (sketchId < 0) return false;
    auto sk = doc.getSketch(sketchId);
    if (!sk) return false;
    auto regions = sk->buildRegions();
    if (regions.empty()) return false;
    bool any = false;
    for (size_t i = 0; i < m_targets.size(); ++i) {
        if (i >= m_sketchSourceIds.size() ||
            m_sketchSourceIds[i] != sketchId) continue;
        int idx = (i < m_sketchSourceRegions.size())
                      ? m_sketchSourceRegions[i] : -1;
        if (idx < 0 || idx >= static_cast<int>(regions.size())) idx = 0;
        if (regions[idx].face.IsNull()) continue;
        m_targets[i].profile = regions[idx].face;
        any = true;
    }
    return any;
}

void PushPullOp::refreshFaceTargets(Document& doc) {
    if (m_targetRefs.size() != m_targets.size())
        m_targetRefs.resize(m_targets.size());
    for (size_t i = 0; i < m_targets.size(); ++i) {
        Target& t = m_targets[i];
        if (t.sourceBodyId < 0) continue;            // free-floating
        // SKETCH-sourced target on a body: the profile is a sketch-region
        // face, which is NEVER a face of the source body — the liveness scan
        // below always fails and resolve() swaps in the closest BODY face
        // (the host face), silently replacing the drawn region with the whole
        // face ("push a hole" moved the face and ignored the sketch). Sketch
        // profiles rebuild via rebuildProfileFromSketch instead; only pure
        // face-driven targets belong to this ref machinery.
        if (i < m_sketchSourceIds.size() && m_sketchSourceIds[i] >= 0)
            continue;
        TopoDS_Shape src;
        try { src = doc.getBody(t.sourceBodyId); } catch (...) { continue; }
        if (src.IsNull()) continue;
        // Mint once while the profile is still a valid face of the source body.
        if (m_targetRefs[i].empty() && !t.profile.IsNull()) {
            materializr::topo::Context mc;
            mc.doc = &doc; mc.shape = src; mc.type = TopAbs_FACE;
            m_targetRefs[i] = materializr::topo::mint(t.profile, mc);
        }
        // Re-resolve if the stored handle is no longer live on the source body
        // (an upstream edit rebuilt it and MOVED the face).
        bool live = false;
        if (!t.profile.IsNull())
            for (TopExp_Explorer ex(src, TopAbs_FACE); ex.More(); ex.Next())
                if (ex.Current().IsSame(t.profile)) { live = true; break; }
        if (!live && !m_targetRefs[i].empty()) {
            materializr::topo::Context rc;
            rc.doc = &doc; rc.shape = src; rc.type = TopAbs_FACE;
            rc.crossRebuild = true;   // source body was rebuilt upstream
            TopoDS_Shape f;
            if (materializr::topo::resolve(m_targetRefs[i], rc, f) &&
                !f.IsNull() && f.ShapeType() == TopAbs_FACE)
                t.profile = TopoDS::Face(f);
        }
    }
}

// A boolean result that keeps the document renderable: at least one real
// solid with non-trivial volume. A cut deeper than its body returns an empty
// (or solid-less) compound — storing that made the body untessellatable and
// it vanished on commit (the pull-through-body bug, PR #13).
static bool keepsASolid(const TopoDS_Shape& s) {
    if (s.IsNull()) return false;
    if (!TopExp_Explorer(s, TopAbs_SOLID).More()) return false;
    try {
        GProp_GProps gp;
        BRepGProp::VolumeProperties(s, gp);
        return gp.Mass() > 1e-7;
    } catch (...) { return false; }
}

// ---- Face-lineage propagation (see PushPullOp.h) -------------------------

void PushPullOp::snapshotLineage(Document& doc, int bodyId) {
    materializr::topo::FaceIdMap m;
    if (const auto* im = doc.bodyFaceIds(bodyId)) m = *im;
    m_prevFaceIds[bodyId] = std::move(m);
}

void PushPullOp::captureLedger(int bodyId, const TopoDS_Shape& before,
                               BRepBuilderAPI_MakeShape& op) {
    materializr::topo::GenerationLedger& led = m_ledgers[bodyId];
    led = materializr::topo::GenerationLedger{};
    try { led.capture(op, before, TopAbs_FACE); } catch (...) {}
}

void PushPullOp::publishLineage(Document& doc, int bodyId,
                                const TopoDS_Shape& before,
                                const TopoDS_Shape& rawResult,
                                const Handle(BRepTools_History)& unifyHist,
                                const TopoDS_Shape& result) {
    auto lit = m_ledgers.find(bodyId);
    if (lit == m_ledgers.end() || result.IsNull()) return;

    // The body's ancestry before this rebuild (snapshotLineage stored it while
    // the doc's map was still populated — updateBody has since cleared it).
    materializr::topo::FaceIdMap inMap;
    if (auto pit = m_prevFaceIds.find(bodyId); pit != m_prevFaceIds.end())
        inMap = pit->second;

    // Stage 1 — propagate ancestry through the BOOLEAN onto its PRE-unify
    // result (the ledger names those faces), then give every pre-unify face an
    // id: inherited where possible, else a STABLE minted one (reused while the
    // uncovered count is unchanged, so a downstream fillet's captured pairs
    // survive the next re-execute — see BooleanOp).
    materializr::topo::FaceIdMap mid = materializr::topo::propagate(
        {{&inMap, before}}, lit->second, rawResult);
    std::vector<TopoDS_Shape> uncovered;
    for (TopExp_Explorer ex(rawResult, TopAbs_FACE); ex.More(); ex.Next())
        if (!materializr::topo::idsFor(mid, ex.Current()))
            uncovered.push_back(ex.Current());
    auto& minted = m_mintedIds[bodyId];
    if (minted.size() != uncovered.size()) {
        minted.clear();
        for (size_t i = 0; i < uncovered.size(); ++i)
            minted.push_back(doc.mintFaceId());
    }
    for (size_t i = 0; i < uncovered.size(); ++i)
        materializr::topo::addId(mid, uncovered[i], minted[i]);

    // Stage 2 — carry that complete map through the UnifySameDomain merge onto
    // the final faces (a no-op when nothing was unified).
    materializr::topo::FaceIdMap next =
        result.IsEqual(rawResult)
            ? std::move(mid)
            : materializr::topo::carryThrough(mid, unifyHist, result);
    materializr::topo::complete(next, result,
                                [&doc]() { return doc.mintFaceId(); });

    doc.setBodyLedger(bodyId, &lit->second);
    doc.setBodyFaceIds(bodyId, std::move(next));
}

bool PushPullOp::execute(Document& doc) {
    // Direct re-execute support (e.g. cascade after a sketch constraint
    // edit): fold the previously-created body ids back into the reuse pool
    // so addOrPutBody re-uses the same ids and updates the existing bodies
    // in place. Without this each re-execute would allocate fresh body ids
    // and pile up duplicate bodies at the same coordinates.
    if (m_reuseBodyIds.empty() && !m_createdBodyIds.empty()) {
        m_reuseBodyIds = std::move(m_createdBodyIds);
    }
    m_previousBodies.clear();
    m_createdBodyIds.clear();
    // Fresh undo snapshot each execute (m_mintedIds is KEPT — reusing it gives
    // the prism's new faces stable ids across re-executes). m_ledgers is NOT
    // cleared: Document::setBodyLedger holds pointers into it for bodies this
    // op touched, and clearing would free those nodes (dangling for any body a
    // later re-execute no longer touches). captureLedger overwrites per body.
    m_prevFaceIds.clear();
    m_reuseIdx = 0; // walks m_reuseBodyIds as each free-floating output is emitted
    if (m_targets.empty() || std::abs(m_distance) < 1e-6) return false;

    // Follow upstream edits: re-resolve any face-driven target whose profile
    // handle has gone stale on a rebuilt source body (sketch targets rebuild
    // separately via rebuildProfilesFromSketch).
    refreshFaceTargets(doc);

    bool anyChange = false;

    std::unordered_set<int> savedBodies;

    // Subtract `prism` from every VISIBLE body it intersects, except `excludeId`
    // (the source body, already handled by the normal path). Each body cut
    // separately; hidden bodies skipped; invalid or no-overlap results skipped.
    // Returns how many bodies were actually cut. This is what makes a push/pull
    // cut go THROUGH everything in its path, not just the sketch's source body.
    auto cutVisibleBodies = [&](const TopoDS_Shape& prism, int excludeId) -> int {
        int n = 0;
        Bnd_Box prismBox; BRepBndLib::Add(prism, prismBox);
        // The tool prism's own volume sets the scale for "a real cut". A prism
        // that only GRAZES a body — or lands coincident with a hole the same
        // sketch already cut — removes a numerically-negligible sliver (a
        // BRepAlgoAPI_Cut of coincident faces yields ~1e-3 mm³ of noise). Such
        // a result is valid but tessellates as garbage, and committing it is
        // never what the user meant. Require the removed volume to be a real
        // fraction (0.1%) of the tool volume before a cut counts, so a graze
        // no-ops instead of imploding the body.
        double prismVol = 0.0;
        try { GProp_GProps gp; BRepGProp::VolumeProperties(prism, gp);
              prismVol = gp.Mass(); } catch (...) {}
        const double minRemoved = std::max(1e-6, prismVol * 1e-3);
        for (int bid : doc.getAllBodyIds()) {
            if (bid == excludeId) continue;
            if (!doc.isBodyVisible(bid)) continue;          // respect hidden
            TopoDS_Shape body;
            try { body = doc.getBody(bid); } catch (...) { continue; }
            if (body.IsNull()) continue;
            Bnd_Box bbox; BRepBndLib::Add(body, bbox);
            if (prismBox.IsOut(bbox)) continue;             // cheap disjoint reject
            try {
                BRepAlgoAPI_Cut cut(body, prism); cut.Build();
                if (!cut.IsDone()) continue;
                TopoDS_Shape result = cut.Shape();
                if (result.IsNull() || !BRepCheck_Analyzer(result).IsValid()) continue;
                if (!keepsASolid(result)) {
                    std::fprintf(stderr, "Push/Pull declined for body %d: the "
                                 "cut would remove the entire body\n", bid);
                    continue;
                }
                GProp_GProps gb; BRepGProp::VolumeProperties(body, gb);
                GProp_GProps gr; BRepGProp::VolumeProperties(result, gr);
                if (gb.Mass() - gr.Mass() < minRemoved) continue; // graze / coincident no-op
                // Face lineage: capture the cut's face derivation while the op
                // is alive; propagate on the PRE-unify result then carry the
                // map through the merge (below).
                captureLedger(bid, body, cut);
                TopoDS_Shape rawResult = result;
                Handle(BRepTools_History) unifyHist;
                result = materializr::unifySameDomain(result, "Push/Pull cut",
                                                      /*concatBSplines=*/true,
                                                      &unifyHist);
                if (!savedBodies.count(bid)) {
                    m_previousBodies.emplace_back(bid, body);
                    savedBodies.insert(bid);
                }
                snapshotLineage(doc, bid);
                doc.updateBody(bid, result);
                publishLineage(doc, bid, body, rawResult, unifyHist, result);
                ++n;
            } catch (...) { continue; }
        }
        return n;
    };

    for (const auto& tgt : m_targets) {
        if (tgt.profile.IsNull()) continue;

        // Compute push/pull direction. For a flat face this is the face's
        // outward normal at its UV midpoint. For a CURVED face (chamfer cone,
        // fillet torus, cylinder side, etc.) that UV-midpoint normal is the
        // surface tangent perpendicular at one specific point — sloped and
        // dependent on where you happened to click. The user expects a stable
        // axis-aligned direction instead, so we use the surface's natural
        // rotation axis for chamfers/fillets/cylinders/revolves. Sign-correct
        // so positive distance still pushes outward.
        // (Must mirror the logic in Application::beginPushPull so the live
        // arrow and the executed extrusion agree on direction.)
        gp_Vec faceNormal(0, 0, 1);
        try {
            BRepGProp_Face prop(tgt.profile);
            double u1, u2, v1, v2;
            prop.Bounds(u1, u2, v1, v2);
            gp_Pnt center;
            gp_Vec n;
            prop.Normal((u1 + u2) * 0.5, (v1 + v2) * 0.5, center, n);
            if (n.Magnitude() > 1e-10) {
                faceNormal = n.Normalized();
                // NO outward correction: BRepGProp_Face::Normal() already
                // applies the face's topological orientation, so this IS
                // the outward normal — verified against the solid
                // classifier on every face of a pocketed box (REVERSED
                // cavity walls included). Two generations of "fixes" here
                // each broke a case by distrusting it: a classifier probe
                // (misfired on thin bodies), then a bbox-centre heuristic
                // (flipped cavity walls of hollow bodies), then an
                // orientation-flag flip (double-applied what BRepGProp had
                // already done, inverting pocket faces).
                Handle(Geom_Surface) surf = BRep_Tool::Surface(tgt.profile);
                gp_Dir axis; bool hasAxis = false;
                if (auto cone = Handle(Geom_ConicalSurface)::DownCast(surf);
                        !cone.IsNull()) { axis = cone->Axis().Direction(); hasAxis = true; }
                else if (auto tor = Handle(Geom_ToroidalSurface)::DownCast(surf);
                        !tor.IsNull()) { axis = tor->Axis().Direction(); hasAxis = true; }
                else if (auto cyl = Handle(Geom_CylindricalSurface)::DownCast(surf);
                        !cyl.IsNull()) { axis = cyl->Axis().Direction(); hasAxis = true; }
                else if (auto rev = Handle(Geom_SurfaceOfRevolution)::DownCast(surf);
                        !rev.IsNull()) { axis = rev->Axis().Direction(); hasAxis = true; }
                if (hasAxis) {
                    gp_Vec axisVec(axis);
                    if (axisVec.Dot(faceNormal) < 0) axisVec.Reverse();
                    faceNormal = axisVec.Normalized();
                } else if (tgt.sourceBodyId >= 0) {
                    // Planar face: correct a genuinely-inverted orientation
                    // (bug #5 — BRepGProp_Face::Normal pointed INTO the solid)
                    // via the unambiguous antipodal classifier test. Pockets,
                    // cavity walls and thin bodies don't produce that reading,
                    // so they're left exactly as the war story requires.
                    faceNormal = correctedOutwardNormal(
                        doc.getBody(tgt.sourceBodyId), tgt.profile, center, faceNormal);
                }
            }
        } catch (...) {}

        // Sign of distance determines prism direction & boolean mode
        double dir = m_distance >= 0.0 ? 1.0 : -1.0;
        gp_Vec prismVec = faceNormal * (std::abs(m_distance) * dir);

        TopoDS_Shape prism;
        try {
            // Deep-copy the profile so this prism gets its OWN TShapes.
            // MakePrism INSTANCES the profile face as the prism's caps, so
            // two pulls from the same cached region face produced bodies
            // SHARING face TShapes (with each other AND the sketch region)
            // — which scrambled every TShape-keyed subsystem (selection
            // highlight, face context, hover): clicks selected correctly
            // but looked dead.
            TopoDS_Shape ownProfile = BRepBuilderAPI_Copy(tgt.profile).Shape();
            if (m_symmetric) {
                // One prism spanning BOTH sides: start the sweep a full
                // |distance| behind the plane and sweep 2x forward.
                gp_Trsf back;
                back.SetTranslation(prismVec * -1.0);
                ownProfile =
                    BRepBuilderAPI_Transform(ownProfile, back).Shape();
                prismVec *= 2.0;
            }
            BRepPrimAPI_MakePrism mk(ownProfile, prismVec);
            mk.Build();
            if (!mk.IsDone()) continue;
            // Deep-copy the RESULT too: MakePrism instances the profile
            // TShape as BOTH caps, so even with a copied profile the
            // front and back cap of one prism share a face object — and
            // the TShape-keyed selection highlight then draws the back
            // cap's highlight on the front instance (invisible from
            // behind: "selection doesn't register"). Copying the prism
            // makes every face unique.
            prism = BRepBuilderAPI_Copy(mk.Shape()).Shape();
        } catch (...) { continue; }

        if (tgt.sourceBodyId >= 0) {
            // Save original (once per body)
            if (!savedBodies.count(tgt.sourceBodyId)) {
                try {
                    m_previousBodies.emplace_back(tgt.sourceBodyId,
                                                  doc.getBody(tgt.sourceBodyId));
                    savedBodies.insert(tgt.sourceBodyId);
                } catch (...) { continue; }
            }

            TopoDS_Shape current = doc.getBody(tgt.sourceBodyId);
            TopoDS_Shape result;
            try {
                if (m_distance > 0) {
                    BRepAlgoAPI_Fuse fuse(current, prism);
                    fuse.Build();
                    if (!fuse.IsDone()) continue;
                    result = fuse.Shape();
                    captureLedger(tgt.sourceBodyId, current, fuse);
                } else {
                    BRepAlgoAPI_Cut cut(current, prism);
                    cut.Build();
                    if (!cut.IsDone()) continue;
                    result = cut.Shape();
                    captureLedger(tgt.sourceBodyId, current, cut);
                    // CUT vs ADD: when the inward sweep passes through space
                    // the body doesn't own (an existing hole), the cut removes
                    // ~nothing — the gesture is a FILL, not a cut. The margin
                    // is relative to the tool volume: coincident-face booleans
                    // leave ~1e-3 mm³ slivers, and the plug/hole pair can
                    // mismatch by that much noise, so an absolute epsilon
                    // can't tell "grazed" from "cut". Fuse instead — but only
                    // accept a result that welds into ONE solid; a prism that
                    // merely missed the body entirely must stay a no-op, not
                    // graft a disjoint lump onto it.
                    double removed = 0.0, toolVol = 0.0;
                    try {
                        GProp_GProps gc, gr, gt;
                        BRepGProp::VolumeProperties(current, gc);
                        BRepGProp::VolumeProperties(result, gr);
                        BRepGProp::VolumeProperties(prism, gt);
                        removed = gc.Mass() - gr.Mass();
                        toolVol = gt.Mass();
                    } catch (...) {}
                    if (removed < std::max(1e-6, toolVol * 1e-3)) {
                        BRepAlgoAPI_Fuse fill(current, prism);
                        fill.Build();
                        if (!fill.IsDone()) continue;
                        TopoDS_Shape fused = fill.Shape();
                        int nSolids = 0;
                        for (TopExp_Explorer sx(fused, TopAbs_SOLID);
                             sx.More(); sx.Next()) ++nSolids;
                        if (nSolids != 1 ||
                            !BRepCheck_Analyzer(fused).IsValid()) continue;
                        result = fused;
                        captureLedger(tgt.sourceBodyId, current, fill);
                    }
                }
                if (m_distance < 0 && !keepsASolid(result)) {
                    std::fprintf(stderr, "Push/Pull declined for body %d: the "
                                 "cut would remove the entire body\n",
                                 tgt.sourceBodyId);
                    continue;
                }
                TopoDS_Shape rawResult = result;
                Handle(BRepTools_History) unifyHist;
                result = materializr::unifySameDomain(result, "Push/Pull",
                                                      /*concatBSplines=*/true,
                                                      &unifyHist);
                snapshotLineage(doc, tgt.sourceBodyId);
                doc.updateBody(tgt.sourceBodyId, result);
                publishLineage(doc, tgt.sourceBodyId, current, rawResult,
                               unifyHist, result);
                anyChange = true;
            } catch (...) { continue; }
        } else {
            // Free-floating prism. Cut-intersecting subtracts it from every
            // visible body it overlaps; if it hits nothing it falls back to a
            // new body (the "free-space sketch that runs into a body cuts it"
            // feature).
            //
            // BUT a REPLAY must reproduce what was saved. If this op previously
            // created a body for this output (a reuse id is queued — set on redo,
            // or on reload from the saved diff's created-id), it was a NEW-BODY
            // extrude, not a cut. It must recreate that body even though its
            // prism now overlaps an upstream body it didn't overlap at author
            // time. Without this guard a reloaded additive extrude (e.g. a lid
            // built from a duplicated sketch) silently cuts the overlapping box
            // instead of creating its body — and the downstream boolean that
            // consumes the missing body then hard-fails, stranding the whole
            // replay (the "editing the box deletes its floor" bug).
            bool willReuse = m_reuseIdx < m_reuseBodyIds.size();
            bool cutAny = m_cutIntersecting && !willReuse &&
                          (cutVisibleBodies(prism, -1) > 0);
            if (cutAny) anyChange = true;
            if (!cutAny) {
                // Free-floating: create a new body. On redo, m_reuseBodyIds holds
                // the ids from the previous execute so addOrPutBody picks the
                // same one back up (Document's tombstone restore then brings the
                // folder/colour/visibility/name back).
                int id = willReuse ? m_reuseBodyIds[m_reuseIdx] : -1;
                doc.addOrPutBody(id, prism, m_distance > 0 ? "Push" : "Pull");
                m_createdBodyIds.push_back(id);
                ++m_reuseIdx;
                anyChange = true;
            }
        }

        // Attached CUT that runs through the model: after the source body was
        // cut above, also remove the prism from every OTHER visible body in its
        // path (hidden bodies skipped). Only for cut direction — an extrude
        // (add) still only affects its source body.
        if (m_cutIntersecting && tgt.sourceBodyId >= 0 && m_distance < 0.0)
            if (cutVisibleBodies(prism, tgt.sourceBodyId) > 0) anyChange = true;
    }

    // Refused/no-op targets leave saved snapshots behind; only a real
    // mutation makes the op a success — History::pushOperation rejects the
    // rest instead of committing a step that did nothing.
    return anyChange;
}

bool PushPullOp::undo(Document& doc) {
    try {
        // Remove created bodies first
        for (int id : m_createdBodyIds) {
            doc.removeBody(id);
        }
        // Move the just-removed ids into the reuse pool so the next execute
        // (redo) reinserts each free-floating result under the same id and
        // recovers the tombstoned metadata.
        m_reuseBodyIds = std::move(m_createdBodyIds);
        m_createdBodyIds.clear();
        m_reuseIdx = 0;
        // Restore mutated bodies — and their pre-op face lineage, so a partial
        // replay that rolls back to here still has ancestry for the ops it
        // re-runs (updateBody clears the map; the minters never re-run).
        for (const auto& [id, shape] : m_previousBodies) {
            doc.updateBody(id, shape);
            if (auto it = m_prevFaceIds.find(id);
                it != m_prevFaceIds.end() && !it->second.empty())
                doc.setBodyFaceIds(id, it->second);
        }
        m_previousBodies.clear();
        return true;
    } catch (...) {
        return false;
    }
}

std::string PushPullOp::description() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Push/Pull %s (%zu region%s)", materializr::fmtLength(m_distance).c_str(), m_targets.size(), m_targets.size() == 1 ? "" : "s");
    return buf;
}

void PushPullOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Push/Pull"));
    ImGui::Separator();
    materializr::lengthField(materializr::tr("Distance"), &m_distance);
    ImGui::Text(materializr::tr("Regions: %zu"), m_targets.size());
}

OperationDiff PushPullOp::captureDiff() const {
    OperationDiff d;
    for (const auto& [id, shape] : m_previousBodies)
        if (id >= 0 && !shape.IsNull()) d.modifiedBefore.push_back({id, shape});
    for (int id : m_createdBodyIds)
        if (id >= 0) d.created.push_back(id);
    return d;
}

std::string PushPullOp::serializeParams() const {
    // Profiles are NOT stored — each sketch-sourced target re-derives its face
    // from (sketch id, region index) on reload. Targets without a sketch
    // source (a push/pull on a bare body face) still serialise their scalars,
    // but rehydrateFromReload declines them — the face reference needs
    // persistent topological naming to survive a reload.
    std::string blob;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "dist=%.6f;sym=%d;cut=%d;count=%d",
                  m_distance, m_symmetric ? 1 : 0, m_cutIntersecting ? 1 : 0,
                  static_cast<int>(m_targets.size()));
    blob += buf;
    for (size_t i = 0; i < m_targets.size(); ++i) {
        int sk = (i < m_sketchSourceIds.size())     ? m_sketchSourceIds[i]     : -1;
        int rg = (i < m_sketchSourceRegions.size()) ? m_sketchSourceRegions[i] : -1;
        std::snprintf(buf, sizeof(buf), ";s%zu=%d;r%zu=%d;b%zu=%d",
                      i, sk, i, rg, i, m_targets[i].sourceBodyId);
        blob += buf;
        // Face-driven target (no sketch source): persist the profile face as
        // an ordinal index into the source body's PRE-OP shape (the face was
        // picked off the body before this op mutated it).
        if (sk < 0 && m_targets[i].sourceBodyId >= 0 &&
            !m_targets[i].profile.IsNull()) {
            for (const auto& [id, shape] : m_previousBodies) {
                if (id != m_targets[i].sourceBodyId) continue;
                int fIdx = SubShapeIndex::indexOf(shape, m_targets[i].profile,
                                                  TopAbs_FACE);
                if (fIdx > 0) {
                    std::snprintf(buf, sizeof(buf), ";f%zu=%d", i, fIdx);
                    blob += buf;
                }
                break;
            }
        }
        // Topological name of the profile face (additive, robust to a moving
        // edit). The ref blob is delimiter-free (no ';' or '='), so it round-
        // trips through the per-target key parser.
        if (i < m_targetRefs.size() && !m_targetRefs[i].empty()) {
            blob += ";tr" + std::to_string(i) + "=" + m_targetRefs[i].serialize();
        }
    }
    return blob;
}

bool PushPullOp::deserializeParams(const std::string& blob) {
    bool any = false;
    int count = 0;
    // First pass: scalars + target count, so the vectors can be sized before
    // the per-target keys land.
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "dist")  { m_distance = std::atof(val.c_str()); any = true; }
        else if (key == "sym")   { m_symmetric = std::atoi(val.c_str()) != 0; any = true; }
        else if (key == "cut")   { m_cutIntersecting = std::atoi(val.c_str()) != 0; any = true; }
        else if (key == "count") { count = std::atoi(val.c_str()); any = true; }
        pos = end + 1;
    }
    // `count` is supplied by the file and is the SIZE of the five allocations
    // below — the indexed writes further down are bounds-checked against it, but
    // that happens far too late: "count=2000000000" allocates first. Bound it here.
    if (count <= 0) return any;
    if (count > materializr::kMaxProfiles) return false;
    m_targets.assign(count, Target{});          // profiles rebuilt on rehydrate
    m_sketchSourceIds.assign(count, -1);
    m_sketchSourceRegions.assign(count, -1);
    m_faceIndices.assign(count, 0);
    m_targetRefs.assign(count, materializr::topo::Ref{});
    pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if (key.size() >= 3 && key[0] == 't' && key[1] == 'r') {
            int idx = std::atoi(key.c_str() + 2);
            if (idx >= 0 && idx < count)
                m_targetRefs[idx] = materializr::topo::Ref::parse(val);
            any = true;
        }
        else if (key.size() >= 2 && (key[0] == 's' || key[0] == 'r' ||
                                key[0] == 'b' || key[0] == 'f')) {
            int idx = std::atoi(key.c_str() + 1);
            int v   = std::atoi(val.c_str());
            if (idx >= 0 && idx < count) {
                if      (key[0] == 's') m_sketchSourceIds[idx]      = v;
                else if (key[0] == 'r') m_sketchSourceRegions[idx]  = v;
                else if (key[0] == 'f') m_faceIndices[idx]          = v;
                else                    m_targets[idx].sourceBodyId = v;
                any = true;
            }
        }
        pos = end + 1;
    }
    return any;
}

bool PushPullOp::rehydrateFromReload(const ReloadState& state, Document& doc) {
    if (m_targets.empty()) return false;
    // Each target re-derives its profile one of two ways:
    //   - sketch-sourced: rebuild the region face from the persistent sketch
    //   - face-driven: resolve the saved ordinal face index against the
    //     source body's PRE-OP shape from the reload state
    // Any target that can do neither poisons the whole op → ReplayOp.
    for (size_t i = 0; i < m_targets.size(); ++i) {
        bool sketchSourced = i < m_sketchSourceIds.size() && m_sketchSourceIds[i] >= 0;
        bool faceIndexed   = i < m_faceIndices.size() && m_faceIndices[i] > 0 &&
                             m_targets[i].sourceBodyId >= 0;
        if (!sketchSourced && !faceIndexed) return false;
    }
    // Sketch-sourced targets: rebuild each distinct source sketch's targets
    // (the helper fills every target bound to that sketch in one call).
    for (size_t i = 0; i < m_targets.size(); ++i) {
        if (m_sketchSourceIds[i] < 0) continue;
        if (!m_targets[i].profile.IsNull()) continue; // already rebuilt
        if (!rebuildProfileFromSketch(doc, m_sketchSourceIds[i])) return false;
    }
    // Face-driven targets: resolve against the source body's before-shape.
    for (size_t i = 0; i < m_targets.size(); ++i) {
        if (m_sketchSourceIds[i] >= 0) continue;
        const TopoDS_Shape* before = nullptr;
        for (const auto& [id, shp] : state.modifiedBefore) {
            if (id == m_targets[i].sourceBodyId) { before = &shp; break; }
        }
        if (!before) return false;
        TopoDS_Shape f = SubShapeIndex::at(*before, m_faceIndices[i], TopAbs_FACE);
        if (f.IsNull() || f.ShapeType() != TopAbs_FACE) return false;
        m_targets[i].profile = TopoDS::Face(f);
    }
    for (const auto& t : m_targets) {
        if (t.profile.IsNull()) return false;
    }
    // A push/pull must have created or mutated SOMETHING; both sets empty
    // means the step's diff is missing from the file — decline so undo
    // doesn't silently no-op.
    if (state.created.empty() && state.modifiedBefore.empty()) {
        std::fprintf(stderr,
            "[pushpull] rehydrateFromReload DECLINED: state.created and "
            "state.modifiedBefore are BOTH EMPTY (dist=%.3f). This step has "
            "NO body-tracking data in the file — will fall back to ReplayOp.\n",
            m_distance);
        return false;
    }
    // Post-execution bookkeeping from the saved step's body delta, so undo()
    // removes/restores exactly the right bodies and a distance edit re-executes
    // under the same ids (tombstone metadata included).
    m_createdBodyIds = state.created;
    m_previousBodies = state.modifiedBefore;
    m_reuseBodyIds.clear();
    m_reuseIdx = 0;
    return true;
}
