#pragma once

#include "core/BodyChanges.h"
#include "core/Document.h"

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <map>
#include <memory>
#include <utility>
#include <vector>

class PushPullOp;

namespace materializr {

// Runs a push/pull preview's real PushPullOp::execute() off the main thread.
//
// prepare() runs on the main thread: it deep-copies every body the op may
// touch (the targets' host bodies, plus every visible body the swept tool
// could reach when the op cuts through the model) into a scratch Document,
// maps face-target profiles onto the copies, and builds a PushPullOp against
// the scratch ids. run() then executes that op on the worker, touching only
// the scratch document, and reports each changed live body's new shape plus
// the shapes of bodies the op created. The live document is never read by
// the worker: booleans read triangulations, and the mesh worker may be
// writing them.
//
// The copies come from `originals`, the bodies as they were before the
// gesture started: the live document carries the previous preview frame,
// which is not what the next one is computed from.

struct PreviewTarget {
    TopoDS_Face profile;
    int sourceBodyId = -1; // -1: free-floating (new body)
    int sketchId = -1;     // >= 0: sketch-region profile, never a face of the host
    int regionIndex = -1;
};

struct PreviewParams {
    double distance = 0.0;
    bool symmetric = false;
    bool cutIntersecting = false;
};

struct PreviewResult {
    bool ok = false;                                    // the op reported a change
    std::vector<std::pair<int, TopoDS_Shape>> bodies;   // live body id -> new shape
    std::vector<TopoDS_Shape> created;                  // new bodies, in creation order
    double millis = 0.0;                                // execute() wall time on the worker
};

class PreviewJob {
public:
    // Main thread. Null when nothing could be prepared (no usable target).
    static std::unique_ptr<PreviewJob> prepare(const BodySnapshot& originals,
                                               const std::vector<PreviewTarget>& targets,
                                               const PreviewParams& params);
    ~PreviewJob();

    // Worker thread. Safe to call exactly once.
    PreviewResult run();

    // Scratch bodies that stand for live ones (for tests and diagnostics).
    size_t copiedBodies() const { return m_scratchToLive.size(); }
    // The profiles the scratch op will sweep, one per usable target, in
    // order. For tests: none of them may share a TShape with a live face.
    const std::vector<TopoDS_Face>& profiles() const { return m_profiles; }

private:
    PreviewJob();
    static std::unique_ptr<PreviewJob> prepareOrThrow(const BodySnapshot& originals,
                                                      const std::vector<PreviewTarget>& targets,
                                                      const PreviewParams& params);
    std::unique_ptr<Document> m_scratch;
    std::map<int, int> m_scratchToLive;
    std::unique_ptr<PushPullOp> m_op;
    std::vector<TopoDS_Face> m_profiles;
};

} // namespace materializr
