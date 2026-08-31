#include "ProjectSketchOp.h"
#include "Sketch.h"
#include "SubShapeIndex.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <BRepProj_Projection.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRepLib.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom2d_Line.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Dir2d.hxx>
#include <gp_Vec2d.hxx>
#include <gp_Ax3.hxx>
#include <algorithm>
#include <vector>
#include <cmath>
#include <thread>
#include <atomic>
#include <chrono>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <TopExp_Explorer.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <ShapeFix_Face.hxx>
#include <TopTools_ListOfShape.hxx>
// The TopTools_ListIteratorOfListOfShape typedef is provided by
// TopTools_ListOfShape.hxx; the standalone shim header was dropped in OCCT 8.0.
#include <TopoDS.hxx>
#include <gp_Pln.hxx>
#include <imgui.h>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"
#include "ParamParse.h"

namespace {

double shapeVolume(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

double faceArea(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::SurfaceProperties(s, g);
    return g.Mass();
}

// Project one sketch wire onto the target face; of the closed candidates
// (a full cylindrical face yields a near AND a far wire), keep the one
// nearest the sketch plane along the projection direction.
TopoDS_Wire projectNearest(const TopoDS_Wire& w, const TopoDS_Face& f,
                           const gp_Dir& dir, const gp_Pnt& sketchOrigin) {
    TopoDS_Wire best;
    int total = 0, closedCount = 0;
    try {
        BRepProj_Projection proj(w, f, dir);
        double bestD = 1e100;
        for (; proj.More(); proj.Next()) {
            TopoDS_Wire c = proj.Current();
            ++total;
            if (!BRep_Tool::IsClosed(c)) continue;
            ++closedCount;
            GProp_GProps g;
            BRepGProp::LinearProperties(c, g);
            gp_Vec d(sketchOrigin, g.CentreOfMass());
            double t = d.Dot(gp_Vec(dir));
            if (t < bestD) { bestD = t; best = c; }
        }
    } catch (...) {}
    if (best.IsNull()) {
        std::fprintf(stderr,
            "[ProjectSketch]   projection produced %d wire(s), %d closed "
            "— need at least one closed wire to stamp.\n",
            total, closedCount);
    }
    return best;
}

// One projected wire as an oriented face ON the target's surface.
// ShapeFix_Face supplies pcurves / natural-bound trimming on periodic
// surfaces; a clockwise-wound wire shows up as negative area, in which
// case one reversed retry fixes it.
TopoDS_Face singleWireFace(const Handle(Geom_Surface)& surf, TopoDS_Wire w) {
    // On a PERIODIC surface (cylinder) a closed wire bounds two faces: the small
    // region AND its giant complement (the rest of the wrap-around). "First
    // valid" can grab the complement — fine for the outer, fatal for a hole
    // (the cut then removes everything-but-the-hole). So on periodic surfaces
    // keep the SMALLER of the two valid faces. The wrapped wires are clean
    // (no projection slivers), so smallest-area is safe here. Planar faces are
    // bounded — no complement — so they keep the original first-valid behaviour.
    const bool periodic = surf->IsUPeriodic() || surf->IsVPeriodic();
    TopoDS_Face best;
    double bestArea = 1e300;
    for (int attempt = 0; attempt < 2; ++attempt) {
        BRepBuilderAPI_MakeFace mf(surf, w);
        if (mf.IsDone()) {
            ShapeFix_Face fix(mf.Face());
            fix.FixOrientationMode() = 1;
            fix.FixWireMode() = 1;
            fix.Perform();
            TopoDS_Face cand = fix.Face();
            double a = faceArea(cand);
            if (a > 0.0 && BRepCheck_Analyzer(cand).IsValid()) {
                if (!periodic) return cand;
                if (a < bestArea) { bestArea = a; best = cand; }
            }
        }
        w.Reverse();
    }
    return best;
}

// Wrap a sketch wire onto a CYLINDER, label-style: the flat horizontal maps to
// arc-angle (u), the axial position to height (v). The loop is built directly in
// the surface's (u,v) parameter space with CONTINUOUS u (no wrap into [0,2π]),
// so it never splits at the silhouette or seam the way ray-projection does —
// a wide logo wraps cleanly all the way around. `uO` is the angle of the front
// (where the sketch faces), so the sketch origin lands centred there.
TopoDS_Wire wrapWireOnCylinder(const TopoDS_Wire& w,
                               const Handle(Geom_CylindricalSurface)& cyl,
                               const gp_Pnt& O, const gp_Dir& sketchX,
                               double uO) {
    const gp_Ax3& ax = cyl->Position();
    const gp_Pnt P = ax.Location();
    const gp_Dir Z = ax.Direction();
    const double r = cyl->Radius();
    if (r < 1e-9) return TopoDS_Wire();

    // Sample the wire into an ordered point list.
    std::vector<gp_Pnt> pts;
    try {
        for (BRepTools_WireExplorer ex(w); ex.More(); ex.Next()) {
            BRepAdaptor_Curve c(ex.Current());
            GCPnts_QuasiUniformDeflection d(c, 0.05);
            if (!d.IsDone() || d.NbPoints() < 2) continue;
            std::vector<gp_Pnt> seg;
            for (int i = 1; i <= d.NbPoints(); ++i) seg.push_back(d.Value(i));
            if (ex.Current().Orientation() == TopAbs_REVERSED)
                std::reverse(seg.begin(), seg.end());
            for (size_t i = pts.empty() ? 0 : 1; i < seg.size(); ++i)
                pts.push_back(seg[i]);
        }
    } catch (...) { return TopoDS_Wire(); }
    if (pts.size() < 3) return TopoDS_Wire();

    // Map to (u, v): u = uO + horizontal/r (continuous), v = axial height.
    std::vector<gp_Pnt2d> uv;
    uv.reserve(pts.size() + 1);
    for (const auto& pt : pts) {
        double s = gp_Vec(O, pt).Dot(gp_Vec(sketchX));
        double h = gp_Vec(P, pt).Dot(gp_Vec(Z));
        // -s: the cylinder's u winds opposite the sketch's X, so without this the
        // wrapped logo comes out mirrored.
        uv.emplace_back(uO - s / r, h);
    }
    if (uv.front().Distance(uv.back()) > 1e-9) uv.push_back(uv.front());

    // Build pcurve line segments on the cylinder, assemble into a wire.
    try {
        BRepBuilderAPI_MakeWire mw;
        for (size_t i = 0; i + 1 < uv.size(); ++i) {
            const gp_Pnt2d& a = uv[i];
            const gp_Pnt2d& b = uv[i + 1];
            double len = a.Distance(b);
            if (len < 1e-12) continue;
            Handle(Geom2d_Line) ln = new Geom2d_Line(a, gp_Dir2d(gp_Vec2d(a, b)));
            TopoDS_Edge e = BRepBuilderAPI_MakeEdge(ln, cyl, 0.0, len);
            BRepLib::BuildCurve3d(e);
            mw.Add(e);
        }
        if (mw.IsDone()) return mw.Wire();
    } catch (...) {}
    return TopoDS_Wire();
}

// Region face on the target surface: outer wire face MINUS hole wire
// faces, via a boolean cut. Building outer+holes into one MakeFace needs
// every wire's winding coordinated — with several projected holes the
// orientation search chased its tail (a six-bladed aperture logo failed
// both flip attempts). Single-wire faces orient reliably, and the cut
// needs no orientation reasoning at all.
TopoDS_Shape faceOnSurface(const TopoDS_Face& target, TopoDS_Wire outer,
                           const std::vector<TopoDS_Wire>& holes) {
    Handle(Geom_Surface) surf = BRep_Tool::Surface(target);
    TopoDS_Face outerF = singleWireFace(surf, outer);
    if (outerF.IsNull()) return TopoDS_Shape();
    if (holes.empty()) return outerF;

    TopTools_ListOfShape holeFaces;
    for (const auto& h : holes) {
        TopoDS_Face hf = singleWireFace(surf, h);
        if (hf.IsNull()) return TopoDS_Shape();
        holeFaces.Append(hf);
    }
    try {
        BRepAlgoAPI_Cut cut;
        TopTools_ListOfShape args;
        args.Append(outerF);
        cut.SetArguments(args);
        cut.SetTools(holeFaces);
        cut.Build();
        if (!cut.IsDone()) return TopoDS_Shape();
        TopoDS_Shape res = cut.Shape();
        if (res.IsNull() || faceArea(res) <= 0.0) return TopoDS_Shape();
        return res;
    } catch (...) { return TopoDS_Shape(); }
}

} // namespace

ProjectSketchOp::ProjectSketchOp() = default;

void ProjectSketchOp::setBody(int id) { m_bodyId = id; }
void ProjectSketchOp::setTargetFace(const TopoDS_Face& f) { m_targetFace = f; }
void ProjectSketchOp::setSketchId(int id) { m_sketchId = id; }
void ProjectSketchOp::setRegionFilter(std::vector<int> indices) {
    m_regionFilter = std::move(indices);
}
void ProjectSketchOp::setDepth(double d) { m_depth = d; }
void ProjectSketchOp::setMode(Mode m) { m_mode = m; }

bool ProjectSketchOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_sketchId < 0 || m_targetFace.IsNull() ||
        m_depth < 0.01) {
        return false;
    }
    auto sketch = doc.getSketch(m_sketchId);
    if (!sketch) {
        std::fprintf(stderr, "[ProjectSketch] sketch %d not found\n",
                     m_sketchId);
        return false;
    }
    try {
        m_previousShape = doc.getBody(m_bodyId);

        // Re-resolve the target face against the (possibly rebuilt) body.
        // Mint the topo name on the first run; when the stored handle is no
        // longer a live sub-shape (an upstream edit moved/rebuilt the face),
        // resolve the name instead — a stale handle's plane still reads, so
        // without this the stamp lands at the OLD surface, buried or floating.
        {
            if (m_targetRef.empty()) {
                materializr::topo::Context mc;
                mc.doc = &doc; mc.shape = m_previousShape;
                mc.type = TopAbs_FACE;
                m_targetRef = materializr::topo::mint(m_targetFace, mc);
            }
            bool live = false;
            for (TopExp_Explorer ex(m_previousShape, TopAbs_FACE); ex.More();
                 ex.Next())
                if (ex.Current().IsSame(m_targetFace)) { live = true; break; }
            if (!live && !m_targetRef.empty()) {
                materializr::topo::Context rc;
                rc.doc = &doc; rc.shape = m_previousShape;
                rc.type = TopAbs_FACE;
                rc.crossRebuild = true;
                TopoDS_Shape f;
                if (materializr::topo::resolve(m_targetRef, rc, f) &&
                    !f.IsNull() && f.ShapeType() == TopAbs_FACE) {
                    // Orientation sanity guard (see MoveFaceOp): a resolution
                    // that flips the face normal is a mis-resolve — keep the
                    // stale handle's behaviour rather than stamp a wrong wall.
                    auto normalOf = [](const TopoDS_Face& fc, gp_Vec& n) -> bool {
                        try {
                            BRepGProp_Face p(fc);
                            double u1, u2, v1, v2; p.Bounds(u1, u2, v1, v2);
                            gp_Pnt c; p.Normal((u1+u2)*0.5, (v1+v2)*0.5, c, n);
                            return n.Magnitude() > 1e-9;
                        } catch (...) { return false; }
                    };
                    gp_Vec nOld, nNew;
                    if (normalOf(m_targetFace, nOld) &&
                        normalOf(TopoDS::Face(f), nNew) &&
                        nOld.Normalized().Dot(nNew.Normalized()) > 0.7)
                        m_targetFace = TopoDS::Face(f);
                }
            }
        }

        auto regions = sketch->buildRegions();
        if (regions.empty()) {
            std::fprintf(stderr,
                         "[ProjectSketch] sketch has no closed regions\n");
            return false;
        }

        // Projection direction: sketch-plane normal, oriented toward the
        // target face.
        const gp_Pln& pln = sketch->getPlane();
        gp_Pnt org = pln.Location();
        gp_Dir dir = pln.Axis().Direction();
        GProp_GProps fg;
        BRepGProp::SurfaceProperties(m_targetFace, fg);
        gp_Vec toFace(org, fg.CentreOfMass());
        if (toFace.Dot(gp_Vec(dir)) < 0.0) dir.Reverse();

        // If the target is a CYLINDER, wrap the sketch around it (label-style)
        // rather than ray-projecting: ray projection can't reach past the
        // silhouette, so wide logos lost their edge regions. Flat / other faces
        // keep the ray projection.
        Handle(Geom_Surface) tsurf = BRep_Tool::Surface(m_targetFace);
        Handle(Geom_CylindricalSurface) cyl =
            Handle(Geom_CylindricalSurface)::DownCast(tsurf);
        const gp_Dir sketchX = pln.XAxis().Direction();
        double uO = 0.0;
        if (!cyl.IsNull()) {
            const gp_Ax3& cax = cyl->Position();
            // Front = where the outward normal faces the sketch (equals -dir),
            // in the cylinder's cross-section basis.
            double nx = -gp_Vec(dir).Dot(gp_Vec(cax.XDirection()));
            double ny = -gp_Vec(dir).Dot(gp_Vec(cax.YDirection()));
            uO = std::atan2(ny, nx);
        }
        auto projectWire = [&](const TopoDS_Wire& wir) -> TopoDS_Wire {
            return cyl.IsNull()
                ? projectNearest(wir, m_targetFace, dir, org)
                : wrapWireOnCylinder(wir, cyl, org, sketchX, uO);
        };

        // One stamp tool per region: project outer + hole wires, rebuild as
        // a face on the target surface, sweep along the projection direction
        // with a small overlap past the surface so the boolean never sees a
        // tangent contact.
        const double eps = 0.05;
        const gp_Vec dv(dir);
        TopTools_ListOfShape tools;
        double toolVolume = 0.0;
        int skipped = 0;
        reportProgress(0.0f, "Projecting sketch onto face\xE2\x80\xA6");
        for (size_t ri = 0; ri < regions.size(); ++ri) {
            if (!m_regionFilter.empty()) {
                bool wanted = false;
                for (int idx : m_regionFilter)
                    if (idx == static_cast<int>(ri)) { wanted = true; break; }
                if (!wanted) continue;
            }
            // 0..0.9 over the regions; the final boolean owns the last tenth.
            if (reportProgress(0.9f * static_cast<float>(ri) / regions.size(),
                               "Projecting sketch onto face\xE2\x80\xA6"))
                return false; // user cancelled
            const auto& reg = regions[ri];
            std::fprintf(stderr,
                "[ProjectSketch] region %zu: projecting outer wire onto face.\n",
                ri);
            TopoDS_Wire outer = projectWire(reg.outerWire);
            if (outer.IsNull()) { skipped++; continue; }
            std::vector<TopoDS_Wire> holes;
            bool holesOk = true;
            for (const auto& hw : reg.holeWires) {
                TopoDS_Wire ph = projectWire(hw);
                if (ph.IsNull()) { holesOk = false; break; }
                holes.push_back(ph);
            }
            if (!holesOk) { skipped++; continue; }
            TopoDS_Shape sub = faceOnSurface(m_targetFace, outer, holes);
            if (sub.IsNull()) { skipped++; continue; }

            gp_Trsf shift;
            TopoDS_Shape tool;
            if (m_mode == Mode::Engrave) {
                // start a hair OUTSIDE the surface, dig in along dir
                shift.SetTranslation(dv * (-eps));
                TopoDS_Shape start =
                    BRepBuilderAPI_Transform(sub, shift).Shape();
                tool = BRepPrimAPI_MakePrism(start, dv * (m_depth + eps))
                           .Shape();
            } else {
                // start a hair INSIDE the body, raise out against dir
                shift.SetTranslation(dv * eps);
                TopoDS_Shape start =
                    BRepBuilderAPI_Transform(sub, shift).Shape();
                tool = BRepPrimAPI_MakePrism(start, dv * (-(m_depth + eps)))
                           .Shape();
            }
            if (tool.IsNull()) { skipped++; continue; }
            toolVolume += std::abs(shapeVolume(tool));
            tools.Append(tool);
        }
        if (tools.IsEmpty()) {
            std::fprintf(stderr,
                         "[ProjectSketch] no region projected cleanly onto "
                         "the face — the sketch must land fully inside it\n");
            return false;
        }
        if (skipped > 0) {
            std::fprintf(stderr,
                         "[ProjectSketch] %d region(s) skipped (projected "
                         "off the face)\n", skipped);
        }
        // The final boolean is one monolithic OCCT call (can't report mid-call);
        // base ± a list of tools, returning the validated result or a null
        // shape on failure.
        auto applyTools = [&](const TopoDS_Shape& base,
                              const TopTools_ListOfShape& tls,
                              double fuzzy = 0.0) -> TopoDS_Shape {
            TopTools_ListOfShape a;
            a.Append(base);
            TopoDS_Shape r;
            try {
                if (m_mode == Mode::Engrave) {
                    BRepAlgoAPI_Cut op;
                    op.SetArguments(a); op.SetTools(tls);
                    if (fuzzy > 0.0) op.SetFuzzyValue(fuzzy);
                    op.SetRunParallel(Standard_True); op.Build();
                    if (op.IsDone()) r = op.Shape();
                } else {
                    BRepAlgoAPI_Fuse op;
                    op.SetArguments(a); op.SetTools(tls);
                    if (fuzzy > 0.0) op.SetFuzzyValue(fuzzy);
                    op.SetRunParallel(Standard_True); op.Build();
                    if (op.IsDone()) r = op.Shape();
                }
            } catch (...) {}
            if (r.IsNull() || !BRepCheck_Analyzer(r).IsValid()) return TopoDS_Shape();
            return r;
        };

        // Run the boolean(s) on a WORKER thread — the combined cut is one
        // monolithic OCCT call that can't report mid-run, so running it on the
        // main thread froze the UI ("not responding"). The main thread pumps an
        // INDETERMINATE bar + events while the worker computes. The worker has
        // its own FPU state too, so the main thread's GL can't corrupt the
        // boolean. No reportProgress inside the worker — rendering is main-only.
        TopoDS_Shape result;
        std::atomic<bool> done{false};
        std::thread worker([&]() {
            try {
                // One boolean first — cleanest and fastest when every tool's good.
                result = applyTools(m_previousShape, tools);
                if (result.IsNull()) {
                    // A single bad tool sinks one combined boolean. Fall back to
                    // batches, then per-tool, so only the genuinely-degenerate
                    // tools drop — "Select all" survives a few bad regions.
                    std::fprintf(stderr,
                        "[ProjectSketch] combined boolean failed — batching\n");
                    std::vector<TopoDS_Shape> tv;
                    for (TopTools_ListIteratorOfListOfShape it(tools); it.More(); it.Next())
                        tv.push_back(it.Value());
                    TopoDS_Shape acc = m_previousShape;
                    size_t applied = 0;
                    const size_t kBatch = 8;
                    for (size_t s = 0; s < tv.size(); s += kBatch) {
                        TopTools_ListOfShape batch;
                        for (size_t k = s; k < std::min(s + kBatch, tv.size()); ++k)
                            batch.Append(tv[k]);
                        TopoDS_Shape next = applyTools(acc, batch);
                        if (!next.IsNull()) { acc = next; applied += batch.Extent(); }
                        else for (size_t k = s; k < std::min(s + kBatch, tv.size()); ++k) {
                            TopTools_ListOfShape one;
                            one.Append(tv[k]);
                            // No fuzzy retry: a fuzzy-tolerance boolean "rescued"
                            // a sliver region but silently merged/omitted faces on
                            // its neighbours and left the body invalid. Cleanly
                            // dropping the one bad tool beats corrupting adjacent
                            // ones.
                            TopoDS_Shape r1 = applyTools(acc, one);
                            if (!r1.IsNull()) { acc = r1; ++applied; }
                            else std::fprintf(stderr,
                                "[ProjectSketch] tool %zu dropped (degenerate)\n", k);
                        }
                    }
                    if (applied > 0) result = acc;
                }
            } catch (...) {}
            done.store(true);
        });
        bool cancelled = false;
        while (!done.load()) {
            if (reportProgress(-1.0f, "Applying to body\xE2\x80\xA6")) cancelled = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        worker.join();
        if (cancelled) return false;
        if (result.IsNull()) {
            std::fprintf(stderr, "[ProjectSketch] nothing applied\n");
            return false;
        }

        // The volume must move the right way, by no more than the tools.
        double v0 = shapeVolume(m_previousShape);
        double v1 = shapeVolume(result);
        double delta = (m_mode == Mode::Engrave) ? (v0 - v1) : (v1 - v0);
        if (delta < -1e-6 || delta > toolVolume * 1.5 + 1e-6) {
            std::fprintf(stderr,
                         "[ProjectSketch] boolean produced a suspicious "
                         "volume change (%.3f of %.3f tool) — refusing\n",
                         delta, toolVolume);
            return false;
        }

        doc.updateBody(m_bodyId, result);
        return true;
    } catch (...) {
        std::fprintf(stderr, "[ProjectSketch] execute threw\n");
        return false;
    }
}

