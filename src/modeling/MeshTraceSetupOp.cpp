#include "MeshTraceSetupOp.h"
#include "Sketch.h"
#include "../core/Document.h"

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS_Shape.hxx>

#include <cstdlib>
#include <memory>
#include <sstream>

namespace {
struct PlaneDef { const char* name; gp_Dir normal; gp_Dir xDir; };
// Same normal/xDir pairs as ToolAction::StartSketchXY/XZ/YZ in
// Application::handleToolAction - just centered on the mesh, not the origin.
const PlaneDef kDefs[3] = {
    {"Trace: Top",   gp_Dir(0, 1, 0), gp_Dir(1, 0, 0)},
    {"Trace: Front", gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)},
    {"Trace: Right", gp_Dir(1, 0, 0), gp_Dir(0, 0, -1)},
};
} // namespace

bool MeshTraceSetupOp::execute(Document& doc) {
    try {
        TopoDS_Shape shape = doc.getBody(m_bodyId);
        if (shape.IsNull()) return false;
        Bnd_Box box;
        BRepBndLib::Add(shape, box);
        if (box.IsVoid()) return false;
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        const double cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5, cz = (z0 + z1) * 0.5;
        // Each plane sits AT the mesh's boundary along its own normal axis
        // (the bottom, the near side, the front), not the bbox center - a
        // profile traced there is the part's actual outer edge, not a slice
        // through its middle. Still centered on the OTHER two axes so it's
        // not off to one corner. Origins, matched to kDefs' order:
        //   Top   (normal +Y): bottom face  -> y0
        //   Front (normal +Z): near/front face -> z0
        //   Right (normal +X): near/side face  -> x0
        const gp_Pnt origins[3] = {
            {cx, y0, cz},
            {cx, cy, z0},
            {x0, cy, cz},
        };

        // Captured only on the very first run (planeId still -1): a redo
        // must restore the ORIGINAL visibility, not "false" from having
        // just been hidden by this same op on the prior execute().
        if (m_planes[0].planeId < 0) m_wasBodyVisible = doc.isBodyVisible(m_bodyId);

        for (int i = 0; i < 3; ++i) {
            const auto& d = kDefs[i];
            gp_Pln pln(gp_Ax3(origins[i], d.normal, d.xDir));
            PlaneRec& rec = m_planes[i];
            rec.name = d.name;
            // reuseId = rec.planeId: -1 the first time (fresh id), the prior
            // id on redo (undo() removed but did not forget it) - keeps any
            // other reference to this plane valid across undo/redo.
            rec.planeId = doc.addPlane(pln, rec.name, rec.planeId);
            if (rec.planeId < 0) return false;

            auto sk = std::make_shared<materializr::Sketch>();
            sk->setPlane(pln);
            if (rec.sketchId < 0) rec.sketchId = doc.addSketch(sk, rec.name);
            else doc.putSketch(rec.sketchId, sk, rec.name);

            MeshTraceEntry e;
            e.bodyId = m_bodyId;
            e.opacity = 0.5f;
            e.sketchId = rec.sketchId;
            // Shadow by default: the plane sits AT the boundary now, where a
            // cross-section is degenerate (the body doesn't straddle it, so
            // sliceSection finds nothing to cut) - Shadow's silhouette is
            // position-independent and is what "trace the outer edge" needs.
            e.mode = MeshTraceMode::Shadow;
            doc.setMeshTrace(rec.planeId, e);
        }
        doc.setBodyVisible(m_bodyId, false);
        return true;
    } catch (...) {
        return false;
    }
}

bool MeshTraceSetupOp::undo(Document& doc) {
    // Ids are KEPT (not reset to -1) so a redo re-adds under the same ones -
    // same discipline as ConstructionPlaneOp::undo.
    for (int i = 0; i < 3; ++i) {
        if (m_planes[i].sketchId >= 0) doc.removeSketch(m_planes[i].sketchId);
        if (m_planes[i].planeId >= 0) doc.removePlane(m_planes[i].planeId);
    }
    if (m_bodyId >= 0) doc.setBodyVisible(m_bodyId, m_wasBodyVisible);
    return true;
}

std::string MeshTraceSetupOp::description() const {
    // Lead with an alphabetic op name, not the digit - HistoryPanel only
    // translates the leading alphabetic run of a composed description (see
    // its comment), and a leading "3" makes that run zero-length so this
    // would otherwise never translate no matter what the catalogue holds.
    return "Mesh trace setup (3 planes + sketches)";
}

std::string MeshTraceSetupOp::serializeParams() const {
    std::ostringstream os;
    os << "body=" << m_bodyId << ";visible=" << (m_wasBodyVisible ? 1 : 0);
    for (int i = 0; i < 3; ++i)
        os << ";p" << i << "=" << m_planes[i].planeId
           << ";s" << i << "=" << m_planes[i].sketchId;
    return os.str();
}

bool MeshTraceSetupOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        std::string key = blob.substr(pos, eq - pos);
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string val = blob.substr(eq + 1, end - eq - 1);
        const int n = std::atoi(val.c_str());
        if (key == "body") { m_bodyId = n; any = true; }
        else if (key == "visible") { m_wasBodyVisible = (n != 0); }
        else if (key.size() == 2 && (key[0] == 'p' || key[0] == 's') &&
                 key[1] >= '0' && key[1] <= '2') {
            const int idx = key[1] - '0';
            if (key[0] == 'p') m_planes[idx].planeId = n;
            else m_planes[idx].sketchId = n;
        }
        pos = (end < blob.size()) ? end + 1 : blob.size();
    }
    return any;
}
