#include "ui/LengthField.h"
#include "TransformOp.h"
#include "Sketch.h"
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_ModifyShape.hxx>
#include <gp_Trsf.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Mat.hxx>
#include <gp_XYZ.hxx>
#include <gp_Vec.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <BRep_Tool.hxx>
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

TransformOp::TransformOp() = default;

void TransformOp::setBodyId(int id) {
    m_bodyId = id;
}

void TransformOp::setType(TransformType type) {
    m_type = type;
}

void TransformOp::setTranslation(double dx, double dy, double dz) {
    m_dx = dx;
    m_dy = dy;
    m_dz = dz;
}

void TransformOp::setRotation(double ax, double ay, double az, double angleDeg) {
    m_ax = ax;
    m_ay = ay;
    m_az = az;
    m_angle = angleDeg;
}

void TransformOp::setScale(double factor) {
    m_scale = factor;
    m_nonUniform = false;
}

void TransformOp::setScaleXYZ(double sx, double sy, double sz) {
    m_sx = sx;
    m_sy = sy;
    m_sz = sz;
    m_nonUniform = true;
}

void TransformOp::setCenter(double cx, double cy, double cz) {
    m_cx = cx;
    m_cy = cy;
    m_cz = cz;
}

bool TransformOp::execute(Document& doc) {
    if (m_bodyId < 0) {
        return false;
    }

    try {
        // Store previous shape for undo
        m_previousShape = doc.getBody(m_bodyId);
        // And the input face lineage: updateBody wipes it, and a partial
        // replay never re-runs the op that minted it — undo restores this.
        m_prevFaceIds.clear();
        if (const auto* im = doc.bodyFaceIds(m_bodyId)) m_prevFaceIds = *im;
        // Same for sketch planes anchored to this body — they follow the
        // host face/body through Translate / Rotate so sketches drawn on it
        // stay registered to "where they were drawn" even after a move.
        // Scale is deliberately skipped: it changes physical dimensions, which
        // a dimension-driven sketch shouldn't silently absorb. Effectively the
        // sketch detaches on Scale.
        // Link model (2026-06): a body move no longer AUTO-drags its source
        // sketch (that auto-propagation caused the edit-after-move double-
        // transform). Instead the gizmo commit explicitly lists the sketches that
        // should ride along — a unison move (body + its driving sketch moved
        // together). Capture their current planes so the apply loop below
        // transforms them by the same rigid trsf, and undo restores them. This
        // keeps the unison move a single atomic op: the sketch always follows.
        m_previousSketchPlanes.clear();
        for (int sid : m_followSketchIds)
            if (auto sk = doc.getSketch(sid))
                m_previousSketchPlanes.push_back({sid, sk->getPlane()});

        // De-link any sketches this body-only move broke from the body. Capture
        // their prior detached state once so undo can restore the link.
        if (!m_detachSketchIds.empty()) {
            if (!m_haveDetachBefore) {
                for (int sid : m_detachSketchIds)
                    if (auto sk = doc.getSketch(sid))
                        m_detachBefore.push_back({sid, sk->isDetachedFromBody()});
                m_haveDetachBefore = true;
            }
            for (int sid : m_detachSketchIds)
                if (auto sk = doc.getSketch(sid)) sk->setDetachedFromBody(true);
        }

        // Reloaded legacy step: apply the reconstructed rigid transform straight
        // to the LIVE body so any upstream edit (a fillet on this body) survives.
        // Rigid/affine move: carry face lineage 1:1 through the transform
        // (the builder maps each input face to its moved twin). Used by every
        // build path below — without it a transform severs the ancestry chain
        // a downstream fillet/chamfer resolves its edges through.
        auto carryFaceIds = [&](BRepBuilderAPI_ModifyShape& tf) {
            if (m_prevFaceIds.empty()) return;
            materializr::topo::FaceIdMap moved;
            for (auto& e : m_prevFaceIds) {
                try {
                    TopoDS_Shape nf = tf.ModifiedShape(e.face);
                    if (!nf.IsNull()) moved.push_back({nf, e.ids});
                } catch (...) {}
            }
            doc.setBodyFaceIds(m_bodyId, std::move(moved));
        };

        if (m_useRawTrsf) {
            BRepBuilderAPI_Transform tf(m_previousShape, m_rawTrsf, true);
            tf.Build();
            if (!tf.IsDone()) return false;
            doc.updateBody(m_bodyId, tf.Shape());
            carryFaceIds(tf);
            for (const auto& [sid, prevPln] : m_previousSketchPlanes) {
                auto sk = doc.getSketch(sid);
                if (sk) sk->setPlane(prevPln.Transformed(m_rawTrsf));
            }
            return true;
        }

        gp_Pnt center(m_cx, m_cy, m_cz);

        // Non-uniform scale needs a general transform (gp_Trsf can't); scale each
        // axis about the centre with gp_GTrsf (translation keeps the centre fixed).
        if (m_type == TransformType::Scale && m_nonUniform) {
            gp_GTrsf gt;
            gt.SetVectorialPart(gp_Mat(m_sx, 0, 0, 0, m_sy, 0, 0, 0, m_sz));
            gt.SetTranslationPart(gp_XYZ(m_cx - m_sx * m_cx,
                                         m_cy - m_sy * m_cy,
                                         m_cz - m_sz * m_cz));
            BRepBuilderAPI_GTransform gtf(m_previousShape, gt, true);
            gtf.Build();
            if (!gtf.IsDone()) return false;
            doc.updateBody(m_bodyId, gtf.Shape());
            carryFaceIds(gtf);
            return true;
        }

        gp_Trsf trsf;
        switch (m_type) {
            case TransformType::Translate: {
                trsf.SetTranslation(gp_Vec(m_dx, m_dy, m_dz));
                break;
            }
            case TransformType::Rotate: {
                double angleRad = m_angle * M_PI / 180.0;
                gp_Ax1 axis(center, gp_Dir(m_ax, m_ay, m_az));
                trsf.SetRotation(axis, angleRad);
                break;
            }
            case TransformType::Scale: {
                trsf.SetScale(center, m_scale);
                break;
            }
        }

        BRepBuilderAPI_Transform transform(m_previousShape, trsf, true);
        transform.Build();
        if (!transform.IsDone()) {
            return false;
        }

        doc.updateBody(m_bodyId, transform.Shape());
        carryFaceIds(transform);

        // Propagate the SAME gp_Trsf to every sketch anchored to this body
        // so the sketch's plane (and therefore its 2D coordinate frame)
        // stays glued to the body's face. The sketch's own 2D points are
        // already in plane-local coordinates, so transforming just the
        // plane is enough to keep the world-space sketch geometry aligned.
        for (const auto& [sid, prevPln] : m_previousSketchPlanes) {
            auto sk = doc.getSketch(sid);
            if (!sk) continue;
            gp_Pln newPln = prevPln; // copy
            newPln.Transform(trsf);
            sk->setPlane(newPln);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool TransformOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) {
        return false;
    }

    try {
        doc.updateBody(m_bodyId, m_previousShape);
        // Restore the input face lineage captured at execute — updateBody
        // just wiped it, and if this undo is part of a PARTIAL replay
        // (editStep starting after the map's producer), nothing upstream
        // will re-mint it.
        if (!m_prevFaceIds.empty())
            doc.setBodyFaceIds(m_bodyId, m_prevFaceIds);
        // Restore the sketch planes we snapshotted in execute(). Even if
        // some sketches have been removed since, we just skip the missing
        // ones — restoration is best-effort.
        for (const auto& [sid, prevPln] : m_previousSketchPlanes) {
            auto sk = doc.getSketch(sid);
            if (sk) sk->setPlane(prevPln);
        }
        // Re-link sketches this move had de-linked.
        for (const auto& [sid, wasDetached] : m_detachBefore)
            if (auto sk = doc.getSketch(sid)) sk->setDetachedFromBody(wasDetached);
        return true;
    } catch (...) {
        return false;
    }
}

std::string TransformOp::description() const {
    switch (m_type) {
        case TransformType::Translate:
            return "Translate (" + std::to_string(m_dx) + ", " +
                   std::to_string(m_dy) + ", " + std::to_string(m_dz) + ")";
        case TransformType::Rotate:
            return "Rotate " + std::to_string(m_angle) + " deg around (" +
                   std::to_string(m_ax) + ", " + std::to_string(m_ay) + ", " +
                   std::to_string(m_az) + ")";
        case TransformType::Scale:
            return "Scale by " + std::to_string(m_scale);
    }
    return "Transform";
}

void TransformOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Transform"));
    ImGui::Separator();

    const char* typeItems[] = { "Translate", "Rotate", "Scale" };
    int typeIndex = static_cast<int>(m_type);
    if (ImGui::Combo(materializr::tr("Type"), &typeIndex, typeItems, 3)) {
        m_type = static_cast<TransformType>(typeIndex);
    }

    materializr::inputNumberInt("Body ID", &m_bodyId);

    switch (m_type) {
        case TransformType::Translate:
            materializr::lengthField("X", &m_dx);
            materializr::lengthField("Y", &m_dy);
            materializr::lengthField("Z", &m_dz);
            break;
        case TransformType::Rotate:
            materializr::inputNumber(materializr::tr("Axis X"), &m_ax, 0.1, 1.0, "%g");
            materializr::inputNumber(materializr::tr("Axis Y"), &m_ay, 0.1, 1.0, "%g");
            materializr::inputNumber(materializr::tr("Axis Z"), &m_az, 0.1, 1.0, "%g");
            materializr::inputNumber(materializr::tr("Angle (deg)"), &m_angle, 1.0, 15.0, "%.1f");
            break;
        case TransformType::Scale:
            materializr::inputNumber(materializr::tr("Scale Factor"), &m_scale, 0.1, 0.5, "%g");
            break;
    }
}

