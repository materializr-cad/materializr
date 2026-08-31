#include "ShellOp.h"
#include "SubShapeIndex.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffset_Mode.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <ShapeFix_Shape.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GeomAbs_JoinType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Standard_ErrorHandler.hxx> // OCC_CATCH_SIGNALS
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Vec.hxx>
#include <algorithm>
#include <imgui.h>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"
#include "modeling/ParamParse.h"

namespace {

// Signed volume of a shape (net of any inner void), used to tell a real hollow
// from a no-op result MakeThickSolid sometimes hands back (see execute()).
double shapeVolume(const TopoDS_Shape& s) {
    if (s.IsNull()) return 0.0;
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

// A face's outward normal and a point on it, sampled at its parametric centre.
bool faceNormalPoint(const TopoDS_Face& f, gp_Dir& outN, gp_Pnt& outP) {
    try {
        BRepGProp_Face gf(f);
        Standard_Real u0, u1, v0, v1; gf.Bounds(u0, u1, v0, v1);
        gp_Pnt p; gp_Vec n;
        gf.Normal(0.5 * (u0 + u1), 0.5 * (v0 + v1), p, n);
        if (n.Magnitude() < 1e-9) return false;
        outN = gp_Dir(n);
        outP = p;
        return true;
    } catch (...) { return false; }
}

// Is `face` one of `shape`'s faces (same underlying TShape)?
bool faceInShape(const TopoDS_Face& face, const TopoDS_Shape& shape) {
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
        if (ex.Current().IsSame(face)) return true;
    return false;
}

} // namespace

ShellOp::ShellOp() = default;

std::vector<double> ShellOp::roundedFaceRadii(const TopoDS_Shape& body) {
    std::vector<double> radii;
    if (body.IsNull()) return radii;
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        try {
            BRepAdaptor_Surface sa(TopoDS::Face(ex.Current()));
            double r = -1.0;
            if (sa.GetType() == GeomAbs_Cylinder)   r = sa.Cylinder().Radius();
            else if (sa.GetType() == GeomAbs_Torus) r = sa.Torus().MinorRadius();
            if (r <= 0.0) continue;
            bool seen = false;
            for (double e : radii) if (std::abs(e - r) < 1e-4) { seen = true; break; }
            if (!seen) radii.push_back(r);
        } catch (...) {}
    }
    std::sort(radii.begin(), radii.end());
    return radii;
}

void ShellOp::captureFaceAnchors(const TopoDS_Shape& shape) {
    if (!m_faceAnchors.empty() || m_facesToRemove.IsEmpty() || shape.IsNull())
        return;
    for (const TopoDS_Shape& s : m_facesToRemove) {
        gp_Dir n; gp_Pnt p;
        if (faceNormalPoint(TopoDS::Face(s), n, p))
            m_faceAnchors.push_back({ n, p });
    }
}

bool ShellOp::rebindFaces(const TopoDS_Shape& shape) {
    if (shape.IsNull()) return false;
    // Nothing to open (a fully-closed hollow) is legitimate.
    if (m_facesToRemove.IsEmpty() && m_faceAnchors.empty()) return true;

    // Fast path: every stored face is still a live face of this body.
    bool allLive = !m_facesToRemove.IsEmpty();
    for (const TopoDS_Shape& s : m_facesToRemove)
        if (!faceInShape(TopoDS::Face(s), shape)) { allLive = false; break; }
    if (allLive) return true;

    // Rebind from anchors: for each opened face, pick the body face whose
    // normal best aligns AND whose plane is nearest the anchor point (so a
    // lateral resize that keeps the cap's orientation still matches it).
    if (m_faceAnchors.empty()) return false;
    TopTools_ListOfShape rebound;
    for (const FaceAnchor& a : m_faceAnchors) {
        TopoDS_Face best; double bestScore = -1e18;
        for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
            TopoDS_Face f = TopoDS::Face(ex.Current());
            gp_Dir n; gp_Pnt p;
            if (!faceNormalPoint(f, n, p)) continue;
            double dot = n.Dot(a.normal);
            if (dot < 0.9) continue; // must face (nearly) the same way
            // Distance from the anchor point to this face's plane along n.
            double planeDist = std::abs(gp_Vec(p, a.point).Dot(gp_Vec(n)));
            // Prefer best alignment, then nearest plane.
            double score = dot - 0.01 * planeDist;
            if (score > bestScore) { bestScore = score; best = f; }
        }
        if (best.IsNull()) return false;
        rebound.Append(best);
    }
    m_facesToRemove = rebound;
    return true;
}