bool ProjectSketchOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) return false;
    try {
        doc.updateBody(m_bodyId, m_previousShape);
        return true;
    } catch (...) { return false; }
}

std::string ProjectSketchOp::description() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s sketch %.2f mm",
                  m_mode == Mode::Engrave ? "Engrave" : "Emboss", m_depth);
    return buf;
}

void ProjectSketchOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Projection"));
    ImGui::Separator();
    ImGui::Text(materializr::tr("Mode: %s"),
                m_mode == Mode::Engrave ? "Engrave" : "Emboss");
    materializr::inputNumber(materializr::tr("Depth (mm)"), &m_depth, 0.1, 1.0, "%.2f");
    ImGui::Text(materializr::tr("Sketch ID: %d"), m_sketchId);
    ImGui::Text(materializr::tr("Body ID: %d"), m_bodyId);
}

std::string ProjectSketchOp::serializeParams() const {
    std::string blob;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "body=%d;sketch=%d;depth=%.6f;mode=%d",
                  m_bodyId, m_sketchId, m_depth, static_cast<int>(m_mode));
    blob += buf;
    if (!m_regionFilter.empty()) {
        blob += ";regions=";
        for (size_t i = 0; i < m_regionFilter.size(); ++i) {
            if (i) blob += ',';
            blob += std::to_string(m_regionFilter[i]);
        }
    }
    if (!m_previousShape.IsNull() && !m_targetFace.IsNull()) {
        std::string idx = SubShapeIndex::serialize(
            m_previousShape, {m_targetFace}, TopAbs_FACE);
        if (!idx.empty()) blob += ";face=" + idx;
    }
    // Topological face name (see MoveFaceOp); a single length-prefixed opaque
    // blob written LAST — read to end-of-string. Absent in old files.
    if (!m_targetRef.empty()) {
        std::string b = m_targetRef.serialize();
        blob += ";faceref=" + std::to_string(b.size()) + ":" + b;
    }
    return blob;
}

