#include "PatchOp.h"

#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_MakeFilling.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Standard_ErrorHandler.hxx> // OCC_CATCH_SIGNALS
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Shell.hxx>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "../i18n.h"
#include "ParamParse.h"
#include "SubShapeIndex.h"

namespace {

// The parameter blob is untrusted (see ParamParse.h). A boundary ring is a
// handful of edges in every real model; anything past this is a crafted file
// asking us to allocate, not a part.
constexpr int kMaxPatchEdges = 4096;

// The continuity value MakeFilling actually wants.
//
// Curvature is GeomAbs_C1, not GeomAbs_G2, and that is not a typo. The kernel
// documents Add(edge, face, Order) as taking GeomAbs_C0 / G1 / G2, but the
// value is carried to BRepFill_CurveConstraint as a plain Standard_Integer:
//
//     myOrder = Tang;
//     if ((Tang < -1) || (Tang > 2))
//       throw Standard_Failure("BRepFill : The continuity is not G0 G1 or G2");
//
// and in the GeomAbs_Shape enum G2 == 3, so passing the documented value throws
// the very error that claims the value is wrong. The enumerator whose ordinal
// is 2 — the one the constraint reads as second-order — is GeomAbs_C1.
GeomAbs_Shape occOrder(PatchOp::Continuity c) {
    switch (c) {
        case PatchOp::Continuity::Tangent:   return GeomAbs_G1;   // ordinal 1
        case PatchOp::Continuity::Curvature: return GeomAbs_C1;   // ordinal 2
        case PatchOp::Continuity::Position:
        default:                             return GeomAbs_C0;   // ordinal 0
    }
}

bool isSolid(const TopoDS_Shape& s) {
    return !s.IsNull() && TopExp_Explorer(s, TopAbs_SOLID).More();
}

// Edges of `s` carried by exactly one face: the shape's open boundary. A
// degenerate edge (a cone apex, a sphere pole) is a parametric artefact with
// no free side, so it never counts as a hole to be filled.
int freeEdgeCount(const TopoDS_Shape& s) {
    if (s.IsNull()) return 0;
    TopTools_IndexedDataMapOfShapeListOfShape anc;
    TopExp::MapShapesAndAncestors(s, TopAbs_EDGE, TopAbs_FACE, anc);
    int n = 0;
    for (int i = 1; i <= anc.Extent(); ++i) {
        const TopoDS_Edge& e = TopoDS::Edge(anc.FindKey(i));
        if (BRep_Tool::Degenerated(e)) continue;
        if (anc.FindFromIndex(i).Extent() == 1) ++n;
    }
    return n;
}

double area(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::SurfaceProperties(s, g);
    return g.Mass();
}

double volume(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

bool faceCarriesEdge(const TopoDS_Face& f, const TopoDS_Edge& e) {
    for (TopExp_Explorer ex(f, TopAbs_EDGE); ex.More(); ex.Next())
        if (ex.Current().IsSame(e)) return true;
    return false;
}

} // namespace

PatchOp::PatchOp() = default;

void PatchOp::setEdges(const std::vector<TopoDS_Edge>& edges) { m_edges = edges; }
void PatchOp::setBody(int bodyId) { m_bodyId = bodyId; }
void PatchOp::setSupportFaces(const std::vector<TopoDS_Face>& faces) { m_supports = faces; }

// ── Input resolution ────────────────────────────────────────────────────────

std::vector<TopoDS_Edge> PatchOp::resolveEdges(const TopoDS_Shape& base) const {
    // Handles stored in this op are from the body as it stood when the patch
    // was first made. A replay (or a reload) hands us a rebuilt body whose
    // TShapes are all new, so constrain the CURRENT edges where we can find
    // them and fall back to the stored geometry where we can't — a fit against
    // a stale-but-correct curve still lands in the right place, it just won't
    // sew.
    if (base.IsNull() || m_edges.empty()) return m_edges;
    std::vector<TopoDS_Edge> live = m_edges;
    if (SubShapeIndex::rebindEdges(base, live)) return live;
    return m_edges;
}

TopoDS_Face PatchOp::supportFor(
    const TopoDS_Edge& edge,
    const TopTools_IndexedDataMapOfShapeListOfShape& ancestors) const {
    // An explicitly-picked face wins: it is the user resolving the ambiguity
    // that arises whenever an edge has a face on both sides (a patch bridging
    // a notch can blend into either wall, and only they know which).
    for (const TopoDS_Face& f : m_supports)
        if (!f.IsNull() && faceCarriesEdge(f, edge)) return f;

    if (!ancestors.Contains(edge)) return TopoDS_Face();
    const TopTools_ListOfShape& faces = ancestors.FindFromKey(edge);
    // One face = a free boundary edge, which is the case this tool is for and
    // has no ambiguity. Two or more = an interior edge; take the first rather
    // than refusing, so blending into a notch still works without forcing the
    // user to pick supports by hand.
    if (faces.IsEmpty()) return TopoDS_Face();
    return TopoDS::Face(faces.First());
}

// ── Sewing the patch back into the body ─────────────────────────────────────

TopoDS_Shape PatchOp::healInto(const TopoDS_Shape& base,
                               const TopoDS_Face& patch) const {
    if (base.IsNull() || patch.IsNull()) return TopoDS_Shape();

    const bool wasSolid = isSolid(base);
    const int baseFree = freeEdgeCount(base);
    // A closed solid has nothing to heal. Filling a void means there IS a
    // void; without one the patch belongs beside the body, not inside it.
    if (wasSolid && baseFree == 0) return TopoDS_Shape();

    // Tolerance ladder, same reasoning as the loft's sew-into-body path: the
    // patch boundary is a fitted approximation of the rim, so it lands microns
    // off along the runs. Start tight so a clean rim sews exactly, and only
    // loosen for a rim that needs it.
    for (double tol : {1e-4, 1e-3, 1e-2}) {
        try {
            OCC_CATCH_SIGNALS
            BRepBuilderAPI_Sewing sew(tol);
            for (TopExp_Explorer ex(base, TopAbs_FACE); ex.More(); ex.Next())
                sew.Add(ex.Current());
            sew.Add(patch);
            sew.Perform();
            const TopoDS_Shape sewn = sew.SewedShape();
            if (sewn.IsNull()) continue;

            if (sew.NbFreeEdges() == 0) {
                for (TopExp_Explorer ex(sewn, TopAbs_SHELL); ex.More(); ex.Next()) {
                    BRepBuilderAPI_MakeSolid ms(TopoDS::Shell(ex.Current()));
                    if (!ms.IsDone()) break;
                    const TopoDS_Shape solid = ms.Solid();
                    if (BRepCheck_Analyzer(solid).IsValid() &&
                        std::abs(volume(solid)) > 1e-9)
                        return solid;
                }
                // Closed but not solidifiable (an inside-out shell, say): the
                // sewn shell is still strictly better than an unpatched hole.
                if (!wasSolid && BRepCheck_Analyzer(sewn).IsValid()) return sewn;
                continue;
            }

            // Still open. Only accept it if the patch actually reduced the
            // opening — otherwise we would be swapping one broken shape for
            // another and calling it a fix.
            if (!wasSolid && freeEdgeCount(sewn) < baseFree &&
                BRepCheck_Analyzer(sewn).IsValid())
                return sewn;
        } catch (...) {
            // Fall through to the next tolerance.
        }
    }
    return TopoDS_Shape();
}

// ── Fitting ─────────────────────────────────────────────────────────────────

bool PatchOp::fitOnce(const std::vector<TopoDS_Edge>& edges,
                      const TopTools_IndexedDataMapOfShapeListOfShape& ancestors,
                      GeomAbs_Shape order, const TopoDS_Face& initFace,
                      Fit& out) const {
    try {
        OCC_CATCH_SIGNALS

        BRepOffsetAPI_MakeFilling fill(
            m_solver.degree, m_solver.nbPtsOnCur, m_solver.nbIter,
            m_solver.anisotropic ? Standard_True : Standard_False,
            m_solver.tol2d, m_solver.tol3d, m_solver.tolAng, m_solver.tolCurv,
            m_solver.maxDeg, m_solver.maxSegments);
        if (!initFace.IsNull()) fill.LoadInitSurface(initFace);

        out.unsupported = 0;
        for (const TopoDS_Edge& e : edges) {
            if (e.IsNull()) continue;
            TopoDS_Face sup;
            if (order != GeomAbs_C0) sup = supportFor(e, ancestors);
            if (!sup.IsNull()) {
                fill.Add(e, sup, order);
            } else {
                // No neighbour to blend with: this stretch of the boundary is
                // held in position only. Degrading per-edge rather than failing
                // the whole patch is deliberate — a ring where three sides touch
                // the body and one spans open air is a normal bridging case, not
                // an error.
                if (order != GeomAbs_C0) ++out.unsupported;
                fill.Add(e, GeomAbs_C0);
            }
        }

        fill.Build();
        if (!fill.IsDone() || fill.Shape().IsNull()) return false;
        if (fill.Shape().ShapeType() != TopAbs_FACE) return false;

        const TopoDS_Face patch = TopoDS::Face(fill.Shape());
        if (!BRepCheck_Analyzer(patch).IsValid() || area(patch) <= 1e-12)
            return false;

        out.face = patch;
        out.g0 = fill.G0Error();
        out.g1 = fill.G1Error();
        out.g2 = fill.G2Error();
        return true;
    } catch (...) {
        // Every failure mode of this solver arrives as an exception rather than
        // a return code — a null pcurve, a degenerate initial plane
        // ("Geom_RectangularTrimmedSurface::V1==V2"), a plate that will not
        // converge. The caller's ladder decides what to try next.
        return false;
    }
}

// ── Operation ───────────────────────────────────────────────────────────────

bool PatchOp::execute(Document& doc) {
    if (m_edges.empty()) return false;

    try {
        OCC_CATCH_SIGNALS

        TopoDS_Shape base;
        if (m_bodyId >= 0) base = doc.getBody(m_bodyId);

        const std::vector<TopoDS_Edge> edges = resolveEdges(base);
        TopTools_IndexedDataMapOfShapeListOfShape ancestors;
        if (!base.IsNull())
            TopExp::MapShapesAndAncestors(base, TopAbs_EDGE, TopAbs_FACE, ancestors);

        const GeomAbs_Shape order = occOrder(m_continuity);

        // Fit ladder. Each rung is a real failure mode seen on this kernel, and
        // each is tried in turn rather than reported, because the user asked for
        // a filled void and a position-only patch is still one:
        //
        //  1. Straight ask. Works whenever the surrounding faces lean at least a
        //     couple of degrees away from the plane of the opening.
        //  2. Same ask, seeded with a C0 pre-fit as the initial surface. Rescues
        //     the case where GeomPlate's own average-plane guess is degenerate
        //     and the constructor throws before the solve even starts.
        //  3. Position only. The surface the user can always have.
        Fit fit;
        bool fitted = fitOnce(edges, ancestors, order, TopoDS_Face(), fit);
        if (!fitted && order != GeomAbs_C0) {
            Fit seed;
            if (fitOnce(edges, ancestors, GeomAbs_C0, TopoDS_Face(), seed))
                fitted = fitOnce(edges, ancestors, order, seed.face, fit);
        }
        if (!fitted && order != GeomAbs_C0)
            fitted = fitOnce(edges, ancestors, GeomAbs_C0, TopoDS_Face(), fit);
        if (!fitted) {
            std::fprintf(stderr,
                "[Patch] the surface fit didn't converge on these %zu edges.\n",
                edges.size());
            return false;
        }

        m_g0Error = fit.g0;
        m_g1Error = fit.g1;
        m_g2Error = fit.g2;
        m_unsupported = fit.unsupported;

        // Whether the tangency was actually delivered, as opposed to asked for.
        //
        // GeomPlate discards a G1/G2 constraint outright — silently, keeping
        // IsDone() true — when the angle between its initial surface's normal
        // and the target normal exceeds ~89.4 degrees
        // (Plate_GtoCConstraint: `if (fabs(cos_normales) < COSMIN) return;`).
        // Its initial surface is the average plane through the rim, so the
        // constraint survives right up until the surrounding faces stand
        // perpendicular to the opening — a flat lid on vertical walls being the
        // exact case, where a tangent patch would have to balloon without limit
        // anyway. Measured on a cone frustum: a wall leaning 0.6 degrees off
        // vertical loses tangency entirely, 2.9 degrees gets it to 1e-2 rad, and
        // anything past ~6 degrees lands at 1e-5. The user is told which of
        // those they got rather than being handed a flat patch labelled tangent.
        m_continuityAchieved =
            order == GeomAbs_C0 ||
            (fit.unsupported == 0 && fit.g1 <= std::max(m_solver.tolAng, 1e-3));

        const TopoDS_Face patch = fit.face;
        m_patchFace = patch;

        // Prefer healing the body over adding a second object: "fill the void"
        // means the part ends up whole, not shadowed by a loose surface.
        m_healOutcome = base.IsNull() ? Heal::NoSingleBody
                      : (isSolid(base) && freeEdgeCount(base) == 0)
                            ? Heal::BodyIsClosed
                            : Heal::SewFailed;
        if (!base.IsNull()) {
            const TopoDS_Shape healed = healInto(base, patch);
            if (!healed.IsNull()) {
                m_previousShape = base;
                m_healed = true;
                m_healOutcome = Heal::Sewn;
                // A previously-created standalone body is stale now (the user
                // raised continuity and the patch finally closed). Drop it so
                // the document doesn't keep both.
                if (m_createdBodyId >= 0) {
                    doc.removeBody(m_createdBodyId);
                    m_createdBodyId = -1;
                }
                doc.updateBody(m_bodyId, healed);
                return true;
            }
        }

        // Standalone surface. Undo must not restore a body shape we never
        // touched, so clear the healed bookkeeping explicitly — this op is
        // re-executed in place during a live preview and can flip modes
        // between frames.
        if (m_healed && m_bodyId >= 0 && !m_previousShape.IsNull())
            doc.updateBody(m_bodyId, m_previousShape);
        m_healed = false;
        m_previousShape = TopoDS_Shape();
        doc.addOrPutBody(m_createdBodyId, patch, "Patch");
        return true;
    } catch (...) {
        return false;
    }
}

bool PatchOp::undo(Document& doc) {
    try {
        if (m_healed) {
            if (m_bodyId >= 0 && !m_previousShape.IsNull())
                doc.updateBody(m_bodyId, m_previousShape);
        } else if (m_createdBodyId >= 0) {
            doc.removeBody(m_createdBodyId);
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string PatchOp::description() const {
    const char* c = m_continuity == Continuity::Curvature ? "curvature"
                  : m_continuity == Continuity::Tangent   ? "tangent"
                                                          : "position";
    return std::string("Patch across ") + std::to_string(m_edges.size()) +
           " edges (" + c + ")";
}

void PatchOp::renderProperties() {
    ImGui::Text(materializr::tr("Boundary edges: %d"), static_cast<int>(m_edges.size()));
    const char* c = m_continuity == Continuity::Curvature ? "Curvature (G2)"
                  : m_continuity == Continuity::Tangent   ? "Tangent (G1)"
                                                          : "Position (C0)";
    ImGui::Text(materializr::tr("Continuity: %s"), materializr::tr(c));
    ImGui::TextDisabled(materializr::tr("Fit: gap %.4f mm, tangency %.2f deg"),
                        m_g0Error, m_g1Error * 180.0 / 3.14159265358979323846);
    if (m_healed)
        ImGui::TextDisabled("%s", materializr::tr("Sewn into the body."));
}

// ── Serialization ───────────────────────────────────────────────────────────

std::string PatchOp::serializeParams() const {
    // Scalars, then ALL geometry as one length-prefixed ASCII BREP compound
    // (edges first, then support faces) — the LoftOp / BoundaryFillOp
    // discipline. Carrying the geometry rather than sub-shape indices is what
    // lets a reloaded patch re-fit even when the body it was cut against has
    // been rebuilt underneath it.
    std::ostringstream head;
    head << "body=" << m_bodyId
         << ";created=" << m_createdBodyId
         << ";healed=" << (m_healed ? 1 : 0)
         << ";cont=" << static_cast<int>(m_continuity)
         << ";ne=" << m_edges.size()
         << ";ns=" << m_supports.size()
         << ";deg=" << m_solver.degree
         << ";npc=" << m_solver.nbPtsOnCur
         << ";iter=" << m_solver.nbIter
         << ";aniso=" << (m_solver.anisotropic ? 1 : 0)
         << ";t2d=" << m_solver.tol2d
         << ";t3d=" << m_solver.tol3d
         << ";tang=" << m_solver.tolAng
         << ";tcurv=" << m_solver.tolCurv
         << ";maxdeg=" << m_solver.maxDeg
         << ";maxseg=" << m_solver.maxSegments;

    BRep_Builder bb;
    TopoDS_Compound comp;
    bb.MakeCompound(comp);
    for (const auto& e : m_edges) if (!e.IsNull()) bb.Add(comp, e);
    for (const auto& f : m_supports) if (!f.IsNull()) bb.Add(comp, f);

    std::ostringstream os;
    BRepTools::Write(comp, os);
    const std::string brep = os.str();
    return head.str() + ";brep=" + std::to_string(brep.size()) + ":" + brep;
}

bool PatchOp::deserializeParams(const std::string& blob) {
    m_edges.clear();
    m_supports.clear();
    int ne = -1, ns = -1;
    bool any = false, gotGeometry = false;
    size_t pos = 0;

    while (pos < blob.size()) {
        const size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        const std::string key = blob.substr(pos, eq - pos);

        if (key == "brep") {
            const size_t colon = blob.find(':', eq);
            if (colon == std::string::npos) break;
            // Checked length, bounded by subtraction (ParamParse.h).
            size_t nBytes = 0, payload = 0;
            if (!materializr::readLenPrefix(blob, eq + 1, colon, nBytes, payload)) break;
            std::istringstream is(blob.substr(payload, nBytes));
            TopoDS_Shape comp;
            BRep_Builder bb;
            try { BRepTools::Read(comp, is, bb); } catch (...) { return false; }
            // ne/ns always precede ";brep=" in the serialized form, so the
            // counts are known here and the compound can be split by them.
            if (ne < 0 || ne > kMaxPatchEdges) return false;
            if (ns < 0 || ns > kMaxPatchEdges) return false;
            TopoDS_Iterator it(comp);
            for (int i = 0; i < ne; ++i, it.Next()) {
                if (!it.More() || it.Value().ShapeType() != TopAbs_EDGE) return false;
                m_edges.push_back(TopoDS::Edge(it.Value()));
            }
            for (int i = 0; i < ns; ++i, it.Next()) {
                if (!it.More() || it.Value().ShapeType() != TopAbs_FACE) return false;
                m_supports.push_back(TopoDS::Face(it.Value()));
            }
            gotGeometry = true;
            any = true;
            break;
        }

        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        const std::string val = blob.substr(eq + 1, end - eq - 1);
        int iv = 0;
        const bool isInt = materializr::parseWholeInt(val, iv);
        const double dv = std::atof(val.c_str());

        if      (key == "body")    { if (!isInt) return false; m_bodyId = iv; any = true; }
        else if (key == "created") { if (!isInt) return false; m_createdBodyId = iv; any = true; }
        else if (key == "healed")  { m_healed = isInt && iv != 0; any = true; }
        else if (key == "cont") {
            if (!isInt || iv < 0 || iv > 2) return false;
            m_continuity = static_cast<Continuity>(iv);
            any = true;
        }
        else if (key == "ne") { if (!isInt || iv < 0 || iv > kMaxPatchEdges) return false; ne = iv; any = true; }
        else if (key == "ns") { if (!isInt || iv < 0 || iv > kMaxPatchEdges) return false; ns = iv; any = true; }
        // Solver knobs SIZE the fit's internal work (a degree-2000 plate over
        // 100000 samples is an allocation request, not a shape), so each is
        // clamped to the range the panel itself offers rather than trusted.
        else if (key == "deg")    { if (isInt) m_solver.degree      = std::min(std::max(iv, 2), 12); }
        else if (key == "npc")    { if (isInt) m_solver.nbPtsOnCur  = std::min(std::max(iv, 4), 100); }
        else if (key == "iter")   { if (isInt) m_solver.nbIter      = std::min(std::max(iv, 1), 10); }
        else if (key == "aniso")  { m_solver.anisotropic = isInt && iv != 0; }
        else if (key == "maxdeg") { if (isInt) m_solver.maxDeg      = std::min(std::max(iv, 3), 25); }
        else if (key == "maxseg") { if (isInt) m_solver.maxSegments = std::min(std::max(iv, 1), 100); }
        else if (key == "t2d" || key == "t3d" || key == "tang" || key == "tcurv") {
            // A crafted blob can spell inf/nan; those poison the plate solver
            // rather than throwing cleanly.
            if (!std::isfinite(dv) || dv <= 0.0 || dv > 1e3) return false;
            if      (key == "t2d")  m_solver.tol2d   = dv;
            else if (key == "t3d")  m_solver.tol3d   = dv;
            else if (key == "tang") m_solver.tolAng  = dv;
            else                    m_solver.tolCurv = dv;
        }
        pos = end + 1;
    }

    return any && gotGeometry && !m_edges.empty();
}

bool PatchOp::rehydrateFromReload(const ReloadState& state, Document&) {
    if (m_edges.empty()) return false;

    for (const auto& [id, shape] : state.modifiedBefore) {
        if (id == m_bodyId) {
            m_previousShape = shape;
            m_healed = true;
            return true;
        }
    }
    // Not a heal, so it must be the standalone form: adopt whichever body the
    // step created.
    m_healed = false;
    if (m_createdBodyId < 0 && !state.created.empty())
        m_createdBodyId = state.created.front();
    return m_createdBodyId >= 0;
}

OperationDiff PatchOp::captureDiff() const {
    OperationDiff d;
    if (m_healed) {
        if (m_bodyId >= 0 && !m_previousShape.IsNull())
            d.modifiedBefore.emplace_back(m_bodyId, m_previousShape);
    } else if (m_createdBodyId >= 0) {
        d.created.push_back(m_createdBodyId);
    }
    return d;
}

bool PatchOp::ownsFace(const TopoDS_Shape& face) const {
    return !m_patchFace.IsNull() && !face.IsNull() && face.IsSame(m_patchFace);
}

void PatchOp::snapshotEditState() {
    m_editSnap.edges = m_edges;
    m_editSnap.supports = m_supports;
    m_editSnap.previousShape = m_previousShape;
    m_editSnap.patchFace = m_patchFace;
    m_editSnap.healed = m_healed;
    m_editSnap.valid = true;
}

void PatchOp::restoreEditState() {
    if (!m_editSnap.valid) return;
    m_edges = m_editSnap.edges;
    m_supports = m_editSnap.supports;
    m_previousShape = m_editSnap.previousShape;
    m_patchFace = m_editSnap.patchFace;
    m_healed = m_editSnap.healed;
}
