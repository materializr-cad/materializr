#include "PushPullPreview.h"

#include "modeling/PushPullOp.h"

#include <BRepBndLib.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <chrono>
#include <cmath>
#include <set>

namespace materializr {

PreviewJob::PreviewJob() = default;
PreviewJob::~PreviewJob() = default;

std::unique_ptr<PreviewJob> PreviewJob::prepare(const BodySnapshot& originals,
                                                const std::vector<PreviewTarget>& targets,
                                                const PreviewParams& params)
{
    // OCCT reports a refused copy or a stale sub-shape by throwing; a preview
    // that cannot be prepared is simply not previewed off-thread.
    try {
        return prepareOrThrow(originals, targets, params);
    } catch (...) {
        return nullptr;
    }
}

std::unique_ptr<PreviewJob> PreviewJob::prepareOrThrow(const BodySnapshot& originals,
                                                       const std::vector<PreviewTarget>& targets,
                                                       const PreviewParams& params)
{
    if (targets.empty() || std::abs(params.distance) < 1e-6) return nullptr;

    // Which live bodies the op may touch: every host body, and, when the tool
    // cuts through the model, every visible body whose box the swept profile
    // could reach. The reach is the profile box grown by the full sweep in
    // every direction, which over-approximates the prism and is cheap.
    std::set<int> wanted;
    Bnd_Box reach;
    for (const PreviewTarget& t : targets) {
        if (t.sourceBodyId >= 0) wanted.insert(t.sourceBodyId);
        if (!t.profile.IsNull()) BRepBndLib::Add(t.profile, reach);
    }
    if (params.cutIntersecting && !reach.IsVoid()) {
        reach.Enlarge(std::abs(params.distance) * (params.symmetric ? 2.0 : 1.0));
        for (const auto& [id, st] : originals) {
            if (!st.visible || st.shape.IsNull()) continue;
            // A mesh import (100k faces) costs more to copy than the op would
            // gain from cutting it; the real op cuts it once, at commit.
            if (st.mesh) continue;
            Bnd_Box bb;
            BRepBndLib::Add(st.shape, bb);
            if (!reach.IsOut(bb)) wanted.insert(id);
        }
    }

    std::unique_ptr<PreviewJob> job(new PreviewJob());
    job->m_scratch = std::make_unique<Document>();
    std::map<int, BRepBuilderAPI_Copy> copiers; // live id -> copier, for profile mapping
    std::map<int, int> liveToScratch;
    for (int id : wanted) {
        auto it = originals.find(id);
        if (it == originals.end() || it->second.shape.IsNull()) continue;
        BRepBuilderAPI_Copy& c = copiers[id];
        c.Perform(it->second.shape, Standard_True, Standard_False);
        const int sid = job->m_scratch->addBody(c.Shape(), "preview");
        job->m_scratchToLive[sid] = id;
        liveToScratch[id] = sid;
    }

    std::vector<PushPullOp::Target> opTargets;
    std::vector<std::pair<int, std::pair<int, int>>> sketchSources; // index -> (sketch, region)
    for (const PreviewTarget& t : targets) {
        if (t.profile.IsNull()) continue;
        PushPullOp::Target ot;
        ot.profile = t.profile;
        if (t.sourceBodyId >= 0) {
            auto ls = liveToScratch.find(t.sourceBodyId);
            if (ls == liveToScratch.end()) continue; // host not in the originals
            ot.sourceBodyId = ls->second;
            // A face-driven target's profile is a face of the host; on the
            // copy it must be the copied face, or the op's liveness scan would
            // swap in the nearest face of the copy. ModifiedShape THROWS for a
            // shape that is not a sub-shape of what was copied (a profile gone
            // stale under a rebuilt host), so ask first; a stale profile is
            // left as is and the op's own re-resolution handles it.
            if (t.sketchId < 0) {
                TopTools_IndexedMapOfShape faces;
                TopExp::MapShapes(originals.at(t.sourceBodyId).shape, TopAbs_FACE, faces);
                bool mappedOntoCopy = false;
                if (faces.Contains(t.profile)) {
                    const TopoDS_Shape mapped = copiers[t.sourceBodyId].ModifiedShape(t.profile);
                    if (!mapped.IsNull() && mapped.ShapeType() == TopAbs_FACE) {
                        ot.profile = TopoDS::Face(mapped);
                        mappedOntoCopy = true;
                    }
                }
                // A stale profile is still a live face; the worker must not
                // touch it either. Its own copy is geometrically the same and
                // the op re-resolves it against the copied host.
                if (!mappedOntoCopy)
                    ot.profile = TopoDS::Face(
                        BRepBuilderAPI_Copy(t.profile, Standard_True, Standard_False).Shape());
            }
        }
        if (t.sketchId >= 0) {
            // A sketch-region face is live too (SketchRenderer meshes regions
            // in place on the main thread): the worker gets its own copy.
            ot.profile = TopoDS::Face(
                BRepBuilderAPI_Copy(t.profile, Standard_True, Standard_False).Shape());
            sketchSources.emplace_back(static_cast<int>(opTargets.size()),
                                       std::make_pair(t.sketchId, t.regionIndex));
        } else if (t.sourceBodyId < 0) {
            // No host and no sketch: nothing above copied it. Every profile
            // the worker sees is a copy, structurally, not by which branch ran.
            ot.profile = TopoDS::Face(
                BRepBuilderAPI_Copy(t.profile, Standard_True, Standard_False).Shape());
        }
        job->m_profiles.push_back(ot.profile);
        opTargets.push_back(ot);
    }
    if (opTargets.empty()) return nullptr;

    job->m_op = std::make_unique<PushPullOp>();
    job->m_op->setTargets(std::move(opTargets));
    job->m_op->setDistance(params.distance);
    job->m_op->setSymmetric(params.symmetric);
    job->m_op->setCutIntersecting(params.cutIntersecting);
    for (const auto& [index, src] : sketchSources)
        job->m_op->setSketchSource(index, src.first, src.second);
    return job;
}

PreviewResult PreviewJob::run()
{
    PreviewResult r;
    if (!m_scratch || !m_op) return r;
    const BodySnapshot before = snapshotBodies(*m_scratch);
    const auto t0 = std::chrono::steady_clock::now();
    try {
        r.ok = m_op->execute(*m_scratch);
    } catch (...) {
        r.ok = false;
    }
    r.millis = std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - t0).count();
    if (!r.ok) return r;
    for (int sid : changedBodies(before, *m_scratch)) {
        TopoDS_Shape shape;
        try { shape = m_scratch->getBody(sid); } catch (...) { continue; }
        auto live = m_scratchToLive.find(sid);
        if (live != m_scratchToLive.end()) r.bodies.emplace_back(live->second, shape);
        else r.created.push_back(shape);
    }
    return r;
}

} // namespace materializr
