#include "SweepOp.h"
#include "RevolveOp.h"
#include "Sketch.h"
#include <BRepAdaptor_CompCurve.hxx>
#include <BRepAlgoAPI_Check.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepLib_FindSurface.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_TransitionMode.hxx>
#include <BRepTools.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Plane.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>
#include <cmath>
#include <memory>
#include <imgui.h>
#include "../i18n.h"
#include "../i18n.h"
#include "ParamParse.h"

SweepOp::SweepOp() = default;

void SweepOp::setProfile(const TopoDS_Shape& profile) {
    m_profile = profile;
}

void SweepOp::setPath(const TopoDS_Wire& path) {
    m_path = path;
}

namespace {

struct ProfileFrame {
    gp_Pnt centre;
    gp_Dir normal;
    bool planar = false;
};

// Centroid of a profile and, when it is flat, its plane's normal.
ProfileFrame frameOf(const TopoDS_Shape& profile) {
    ProfileFrame f;
    GProp_GProps g;
    if (TopExp_Explorer(profile, TopAbs_FACE).More()) BRepGProp::SurfaceProperties(profile, g);
    else BRepGProp::LinearProperties(profile, g);
    f.centre = g.CentreOfMass();
    try {
        BRepLib_FindSurface fs(profile, -1.0, Standard_True);
        if (fs.Found()) {
            Handle(Geom_Plane) pl = Handle(Geom_Plane)::DownCast(fs.Surface());
            if (!pl.IsNull()) { f.normal = pl->Axis().Direction(); f.planar = true; }
        }
    } catch (...) {}
    return f;
}

} // namespace

