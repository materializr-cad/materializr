#include "SnapshotPreview.h"

#include <BRepBuilderAPI_Copy.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <chrono>

namespace materializr {

std::unique_ptr<SnapshotPreviewJob> SnapshotPreviewJob::prepare(int bodyId,
                                                                const TopoDS_Shape& snapshot,
                                                                std::unique_ptr<Operation> op) {
    if (bodyId < 0 || snapshot.IsNull() || !op) return nullptr;
    try {
        BRepBuilderAPI_Copy copier(snapshot, Standard_True, Standard_False);
        if (!copier.IsDone()) return nullptr;
        // ModifiedShape THROWS for a shape the copier never saw, so check
        // membership first: a parameter that is not part of the snapshot
        // (a face reference gone stale) means there is nothing to preview.
        TopTools_IndexedMapOfShape subs;
        TopExp::MapShapes(snapshot, subs);
        for (TopoDS_Shape* p : op->shapeParams()) {
            if (!p || p->IsNull() || !subs.Contains(*p)) return nullptr;
            // The copier maps by IsSame and hands back FORWARD copies; the op
            // was given the sub-shape as oriented in the body.
            *p = copier.ModifiedShape(*p).Oriented(p->Orientation());
        }
        auto job = std::unique_ptr<SnapshotPreviewJob>(new SnapshotPreviewJob());
        job->m_bodyId = bodyId;
        job->m_copy = copier.Shape();
        job->m_op = std::move(op);
        job->m_scratch = std::make_unique<Document>();
        job->m_scratch->putBody(bodyId, job->m_copy);
        return job;
    } catch (...) {
        return nullptr;
    }
}

SnapshotPreviewResult SnapshotPreviewJob::run() {
    SnapshotPreviewResult out;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        if (m_op && m_scratch && m_op->execute(*m_scratch)) {
            out.shape = m_scratch->getBody(m_bodyId);
            out.ok = !out.shape.IsNull();
        }
    } catch (...) {
        out.ok = false;
        out.shape.Nullify();
    }
    out.millis = std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - t0).count();
    return out;
}

std::vector<TopoDS_Shape> SnapshotPreviewJob::params() const {
    std::vector<TopoDS_Shape> out;
    if (!m_op) return out;
    for (TopoDS_Shape* p : m_op->shapeParams()) out.push_back(*p);
    return out;
}

} // namespace materializr
