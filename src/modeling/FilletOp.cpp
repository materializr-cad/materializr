#include "../core/NumFormat.h"
#include "FilletOp.h"
#include "BlendCut.h"
#include "SubShapeIndex.h"
#include "EdgeAnchor.h"
#include "FaceSurfSig.h"
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include "../core/Verbose.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <cstdlib>
#include <cmath>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <cmath>
#include <imgui.h>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"
#include "modeling/ParamParse.h"

namespace {
// Representative point on a face (midpoint of its UV bounds). Stable for the
// same face geometry, so it survives re-tessellation between picks.
bool faceCenter(const TopoDS_Face& face, gp_Pnt& out) {
    try {
        BRepGProp_Face gp(face);
        Standard_Real u0, u1, v0, v1;
        gp.Bounds(u0, u1, v0, v1);
        gp_Vec n;
        gp.Normal((u0 + u1) * 0.5, (v0 + v1) * 0.5, out, n);
        return true;
    } catch (...) { return false; }
}

// Blend radius of a fillet face, if it is a recognisable analytic blend
// surface (cylinder on a straight edge, torus/sphere where edges curve or
// meet). Returns <0 when the face isn't such a surface — those we can't
// discriminate by radius and must fall back to the saved indices.
double faceBlendRadius(const TopoDS_Face& face) {
    try {
        BRepAdaptor_Surface s(face);
        switch (s.GetType()) {
            case GeomAbs_Cylinder: return s.Cylinder().Radius();
            case GeomAbs_Torus:    return s.Torus().MinorRadius();
            case GeomAbs_Sphere:   return s.Sphere().Radius();
            default:               return -1.0;
        }
    } catch (...) { return -1.0; }
}

// Faces present in `result` but NOT in `prev` — the blend faces this fillet
// created. Reload fallback for the history-hover highlight when a save lacks
// generated-face indices (churn-dropped `gen=`).
//
// Matched by the unbounded SURFACE, not the centroid: when this fillet trims a
// corner off an adjacent earlier blend, that face keeps its surface but its
// centroid shifts — a centroid test would wrongly flag it as new and light up
// earlier steps' blends on hover. Non-analytic faces (no cheap surface
// signature) fall back to the centroid test, unchanged.
std::vector<TopoDS_Shape> facesCreatedVsPrev(const TopoDS_Shape& result,
                                             const TopoDS_Shape& prev) {
    std::vector<TopoDS_Face> prevF;
    std::vector<gp_Pnt> prevC;
    for (TopExp_Explorer ex(prev, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face pf = TopoDS::Face(ex.Current());
        gp_Pnt c;
        if (faceCenter(pf, c)) { prevF.push_back(pf); prevC.push_back(c); }
    }
    std::vector<TopoDS_Shape> out;
    for (TopExp_Explorer ex(result, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face f = TopoDS::Face(ex.Current());
        gp_Pnt c;
        if (!faceCenter(f, c)) continue;
        bool wasThere = false;
        for (size_t i = 0; i < prevF.size(); ++i) {
            if (materializr::sameSurface(prevF[i], f) ||
                prevC[i].Distance(c) < 1e-4) { wasThere = true; break; }
        }
        if (!wasThere) out.push_back(f);
    }
    return out;
}
} // namespace

// Every sketch in the document, as EdgeAnchor references. Real bodies are
// carved by several sketches (base extrude + profile cuts), so anchoring
// consults them all; sketches unrelated to this body simply never match.
// Prefers the cascade override (the edited sketch's FINAL state) over the
// live sketch: during a history replay the live sketch is rolled back through
// its SketchEditOp snapshots, so it holds a stale state exactly when this op
// re-executes — while the extrude below was rebuilt from the final one.
// `keep` extends the overrides' lifetime to the caller's scope.
static std::vector<EdgeAnchor::SketchRef> anchorSketches(
        Document& doc, std::vector<std::shared_ptr<materializr::Sketch>>& keep) {
    std::vector<EdgeAnchor::SketchRef> refs;
    for (int sid : doc.getAllSketchIds()) {
        if (auto ov = doc.cascadeSketchOverride(sid)) {
            keep.push_back(ov);
            refs.push_back({ sid, ov.get() });
        } else if (auto sk = doc.getSketch(sid)) {
            keep.push_back(sk);
            refs.push_back({ sid, sk.get() });
        }
    }
    return refs;
}

void FilletOp::computeAnchors(Document& doc) {
    m_edgeAnchors.clear();
    std::vector<std::shared_ptr<materializr::Sketch>> keep;
    m_edgeAnchors = EdgeAnchor::compute(m_edges, anchorSketches(doc, keep));
    // Success trace is --verbose only: execute() (and thus this) runs per
    // PREVIEW FRAME while a fillet is being dragged — an always-on stderr
    // flush per frame is real drag cost. Failure paths below stay loud.
    if (materializr::isVerbose()) {
        int corners = 0, rims = 0, arcs = 0, none = 0;
        for (const auto& a : m_edgeAnchors)
            (a.kind == EdgeAnchor::Anchor::Corner ? corners :
             a.kind == EdgeAnchor::Anchor::Rim    ? rims :
             a.kind == EdgeAnchor::Anchor::None   ? none : arcs)++;
        std::fprintf(stderr,
            "[Fillet] anchored %zu edges: %d corner, %d rim, %d arc, %d none\n",
            m_edges.size(), corners, rims, arcs, none);
    }
}

bool FilletOp::resolveAnchors(Document& doc, const TopoDS_Shape& base) {
    if (m_edgeAnchors.size() != m_edges.size()) return false;
    std::vector<TopoDS_Edge> resolved;
    std::vector<std::shared_ptr<materializr::Sketch>> keep;
    if (!EdgeAnchor::resolve(m_edgeAnchors, anchorSketches(doc, keep), base, resolved))
        return false;
    m_edges = std::move(resolved);
    if (materializr::isVerbose())
        std::fprintf(stderr, "[Fillet] resolved %zu edge(s) via generative anchors\n",
                     m_edges.size());
    return true;
}

FilletOp::FilletOp() = default;

void FilletOp::rememberResult(const TopoDS_Shape& base,
                              const TopoDS_Shape& result) {
    if (base.IsNull() || result.IsNull()) return;
    for (auto& e : m_storedResults) {
        if (e.base.IsEqual(base) && std::abs(e.r - m_radius) < 1e-9) {
            e.result = result;
            e.genFaces = m_generatedFaces;
            return;
        }
    }
    m_storedResults.push_back({base, result, m_radius, m_generatedFaces});
    if (m_storedResults.size() > 8)
        m_storedResults.erase(m_storedResults.begin() + 1);
}

void FilletOp::snapshotEditState() {
    m_editSnap.edges = m_edges;
    m_editSnap.anchors = m_edgeAnchors;
    m_editSnap.refs = m_edgeRefs;
    m_editSnap.pairs = m_edgeFaceIdPairs;
    m_editSnap.prevFaceIds = m_prevFaceIds;
    m_editSnap.previousShape = m_previousShape;
    m_editSnap.resultShape = m_resultShape;
    m_editSnap.generatedFaces = m_generatedFaces;
    m_editSnap.genFaceIds = m_genFaceIds;
    m_editSnap.storedResults = m_storedResults;
    m_editSnap.radius = m_radius;
    m_editSnap.valid = true;
}

void FilletOp::restoreEditState() {
    if (!m_editSnap.valid) return;
    m_edges = m_editSnap.edges;
    m_edgeAnchors = m_editSnap.anchors;
    m_edgeRefs = m_editSnap.refs;
    m_edgeFaceIdPairs = m_editSnap.pairs;
    m_prevFaceIds = m_editSnap.prevFaceIds;
    m_previousShape = m_editSnap.previousShape;
    m_resultShape = m_editSnap.resultShape;
    m_generatedFaces = m_editSnap.generatedFaces;
    m_genFaceIds = m_editSnap.genFaceIds;
    m_storedResults = m_editSnap.storedResults;
    m_radius = m_editSnap.radius;
}

void FilletOp::setBody(int bodyId) {
    m_bodyId = bodyId;
}

void FilletOp::setEdges(const std::vector<TopoDS_Edge>& edges) {
    m_edges = edges;
}

void FilletOp::setRadius(double radius) {
    m_radius = radius;
}

bool FilletOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_edges.empty() || m_radius <= 0.0) {
        return false;
    }

