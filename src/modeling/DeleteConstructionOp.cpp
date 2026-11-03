#include "DeleteConstructionOp.h"
#include <cstdio>
#include <cstdlib>

bool DeleteConstructionOp::execute(Document& doc) {
    if (m_id < 0) return false;
    if (m_isAxis) {
        const AxisEntry* a = doc.getAxis(m_id);
        if (!a) return false;
        m_name = a->name;
        m_visible = a->visible;
        m_origin = a->origin;
        m_direction = a->direction;
        m_halfLength = a->halfLength;
        m_captured = true;
        doc.removeAxis(m_id);
        return true;
    }
    const PlaneEntry* p = doc.getPlane(m_id);
    if (!p) return false;
    m_name = p->name;
    m_visible = p->visible;
    m_plane = p->plane;
    m_halfSize = p->halfSize;
    m_refImage.reset();
    m_meshTrace.reset();
    if (const RefImageEntry* r = doc.getRefImage(m_id)) m_refImage = *r;
    if (const MeshTraceEntry* t = doc.getMeshTrace(m_id)) m_meshTrace = *t;
    m_captured = true;
    doc.removePlane(m_id);
    return true;
}

bool DeleteConstructionOp::undo(Document& doc) {
    if (m_id < 0 || !m_captured) return false;
    if (m_isAxis) {
        if (doc.getAxis(m_id)) return true;
        const int id = doc.addAxis(m_origin, m_direction, m_name, m_id);
        if (id < 0) return false;
        doc.setAxisVisible(id, m_visible);
        return true;
    }
    if (doc.getPlane(m_id)) return true;
    const int id = doc.addPlane(m_plane, m_name, m_id);
    if (id < 0) return false;
    doc.setPlaneVisible(id, m_visible);
    if (m_refImage)  doc.setRefImage(id, *m_refImage);
    if (m_meshTrace) doc.setMeshTrace(id, *m_meshTrace);
    return true;
}

std::string DeleteConstructionOp::description() const {
    const char* what = m_isAxis ? "axis" : "plane";
    if (!m_name.empty()) return "Delete \xE2\x86\x92 " + m_name;
    return std::string("Delete ") + what;
}

std::string DeleteConstructionOp::serializeParams() const {
    // Name LAST, running to end-of-blob, so user names may contain ';' or '='.
    char buf[420];
    if (m_isAxis) {
        std::snprintf(buf, sizeof(buf),
            "kind=axis;id=%d;vis=%d;ox=%.9g;oy=%.9g;oz=%.9g;dx=%.9g;dy=%.9g;dz=%.9g;name=%s",
            m_id, m_visible ? 1 : 0, m_origin.X(), m_origin.Y(), m_origin.Z(),
            m_direction.X(), m_direction.Y(), m_direction.Z(), m_name.c_str());
    } else {
        const gp_Ax3& ax = m_plane.Position();
        std::snprintf(buf, sizeof(buf),
            "kind=plane;id=%d;vis=%d;px=%.9g;py=%.9g;pz=%.9g;"
            "nx=%.9g;ny=%.9g;nz=%.9g;xx=%.9g;xy=%.9g;xz=%.9g;name=%s",
            m_id, m_visible ? 1 : 0,
            ax.Location().X(), ax.Location().Y(), ax.Location().Z(),
            ax.Direction().X(), ax.Direction().Y(), ax.Direction().Z(),
            ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z(),
            m_name.c_str());
    }
    return buf;
}

bool DeleteConstructionOp::deserializeParams(const std::string& blob) {
    bool any = false;
    double v[9] = {0, 0, 0, 0, 0, 1, 1, 0, 0};   // px py pz nx ny nz xx xy xz | ox oy oz dx dy dz
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        const std::string key = blob.substr(pos, eq - pos);
        if (key == "name") { m_name = blob.substr(eq + 1); any = true; break; }
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        const std::string val = blob.substr(eq + 1, end - eq - 1);
        const double d = std::atof(val.c_str());
        if      (key == "kind") { m_isAxis = (val == "axis"); any = true; }
        else if (key == "id")   { m_id = std::atoi(val.c_str()); any = true; }
        else if (key == "vis")  { m_visible = std::atoi(val.c_str()) != 0; }
        else if (key == "px" || key == "ox") v[0] = d;
        else if (key == "py" || key == "oy") v[1] = d;
        else if (key == "pz" || key == "oz") v[2] = d;
        else if (key == "nx" || key == "dx") v[3] = d;
        else if (key == "ny" || key == "dy") v[4] = d;
        else if (key == "nz" || key == "dz") v[5] = d;
        else if (key == "xx") v[6] = d;
        else if (key == "xy") v[7] = d;
        else if (key == "xz") v[8] = d;
        pos = end + 1;
    }
    if (!any) return false;
    try {
        if (m_isAxis) {
            m_origin = gp_Pnt(v[0], v[1], v[2]);
            m_direction = gp_Dir(v[3], v[4], v[5]);
        } else {
            m_plane = gp_Pln(gp_Ax3(gp_Pnt(v[0], v[1], v[2]), gp_Dir(v[3], v[4], v[5]),
                                    gp_Dir(v[6], v[7], v[8])));
        }
        m_captured = true;
    } catch (...) {
        m_captured = false;   // degenerate directions: decline rehydration
    }
    return true;
}

bool DeleteConstructionOp::rehydrateFromReload(const ReloadState& /*state*/, Document& /*doc*/) {
    // The entity is gone from the saved list; the literal in the blob is what a
    // later undo re-adds.
    return m_id >= 0 && m_captured;
}