OperationDiff TransformOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}

std::string TransformOp::serializeParams() const {
    char buf[320];
    if (m_useRawTrsf) {
        std::snprintf(buf, sizeof(buf),
            "body=%d;raw=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
            m_bodyId,
            m_rawTrsf.Value(1,1), m_rawTrsf.Value(1,2), m_rawTrsf.Value(1,3), m_rawTrsf.Value(1,4),
            m_rawTrsf.Value(2,1), m_rawTrsf.Value(2,2), m_rawTrsf.Value(2,3), m_rawTrsf.Value(2,4),
            m_rawTrsf.Value(3,1), m_rawTrsf.Value(3,2), m_rawTrsf.Value(3,3), m_rawTrsf.Value(3,4));
        return buf;
    }
    std::snprintf(buf, sizeof(buf),
        "body=%d;type=%d;dx=%.9g;dy=%.9g;dz=%.9g;ax=%.9g;ay=%.9g;az=%.9g;angle=%.9g;"
        "scale=%.9g;sx=%.9g;sy=%.9g;sz=%.9g;nu=%d;cx=%.9g;cy=%.9g;cz=%.9g",
        m_bodyId, static_cast<int>(m_type), m_dx, m_dy, m_dz, m_ax, m_ay, m_az, m_angle,
        m_scale, m_sx, m_sy, m_sz, m_nonUniform ? 1 : 0, m_cx, m_cy, m_cz);
    return buf;
}