    // FAILURE MUST NOT POISON THE OP (see ChamferOp::execute): resolution
    // rewrites m_edges/anchors/refs against the current body; a failed build
    // must roll them back or one failed edit wedges every later attempt.
    struct ResolutionGuard {
        FilletOp& op;
        std::vector<TopoDS_Edge> edges;
        std::vector<EdgeAnchor::Anchor> anchors;
        std::vector<materializr::topo::Ref> refs;
        std::vector<std::pair<int,int>> pairs;
        bool committed = false;
        explicit ResolutionGuard(FilletOp& o)
            : op(o), edges(o.m_edges), anchors(o.m_edgeAnchors),
              refs(o.m_edgeRefs), pairs(o.m_edgeFaceIdPairs) {}
        ~ResolutionGuard() {
            if (committed) return;
            op.m_edges = std::move(edges);
            op.m_edgeAnchors = std::move(anchors);
            op.m_edgeRefs = std::move(refs);
            op.m_edgeFaceIdPairs = std::move(pairs);
        }
    } guard(*this);

    try {
        // Store previous shape for undo
        m_previousShape = doc.getBody(m_bodyId);

        // Input lineage, completed so EVERY face has an id (see ChamferOp) —
        // feeds the lineage-first edge resolution below and the post-build
        // pair capture, and is restored by undo (partial-replay lifeline).
        materializr::topo::FaceIdMap inLineage;
        if (const auto* im = doc.bodyFaceIds(m_bodyId)) inLineage = *im;
        materializr::topo::complete(inLineage, m_previousShape,
                                    [&doc]() { return doc.mintFaceId(); });

        // Lineage-FIRST edge resolution (parity with ChamferOp, #52): each
        // edge named by its two adjacent faces' ancestry ids — immune to
        // ordinal drift AND alive when the runtime ledger is gone (partial
        // replay). All-or-nothing; on miss, fall through to the classic
        // rebind → anchors → topo-refs chain.
        bool edgesResolvedByLineage = false;
        if (!m_edgeFaceIdPairs.empty() &&
            m_edgeFaceIdPairs.size() == m_edges.size()) {
            TopTools_IndexedDataMapOfShapeListOfShape efm;
            TopExp::MapShapesAndAncestors(m_previousShape, TopAbs_EDGE,
                                          TopAbs_FACE, efm);
            auto faceHas = [&](const TopoDS_Shape& f, int id) {
                const auto* ids = materializr::topo::idsFor(inLineage, f);
                return ids && std::find(ids->begin(), ids->end(), id) != ids->end();
            };
            std::vector<TopoDS_Edge> found;
            for (auto [a, b] : m_edgeFaceIdPairs) {
                TopoDS_Edge hit;
                for (int i = 1; i <= efm.Extent(); ++i) {
                    const TopTools_ListOfShape& fs = efm.FindFromIndex(i);
                    bool hasA = false, hasB = false;
                    for (const TopoDS_Shape& f : fs) {
                        if (faceHas(f, a)) hasA = true;
                        if (faceHas(f, b)) hasB = true;
                    }
                    if (!hasA || !hasB) continue;
                    // DISTINCT-CLAIM (see ChamferOp): fragments of one span
                    // share the same face-id pair — first-hit collapsed the
                    // selection onto one edge and starved the rebuild.
                    bool claimed = false;
                    for (const auto& u : found)
                        if (u.IsSame(efm.FindKey(i))) { claimed = true; break; }
                    if (claimed) continue;
                    hit = TopoDS::Edge(efm.FindKey(i));
                    break;
                }
                if (hit.IsNull()) { found.clear(); break; }
                found.push_back(hit);
            }
            if (found.size() == m_edges.size()) {
                m_edges = std::move(found);
                edgesResolvedByLineage = true;
            }
        }

        // If an upstream edit regenerated the body, our stored edges have
        // stale TShapes — re-bind them to their successors by carrier
        // geometry so editing (say) a neighbouring fillet's radius doesn't
        // kill this op. Fails (loudly, via editStep) only when an edge was
        // genuinely consumed by the upstream change.
        if (!edgesResolvedByLineage &&
            !SubShapeIndex::rebindEdges(m_previousShape, m_edges)) {
            // Ordinal/carrier matching failed — the edges moved (e.g. a sketch
            // DIMENSION edit relocated a filleted corner). Try re-finding them
            // by the sketch vertex they sit over (generative anchoring).
            if (!resolveAnchors(doc, m_previousShape)) {
                // LAST RESORT: topological names. Seam edges from a boolean
                // sit over no sketch feature (anchors fail by construction);
                // their gen-lineage refs resolve through the producing op's
                // ledger, republished on the body by the upstream replay.
                bool topoOk = false;
                if (!m_edgeRefs.empty() &&
                    m_edgeRefs.size() == m_edges.size()) {
                    materializr::topo::Context rc;
                    rc.doc = &doc;
                    rc.shape = m_previousShape;
                    rc.type = TopAbs_EDGE;
                    rc.gen = doc.bodyLedger(m_bodyId);
                    rc.crossRebuild = true;
                    std::vector<TopoDS_Shape> out;
                    if (materializr::topo::resolveSet(m_edgeRefs, rc, out) &&
                        out.size() == m_edges.size()) {
                        for (size_t i = 0; i < out.size(); ++i)
                            m_edges[i] = TopoDS::Edge(out[i]);
                        topoOk = true;
                        std::fprintf(stderr, "[Fillet] edges re-found by "
                                             "topo refs (gen/seam path)\n");
                    }
                }
                if (!topoOk) {
                    std::fprintf(stderr,
                        "[Fillet] rebindEdges + anchors + topo refs failed "
                        "(R=%.2f, %zu edges) — selected edge isn't in the "
                        "current body's edge map.\n",
                        m_radius, m_edges.size());
                    return false;
                }
            }
        }

        // Deduplicate: fragmented topology unified upstream can resolve several
        // stored fragment edges onto ONE current edge — feeding MakeFillet the
        // same edge repeatedly yields an invalid result (#54; chamfer twin).
        {
            std::vector<TopoDS_Edge> uniq;
            for (const auto& e : m_edges) {
                bool dup = false;
                for (const auto& u : uniq) if (u.IsSame(e)) { dup = true; break; }
                if (!dup) uniq.push_back(e);
            }
            if (uniq.size() != m_edges.size()) {
                std::fprintf(stderr, "[Fillet] %zu stored edges resolved to %zu "
                             "distinct — deduped\n", m_edges.size(), uniq.size());
                m_edges = std::move(uniq);
            }
        }
        // Capture generative anchors from the (now-valid) edges the first time
        // we run — so a later dimension edit can re-find them by sketch feature.
        if (m_edgeAnchors.empty()) computeAnchors(doc);
        // And topological names (with the body's producing ledger in context,
        // so a SEAM edge gets its gen-lineage name).
        if (m_edgeRefs.empty() && !m_edges.empty()) {
            materializr::topo::Context mc;
            mc.doc = &doc;
            mc.shape = m_previousShape;
            mc.type = TopAbs_EDGE;
            mc.gen = doc.bodyLedger(m_bodyId);
            for (const auto& e : m_edges)
                m_edgeRefs.push_back(materializr::topo::mint(e, mc));
        }

        // Native attempt first. Every pre-existing gate is preserved, but a
        // failure now NULLIFIES the candidate and falls through to the #55
        // swept-arc cut fallback below instead of aborting outright.
        TopoDS_Shape candidate;
        double usedRadius = m_radius;
        // The successful builder outlives the retry loop: the lineage ledger and
        // Generated() below need the very object that produced `candidate`.
        std::unique_ptr<BRepFilletAPI_MakeFillet> fillet;
        {
        // Nudge past isolated radii OCCT cannot build.
        //
        // BRepFilletAPI fails at SINGLE POINTS, not above a ceiling. Measured on
        // robot dog cover.mzr, body "Extrude", its 114 mm top edge:
        //
        //     R = 1.90  ok      R = 2.00  IsDone()==false      R = 2.10  ok
        //     ...and every radius from 2.1 up to 8.0 builds a valid solid.
        //
        // 2.00 is exactly where the blend's topology changes (83 faces below it,
        // 88 above); on that knife-edge ChFi3d degenerates, while either side is
        // fine. Users type round numbers, so they land precisely on these points
        // and conclude the geometry cannot take the radius. The reported symptom
        // was "I can't fillet beyond about 1.5 mm" on an edge where 2.5, 3, 5
        // and 8 mm all worked.
        //
        // So retry a hair either side before giving up. The offsets are far below
        // anything a user can see or measure (2 microns at R=2) yet enough to
        // step off the degenerate point. Smaller is tried first, so a radius near
        // a GENUINE geometric limit is not nudged past it.
        const double kNudge[] = {0.0, -1e-3, 1e-3, -5e-3, 5e-3};
        for (double rel : kNudge) {
            const double r = m_radius * (1.0 + rel);
            if (r <= 0.0) continue;
            auto attempt = std::make_unique<BRepFilletAPI_MakeFillet>(m_previousShape);
            for (const auto& edge : m_edges) attempt->Add(r, edge);
            try { attempt->Build(); } catch (...) { continue; }
            if (!attempt->IsDone()) {
                std::fprintf(stderr,
                    "[Fillet] BRepFilletAPI.IsDone() returned false (R=%.4f)%s\n",
                    r, rel == 0.0 ? " — retrying just off that radius." : "");
                continue;
            }
            const TopoDS_Shape built = attempt->Shape();
            if (built.IsNull()) {
                std::fprintf(stderr, "[Fillet] result shape is null (R=%.4f).\n", r);
                continue;
            }
            candidate = built;
            usedRadius = r;
            fillet = std::move(attempt);
            if (rel != 0.0)
                std::fprintf(stderr,
                    "[Fillet] R=%.4f failed, R=%.4f built cleanly — using it "
                    "(OCCT degenerates at isolated radii; difference %.1f micron).\n",
                    m_radius, r, std::fabs(r - m_radius) * 1000.0);
            break;
        }

        // IsDone() is necessary but NOT sufficient: when fillet radii on
        // adjacent edges overlap (the classic many-edges-at-once case), OCCT
        // happily returns IsDone()==true with a topologically INVALID solid —
        // self-intersecting blends or dropped faces — which is exactly the
        // "faces disappear / garbage geometry" failure. BRepCheck_Analyzer is
        // the authoritative validity test; reject anything it flags so a
        // corrupt body never gets committed to the document/history. (The bbox
        // and volume checks below catch grosser blow-outs but pass plenty of
        // invalid-but-plausibly-sized results.)
        if (!candidate.IsNull() && !BRepCheck_Analyzer(candidate).IsValid()) {
            std::fprintf(stderr,
                "[Fillet] result failed BRepCheck_Analyzer (R=%.4f, %zu edges) "
                "— invalid topology, refusing to commit.\n",
                usedRadius, m_edges.size());
            candidate.Nullify();
        }

        // OCCT's fillet API is permissive — IsDone() returns true even when
        // the radius exceeds what the geometry can support, and the result
        // is then a self-intersecting / overlapping mess instead of a clean
        // refusal. Two narrow sanity checks reject those without flagging
        // legitimate concave fillets (which ADD material and so make the
        // upper-bound volume check we used to have backwards):
        //   • Bounding box: a fillet should never GROW the body's bbox by
        //     more than a hair. Garbled-cube case (radius > half-extent)
        //     produces inverted shells whose bbox blows out — that's the
        //     signal we catch.
        //   • Volume: must be strictly > 0. Truly degenerate output (zero
        //     or negative volume) is the other failure mode.
        // (Steve: a coffee-cup rim could only fillet to 1.5 mm on the
        //  inside, and not at all on the outside — the old "volume must
        //  not exceed input × 1.01" rule rejected the inside concave
        //  fillets even when geometrically fine.)
        if (!candidate.IsNull()) {
            // AddOptimal walks the actual geometry rather than the looser
            // tolerance-padded extents the plain Add uses. Shelled bodies
            // tend to land in OCCT with face seams at ~1e-3 tolerance,
            // which inflated the result bbox by ~8 mm on a 100 mm cup and
            // tripped the growth gate even on 0.1 mm fillets.
            Bnd_Box bbIn, bbOut;
            BRepBndLib::AddOptimal(m_previousShape, bbIn);
            BRepBndLib::AddOptimal(candidate,       bbOut);
            if (!bbIn.IsVoid() && !bbOut.IsVoid()) {
                Standard_Real ix0, iy0, iz0, ix1, iy1, iz1;
                Standard_Real ox0, oy0, oz0, ox1, oy1, oz1;
                bbIn .Get(ix0, iy0, iz0, ix1, iy1, iz1);
                bbOut.Get(ox0, oy0, oz0, ox1, oy1, oz1);
                const double slop = 1.01; // 1% tolerance for fp noise
                if (ox1 - ox0 > (ix1 - ix0) * slop ||
                    oy1 - oy0 > (iy1 - iy0) * slop ||
                    oz1 - oz0 > (iz1 - iz0) * slop) {
                    std::fprintf(stderr,
                        "[Fillet] bbox grew past slop (R=%.2f): "
                        "%.2fx%.2fx%.2f -> %.2fx%.2fx%.2f mm.\n",
                        m_radius,
                        ix1 - ix0, iy1 - iy0, iz1 - iz0,
                        ox1 - ox0, oy1 - oy0, oz1 - oz0);
                    candidate.Nullify();
                }
            }

            GProp_GProps gpOut;
            BRepGProp::VolumeProperties(candidate, gpOut);
            if (!candidate.IsNull() && gpOut.Mass() < 1e-6) {
                std::fprintf(stderr,
                    "[Fillet] result volume ~= 0 (R=%.2f mm).\n",
                    m_radius);
                candidate.Nullify();
            }
        }

        if (!candidate.IsNull()) {
            // Publish the generation map (input edge -> blend faces) so the
            // "gen" naming strategy can name a blend face by its generating
            // edge — the general-kernel path for op-produced faces. Captured
            // on every execute, so a rebuild's ledger reflects the current
            // geometry.
            m_ledger.capture(*fillet, m_previousShape, TopAbs_EDGE);
            m_ledger.captureAdd(*fillet, m_previousShape, TopAbs_FACE);

            // Record the blend faces generated from each input edge so a later
            // face click can be traced back to this fillet for re-editing.
            m_generatedFaces.clear();
            for (const auto& edge : m_edges) {
                try {
                    const TopTools_ListOfShape& gen = fillet->Generated(edge);
                    // Range-based loop instead of
                    // TopTools_ListIteratorOfListOfShape, whose header was
                    // removed in OCCT 8.0 (still works on 7.x).
                    for (const TopoDS_Shape& s : gen) {
                        if (s.ShapeType() == TopAbs_FACE)
                            m_generatedFaces.push_back(s);
                    }
                } catch (...) {}
            }
        }
        } // native attempt

        if (candidate.IsNull()) {
            // #55: the native blend can't resolve against a surface feature
            // crossing the edge. Build the same removal as a swept-arc
            // boolean cut — collinear fragment selections merge into one
            // span, so the round passes straight through the feature,
            // exactly as if the fillet had preceded it in history. Only
            // reached after the native build failed, so models where
            // MakeFillet works never take this path. Convex straight edges
            // between planar faces only (a cut can't ADD material, so
            // concave fillets never come from here).
            std::vector<TopoDS_Shape> blends;
            TopoDS_Shape cutRes;
            if (materializr::blendcut::cutFillet(m_previousShape, m_edges,
                    m_radius, m_ledger, cutRes, blends)) {
                candidate = cutRes;
                m_generatedFaces = std::move(blends);
                std::fprintf(stderr, "[Fillet] native blend failed — built "
                             "as a swept-arc cut across the feature "
                             "(#55, R=%.2f)\n", m_radius);
            }
        }
        // LAST RESORT before failing: exact previously-successful params on
        // the exact same input body → adopt the stored result (see ChamferOp;
        // the "put the value back" case is the boolean fallback's worst case
        // — everywhere-coincident geometry — yet the answer already exists).
        if (candidate.IsNull()) {
            for (auto it = m_storedResults.rbegin();
                 it != m_storedResults.rend(); ++it) {
                if (it->base.IsNull() || it->result.IsNull()) continue;
                if (!m_previousShape.IsEqual(it->base)) continue;
                if (std::abs(m_radius - it->r) > 1e-9) continue;
                candidate = it->result;
                m_generatedFaces = it->genFaces;
                std::fprintf(stderr, "[Fillet] rebuild failed at known-good "
                             "params — adopting the stored result (same "
                             "input body, R=%.2f)\n", m_radius);
                break;
            }
        }
        if (candidate.IsNull()) return false;

        // Update the body with the filleted shape (kept on the op too, so
        // serializeParams can index the generated faces against the result).
        m_resultShape = candidate;
        // Record this execute's naming so the NEXT run never guesses: each
        // edge as its adjacent faces' lineage ids (see ChamferOp).
        {
            TopTools_IndexedDataMapOfShapeListOfShape efm;
            TopExp::MapShapesAndAncestors(m_previousShape, TopAbs_EDGE,
                                          TopAbs_FACE, efm);
            std::vector<std::pair<int,int>> pairs;
            for (const auto& e : m_edges) {
                int a = -1, b = -1;
                if (efm.Contains(e))
                    for (const TopoDS_Shape& f : efm.FindFromKey(e)) {
                        const auto* ids = materializr::topo::idsFor(inLineage, f);
                        if (!ids || ids->empty()) continue;
                        if (a < 0) a = ids->front();
                        else if (b < 0 && ids->front() != a) b = ids->front();
                    }
                if (a < 0 || b < 0) { pairs.clear(); break; }
                pairs.push_back({a, b});
            }
            if (pairs.size() == m_edges.size()) m_edgeFaceIdPairs = std::move(pairs);
        }
        m_prevFaceIds = inLineage;   // undo restores (partial-replay lifeline)
        doc.updateBody(m_bodyId, m_resultShape);
        doc.setBodyLedger(m_bodyId, &m_ledger);
        {
            // Face lineage (see ChamferOp): carry ancestry, stamp blends with
            // stable ids (reused across re-executes).
            materializr::topo::FaceIdMap next = materializr::topo::propagate(
                {{&inLineage, m_previousShape}}, m_ledger, m_resultShape);
            if (m_genFaceIds.size() != m_generatedFaces.size()) {
                m_genFaceIds.clear();
                for (size_t i = 0; i < m_generatedFaces.size(); ++i)
                    m_genFaceIds.push_back(doc.mintFaceId());
            }
            for (size_t i = 0; i < m_generatedFaces.size(); ++i)
                materializr::topo::addId(next, m_generatedFaces[i], m_genFaceIds[i]);
            materializr::topo::complete(next, m_resultShape,
                                        [&doc]() { return doc.mintFaceId(); });
            doc.setBodyFaceIds(m_bodyId, std::move(next));
        }
        // Remember this build for the adopt-stored-result path (see above).
        rememberResult(m_previousShape, m_resultShape);
        guard.committed = true;   // success — keep the (re)resolved state
        return true;
    } catch (...) {
        return false;
    }
}