bool ProjectSketchOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        // faceref is a length-prefixed opaque blob written last — read to end.
        if (key == "faceref") {
            std::string rest = blob.substr(eq + 1);
            size_t c = rest.find(':');
            if (c != std::string::npos) {
                // Checked length, bounded by subtraction (ParamParse.h).
                size_t n = 0, payload = 0;
                if (materializr::readLenPrefix(rest, 0, c, n, payload))
                    m_targetRef = materializr::topo::Ref::parse(rest.substr(payload, n));
            }
            any = true;
            break;
        }
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "body")   { m_bodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "sketch") { m_sketchId = std::atoi(val.c_str()); any = true; }
        else if (key == "depth")  { m_depth = std::atof(val.c_str()); any = true; }
        else if (key == "mode") {
            m_mode = std::atoi(val.c_str()) == 1 ? Mode::Emboss
                                                 : Mode::Engrave;
            any = true;
        }
        else if (key == "regions") {
            m_regionFilter.clear();
            size_t p = 0;
            while (p < val.size()) {
                size_t c = val.find(',', p);
                if (c == std::string::npos) c = val.size();
                m_regionFilter.push_back(
                    std::atoi(val.substr(p, c - p).c_str()));
                p = c + 1;
            }
            any = true;
        }
        else if (key == "face") {
            m_faceIndices = SubShapeIndex::parse(val);
            any = true;
        }
        pos = end + 1;
    }
    return any;
}

bool ProjectSketchOp::rehydrateFromReload(const ReloadState& state,
                                          Document& /*doc*/) {
    if (m_bodyId < 0 || m_faceIndices.empty()) return false;

    m_previousShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    if (m_previousShape.IsNull()) return false;

    std::vector<TopoDS_Shape> resolved;
    if (!SubShapeIndex::resolveAll(m_previousShape, m_faceIndices,
                                   TopAbs_FACE, resolved) ||
        resolved.empty()) {
        return false;
    }
    m_targetFace = TopoDS::Face(resolved.front());
    return true;
}

OperationDiff ProjectSketchOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}