TopoDS_Shape SweepOp::build(const TopoDS_Shape& profile, const TopoDS_Wire& path,
                            bool keepPerpendicular, std::string* why) {
    auto refuse = [&](const char* msg) { if (why) *why = msg; return TopoDS_Shape(); };
    if (profile.IsNull()) return refuse("No profile.");
    if (path.IsNull()) return refuse("No path.");
    try {
        // The path's two ends and the tangent leaving the first.
        TopoDS_Wire spine = path;
        auto curvePtr = std::make_unique<BRepAdaptor_CompCurve>(spine);
        BRepAdaptor_CompCurve* curve = curvePtr.get();
        gp_Pnt first = curve->Value(curve->FirstParameter());
        gp_Pnt last = curve->Value(curve->LastParameter());
        const bool closedPath = first.Distance(last) < 1e-6;

        const ProfileFrame frame = frameOf(profile);
        // The end the profile belongs to becomes the start.
        if (!closedPath && frame.centre.Distance(last) < frame.centre.Distance(first)) {
            spine = TopoDS::Wire(path.Reversed());
            curvePtr = std::make_unique<BRepAdaptor_CompCurve>(spine);
            curve = curvePtr.get();
            first = curve->Value(curve->FirstParameter());
        }
        gp_Pnt at; gp_Vec tangent;
        curve->D1(curve->FirstParameter(), at, tangent);
        if (tangent.Magnitude() < 1e-12) return refuse("The path has no direction at its start.");
        const gp_Dir t(tangent);

        // Carry the profile onto the start of the path...
        TopoDS_Shape moved = profile;
        if (frame.centre.Distance(first) > 1e-9) {
            gp_Trsf tr; tr.SetTranslation(gp_Vec(frame.centre, first));
            moved = BRepBuilderAPI_Transform(moved, tr, true).Shape();
        }
        // ... and, unless asked to leave it, stand it square to the path.
        if (keepPerpendicular && frame.planar) {
            // Either side of the plane will do; take the shorter turn.
            gp_Dir target = frame.normal.Dot(t) >= 0.0 ? t : t.Reversed();
            gp_Vec axis = gp_Vec(frame.normal).Crossed(gp_Vec(target));
            if (axis.Magnitude() > 1e-9) {
                const double angle = std::atan2(axis.Magnitude(), gp_Vec(frame.normal).Dot(gp_Vec(target)));
                gp_Trsf rot; rot.SetRotation(gp_Ax1(first, gp_Dir(axis)), angle);
                moved = BRepBuilderAPI_Transform(moved, rot, true).Shape();
            }
        }

        // One solid per profile face: its outer wire swept, its holes swept and
        // cut away. BRepOffsetAPI_MakePipe is NOT used: along a path with a
        // corner (a polyline - what a quick sketch gives) it silently sweeps only
        // the first leg and reports success. MakePipeShell rounds the corner.
        auto sweepWire = [&](const TopoDS_Wire& w) -> TopoDS_Shape {
            BRepOffsetAPI_MakePipeShell sh(spine);
            sh.SetMode(Standard_False);                       // corrected Frenet
            sh.SetTransitionMode(BRepBuilderAPI_RoundCorner);
            sh.Add(w);
            sh.Build();
            if (!sh.IsDone() || !sh.MakeSolid()) return TopoDS_Shape();
            return sh.Shape();
        };
        auto sweepFace = [&](const TopoDS_Face& f) -> TopoDS_Shape {
            TopoDS_Wire outer = BRepTools::OuterWire(f);
            TopoDS_Shape solid = sweepWire(outer);
            if (solid.IsNull()) return solid;
            for (TopExp_Explorer wx(f, TopAbs_WIRE); wx.More(); wx.Next()) {
                const TopoDS_Wire w = TopoDS::Wire(wx.Current());
                if (w.IsSame(outer)) continue;
                TopoDS_Shape bore = sweepWire(w);
                if (bore.IsNull()) return TopoDS_Shape();
                BRepAlgoAPI_Cut cut(solid, bore);
                if (!cut.IsDone()) return TopoDS_Shape();
                solid = cut.Shape();
            }
            return solid;
        };
        TopoDS_Shape swept;
        double sectionArea = 0.0;
        for (TopExp_Explorer fx(moved, TopAbs_FACE); fx.More(); fx.Next()) {
            const TopoDS_Face f = TopoDS::Face(fx.Current());
            GProp_GProps fg; BRepGProp::SurfaceProperties(f, fg);
            sectionArea += std::abs(fg.Mass());
            TopoDS_Shape one = sweepFace(f);
            if (one.IsNull()) return refuse("The sweep could not be built along this path.");
            if (swept.IsNull()) { swept = one; continue; }
            BRepAlgoAPI_Fuse fuse(swept, one);
            if (!fuse.IsDone()) return refuse("The sweep could not be built along this path.");
            swept = fuse.Shape();
        }
        if (swept.IsNull()) {
            // A wire profile: no area to sweep into a solid.
            return refuse("The sweep has no volume - the profile must be a closed shape.");
        }

        GProp_GProps gp;
        BRepGProp::VolumeProperties(swept, gp);
        // A pipe can come out inside-out; the body is the same either way.
        if (gp.Mass() < -1e-6) {
            swept = swept.Reversed();
            BRepGProp::VolumeProperties(swept, gp);
        }
        if (gp.Mass() < 1e-6) return refuse("The sweep has no volume - the profile must be a closed shape.");
        if (!BRepCheck_Analyzer(swept).IsValid()) return refuse("The sweep is not a valid solid along this path.");
        // A bend tighter than the section folds the tube through itself, which
        // the validity check above does not see. Pappus: a section square to the
        // path and centred on it sweeps a volume of area x path length - a fold
        // loses a lot of that.
        if (keepPerpendicular && frame.planar && sectionArea > 0.0) {
            GProp_GProps lp; BRepGProp::LinearProperties(spine, lp);
            const double expected = sectionArea * lp.Mass();
            if (std::abs(gp.Mass() - expected) > 0.15 * expected)
                return refuse("The path bends too sharply for this profile - the tube would cross itself.");
        }
        // Anything else that crosses itself.
        BRepAlgoAPI_Check chk(swept, Standard_False, Standard_True);
        if (!chk.IsValid()) return refuse("The path bends too sharply for this profile - the tube would cross itself.");
        return swept;
    } catch (...) {
        return refuse("The sweep could not be built along this path.");
    }
}

bool SweepOp::rebuildFromSketches(Document& doc) {
    if (m_profileSketchId < 0 || m_pathSketchId < 0) return false;
    auto psk = doc.getSketch(m_profileSketchId);
    auto wsk = doc.getSketch(m_pathSketchId);
    if (!psk || !wsk) return false;
    const TopoDS_Shape profile = RevolveOp::profileFromSketch(*psk);
    TopoDS_Wire path = wsk->buildOpenWire(/*exact=*/true);
    if (path.IsNull()) {
        const auto wires = wsk->buildWires();
        if (!wires.empty()) path = wires.front();
    }
    if (profile.IsNull() || path.IsNull()) return false;
    m_profile = profile;
    m_path = path;
    return true;
}