bool FilletOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) {
        return false;
    }

    try {
        doc.updateBody(m_bodyId, m_previousShape);
        // Restore the input lineage captured at execute — updateBody wiped
        // it, and a partial replay won't re-run the op that minted it.
        if (!m_prevFaceIds.empty())
            doc.setBodyFaceIds(m_bodyId, m_prevFaceIds);
        return true;
    } catch (...) {
        return false;
    }
}

std::string FilletOp::description() const {
    return "Fillet R" + materializr::numStr(m_radius) + " on " +
           std::to_string(m_edges.size()) + " edge(s)";
}

void FilletOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Fillet"));
    ImGui::Separator();

    materializr::inputNumber(materializr::tr("Radius"), &m_radius, 0.1, 1.0, "%g");

    ImGui::Text(materializr::tr("Edges: %d selected"), static_cast<int>(m_edges.size()));
    ImGui::Text(materializr::tr("Body ID: %d"), m_bodyId);
}

OperationDiff FilletOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}

std::string FilletOp::serializeParams() const {
    // The edge set is persisted as ordinal indices into the INPUT shape's
    // canonical sub-shape map (see SubShapeIndex.h) — BREP round-trips the
    // shape byte-identically, so the indices resolve on reload. Generated
    // blend faces are indexed against the RESULT shape for click-to-edit.
    std::string blob;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "body=%d;radius=%.6f", m_bodyId, m_radius);
    blob += buf;
    if (!m_previousShape.IsNull() && !m_edges.empty()) {
        std::vector<TopoDS_Shape> edges(m_edges.begin(), m_edges.end());
        std::string idx = SubShapeIndex::serialize(m_previousShape, edges,
                                                   TopAbs_EDGE);
        if (!idx.empty()) blob += ";edges=" + idx;
    }
    if (!m_resultShape.IsNull() && !m_generatedFaces.empty()) {
        std::string idx = SubShapeIndex::serialize(m_resultShape,
                                                   m_generatedFaces,
                                                   TopAbs_FACE);
        if (!idx.empty()) blob += ";gen=" + idx;
    }
    // Generative anchors (additive; old readers ignore the key). See EdgeAnchor.
    std::string anc = EdgeAnchor::serialize(m_edgeAnchors);
    if (!m_genFaceIds.empty()) {
        blob += ";genids=";
        for (size_t i = 0; i < m_genFaceIds.size(); ++i)
            blob += (i ? "," : "") + std::to_string(m_genFaceIds[i]);
    }
    if (!m_edgeFaceIdPairs.empty()) {
        blob += ";edgefaces=";
        for (size_t i = 0; i < m_edgeFaceIdPairs.size(); ++i)
            blob += (i ? "," : "") + std::to_string(m_edgeFaceIdPairs[i].first)
                  + ":" + std::to_string(m_edgeFaceIdPairs[i].second);
    }
    if (!anc.empty()) blob += ";anchor=" + anc;
    // Topological edge names (additive, LAST — length-prefixed opaque blobs
    // read to end-of-string). Persisting them keeps a SEAM fillet/chamfer
    // re-derivable after reload; absent in old files.
    if (!m_edgeRefs.empty()) {
        bool any = false;
        std::string rb;
        for (const auto& r : m_edgeRefs) {
            std::string b = r.serialize();
            rb += std::to_string(b.size()) + ":" + b;
            if (!r.empty()) any = true;
        }
        if (any) blob += ";edgerefs=" + rb;
    }
    return blob;
}

