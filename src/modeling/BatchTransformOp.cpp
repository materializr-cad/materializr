#include "BatchTransformOp.h"
#include "Mate.h"
#include "Sketch.h"
#include "MeshFreeGTransform.h"

#include <BRepBuilderAPI_ModifyShape.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <Standard_ErrorHandler.hxx> // OCC_CATCH_SIGNALS
#include <cmath>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <string>

bool BatchTransformOp::execute(Document& doc) {
    if (m_bodyIds.empty()) return false;

    // Same ownership rule TransformOp enforces. Without it the multi-body
    // gizmo moved a mate-placed body, the op committed, and the next solve
    // snapped it back - the user watching their drag undo itself. Refuse the
    // whole batch rather than transforming some of it: a partial batch is
    // worse than none. Replay is exempt for the same reason as TransformOp.
    if (!doc.isReplaying())
        for (int id : m_bodyIds)
            if (materializr::bodyIsMatePlaced(doc, id)) return false;
    try {
        // A kernel fault inside the transform below is otherwise FATAL: OCCT
        // raises its signal-as-exception, finds no handler ("an exception was
        // raised, but no catch was found") and aborts the process - the app
        // vanishing mid-drag with no dialog. This macro arms the translation
        // for the catch at the bottom, matching ShellOp / MoveFaceOp.
        OCC_CATCH_SIGNALS

        m_previousShapes.clear();
        m_prevFaceIds.clear();
        // A rigid or uniformly-scaled transform is a gp_Trsf, and gp_GTrsf
        // remembers as much in Form(). It matters a great deal which builder
        // gets used:
        //
        //   BRepBuilderAPI_GTransform::Perform runs BRepBuilderAPI_NurbsConvert
        //   on the shape FIRST - unconditionally, whatever the transform is -
        //   rebuilding every surface and pcurve as a NURBS. For a Move or a
        //   Rotate that work is pure loss: it costs a full geometry rebuild per
        //   body (Steve's multi-body drags logged 1.2 s main-loop stalls), it
        //   discards analytic surfaces, and on geometry the converter can't
        //   handle it dereferences null in NewCurve2d and takes the process
        //   with it (Steve, 2026-07-31: "moving objects... is just outright
        //   closing", SIGSEGV at address 0 under NewCurve2d).
        //
        //   BRepBuilderAPI_Transform relocates the shape instead - no rebuild,
        //   nothing for the converter to choke on. It's what the single-body
        //   TransformOp has always used, which is why moving ONE body was
        //   instant and safe while moving two was neither.
        //
        // So only a genuinely affine transform (non-uniform scale) goes the
        // GTransform route, where the conversion is actually required.
        //
        // A uniform scale is no exception: the gizmo hands it over as a
        // SetVectorialPart diagonal, which reads back as gp_Other, yet k*I plus
        // a translation is an exact gp_Trsf. Without this it took the
        // GTransform route and was NURBS-converted for nothing.
        bool rigid = (m_gtrsf.Form() != gp_Other);
        gp_Trsf rigidTrsf = rigid ? m_gtrsf.Trsf() : gp_Trsf();
        if (!rigid) {
            const gp_Mat& v = m_gtrsf.VectorialPart();
            const double k = v(1, 1);
            bool uniform = k > 0.0;
            for (int r = 1; r <= 3 && uniform; ++r)
                for (int c = 1; c <= 3 && uniform; ++c)
                    uniform = std::abs(v(r, c) - (r == c ? k : 0.0)) < 1e-9;
            if (uniform) {
                const gp_XYZ& t = m_gtrsf.TranslationPart();
                rigidTrsf.SetValues(k, 0, 0, t.X(),
                                    0, k, 0, t.Y(),
                                    0, 0, k, t.Z());
                rigid = true;
            }
        }
        // A body that fails must not leave the ones before it transformed.
        auto revert = [&]() {
            for (const auto& [rid, shp] : m_previousShapes) doc.updateBody(rid, shp);
            for (const auto& [rid, map] : m_prevFaceIds)
                if (!map.empty()) doc.setBodyFaceIds(rid, map);
            m_previousShapes.clear();
            m_prevFaceIds.clear();
        };
        for (int id : m_bodyIds) {
            TopoDS_Shape before;
            try { before = doc.getBody(id); } catch (...) { continue; }
            if (before.IsNull()) continue;

            // Face lineage in - carried 1:1 through the move so a downstream
            // fillet/chamfer keeps resolving its edges; restored by undo.
            materializr::topo::FaceIdMap inMap;
            if (const auto* im = doc.bodyFaceIds(id)) inMap = *im;

            // GTransform on a mesh-free copy: on a meshed body (every body on
            // screen) the plain builder leaves null surfaces that crash the
            // next save - see MeshFreeGTransform.h.
            std::unique_ptr<BRepBuilderAPI_Transform>        rtf;
            std::unique_ptr<materializr::MeshFreeGTransform> gtf;
            if (rigid) {
                rtf = std::make_unique<BRepBuilderAPI_Transform>(
                    before, rigidTrsf, /*copy=*/true);
                if (!rtf->IsDone() || rtf->Shape().IsNull()) { revert(); return false; }
            } else {
                gtf = std::make_unique<materializr::MeshFreeGTransform>(before, m_gtrsf);
                if (!gtf->IsDone()) { revert(); return false; }
            }
            auto modified = [&](const TopoDS_Shape& s) {
                return rtf ? rtf->ModifiedShape(s) : gtf->ModifiedShape(s);
            };
            TopoDS_Shape after = rtf ? rtf->Shape() : gtf->Shape();

            m_previousShapes.push_back({id, before});
            m_prevFaceIds[id] = inMap;
            doc.updateBody(id, after);

            if (!inMap.empty()) {
                materializr::topo::FaceIdMap moved;
                for (const auto& e : inMap) {
                    try {
                        TopoDS_Shape nf = modified(e.face);
                        if (!nf.IsNull()) moved.push_back({nf, e.ids});
                    } catch (...) {}
                }
                if (!moved.empty()) doc.setBodyFaceIds(id, std::move(moved));
            }
        }
        // Sketches riding on the moved bodies take the same rigid motion from their
        // own captured pose (absolute, so a replay lands them in the same place).
        m_prevSketchPlanes.clear();
        if (rigid)
            for (int sid : m_followSketchIds)
                if (auto sk = doc.getSketch(sid)) {
                    m_prevSketchPlanes.push_back({sid, sk->getPlane()});
                    gp_Pln moved = sk->getPlane();
                    moved.Transform(rigidTrsf);
                    sk->setPlane(moved);
                }
        return !m_previousShapes.empty();
    } catch (...) {
        // Undo whatever landed before the failure, so a half-applied batch
        // can't leave some bodies moved and others not.
        try {
            for (const auto& [id, shp] : m_previousShapes) doc.updateBody(id, shp);
        } catch (...) {}
        m_previousShapes.clear();
        return false;
    }
}