void ShellOp::captureFaceRefs(const Document& doc, const TopoDS_Shape& shape) {
    if (!m_faceRefs.empty() || m_facesToRemove.IsEmpty() || shape.IsNull())
        return;
    materializr::topo::Context ctx;
    ctx.doc = &doc; ctx.shape = shape; ctx.type = TopAbs_FACE;
    for (const TopoDS_Shape& s : m_facesToRemove)
        m_faceRefs.push_back(materializr::topo::mint(s, ctx));
}

bool ShellOp::resolveFacesTopo(const Document& doc, const TopoDS_Shape& shape) {
    if (m_faceRefs.empty() || shape.IsNull()) return false;
    for (const auto& r : m_faceRefs)
        if (r.empty()) return false;   // any unnameable face -> geometric path
    materializr::topo::Context ctx;
    ctx.doc = &doc; ctx.shape = shape; ctx.type = TopAbs_FACE;
    ctx.crossRebuild = true;   // shape may have been rebuilt upstream
    std::vector<TopoDS_Shape> out;
    if (!materializr::topo::resolveSet(m_faceRefs, ctx, out) ||
        out.size() != m_faceRefs.size())
        return false;
    // SANITY GUARD: each resolved face must point the way the opened face
    // pointed when it was captured (m_faceAnchors, zip-aligned with the refs).
    // A resolution that flips orientation is a MIS-resolve — reject the whole
    // set and let the geometric rebind handle it, so a bad topo answer can
    // never preempt the fallback that used to work.
    if (m_faceAnchors.size() == out.size()) {
        for (size_t i = 0; i < out.size(); ++i) {
            gp_Dir n; gp_Pnt p;
            if (!faceNormalPoint(TopoDS::Face(out[i]), n, p)) return false;
            if (n.Dot(m_faceAnchors[i].normal) < 0.7) return false;
        }
    }
    m_facesToRemove.Clear();
    for (const auto& f : out) m_facesToRemove.Append(f);
    return true;
}

void ShellOp::setBody(int id) {
    m_bodyId = id;
}

void ShellOp::setThickness(double t) {
    m_thickness = t;
}

void ShellOp::addFaceToRemove(const TopoDS_Face& face) {
    m_facesToRemove.Append(face);
}

void ShellOp::clearFacesToRemove() {
    m_facesToRemove.Clear();
}

