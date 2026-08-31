#include "BoundaryFillOp.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <ShapeFix_Face.hxx>
#include <Standard_ErrorHandler.hxx> // OCC_CATCH_SIGNALS
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <sstream>
#include "../i18n.h"
#include "modeling/ParamParse.h"

namespace {

// Planar face from a wire, with the orientation-fix + reverse-retry dance the
// sketch region builder uses (its hard-won lesson: never coordinate wire
// winding by hand).
TopoDS_Face wireFace(const gp_Pln& plane, TopoDS_Wire w) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        BRepBuilderAPI_MakeFace mf(plane, w);
        if (!mf.IsDone()) return TopoDS_Face();
        ShapeFix_Face fix(mf.Face());
        fix.FixOrientationMode() = 1;
        fix.FixWireMode() = 1;
        fix.Perform();
        TopoDS_Face cand = fix.Face();
        GProp_GProps g;
        BRepGProp::SurfaceProperties(cand, g);
        if (g.Mass() > 0.0 && BRepCheck_Analyzer(cand).IsValid()) return cand;
        w.Reverse();
    }
    return TopoDS_Face();
}

double vol(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

} // namespace

BoundaryFillOp::BoundaryFillOp() = default;

void BoundaryFillOp::addProfile(const TopoDS_Wire& outer,
                                const std::vector<TopoDS_Wire>& holes,
                                const gp_Pln& plane) {
    m_outers.push_back(outer);
    m_holes.push_back(holes);
    m_planes.push_back(plane);
}

void BoundaryFillOp::clearProfiles() {
    m_outers.clear();
    m_holes.clear();
    m_planes.clear();
}

bool BoundaryFillOp::execute(Document& doc) {
    if (m_outers.size() < 2) return false;

    try {
        OCC_CATCH_SIGNALS

        // Extrusion length: the combined bounding box of every profile tells
        // us how far each prism must reach to fully pass through the others.
        Bnd_Box bb;
        for (const auto& w : m_outers) BRepBndLib::Add(w, bb);
        if (bb.IsVoid()) return false;
        double x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        const double L = 1.2 * std::sqrt((x1 - x0) * (x1 - x0) +
                                         (y1 - y0) * (y1 - y0) +
                                         (z1 - z0) * (z1 - z0)) + 1.0;

        TopoDS_Shape acc;
        for (size_t i = 0; i < m_outers.size(); ++i) {
            TopoDS_Face f = wireFace(m_planes[i], m_outers[i]);
            if (f.IsNull()) {
                std::fprintf(stderr,
                    "[BoundaryFill] silhouette %zu didn't form a face.\n", i);
                return false;
            }
            // Symmetric prism: shift the face back by L along its normal, then
            // sweep 2L forward, so the silhouette cuts regardless of which
            // side of its plane the volume sits on.
            gp_Vec n(m_planes[i].Axis().Direction());
            gp_Trsf back;
            back.SetTranslation(n.Multiplied(-L));
            TopoDS_Shape f0 = BRepBuilderAPI_Transform(f, back, true).Shape();
            TopoDS_Shape prism =
                BRepPrimAPI_MakePrism(f0, n.Multiplied(2.0 * L)).Shape();
            if (prism.IsNull()) return false;

            // Hole channels: subtract each hole's prism from this silhouette's
            // prism (boolean, per the region builder's no-wire-winding lesson).
            for (const TopoDS_Wire& hw : m_holes[i]) {
                TopoDS_Face hf = wireFace(m_planes[i], hw);
                if (hf.IsNull()) continue;
                TopoDS_Shape h0 =
                    BRepBuilderAPI_Transform(hf, back, true).Shape();
                TopoDS_Shape hPrism =
                    BRepPrimAPI_MakePrism(h0, n.Multiplied(2.0 * L)).Shape();
                if (hPrism.IsNull()) continue;
                BRepAlgoAPI_Cut cut(prism, hPrism);
                cut.Build();
                if (cut.IsDone() && !cut.Shape().IsNull()) prism = cut.Shape();
            }

            if (acc.IsNull()) {
                acc = prism;
            } else {
                BRepAlgoAPI_Common common(acc, prism);
                common.Build();
                if (!common.IsDone() || common.Shape().IsNull()) {
                    std::fprintf(stderr,
                        "[BoundaryFill] intersection failed at silhouette %zu.\n", i);
                    return false;
                }
                acc = common.Shape();
            }
        }

        // An empty intersection means the silhouettes don't overlap anywhere.
        if (acc.IsNull() || vol(acc) < 1e-9) {
            std::fprintf(stderr,
                "[BoundaryFill] the silhouettes don't enclose a common "
                "volume — nothing to fill.\n");
            return false;
        }
        if (!BRepCheck_Analyzer(acc).IsValid()) {
            std::fprintf(stderr, "[BoundaryFill] result failed validation.\n");
            return false;
        }

        doc.addOrPutBody(m_createdBodyId, acc, "Boundary Fill");
        return true;
    } catch (...) {
        return false;
    }
}

bool BoundaryFillOp::undo(Document& doc) {
    try {
        if (m_createdBodyId >= 0) doc.removeBody(m_createdBodyId);
        return true;
    } catch (...) {
        return false;
    }
}

std::string BoundaryFillOp::description() const {
    return "Boundary fill from " + std::to_string(m_outers.size()) +
           " silhouettes";
}

void BoundaryFillOp::renderProperties() {
    ImGui::Text(materializr::tr("Silhouettes: %d"), static_cast<int>(m_outers.size()));
    ImGui::TextWrapped("%s", materializr::tr("Each profile is extruded through the others and the prisms are intersected (visual hull)."));
}