bool FilletOp::deserializeParams(const std::string& blob) {
    // Tolerant key=value parser. Unknown keys are ignored; missing keys keep
    // current defaults. Returns true if at least one key was understood.
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        // edgerefs holds length-prefixed opaque blobs, written last — read to
        // end-of-string (not to the next ';').
        if (key == "edgerefs") {
            std::string rest = blob.substr(eq + 1);
            m_edgeRefs.clear();
            size_t p = 0;
            std::string tok;
            // Checked length + guaranteed cursor advance (ParamParse.h).
            // The old `c + 1 + n` bound wrapped on a negative length and
            // could drive `p` backwards, looping forever.
            while (materializr::readLenRecord(rest, p, tok))
                m_edgeRefs.push_back(materializr::topo::Ref::parse(tok));
            any = true;
            break;
        }
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "radius") { m_radius = std::atof(val.c_str()); any = true; }
        else if (key == "body")   { m_bodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "edges")  { m_edgeIndices = SubShapeIndex::parse(val); any = true; }
        else if (key == "gen")    { m_genFaceIndices = SubShapeIndex::parse(val); any = true; }
        else if (key == "genids") { m_genFaceIds = SubShapeIndex::parse(val); any = true; }
        else if (key == "edgefaces") {
            m_edgeFaceIdPairs.clear();
            size_t q = 0;
            while (q < val.size()) {
                size_t c = val.find(',', q);
                std::string tokp = val.substr(q, c == std::string::npos
                                                     ? std::string::npos : c - q);
                size_t col = tokp.find(':');
                if (col != std::string::npos)
                    m_edgeFaceIdPairs.push_back(
                        {std::atoi(tokp.c_str()),
                         std::atoi(tokp.c_str() + col + 1)});
                if (c == std::string::npos) break;
                q = c + 1;
            }
            any = true;
        }
        else if (key == "anchor") {
            EdgeAnchor::parse(val, m_edgeAnchors);
            any = true;
        }
        pos = end + 1;
    }
    return any;
}