bool ShellOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_thickness <= 0.0) {
        return false;
    }

    try {
        // Store previous shape for undo
        m_previousShape = doc.getBody(m_bodyId);

        // Re-bind the opened faces to the (possibly regenerated) body before
        // offsetting — without this, an upstream sketch edit that rebuilds the
        // body leaves m_facesToRemove pointing at the OLD body's faces, so the
        // opening is silently lost and the whole shell vanishes on the next
        // edit. Mirrors FilletOp's edge rebind. Capture the anchors first (on
        // the initial run they're still valid against this body).
        captureFaceAnchors(m_previousShape);
        captureFaceRefs(doc, m_previousShape);
        // Prefer the sketch-anchored topo resolution — it follows an opened
        // face that a dimension edit MOVED (which the geometric normal+point
        // rebind can't) — then fall back to that rebind for unnameable faces.
        if (!resolveFacesTopo(doc, m_previousShape) &&
            !rebindFaces(m_previousShape)) {
            std::fprintf(stderr,
                "[Shell] could not re-find the opened face(s) on the rebuilt "
                "body (thickness %.3f mm).\n", m_thickness);
            return false;
        }

        // Two join strategies, tried in order:
        //  • Arc (rolling-ball) — the default; rounds inner transitions. This
        //    is the one that OCCT can drive to a hard fault when the wall is
        //    thicker than a concave fillet on the body can absorb (the inner
        //    offset of an R fillet collapses at thickness >= R).
        //  • Intersection (sharp corners) — survives some lofted/BSpline side
        //    walls the arc join can't. BUT on a filleted body at an over-thick
        //    wall it does NOT fail cleanly — it spins in an unbounded internal
        //    loop (OCCT "Cote PT2PT3 nul"), freezing the app.
        // So: if the Arc attempt THREW (thickness exceeds the geometry's
        // capacity), do NOT fall through to Intersection — that's the hang.
        // Only try Intersection when Arc failed *cleanly* (produced an invalid
        // or null result without throwing), i.e. the lofted-wall case.
        //
        // Accept ONLY a genuine OPEN hollow (issue #30). Two disguised failures
        // must be rejected, because OCCT marks both "valid" and both would be
        // committed as a silent do-nothing:
        //   • NO-OP  — volume ≈ the solid: the offset declined the face and
        //     handed the untouched solid straight back.
        //   • SEALED — a closed thick shell (2 shells: outer + inner void). This
        //     is what OCCT produces when the removed face is ringed by fillets:
        //     it can't OPEN the face, so it seals the cavity. From the outside a
        //     sealed hollow is indistinguishable from the solid.
        // A real open cup is a single shell whose volume dropped. If neither
        // join yields one, fail — the user should shell BEFORE filleting.
        const double solidVol = shapeVolume(m_previousShape);
        auto isOpenHollow = [&](const TopoDS_Shape& s) {
            if (s.IsNull() || shapeVolume(s) >= solidVol * 0.99) return false;
            if (!BRepCheck_Analyzer(s).IsValid()) return false;
            TopTools_IndexedMapOfShape shells;
            TopExp::MapShapes(s, TopAbs_SHELL, shells);
            return shells.Extent() == 1;   // 2 shells == sealed void
        };

        enum Outcome { Ok, CleanFail, Threw };
        auto tryShell = [&](Standard_Boolean inter, GeomAbs_JoinType join,
                            TopoDS_Shape& out) -> Outcome {
            try {
                // A thick-solid that exceeds the body's available wall space can
                // SIGSEGV deep in BRepOffset. With OCC_CONVERT_SIGNALS enabled,
                // OCC_CATCH_SIGNALS turns that kernel signal into a
                // Standard_Failure the catch below absorbs.
                OCC_CATCH_SIGNALS
                BRepOffsetAPI_MakeThickSolid mk;
                mk.MakeThickSolidByJoin(m_previousShape, m_facesToRemove,
                                        -m_thickness, 1.0e-3, BRepOffset_Skin,
                                        inter, Standard_False, join);
                mk.Build();
                if (!mk.IsDone() || mk.Shape().IsNull()) return CleanFail;
                TopoDS_Shape s = mk.Shape();
                if (isOpenHollow(s)) { out = s; return Ok; }
                // Repair a near-valid OPEN cup: the arc join on a concave /
                // interior-curve body often opens the face correctly but trips
                // BRepCheck on a face or two, which a single ShapeFix pass mends.
                // A SEALED hollow stays 2-shell after ShapeFix, so isOpenHollow
                // still rejects it — this only rescues genuine open cups.
                ShapeFix_Shape fix(s);
                fix.Perform();
                if (isOpenHollow(fix.Shape())) { out = fix.Shape(); return Ok; }
            } catch (...) {
                return Threw;
            }
            return CleanFail;
        };

        TopoDS_Shape result;
        Outcome arc = tryShell(Standard_False, GeomAbs_Arc, result);
        if (arc != Ok) {
            // Only the clean-fail (lofted-wall) case earns the intersection
            // retry; a throw means the wall is too thick for the geometry, and
            // the intersection join would hang instead of refusing.
            if (arc == Threw ||
                tryShell(Standard_True, GeomAbs_Intersection, result) != Ok) {
                std::fprintf(stderr,
                    "[Shell] failed at thickness %.3f mm — the wall is too thick, "
                    "or the opened face is ringed by fillets (shell BEFORE adding "
                    "fillets: OCCT can't open a fillet-bordered face).\n",
                    m_thickness);
                return false;
            }
        }

        doc.updateBody(m_bodyId, result);
        return true;
    } catch (...) {
        return false;
    }
}

