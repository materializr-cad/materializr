#include "ui/LengthField.h"
#include "RevolveOp.h"
#include "Sketch.h"
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <cstdio>
#include <cstdlib>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <TopoDS.hxx>
#include <imgui.h>
#include <cmath>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

RevolveOp::RevolveOp() = default;

void RevolveOp::setProfile(const TopoDS_Shape& profile) {
    m_profile = profile;
}

void RevolveOp::setAxis(const gp_Ax1& axis) {
    m_axis = axis;
    m_axisOriginX = axis.Location().X();
    m_axisOriginY = axis.Location().Y();
    m_axisOriginZ = axis.Location().Z();
    m_axisDirX = axis.Direction().X();
    m_axisDirY = axis.Direction().Y();
    m_axisDirZ = axis.Direction().Z();
}

void RevolveOp::setAngle(double degrees) {
    m_angle = degrees;
}

void RevolveOp::setMode(RevolveMode mode) {
    m_mode = mode;
}

void RevolveOp::setTargetBody(int bodyId) {
    m_targetBodyId = bodyId;
}

bool RevolveOp::execute(Document& doc) {
    if (m_profile.IsNull()) {
        return false;
    }

    try {
        // Convert degrees to radians
        double angleRad = m_angle * M_PI / 180.0;

        // Reconstruct axis from current UI values
        gp_Pnt origin(m_axisOriginX, m_axisOriginY, m_axisOriginZ);
        double mag = std::sqrt(m_axisDirX * m_axisDirX +
                               m_axisDirY * m_axisDirY +
                               m_axisDirZ * m_axisDirZ);
        if (mag < 1e-10) {
            return false;
        }
        gp_Dir dir(m_axisDirX, m_axisDirY, m_axisDirZ);
        m_axis = gp_Ax1(origin, dir);

        TopoDS_Shape revolvedShape;

        if (std::abs(m_angle - 360.0) < 1e-6) {
            // Full revolution
            BRepPrimAPI_MakeRevol revol(m_profile, m_axis);
            revol.Build();
            if (!revol.IsDone()) {
                return false;
            }
            revolvedShape = revol.Shape();
        } else {
            // Partial revolution
            BRepPrimAPI_MakeRevol revol(m_profile, m_axis, angleRad);
            revol.Build();
            if (!revol.IsDone()) {
                return false;
            }
            revolvedShape = revol.Shape();
        }

        // Apply boolean mode
        switch (m_mode) {
            case RevolveMode::NewBody: {
                // Reuse prior id on redo so folder/colour/etc. survive
                // through undo+redo via Document's tombstone restore.
                doc.addOrPutBody(m_createdBodyId, revolvedShape, "Lathe");
                break;
            }
            case RevolveMode::Union: {
                if (m_targetBodyId < 0) {
                    return false;
                }
                m_previousTargetShape = doc.getBody(m_targetBodyId);
                BRepAlgoAPI_Fuse fuse(m_previousTargetShape, revolvedShape);
                fuse.Build();
                if (!fuse.IsDone()) {
                    return false;
                }
                doc.updateBody(m_targetBodyId, fuse.Shape());
                m_createdBodyId = -1;
                break;
            }
            case RevolveMode::Subtract: {
                if (m_targetBodyId < 0) {
                    return false;
                }
                m_previousTargetShape = doc.getBody(m_targetBodyId);
                BRepAlgoAPI_Cut cut(m_previousTargetShape, revolvedShape);
                cut.Build();
                if (!cut.IsDone()) {
                    return false;
                }
                doc.updateBody(m_targetBodyId, cut.Shape());
                m_createdBodyId = -1;
                break;
            }
            case RevolveMode::Intersect: {
                if (m_targetBodyId < 0) {
                    return false;
                }
                m_previousTargetShape = doc.getBody(m_targetBodyId);
                BRepAlgoAPI_Common common(m_previousTargetShape, revolvedShape);
                common.Build();
                if (!common.IsDone()) {
                    return false;
                }
                doc.updateBody(m_targetBodyId, common.Shape());
                m_createdBodyId = -1;
                break;
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

bool RevolveOp::undo(Document& doc) {
    try {
        if (m_mode == RevolveMode::NewBody) {
            if (m_createdBodyId >= 0) {
                doc.removeBody(m_createdBodyId);
                // Keep m_createdBodyId for tombstone-based restore on redo.
            }
        } else {
            // Restore previous target shape for boolean operations
            if (m_targetBodyId >= 0 && !m_previousTargetShape.IsNull()) {
                doc.updateBody(m_targetBodyId, m_previousTargetShape);
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool RevolveOp::rebuildProfileFromSketch(Document& doc) {
    if (m_sketchId < 0) return false;
    auto sk = doc.getSketch(m_sketchId);
    if (!sk) return false;
    auto regions = sk->buildRegions();
    // Outermost region (largest outer bbox) — mirrors the Revolve popup's
    // creation pick, so a reload re-derives the same profile (its face
    // carries any inner boundaries as holes).
    int bestIdx = -1;
    double bestDiag = -1.0;
    for (size_t i = 0; i < regions.size(); ++i) {
        if (regions[i].face.IsNull() || regions[i].outerWire.IsNull()) continue;
        Bnd_Box bb;
        BRepBndLib::Add(regions[i].outerWire, bb);
        if (bb.IsVoid()) continue;
        double x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        double dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
        double diag = dx * dx + dy * dy + dz * dz;
        if (diag > bestDiag) { bestDiag = diag; bestIdx = static_cast<int>(i); }
    }
    if (bestIdx < 0) return false;
    m_profile = regions[bestIdx].face;
    return true;
}

OperationDiff RevolveOp::captureDiff() const {
    OperationDiff d;
    if (m_mode == RevolveMode::NewBody) {
        if (m_createdBodyId >= 0) d.created.push_back(m_createdBodyId);
    } else if (m_targetBodyId >= 0 && !m_previousTargetShape.IsNull()) {
        d.modifiedBefore.push_back({m_targetBodyId, m_previousTargetShape});
    }
    return d;
}

std::string RevolveOp::serializeParams() const {
    // Profile geometry is re-derived from the source sketch on reload; the
    // axis is geometric (origin + direction), so it serialises directly.
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "sketch=%d;angle=%.6f;mode=%d;target=%d;"
        "ox=%.6f;oy=%.6f;oz=%.6f;dx=%.6f;dy=%.6f;dz=%.6f",
        m_sketchId, m_angle, static_cast<int>(m_mode), m_targetBodyId,
        m_axisOriginX, m_axisOriginY, m_axisOriginZ,
        m_axisDirX, m_axisDirY, m_axisDirZ);
    return buf;
}

bool RevolveOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        std::string val = blob.substr(eq + 1, end - eq - 1);
        double d = std::atof(val.c_str());
        int    i = std::atoi(val.c_str());
        if      (key == "sketch") { m_sketchId = i; any = true; }
        else if (key == "angle")  { m_angle = d; any = true; }
        else if (key == "mode")   { m_mode = static_cast<RevolveMode>(i); any = true; }
        else if (key == "target") { m_targetBodyId = i; any = true; }
        else if (key == "ox")     { m_axisOriginX = d; any = true; }
        else if (key == "oy")     { m_axisOriginY = d; any = true; }
        else if (key == "oz")     { m_axisOriginZ = d; any = true; }
        else if (key == "dx")     { m_axisDirX = d; any = true; }
        else if (key == "dy")     { m_axisDirY = d; any = true; }
        else if (key == "dz")     { m_axisDirZ = d; any = true; }
        pos = end + 1;
    }
    // execute() reconstructs m_axis from the component doubles, so no gp_Ax1
    // rebuild is needed here.
    return any;
}

bool RevolveOp::rehydrateFromReload(const ReloadState& state, Document& doc) {
    if (m_sketchId < 0) return false;
    if (!rebuildProfileFromSketch(doc)) return false;

    if (m_mode == RevolveMode::NewBody) {
        if (state.created.empty()) return false;
        m_createdBodyId = state.created.front();
    } else {
        for (const auto& [id, shp] : state.modifiedBefore) {
            if (id == m_targetBodyId) { m_previousTargetShape = shp; break; }
        }
        if (m_previousTargetShape.IsNull()) return false;
    }
    return true;
}

std::string RevolveOp::description() const {
    std::string desc = "Lathe " + std::to_string(static_cast<int>(m_angle)) + " deg";
    switch (m_mode) {
        case RevolveMode::NewBody:   desc += " (New Body)"; break;
        case RevolveMode::Union:     desc += " (Union)"; break;
        case RevolveMode::Subtract:  desc += " (Subtract)"; break;
        case RevolveMode::Intersect: desc += " (Intersect)"; break;
    }
    return desc;
}

void RevolveOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Lathe"));
    ImGui::Separator();

    materializr::inputNumber(materializr::tr("Angle (deg)"), &m_angle, 1.0, 10.0, "%.1f");
    if (m_angle < 0.0) m_angle = 0.0;
    if (m_angle > 360.0) m_angle = 360.0;

    ImGui::Separator();
    ImGui::Text("%s", materializr::tr("Axis Origin"));
    materializr::lengthField(materializr::tr("Origin X"), &m_axisOriginX);
    materializr::lengthField(materializr::tr("Origin Y"), &m_axisOriginY);
    materializr::lengthField(materializr::tr("Origin Z"), &m_axisOriginZ);

    ImGui::Separator();
    ImGui::Text("%s", materializr::tr("Axis Direction"));
    materializr::inputNumber(materializr::tr("Dir X"), &m_axisDirX, 0.1, 1.0, "%g");
    materializr::inputNumber(materializr::tr("Dir Y"), &m_axisDirY, 0.1, 1.0, "%g");
    materializr::inputNumber(materializr::tr("Dir Z"), &m_axisDirZ, 0.1, 1.0, "%g");

    ImGui::Separator();
    const char* modeItems[] = { "New Body", "Union", "Subtract", "Intersect" };
    int modeIndex = static_cast<int>(m_mode);
    if (ImGui::Combo(materializr::tr("Mode"), &modeIndex, modeItems, 4)) {
        m_mode = static_cast<RevolveMode>(modeIndex);
    }

    if (m_mode != RevolveMode::NewBody) {
        materializr::inputNumberInt("Target Body ID", &m_targetBodyId);
    }
}