bool SweepOp::execute(Document& doc) {
    m_failure.clear();
    if (m_profile.IsNull() || m_path.IsNull()) {
        m_failure = "Pick a profile and a path.";
        return false;
    }
    const TopoDS_Shape swept = build(m_profile, m_path, m_perpendicular, &m_failure);
    if (swept.IsNull()) return false;
    try {
        doc.addOrPutBody(m_createdBodyId, swept, "Sweep");
        return true;
    } catch (...) {
        return false;
    }
}

bool SweepOp::undo(Document& doc) {
    try {
        if (m_createdBodyId >= 0) {
            doc.removeBody(m_createdBodyId);
            // Keep m_createdBodyId - tombstone restore on next execute().
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string SweepOp::description() const {
    return "Sweep profile along path";
}

void SweepOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Sweep"));
    ImGui::Separator();

    if (m_profile.IsNull()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", materializr::tr("No profile selected"));
    } else {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", materializr::tr("Profile set"));
    }

    if (m_path.IsNull()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", materializr::tr("No path selected"));
    } else {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", materializr::tr("Path set"));
    }

    ImGui::Separator();
    ImGui::Checkbox(materializr::tr("Keep the profile square to the path"), &m_perpendicular);
    if (!m_failure.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", m_failure.c_str());
}

OperationDiff SweepOp::captureDiff() const {
    OperationDiff d;
    if (m_createdBodyId >= 0) d.created.push_back(m_createdBodyId);
    return d;
}

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS.hxx>
#include <sstream>

std::string SweepOp::serializeParams() const {
    // Profile + path are raw picked geometry - persist as an ASCII BREP
    // compound [profile, path] embedded in the params (length-prefixed,
    // last; the PARAMS_LEN container is binary-safe).
    std::string blob = "created=" + std::to_string(m_createdBodyId) +
                       ";perp=" + (m_perpendicular ? "1" : "0") +
                       ";ps=" + std::to_string(m_profileSketchId) +
                       ";ws=" + std::to_string(m_pathSketchId);
    BRep_Builder bb;
    TopoDS_Compound comp;
    bb.MakeCompound(comp);
    bb.Add(comp, m_profile);
    bb.Add(comp, m_path);
    std::ostringstream os;
    BRepTools::Write(comp, os);
    const std::string brep = os.str();
    blob += ";brep=" + std::to_string(brep.size()) + ":" + brep;
    return blob;
}

bool SweepOp::deserializeParams(const std::string& blob) {
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
            // the old `colon + 1 + n > blob.size()` wrapped on a
            // negative length and let the guard pass.
            size_t n = 0, payload = 0;
            if (!materializr::readLenPrefix(blob, eq + 1, colon, n, payload)) break;
            std::istringstream is(blob.substr(payload, n));
            TopoDS_Shape comp;
            BRep_Builder bb;
            try { BRepTools::Read(comp, is, bb); } catch (...) { return false; }
            TopoDS_Iterator it(comp);
            if (!it.More()) return false;
            m_profile = it.Value();
            it.Next();
            if (!it.More() || it.Value().ShapeType() != TopAbs_WIRE) return false;
            m_path = TopoDS::Wire(it.Value());
            any = true;
            break;
        }
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        const std::string val = blob.substr(eq + 1, end - eq - 1);
        if (key == "created") { m_createdBodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "perp") m_perpendicular = std::atoi(val.c_str()) != 0;
        else if (key == "ps") m_profileSketchId = std::atoi(val.c_str());
        else if (key == "ws") m_pathSketchId = std::atoi(val.c_str());
        pos = end + 1;
    }
    return any && !m_profile.IsNull() && !m_path.IsNull();
}

bool SweepOp::rehydrateFromReload(const ReloadState& state, Document& doc) {
    // Sketch-sourced: follow the sketches as they are now (the stored copy is the
    // fallback if either has gone).
    if (m_profileSketchId >= 0 && m_pathSketchId >= 0) rebuildFromSketches(doc);
    if (m_profile.IsNull() || m_path.IsNull()) return false;
    if (m_createdBodyId < 0 && !state.created.empty())
        m_createdBodyId = state.created.front();
    return true;
}