bool ShellOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) {
        return false;
    }

    try {
        doc.updateBody(m_bodyId, m_previousShape);
        return true;
    } catch (...) {
        return false;
    }
}

std::string ShellOp::description() const {
    int faceCount = m_facesToRemove.Size();
    return "Shell thickness " + std::to_string(m_thickness) +
           " (" + std::to_string(faceCount) + " open face(s))";
}

void ShellOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Shell"));
    ImGui::Separator();

    materializr::inputNumber(materializr::tr("Thickness"), &m_thickness, 0.1, 1.0, "%g");

    int faceCount = m_facesToRemove.Size();
    ImGui::Text(materializr::tr("Open faces: %d selected"), faceCount);
    ImGui::Text(materializr::tr("Body ID: %d"), m_bodyId);
}

std::string ShellOp::serializeParams() const {
    // The opened faces persist as ordinal indices into the INPUT shape's
    // canonical face map (see SubShapeIndex.h).
    std::string blob;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "body=%d;thickness=%.6f", m_bodyId, m_thickness);
    blob += buf;
    if (!m_previousShape.IsNull() && !m_facesToRemove.IsEmpty()) {
        std::vector<TopoDS_Shape> faces;
        for (const TopoDS_Shape& f : m_facesToRemove) faces.push_back(f);
        std::string idx = SubShapeIndex::serialize(m_previousShape, faces,
                                                   TopAbs_FACE);
        if (!idx.empty()) blob += ";faces=" + idx;
    }
    // Topological face names (additive, robust to a moving edit). Only when
    // EVERY opened face is nameable — otherwise the ordinal `faces=` above and
    // the geometric rebind carry it. Written last; a length-prefixed list so
    // each ref's opaque blob round-trips.
    if (!m_faceRefs.empty()) {
        bool allNamed = true;
        for (const auto& r : m_faceRefs) if (r.empty()) { allNamed = false; break; }
        if (allNamed) {
            std::string rb;
            for (const auto& r : m_faceRefs) {
                std::string b = r.serialize();
                rb += std::to_string(b.size()) + ":" + b;
            }
            blob += ";facerefs=" + rb;
        }
    }
    return blob;
}

bool ShellOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        // facerefs holds a length-prefixed list of opaque ref blobs, written
        // last — read it to end-of-string (not to the next ';').
        if (key == "facerefs") {
            std::string rest = blob.substr(eq + 1);
            m_faceRefs.clear();
            size_t p = 0;
            std::string tok;
            // Checked length + guaranteed cursor advance (ParamParse.h).
            // The old `c + 1 + n` bound wrapped on a negative length and
            // could drive `p` backwards, looping forever.
            while (materializr::readLenRecord(rest, p, tok))
                m_faceRefs.push_back(materializr::topo::Ref::parse(tok));
            any = true;
            break;
        }
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "thickness") { m_thickness = std::atof(val.c_str()); any = true; }
        else if (key == "body")      { m_bodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "faces")     { m_faceIndices = SubShapeIndex::parse(val); any = true; }
        pos = end + 1;
    }
    return any;
}

bool ShellOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_bodyId < 0) return false;

    m_previousShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    if (m_previousShape.IsNull()) return false;

    // Re-resolve the opened faces. A closed shell (no faces removed) is
    // legitimate — m_faceIndices empty just means MakeThickSolid hollows
    // without an opening. But if indices WERE saved, all must resolve.
    m_facesToRemove.Clear();
    if (!m_faceIndices.empty()) {
        std::vector<TopoDS_Shape> resolved;
        if (!SubShapeIndex::resolveAll(m_previousShape, m_faceIndices,
                                       TopAbs_FACE, resolved)) {
            return false;
        }
        for (const auto& f : resolved) m_facesToRemove.Append(f);
    }
    return true;
}

OperationDiff ShellOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}
