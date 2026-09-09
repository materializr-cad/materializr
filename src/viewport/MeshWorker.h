#pragma once

#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Handle.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace materializr {

// Meshes bodies on a worker thread so a heavy edit does not stall the frame.
//
// The worker never touches a live TShape. request() deep-copies the body on
// the main thread and records which copied face stands for which live face;
// the worker meshes only its private copy; collect() hands back one
// triangulation per live face, and land() moves them onto the live faces on
// the main thread. The main thread's own in-place meshers (a selected face
// with no triangulation, for instance) can therefore run while a job is in
// flight without racing it.
//
// One waiting job per body: a newer request for the same body replaces it.
// A running job always finishes; the caller discards its result if the body
// has moved on. The destructor asks the thread to stop and joins it, so exit
// can wait for one mesh time.
class MeshWorker {
public:
    struct Result {
        int bodyId = -1;
        const void* tshape = nullptr; // identity of the live shape that was copied
        float deflection = 0.0f;
        float angularDeflection = 0.0f;
        struct Face {
            TopoDS_Face live;
            Handle(Poly_Triangulation) tri; // null if the mesher produced none
            // Each edge of the live face with its polygon on `tri`, so the
            // landed face is as complete as a mesher pass leaves it: anything
            // reading BRep_Tool::PolygonOnTriangulation (the push/pull ghost,
            // GhostMesh.h) finds the edge polygons on the live edges.
            std::vector<std::pair<TopoDS_Edge, Handle(Poly_PolygonOnTriangulation)>> edges;
        };
        std::vector<Face> faces;
        int unmeshedFaces = 0;
        double millis = 0.0; // mesher wall time on the worker
    };

    MeshWorker();
    ~MeshWorker();

    // Main thread. Copies `shape` and queues it, replacing a waiting job for
    // the same body. False when the copy failed (the caller meshes in the
    // frame instead); nothing is queued then.
    bool request(int bodyId, const TopoDS_Shape& shape, float deflection,
                 float angularDeflection);

    // Main thread. Results finished since the last call, oldest first.
    std::vector<Result> collect();

    // Main thread. Move a result's triangulations onto its live faces.
    // Returns how many faces received one.
    static int land(const Result& r);

    // Jobs waiting or running.
    size_t pending() const;

    // Hold the worker before it takes its next job, and let it go. For tests
    // that need several requests queued before any of them runs.
    void pause();
    void resume();

private:
    struct Job {
        int bodyId = -1;
        const void* tshape = nullptr;
        float deflection = 0.0f;
        float angularDeflection = 0.0f;
        TopoDS_Shape copy;
        struct Face {
            TopoDS_Face live, copy;
            std::vector<std::pair<TopoDS_Edge, TopoDS_Edge>> edges; // (live, copy)
        };
        std::vector<Face> faces;
    };

    void run();

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_queue;
    std::vector<Result> m_done;
    size_t m_running = 0;
    bool m_stop = false;
    bool m_paused = false;
    std::thread m_thread;
};

} // namespace materializr