bool TransformOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string k = blob.substr(pos, eq - pos);
        std::string v = blob.substr(eq + 1, end - eq - 1);
        auto d = [&]{ return std::atof(v.c_str()); };
        if      (k == "body")  { m_bodyId = std::atoi(v.c_str()); any = true; }
        else if (k == "type")  { int t = std::atoi(v.c_str());
                                 if (t >= 0 && t <= 2) m_type = static_cast<TransformType>(t); any = true; }
        else if (k == "dx")    { m_dx = d(); any = true; }
        else if (k == "dy")    { m_dy = d(); any = true; }
        else if (k == "dz")    { m_dz = d(); any = true; }
        else if (k == "ax")    { m_ax = d(); any = true; }
        else if (k == "ay")    { m_ay = d(); any = true; }
        else if (k == "az")    { m_az = d(); any = true; }
        else if (k == "angle") { m_angle = d(); any = true; }
        else if (k == "scale") { m_scale = d(); any = true; }
        else if (k == "sx")    { m_sx = d(); any = true; }
        else if (k == "sy")    { m_sy = d(); any = true; }
        else if (k == "sz")    { m_sz = d(); any = true; }
        else if (k == "nu")    { m_nonUniform = std::atoi(v.c_str()) != 0; any = true; }
        else if (k == "cx")    { m_cx = d(); any = true; }
        else if (k == "cy")    { m_cy = d(); any = true; }
        else if (k == "cz")    { m_cz = d(); any = true; }
        else if (k == "raw")   {
            double m[12] = {0}; int n = 0; size_t p = 0;
            while (n < 12 && p < v.size()) {
                size_t c = v.find(',', p);
                if (c == std::string::npos) c = v.size();
                m[n++] = std::atof(v.substr(p, c - p).c_str());
                p = c + 1;
            }
            if (n == 12) {
                m_rawTrsf.SetValues(m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7], m[8],m[9],m[10],m[11]);
                m_useRawTrsf = true; any = true;
            }
        }
        pos = end + 1;
    }
    return any;
}

