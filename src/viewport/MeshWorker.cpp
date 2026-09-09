#include "MeshWorker.h"

#include "core/MeshParams.h"

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>

#include <chrono>
#include <vector>

namespace materializr {

MeshWorker::MeshWorker() : m_thread([this] { run(); }) {}

MeshWorker::~MeshWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

bool MeshWorker::request(int bodyId, const TopoDS_Shape& shape, float deflection,
                         float angularDeflection)
{
    Job job;
    job.bodyId = bodyId;
    job.tshape = shape.TShape().get();
    job.deflection = deflection;
    job.angularDeflection = angularDeflection;
    try {
        // Geometry copied, mesh not: the copy must not share surfaces or
        // triangulations with anything the main thread can still write.
        BRepBuilderAPI_Copy copier(shape, Standard_True, Standard_False);
        job.copy = copier.Shape();
        for (TopExp_Explorer fe(shape, TopAbs_FACE); fe.More(); fe.Next()) {
            Job::Face f;
            f.live = TopoDS::Face(fe.Current());
            f.copy = TopoDS::Face(copier.ModifiedShape(f.live));
            for (TopExp_Explorer ee(f.live, TopAbs_EDGE); ee.More(); ee.Next()) {
                const TopoDS_Edge& e = TopoDS::Edge(ee.Current());
                // The copier maps by IsSame, so both occurrences of a seam
                // edge come back as the same forward copy; give each the
                // live occurrence's orientation, or the orientation-aware
                // polygon lookup on the worker returns the same side twice.
                f.edges.emplace_back(
                    e, TopoDS::Edge(copier.ModifiedShape(e).Oriented(e.Orientation())));
            }
            job.faces.push_back(std::move(f));
        }
    } catch (...) {
        return false; // OCCT refused the copy: not a worker's problem to have
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Job& waiting : m_queue) {
        if (waiting.bodyId == bodyId) {
            waiting = std::move(job);
            return true;
        }
    }
    m_queue.push_back(std::move(job));
    m_wake.notify_one();
    return true;
}

std::vector<MeshWorker::Result> MeshWorker::collect()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Result> out;
    out.swap(m_done);
    return out;
}

int MeshWorker::land(const Result& r)
{
    BRep_Builder builder;
    // Drop each face's old triangulation and the polygons its edges held for
    // it, then install the new ones. (Clean removes only the polygons bound
    // to that face's own triangulation, so the order is a matter of tidiness,
    // not correctness.)
    for (const auto& f : r.faces) BRepTools::Clean(f.live);
    int landed = 0;
    for (const auto& f : r.faces) {
        if (f.tri.IsNull()) continue;
        builder.UpdateFace(f.live, f.tri);
        TopLoc_Location loc;
        BRep_Tool::Triangulation(f.live, loc);
        // A seam edge (a cylinder's) occurs twice in its face, forward and
        // reversed, with a polygon each; both must go on in one call, the
        // single-polygon UpdateEdge replaces what is there for (tri, loc).
        // Group the occurrences of one edge (IsSame: TShape and Location; a
        // face has few edges, a linear partner search is enough).
        std::vector<std::vector<const std::pair<TopoDS_Edge, Handle(Poly_PolygonOnTriangulation)>*>> groups;
        for (const auto& ep : f.edges) {
            bool placed = false;
            for (auto& g : groups)
                if (g.front()->first.IsSame(ep.first)) { g.push_back(&ep); placed = true; break; }
            if (!placed) groups.push_back({&ep});
        }
        for (const auto& occ : groups) {
            if (occ.size() == 1) {
                if (!occ[0]->second.IsNull())
                    builder.UpdateEdge(occ[0]->first, occ[0]->second, f.tri, loc);
                continue;
            }
            Handle(Poly_PolygonOnTriangulation) forward, reversed;
            for (const auto* ep : occ)
                (ep->first.Orientation() == TopAbs_REVERSED ? reversed : forward) = ep->second;
            // P1 is the FORWARD side, P2 the REVERSED one (that is how
            // BRep_Tool::PolygonOnTriangulation picks them). The builder does
            // not read the edge handle's orientation; it is normalised here
            // for clarity only, the argument order is what pairs the sides.
            if (!forward.IsNull() && !reversed.IsNull())
                builder.UpdateEdge(TopoDS::Edge(occ[0]->first.Oriented(TopAbs_FORWARD)),
                                   forward, reversed, f.tri, loc);
            else if (!forward.IsNull())
                builder.UpdateEdge(occ[0]->first, forward, f.tri, loc);
            else if (!reversed.IsNull())
                builder.UpdateEdge(occ[0]->first, reversed, f.tri, loc);
        }
        ++landed;
    }
    return landed;
}

void MeshWorker::pause()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_paused = true;
}

void MeshWorker::resume()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_paused = false;
    }
    m_wake.notify_all();
}

size_t MeshWorker::pending() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size() + m_running;
}

void MeshWorker::run()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || (!m_paused && !m_queue.empty()); });
            if (m_stop) return;
            job = std::move(m_queue.front());
            m_queue.pop_front();
            ++m_running;
        }
        Result r;
        r.bodyId = job.bodyId;
        r.tshape = job.tshape;
        r.deflection = job.deflection;
        r.angularDeflection = job.angularDeflection;
        try {
            const auto t0 = std::chrono::steady_clock::now();
            BRepMesh_IncrementalMesh mesher(
                job.copy, materializr::meshParams(job.deflection, job.angularDeflection, true));
            r.millis = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - t0).count();
            for (const auto& jf : job.faces) {
                Result::Face rf;
                rf.live = jf.live;
                TopLoc_Location loc;
                rf.tri = BRep_Tool::Triangulation(jf.copy, loc);
                if (rf.tri.IsNull()) {
                    ++r.unmeshedFaces;
                } else {
                    for (const auto& [liveEdge, copyEdge] : jf.edges)
                        rf.edges.emplace_back(
                            liveEdge, BRep_Tool::PolygonOnTriangulation(copyEdge, rf.tri, loc));
                }
                r.faces.push_back(std::move(rf));
            }
        } catch (...) {
            r.faces.clear();
            r.unmeshedFaces = static_cast<int>(job.faces.size());
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_done.push_back(std::move(r));
        --m_running;
    }
}

} // namespace materializr