bool FilletOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_bodyId < 0 || m_edgeIndices.empty()) return false;

    // Bind the before/after shapes for our body from the saved step.
    m_previousShape.Nullify();
    m_resultShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    for (const auto& [id, shp] : state.modifiedAfter)
        if (id == m_bodyId) { m_resultShape = shp; break; }
    if (m_previousShape.IsNull()) return false;

    // Re-resolve the filleted edges against the input shape. ALL must resolve
    // — a partial set would fillet the wrong geometry, so decline to ReplayOp.
    std::vector<TopoDS_Shape> resolved;
    if (!SubShapeIndex::resolveAll(m_previousShape, m_edgeIndices,
                                   TopAbs_EDGE, resolved)) {
        return false;
    }
    m_edges.clear();
    for (const auto& s : resolved) m_edges.push_back(TopoDS::Edge(s));

    // Blend faces (click-to-edit mapping) resolve against the result —
    // best-effort: their absence only disables face-click mapping.
    m_generatedFaces.clear();
    if (!m_resultShape.IsNull() && !m_genFaceIndices.empty()) {
        std::vector<TopoDS_Shape> gen;
        if (SubShapeIndex::resolveAll(m_resultShape, m_genFaceIndices,
                                      TopAbs_FACE, gen)) {
            m_generatedFaces = std::move(gen);
        }
    }
    // Fallback for a save without generated-face indices (churn-dropped gen=):
    // recover the blend faces geometrically so the history-hover highlight
    // still previews. See ChamferOp.
    if (m_generatedFaces.empty() && !m_resultShape.IsNull() &&
        !m_previousShape.IsNull())
        m_generatedFaces = facesCreatedVsPrev(m_resultShape, m_previousShape);
    // Seed the known-good cache with the LOADED build (entry 0, never
    // evicted): the original save is the answer for "put the value back".
    rememberResult(m_previousShape, m_resultShape);
    return true;
}