bool TransformOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_bodyId < 0) return false;
    m_previousShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    return !m_previousShape.IsNull();
}

bool TransformOp::rigidTrsfBetween(const TopoDS_Shape& before,
                                   const TopoDS_Shape& after, gp_Trsf& out) {
    if (before.IsNull() || after.IsNull()) return false;
    TopTools_IndexedMapOfShape vb, va;
    TopExp::MapShapes(before, TopAbs_VERTEX, vb);
    TopExp::MapShapes(after,  TopAbs_VERTEX, va);
    const int n = vb.Extent();
    if (n < 3 || va.Extent() != n) return false;  // not congruent / too few points
    auto Pb = [&](int i){ return BRep_Tool::Pnt(TopoDS::Vertex(vb.FindKey(i))); };
    auto Pa = [&](int i){ return BRep_Tool::Pnt(TopoDS::Vertex(va.FindKey(i))); };

    // Three vertices that form a non-degenerate triangle in `before`.
    gp_Pnt b0 = Pb(1), a0 = Pa(1);
    int i1 = -1;
    for (int i = 2; i <= n; ++i) if (Pb(i).Distance(b0) > 1e-6) { i1 = i; break; }
    if (i1 < 0) return false;
    gp_Pnt b1 = Pb(i1), a1 = Pa(i1);
    gp_Vec v01(b0, b1);
    int i2 = -1;
    for (int i = 2; i <= n; ++i) {
        if (i == i1) continue;
        gp_Vec v0i(b0, Pb(i));
        if (v01.Crossed(v0i).Magnitude() > 1e-6) { i2 = i; break; }
    }
    if (i2 < 0) return false;
    gp_Pnt b2 = Pb(i2), a2 = Pa(i2);

    auto frame = [](const gp_Pnt& o, const gp_Pnt& x, const gp_Pnt& y) {
        gp_Vec vx(o, x), vy(o, y), vz = vx.Crossed(vy);
        return gp_Ax3(o, gp_Dir(vz), gp_Dir(vx));
    };
    try {
        out.SetDisplacement(frame(b0, b1, b2), frame(a0, a1, a2));
    } catch (...) { return false; }

    // Verify it actually maps before→after (rejects non-rigid changes / scale).
    if (b0.Transformed(out).Distance(a0) > 1e-3) return false;
    if (b1.Transformed(out).Distance(a1) > 1e-3) return false;
    if (b2.Transformed(out).Distance(a2) > 1e-3) return false;
    return true;
}