bool BatchTransformOp::undo(Document& doc) {
    try {
        for (const auto& [id, shp] : m_previousShapes) {
            doc.updateBody(id, shp);
            auto it = m_prevFaceIds.find(id);
            if (it != m_prevFaceIds.end() && !it->second.empty())
                doc.setBodyFaceIds(id, it->second);
        }
        m_previousShapes.clear();
        for (const auto& [sid, pln] : m_prevSketchPlanes)
            if (auto sk = doc.getSketch(sid)) sk->setPlane(pln);
        m_prevSketchPlanes.clear();
        return true;
    } catch (...) {
        return false;
    }
}

OperationDiff BatchTransformOp::captureDiff() const {
    OperationDiff d;
    for (const auto& [id, shp] : m_previousShapes)
        if (!shp.IsNull()) d.modifiedBefore.push_back({id, shp});
    return d;
}

std::string BatchTransformOp::serializeParams() const {
    std::string s = "bodies=";
    for (size_t i = 0; i < m_bodyIds.size(); ++i)
        s += (i ? "," : "") + std::to_string(m_bodyIds[i]);
    char buf[512];
    // gp_GTrsf as a 3x4 affine matrix (row 1..3, col 1..4).
    std::snprintf(buf, sizeof(buf),
        ";g=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
        m_gtrsf.Value(1,1), m_gtrsf.Value(1,2), m_gtrsf.Value(1,3), m_gtrsf.Value(1,4),
        m_gtrsf.Value(2,1), m_gtrsf.Value(2,2), m_gtrsf.Value(2,3), m_gtrsf.Value(2,4),
        m_gtrsf.Value(3,1), m_gtrsf.Value(3,2), m_gtrsf.Value(3,3), m_gtrsf.Value(3,4));
    s += buf;
    // label/desc last - they carry no ';' or '=' (Move/Rotate/Scale strings).
    if (!m_label.empty()) s += ";label=" + m_label;
    if (!m_desc.empty())  s += ";desc="  + m_desc;
    return s;
}

bool BatchTransformOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string k = blob.substr(pos, eq - pos);
        std::string v = blob.substr(eq + 1, end - eq - 1);
        if (k == "bodies") {
            m_bodyIds.clear();
            size_t p = 0;
            while (p < v.size()) {
                size_t c = v.find(',', p);
                if (c == std::string::npos) c = v.size();
                m_bodyIds.push_back(std::atoi(v.substr(p, c - p).c_str()));
                p = c + 1;
            }
            any = true;
        } else if (k == "g") {
            double m[12] = {0}; int n = 0; size_t p = 0;
            while (n < 12 && p < v.size()) {
                size_t c = v.find(',', p);
                if (c == std::string::npos) c = v.size();
                m[n++] = std::atof(v.substr(p, c - p).c_str());
                p = c + 1;
            }
            if (n == 12) {
                m_gtrsf.SetValue(1,1,m[0]); m_gtrsf.SetValue(1,2,m[1]);
                m_gtrsf.SetValue(1,3,m[2]); m_gtrsf.SetValue(1,4,m[3]);
                m_gtrsf.SetValue(2,1,m[4]); m_gtrsf.SetValue(2,2,m[5]);
                m_gtrsf.SetValue(2,3,m[6]); m_gtrsf.SetValue(2,4,m[7]);
                m_gtrsf.SetValue(3,1,m[8]); m_gtrsf.SetValue(3,2,m[9]);
                m_gtrsf.SetValue(3,3,m[10]); m_gtrsf.SetValue(3,4,m[11]);
                any = true;
            }
        } else if (k == "label") { m_label = v; any = true; }
        else if (k == "desc")   { m_desc  = v; any = true; }
        pos = end + 1;
    }
    return any;
}

bool BatchTransformOp::rehydrateFromReload(const ReloadState& state,
                                           Document& /*doc*/) {
    if (m_bodyIds.empty()) return false;
    // Capture the per-body pre-op shapes (the state editStep rolls back to) so
    // undo/captureDiff work in-session; execute re-applies m_gtrsf on replay.
    m_previousShapes.clear();
    for (int id : m_bodyIds)
        for (const auto& [mid, shp] : state.modifiedBefore)
            if (mid == id) { m_previousShapes.push_back({id, shp}); break; }
    return !m_previousShapes.empty();
}