void FilletOp::refreshGeneratedFaces(const TopoDS_Shape& currentBody,
                                     const materializr::topo::FaceIdMap* lineage) {
    if (currentBody.IsNull()) return;
    // Lineage first — see ChamferOp::refreshGeneratedFaces.
    if (lineage && !m_genFaceIds.empty()) {
        std::vector<TopoDS_Shape> mine;
        for (const auto& e : *lineage)
            for (int id : e.ids)
                if (std::find(m_genFaceIds.begin(), m_genFaceIds.end(), id) !=
                    m_genFaceIds.end()) { mine.push_back(e.face); break; }
        if (!mine.empty()) { m_generatedFaces = std::move(mine); return; }
    }

    // The saved indices were captured against THIS fillet's local result shape;
    // resolving them against the final body (which may have more faces from
    // later fillets, and may have been moved by downstream Transforms) can drift
    // onto a neighbouring fillet's faces. So we rebind by geometry instead:
    // a constant-radius fillet's blend faces are analytic surfaces of radius
    // ≈ m_radius. Matching on radius keeps a 3 mm fillet from ever claiming a
    // neighbouring 4 mm fillet's faces, and is invariant under rigid moves.
    const double rtol = std::max(1e-3, 1e-2 * m_radius);

    std::vector<TopoDS_Shape> result;
    for (TopExp_Explorer ex(currentBody, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face& f = TopoDS::Face(ex.Current());
        double r = faceBlendRadius(f);
        if (r >= 0.0 && std::fabs(r - m_radius) <= rtol)
            result.push_back(f);
    }

    // Add any index-resolved faces the radius scan can't classify (free-form
    // blends), but exclude index faces whose radius clearly belongs to a
    // DIFFERENT fillet — those are the drift this rebind exists to reject.
    std::vector<TopoDS_Shape> idxFaces;
    if (!m_genFaceIndices.empty() &&
        SubShapeIndex::resolveAll(currentBody, m_genFaceIndices, TopAbs_FACE, idxFaces)) {
        for (const auto& s : idxFaces) {
            // A fillet blend is a cylinder / torus / sphere, or a free-form
            // (bspline) blend — NEVER a plane or a cone. Reject those: they're
            // ordinal-index drift onto unrelated faces once downstream ops
            // reorder the body's face map (e.g. a later countersink chamfer's
            // cone), which the -1 "unclassifiable" radius below would otherwise
            // wave straight through, letting the fillet steal the chamfer's
            // face (#49).
            try {
                GeomAbs_SurfaceType t =
                    BRepAdaptor_Surface(TopoDS::Face(s)).GetType();
                if (t == GeomAbs_Plane || t == GeomAbs_Cone) continue;
            } catch (...) { continue; }
            double r = faceBlendRadius(TopoDS::Face(s));
            if (r >= 0.0 && std::fabs(r - m_radius) > rtol) continue; // wrong fillet
            bool dup = false;
            for (const auto& g : result) if (g.IsSame(s)) { dup = true; break; }
            if (!dup) result.push_back(s);
        }
    }

    if (!result.empty())
        m_generatedFaces = std::move(result);
    else if (!idxFaces.empty())
        m_generatedFaces = std::move(idxFaces); // last-resort: trust the indices
}

bool FilletOp::ownsFace(const TopoDS_Shape& face) const {
    return ownsFaceScore(face) > 0;
}

int FilletOp::ownsFaceScore(const TopoDS_Shape& face) const {
    if (face.IsNull() || face.ShapeType() != TopAbs_FACE) return 0;
    // A fillet blend is NEVER a plane (straight edges blend to cylinders,
    // curved/corner cases to tori/spheres/bsplines). Rehydrated generated-face
    // indices can mis-resolve after an old-save reload (ordinal drift) and
    // claim a big planar neighbour — clicking the slab top then opened the
    // fillet editor instead of the face's own properties.
    try {
        BRepAdaptor_Surface bs(TopoDS::Face(face));
        if (bs.GetType() == GeomAbs_Plane) return 0;
    } catch (...) {}
    for (const auto& f : m_generatedFaces) {
        if (f.IsSame(face)) return 2;   // exact identity on the live body
    }
    // Geometric fallback for when the body's faces were rebuilt (e.g. after a
    // replay) and are no longer IsSame to the stored ones — a WEAKER match, so
    // an exact owner elsewhere in history wins over this (#49).
    gp_Pnt q;
    if (!faceCenter(TopoDS::Face(face), q)) return 0;
    for (const auto& f : m_generatedFaces) {
        gp_Pnt p;
        if (faceCenter(TopoDS::Face(f), p) && p.Distance(q) < 1e-4) return 1;
    }
    return 0;
}