std::string BoundaryFillOp::serializeParams() const {
    // Scalars + per-profile hole counts + plane frames, then ALL geometry as
    // one length-prefixed ASCII BREP compound (outer_i, holes_i..., repeat) —
    // the LoftOp discipline.
    std::ostringstream head;
    head << "created=" << m_createdBodyId << ";np=" << m_outers.size();
    for (size_t i = 0; i < m_outers.size(); ++i) {
        head << ";h" << i << "=" << m_holes[i].size();
        const gp_Ax3& ax = m_planes[i].Position();
        gp_Pnt o = ax.Location();
        gp_Dir x = ax.XDirection(), y = ax.YDirection();
        head << ";p" << i << "=" << o.X() << "," << o.Y() << "," << o.Z() << ","
             << x.X() << "," << x.Y() << "," << x.Z() << ","
             << y.X() << "," << y.Y() << "," << y.Z();
    }
    BRep_Builder bb;
    TopoDS_Compound comp;
    bb.MakeCompound(comp);
    for (size_t i = 0; i < m_outers.size(); ++i) {
        bb.Add(comp, m_outers[i]);
        for (const auto& h : m_holes[i]) bb.Add(comp, h);
    }
    std::ostringstream os;
    BRepTools::Write(comp, os);
    const std::string brep = os.str();
    return head.str() + ";brep=" + std::to_string(brep.size()) + ":" + brep;
}

bool BoundaryFillOp::deserializeParams(const std::string& blob) {
    clearProfiles();
    int np = 0;
    std::vector<int> holeCounts;
    std::vector<gp_Pln> planes;
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        std::string key = blob.substr(pos, eq - pos);
        if (key == "brep") {
            size_t colon = blob.find(':', eq);
            if (colon == std::string::npos) break;
            // Checked length, bounded by subtraction (ParamParse.h):
            // the old `colon + 1 + nBytes > blob.size()` wrapped on a
            // negative length and let the guard pass.
            size_t nBytes = 0, payload = 0;
            if (!materializr::readLenPrefix(blob, eq + 1, colon, nBytes, payload)) break;
            std::istringstream is(blob.substr(payload, nBytes));
            TopoDS_Shape comp;
            BRep_Builder bb;
            try { BRepTools::Read(comp, is, bb); } catch (...) { return false; }
            TopoDS_Iterator it(comp);
            for (int i = 0; i < np && it.More(); ++i) {
                if (it.Value().ShapeType() != TopAbs_WIRE) return false;
                TopoDS_Wire outer = TopoDS::Wire(it.Value());
                it.Next();
                std::vector<TopoDS_Wire> holes;
                int nh = i < static_cast<int>(holeCounts.size()) ? holeCounts[i] : 0;
                for (int j = 0; j < nh && it.More(); ++j, it.Next()) {
                    if (it.Value().ShapeType() != TopAbs_WIRE) return false;
                    holes.push_back(TopoDS::Wire(it.Value()));
                }
                gp_Pln pl = i < static_cast<int>(planes.size()) ? planes[i]
                                                                : gp_Pln();
                addProfile(outer, holes, pl);
            }
            any = true;
            break;
        }
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "created") { m_createdBodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "np")      { np = std::atoi(val.c_str()); any = true; }
        // h<N> / p<N>: N comes from the file and SIZES the vector below, so it is
        // bounded before the resize. Unbounded, "p2000000000=" asked for a ~200 GB
        // allocation, and idx == INT_MAX made `idx + 1` signed-overflow (UB).
        else if (!key.empty() && key[0] == 'h' && key.size() > 1) {
            int idx = materializr::parseIndexKey(key, 1);
            if (idx < 0 || idx >= materializr::kMaxProfiles) return false;
            int nh = 0;
            if (!materializr::parseWholeInt(val, nh) || nh < 0 || nh > materializr::kMaxHolesPerProfile)
                return false;
            if (idx >= static_cast<int>(holeCounts.size()))
                holeCounts.resize(idx + 1, 0);
            holeCounts[idx] = nh;
        }
        else if (!key.empty() && key[0] == 'p' && key.size() > 1) {
            int idx = materializr::parseIndexKey(key, 1);
            if (idx < 0 || idx >= materializr::kMaxProfiles) return false;
            double v[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
            std::istringstream ps(val);
            for (int k = 0; k < 9; ++k) {
                std::string tokn;
                if (!std::getline(ps, tokn, ',')) break;
                v[k] = std::atof(tokn.c_str());
            }
            // A crafted blob can spell inf/nan; those poison the OCCT constructors
            // below rather than throwing cleanly.
            for (double d : v) if (!std::isfinite(d)) return false;
            if (idx >= static_cast<int>(planes.size()))
                planes.resize(idx + 1, gp_Pln());
            try {
                gp_Dir xd(v[3], v[4], v[5]), yd(v[6], v[7], v[8]);
                planes[idx] = gp_Pln(gp_Ax3(gp_Pnt(v[0], v[1], v[2]),
                                            xd.Crossed(yd), xd));
            } catch (...) { return false; }
        }
        pos = end + 1;
    }
    return any && static_cast<int>(m_outers.size()) == np && np >= 2;
}

bool BoundaryFillOp::rehydrateFromReload(const ReloadState& state, Document&) {
    if (m_outers.size() < 2) return false;
    if (m_createdBodyId < 0 && !state.created.empty())
        m_createdBodyId = state.created.front();
    return true;   // geometry is self-contained; execute() re-intersects
}

OperationDiff BoundaryFillOp::captureDiff() const {
    OperationDiff d;
    if (m_createdBodyId >= 0) d.created.push_back(m_createdBodyId);
    return d;
}
