#pragma once

#include "core/Document.h"
#include "core/Operation.h"

#include <TopoDS_Shape.hxx>

#include <memory>
#include <vector>

namespace materializr {

// What one off-thread snapshot-body preview produced: the body's new shape
// when the op accepted its parameters, else nothing (the engine then shows
// the gesture-start snapshot).
struct SnapshotPreviewResult {
    bool ok = false;
    TopoDS_Shape shape;
    double millis = 0.0;
};

// One snapshot-body preview frame run off the main thread. prepare() copies
// the gesture-start snapshot (BRepBuilderAPI_Copy, so the worker never reads
// a TShape the frame may be meshing), points the op's shape parameters at
// that copy and parks the copy in a scratch Document under the LIVE body id,
// so execute() runs unchanged; run() executes there and reports the scratch
// body. The op is built by the controller exactly as for an inline frame.
class SnapshotPreviewJob {
public:
    // Null when the copy failed or a shape parameter is not part of the
    // snapshot (a stale face reference): there is nothing to preview.
    static std::unique_ptr<SnapshotPreviewJob> prepare(int bodyId,
                                                       const TopoDS_Shape& snapshot,
                                                       std::unique_ptr<Operation> op);

    // Worker thread. Catches what the op throws.
    SnapshotPreviewResult run();

    // The private copy the op runs on, and the op's shape parameters after
    // remapping (for tests: none of them may be a live sub-shape).
    const TopoDS_Shape& copy() const { return m_copy; }
    std::vector<TopoDS_Shape> params() const;

private:
    int m_bodyId = -1;
    TopoDS_Shape m_copy;
    std::unique_ptr<Operation> m_op;
    std::unique_ptr<Document> m_scratch;
};

} // namespace materializr
