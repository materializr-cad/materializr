#include "../core/NumFormat.h"
#include "ChamferOp.h"
#include "BlendCut.h"
#include "SubShapeIndex.h"
#include "EdgeAnchor.h"
#include "FaceSurfSig.h"
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <BRepGProp_Face.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS_Vertex.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <gp_Lin.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <imgui.h>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"
#include "ParamParse.h"

namespace {
// Representative point on a face (midpoint of its UV bounds).
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

// Faces present in `result` but NOT in `prev` — i.e. the faces this op CREATED.
// Reload fallback for the history-hover highlight when a save carries no
// generated-face indices (an older/churn-corrupted file where the fill
// chamfer's `gen=` was dropped): the bevel faces are exactly the new ones.
//
// Matched by the unbounded SURFACE, not the centroid: when this chamfer trims a
// corner off an adjacent earlier bevel, that face keeps its surface but its
// centroid shifts — a centroid test would wrongly flag it as new and light up
// earlier steps' bevels on hover. Non-analytic faces (no cheap surface
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
            // Same unbounded surface = the same face re-trimmed, not created.
            // sameSurface returns false for non-analytic faces, so the centroid
            // check still covers B-spline/Bézier as it did before.
            if (materializr::sameSurface(prevF[i], f) ||
                prevC[i].Distance(c) < 1e-4) { wasThere = true; break; }
        }
        if (!wasThere) out.push_back(f);
    }
    return out;
}
} // namespace

ChamferOp::ChamferOp() = default;

void ChamferOp::rememberResult(const TopoDS_Shape& base,
                               const TopoDS_Shape& result) {
    if (base.IsNull() || result.IsNull()) return;
    // Refresh an existing (base, params) entry rather than duplicating.
    for (auto& e : m_storedResults) {
        if (e.base.IsEqual(base) && std::abs(e.d - m_distance) < 1e-9 &&
            std::abs(e.d2 - m_distance2) < 1e-9) {
            e.result = result;
            e.genFaces = m_generatedFaces;
            return;
        }
    }
    m_storedResults.push_back(
        {base, result, m_distance, m_distance2, m_generatedFaces});
    // Bounded; keep entry 0 (the loaded original) forever, evict the oldest
    // in-session entry after that.
    if (m_storedResults.size() > 8)
        m_storedResults.erase(m_storedResults.begin() + 1);
}

void ChamferOp::snapshotEditState() {
    m_editSnap.edges = m_edges;
    m_editSnap.anchors = m_edgeAnchors;
    m_editSnap.refs = m_edgeRefs;
    m_editSnap.pairs = m_edgeFaceIdPairs;
    m_editSnap.prevFaceIds = m_prevFaceIds;
    m_editSnap.previousShape = m_previousShape;
    m_editSnap.resultShape = m_resultShape;
    m_editSnap.generatedFaces = m_generatedFaces;
    m_editSnap.genFaceIds = m_genFaceIds;
    m_editSnap.distance = m_distance;
    m_editSnap.distance2 = m_distance2;
    m_editSnap.storedResults = m_storedResults;
    m_editSnap.refFaceId = m_refFaceId;
    m_editSnap.valid = true;
}

void ChamferOp::restoreEditState() {
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
    m_distance = m_editSnap.distance;
    m_distance2 = m_editSnap.distance2;
    m_storedResults = m_editSnap.storedResults;
    m_refFaceId = m_editSnap.refFaceId;
}

void ChamferOp::setBody(int bodyId) {
    m_bodyId = bodyId;
}

void ChamferOp::setEdges(const std::vector<TopoDS_Edge>& edges) {
    m_edges = edges;
}

void ChamferOp::setDistance(double distance) {
    m_distance = distance;
}

TopoDS_Face ChamferOp::sharedReferenceFace(const TopoDS_Shape& body,
                                           const std::vector<TopoDS_Edge>& edges) {
    if (edges.empty() || body.IsNull()) return TopoDS_Face();
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaceMap;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, edgeFaceMap);
    if (!edgeFaceMap.Contains(edges.front())) return TopoDS_Face();

    // Candidates = faces adjacent to the first edge; intersect down with each
    // subsequent edge's adjacent faces. Whatever survives borders every edge.
    std::vector<TopoDS_Shape> cands;
    for (const TopoDS_Shape& f : edgeFaceMap.FindFromKey(edges.front()))
        cands.push_back(f);
    for (size_t i = 1; i < edges.size(); ++i) {
        if (!edgeFaceMap.Contains(edges[i])) return TopoDS_Face();
        const TopTools_ListOfShape& fs = edgeFaceMap.FindFromKey(edges[i]);
        std::vector<TopoDS_Shape> keep;
        for (const auto& c : cands)
            for (const TopoDS_Shape& f : fs)
                if (f.IsSame(c)) { keep.push_back(c); break; }
        cands.swap(keep);
        if (cands.empty()) return TopoDS_Face(); // no common face for this set
    }
    return TopoDS::Face(cands.front());
}

// See FilletOp::anchorSketches — anchoring consults every sketch in the doc,
// preferring the cascade override (the edited sketch's final state) over the
// live sketch, which is rolled back through its snapshots mid-replay.
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

void ChamferOp::computeAnchors(Document& doc) {
    m_edgeAnchors.clear();
    std::vector<std::shared_ptr<materializr::Sketch>> keep;
    m_edgeAnchors = EdgeAnchor::compute(m_edges, anchorSketches(doc, keep));
}

bool ChamferOp::resolveAnchors(Document& doc, const TopoDS_Shape& base) {
    if (m_edgeAnchors.size() != m_edges.size()) return false;
    std::vector<TopoDS_Edge> resolved;
    std::vector<std::shared_ptr<materializr::Sketch>> keep;
    if (!EdgeAnchor::resolve(m_edgeAnchors, anchorSketches(doc, keep), base, resolved))
        return false;
    m_edges = std::move(resolved);
    return true;
}

bool ChamferOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_edges.empty() || m_distance <= 0.0) {
        return false;
    }

    // FAILURE MUST NOT POISON THE OP. Resolution below rewrites m_edges (and
    // can rewrite anchors/refs) against the current body; if the build then
    // fails, those half-resolved members would feed the NEXT attempt — which
    // is how one failed edit wedged every later edit until reload (probe:
    // fail at d2=14, then even the fresh-working d2=16 refused). Snapshot the
    // resolution state and roll it back on every non-success exit.
    struct ResolutionGuard {
        ChamferOp& op;
        std::vector<TopoDS_Edge> edges;
        std::vector<EdgeAnchor::Anchor> anchors;
        std::vector<materializr::topo::Ref> refs;
        std::vector<std::pair<int,int>> pairs;
        bool committed = false;
        explicit ResolutionGuard(ChamferOp& o)
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

        // Input lineage, completed so EVERY face has an id (minting is
        // deterministic in face order, so a replay that runs the same chain
        // mints the same ids). This is what edge/reference resolution below
        // and the post-build recording key off.
        materializr::topo::FaceIdMap inLineage;
        if (const auto* im = doc.bodyFaceIds(m_bodyId)) inLineage = *im;
        materializr::topo::complete(inLineage, m_previousShape,
                                    [&doc]() { return doc.mintFaceId(); });

        // Lineage-FIRST edge resolution (#52): each edge is named by its two
        // adjacent faces' ancestry ids — immune to ordinal drift and to the
        // geometric divergence of a replayed body. All-or-nothing; on miss,
        // fall through to the classic rebind → anchors → topo-refs chain.
        const char* dbgTier = "rebind";
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
                    // DISTINCT-CLAIM (EdgeAnchor's rule): fragment edges of
                    // one span share the SAME face-id pair, so first-hit
                    // handed every pair the same edge — collapsing a
                    // 3-fragment selection to 1 and starving the rebuild
                    // (Steve's "17 works fresh, fails after an edit").
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
                dbgTier = "lineage";
            }
        }

        // Re-bind stored edges to the (possibly regenerated) body before
        // chamfering — see FilletOp::execute. Without this, editing an
        // upstream fillet's radius left every chamfer edge stale: the
        // edge-face map silently skipped them all and the chamfer vanished.
        if (!edgesResolvedByLineage &&
            !SubShapeIndex::rebindEdges(m_previousShape, m_edges)) {
            // Ordinal/carrier match failed (a sketch dimension edit moved the
            // edges) — re-find them by their sketch feature. See EdgeAnchor.
            dbgTier = "anchors";
            if (!resolveAnchors(doc, m_previousShape)) {
                dbgTier = "toporefs";
                // LAST RESORT: topological names — the boolean-SEAM case,
                // where anchors fail by construction. Mirrors FilletOp.
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
                        std::fprintf(stderr, "[Chamfer] edges re-found by "
                                             "topo refs (gen/seam path)\n");
                    }
                }
                if (!topoOk) {
                    std::fprintf(stderr, "[Chamfer] rebind+anchors+toporefs "
                        "ALL failed (d=%.2f/%.2f, %zu edges)\n",
                        m_distance, m_distance2, m_edges.size());
                    return false;
                }
            }
        }
        // Deduplicate: on a body whose fragmented faces were later unified,
        // several stored fragment edges rebind onto ONE clean edge — feeding
        // MakeChamfer the same edge repeatedly fails the whole build (#53's
        // light-cover replay). Chamfering it once is the original intent.
        {
            std::vector<TopoDS_Edge> uniq;
            for (const auto& e : m_edges) {
                bool dup = false;
                for (const auto& u : uniq) if (u.IsSame(e)) { dup = true; break; }
                if (!dup) uniq.push_back(e);
            }
            if (uniq.size() != m_edges.size()) {
                std::fprintf(stderr, "[Chamfer] %zu stored edges resolved to "
                             "%zu distinct — fragmented topology was unified "
                             "upstream\n", m_edges.size(), uniq.size());
                m_edges = std::move(uniq);
            }
        }
        if (std::getenv("MZR_CHAMFER_DBG")) {
            std::fprintf(stderr, "[chdbg] d=%.2f/%.2f tier=%s edges=%zu:",
                         m_distance, m_distance2, dbgTier, m_edges.size());
            for (const auto& e : m_edges) {
                try {
                    BRepAdaptor_Curve c(e);
                    gp_Pnt m = c.Value(
                        0.5 * (c.FirstParameter() + c.LastParameter()));
                    std::fprintf(stderr, " (%.2f,%.2f,%.2f)",
                                 m.X(), m.Y(), m.Z());
                } catch (...) { std::fprintf(stderr, " (?)"); }
            }
            std::fprintf(stderr, "\n");
        }
        if (m_edgeAnchors.empty()) computeAnchors(doc);
        // Topological names, minted with the body's producing ledger in
        // context so a SEAM edge gets its gen-lineage name.
        if (m_edgeRefs.empty() && !m_edges.empty()) {
            materializr::topo::Context mc;
            mc.doc = &doc;
            mc.shape = m_previousShape;
            mc.type = TopAbs_EDGE;
            mc.gen = doc.bodyLedger(m_bodyId);
            for (const auto& e : m_edges)
                m_edgeRefs.push_back(materializr::topo::mint(e, mc));
        }

        // Everything below re-derives from m_edges, so the whole build is a
        // retryable unit: if it fails, the caller may RE-RESOLVE the edges
        // (anchors — the upstream-re-derivation case) and attempt again.
        TopTools_IndexedDataMapOfShapeListOfShape edgeFaceMap;
        TopoDS_Face sharedRef;
        auto prepare = [&]() {
        edgeFaceMap.Clear();
        TopExp::MapShapesAndAncestors(m_previousShape, TopAbs_EDGE, TopAbs_FACE, edgeFaceMap);

        // For an asymmetric chamfer across multiple edges, distance 1 must be
        // measured along ONE consistent face (the loop's shared face) for every
        // edge, or A/B would flip from edge to edge. sharedRef is that face when
        // it exists (always, for a single edge); null = no common face, in which
        // case we fall back to each edge's first face (symmetric is unaffected).
        sharedRef = TopoDS_Face();
        if (m_distance2 > 0.0) {
            // Persisted reference id wins (deterministic); guess only without it.
            if (m_refFaceId >= 0) {
                for (TopExp_Explorer fx(m_previousShape, TopAbs_FACE);
                     fx.More(); fx.Next()) {
                    const auto* ids = materializr::topo::idsFor(inLineage, fx.Current());
                    if (ids && std::find(ids->begin(), ids->end(), m_refFaceId)
                                   != ids->end()) {
                        sharedRef = TopoDS::Face(fx.Current());
                        break;
                    }
                }
            }
            if (sharedRef.IsNull())
                sharedRef = sharedReferenceFace(m_previousShape, m_edges);
        }
        };  // prepare()

        // Build with (dAlongRef, dOther). Split into a lambda because the
        // asymmetric reference face is NOT persisted: on a replayed body,
        // sharedReferenceFace / faces.First() can pick the OTHER adjacent
        // face than the original session did, and a 11.4 mm setback aimed
        // along a 2 mm face simply cannot build (!IsDone) — the "two-distance
        // chamfer dies on replay / turns into a regular face" bug. When the
        // first orientation fails, retrying with the distances swapped is the
        // SAME chamfer measured from the other face, and recovers the
        // original geometry whenever only one orientation is feasible.
        auto tryBuild = [&](double dRef, double dOther) -> TopoDS_Shape {
            BRepFilletAPI_MakeChamfer mk(m_previousShape);
            for (const auto& edge : m_edges) {
                if (!edgeFaceMap.Contains(edge)) continue;
                const TopTools_ListOfShape& faces = edgeFaceMap.FindFromKey(edge);
                if (faces.IsEmpty()) continue;
                TopoDS_Face face = sharedRef.IsNull()
                                       ? TopoDS::Face(faces.First())
                                       : sharedRef;
                mk.Add(dRef, dOther, edge, face);
            }
            try {
                mk.Build();
                if (!mk.IsDone()) return TopoDS_Shape();
                TopoDS_Shape s = mk.Shape();
                if (s.IsNull()) return TopoDS_Shape();
                // Publish the generation maps from THIS builder (the one that
                // actually produced the shape).
                m_ledger.capture(mk, m_previousShape, TopAbs_EDGE);
                m_ledger.captureAdd(mk, m_previousShape, TopAbs_FACE);
                m_generatedFaces.clear();
                for (const auto& edge : m_edges) {
                    try {
                        for (const TopoDS_Shape& g : mk.Generated(edge))
                            if (g.ShapeType() == TopAbs_FACE)
                                m_generatedFaces.push_back(g);
                    } catch (...) {}
                }
                return s;
            } catch (...) { return TopoDS_Shape(); }
        };

        const double dB = (m_distance2 > 0.0) ? m_distance2 : m_distance;
        auto attemptBoth = [&]() -> TopoDS_Shape {
            prepare();
            TopoDS_Shape c = tryBuild(m_distance, dB);
            if (c.IsNull() && m_distance2 > 0.0) {
                c = tryBuild(m_distance2, m_distance);
                if (!c.IsNull())
                    std::fprintf(stderr, "[Chamfer] asymmetric reference "
                                 "flipped on this body — rebuilt with "
                                 "distances swapped (d=%.2f/%.2f)\n",
                                 m_distance, m_distance2);
            }
            return c;
        };
        TopoDS_Shape candidate = attemptBoth();
        if (candidate.IsNull() && !edgesResolvedByLineage) {
            // The edges rebind found carriers, but the chamfer can't BUILD
            // there — the tell of an upstream PARAMETRIC re-derivation (a
            // sketch edit changed the body; stale carriers still exist but
            // the rim moved). Re-find the edges by their sketch features and
            // try once more — this is what the anchors are for (#52).
            std::vector<TopoDS_Edge> keepEdges = m_edges;
            if (resolveAnchors(doc, m_previousShape)) {
                candidate = attemptBoth();
                if (!candidate.IsNull())
                    std::fprintf(stderr, "[Chamfer] edges re-anchored after a "
                                 "re-derived upstream body (d=%.2f/%.2f)\n",
                                 m_distance, m_distance2);
            }
            if (candidate.IsNull()) m_edges = std::move(keepEdges);
        }
        // IsDone() is necessary but NOT sufficient: native can "succeed" with
        // topologically INVALID output (self-intersections, dropped faces) —
        // classically many-adjacent-edges, but also a big ramp chamfer whose
        // blend collides with a feature (#57: A=16 across the square hole).
        // NULL such a candidate here so the fallbacks below get their chance;
        // previously the garbage native result blocked them and the whole op
        // failed where the fill would have built fine.
        if (!candidate.IsNull() && !BRepCheck_Analyzer(candidate).IsValid()) {
            std::fprintf(stderr,
                "[Chamfer] native result failed BRepCheck_Analyzer "
                "(d=%.2f, %zu edges) — trying the cut/fill fallbacks.\n",
                m_distance, m_edges.size());
            candidate.Nullify();
        }
        // Valid is STILL not sufficient: ChFi3d can return a PARTIAL blend —
        // it runs the bevel up to an obstacle (a hole, a countersink), tapers
        // out, and never resumes, leaving most of the edge untouched (#57:
        // the ramp dying mid-run with a pointed taper). Detect it by
        // COVERAGE: the generated faces must reach both ends of every
        // selected edge (interior gaps are fine — a through-feature's
        // stop-faces are legitimate). A short native result is benched, the
        // fallbacks get their chance, and it is restored only if they can't
        // build either.
        TopoDS_Shape nativeBench;
        if (!candidate.IsNull() && !m_generatedFaces.empty()) {
            const double endTol =
                std::max(m_distance, dB) * 1.5 + 0.5;
            bool shortCoverage = false;
            for (const auto& edge : m_edges) {
                BRepAdaptor_Curve ec(edge);
                if (ec.GetType() != GeomAbs_Line) continue;
                gp_Pnt pa = ec.Value(ec.FirstParameter());
                gp_Pnt pb = ec.Value(ec.LastParameter());
                gp_Vec dir(pa, pb);
                const double len = dir.Magnitude();
                if (len < 1e-9) continue;
                dir.Normalize();
                gp_Lin eline(pa, gp_Dir(dir));
                // Per-face covered interval along the edge, then union.
                // Ends-only (min/max) let native fragments AT the corners
                // mask a dead middle: ChFi3d blended a few mm at each end
                // (against neighbouring caps) and skipped the whole centre —
                // ends "covered", gate passed, fill never ran. An interior
                // gap on a single continuous edge is never legitimate (the
                // stop-face through-hole case only arises on FRAGMENTED
                // edges, each checked separately here).
                std::vector<std::pair<double, double>> spans;
                for (const auto& gf : m_generatedFaces) {
                    double lo = 1e18, hi = -1e18;
                    for (TopExp_Explorer vx(gf, TopAbs_VERTEX); vx.More();
                         vx.Next()) {
                        gp_Pnt v = BRep_Tool::Pnt(
                            TopoDS::Vertex(vx.Current()));
                        if (eline.Distance(v) >
                            std::max(m_distance, dB) * 2.0 + 0.5)
                            continue;
                        const double t = gp_Vec(pa, v).Dot(dir);
                        lo = std::min(lo, t);
                        hi = std::max(hi, t);
                    }
                    if (hi >= lo) spans.push_back({lo, hi});
                }
                if (spans.empty()) continue;
                std::sort(spans.begin(), spans.end());
                double covTo = spans.front().first; // start of coverage
                const double covFrom = covTo;
                double maxGap = 0.0;
                for (const auto& sp : spans) {
                    if (sp.first > covTo)
                        maxGap = std::max(maxGap, sp.first - covTo);
                    covTo = std::max(covTo, sp.second);
                }
                if (covFrom > endTol || covTo < len - endTol ||
                    maxGap > endTol) {
                    shortCoverage = true;
                    break;
                }
            }
            if (shortCoverage) {
                std::fprintf(stderr,
                    "[Chamfer] native blend only covers part of the edge "
                    "(partial taper) — trying the cut/fill fallbacks "
                    "(d=%.2f/%.2f).\n", m_distance, dB);
                nativeBench = candidate;
                candidate.Nullify();
            }
        }
        if (candidate.IsNull()) {
            // #55: the native blend can't resolve against a surface feature
            // crossing the edge (a drilled hole or pocket fragments it and
            // ChFi3d gives up at the feature walls). Build the same removal
            // as a swept-wedge boolean cut instead — collinear fragment
            // selections merge into one span, so the bevel passes straight
            // through the feature, exactly as if the chamfer had preceded it
            // in history. Only reached after every native attempt failed, so
            // models where MakeChamfer works never take this path.
            prepare();
            std::vector<TopoDS_Shape> blends;
            TopoDS_Shape cutRes;
            if (materializr::blendcut::cutChamfer(
                    m_previousShape, m_edges, m_distance, dB, sharedRef,
                    m_ledger, cutRes, blends)) {
                candidate = cutRes;
                m_generatedFaces = std::move(blends);
                std::fprintf(stderr, "[Chamfer] native blend failed — built "
                             "as a swept-wedge cut across the feature "
                             "(#55, d=%.2f/%.2f)\n", m_distance, dB);
            } else if (materializr::blendcut::fillChamfer(
                           m_previousShape, m_edges, m_distance, dB, sharedRef,
                           m_ledger, cutRes, blends) ||
                       (m_distance2 > 0.0 &&
                        materializr::blendcut::fillChamfer(
                            m_previousShape, m_edges, dB, m_distance, sharedRef,
                            m_ledger, cutRes, blends))) {
                // Interior corner: the chamfer is a RAMP (adds material), and
                // its footprint crosses a feature native can't resolve — fuse
                // the ramp and re-pierce the feature (#57). Asymmetric retries
                // with the distances swapped, mirroring the native retry.
                candidate = cutRes;
                m_generatedFaces = std::move(blends);
                std::fprintf(stderr, "[Chamfer] native blend failed — built "
                             "as a corner-fill ramp across the feature "
                             "(#57, d=%.2f/%.2f)\n", m_distance, dB);
            }
        }
        if (candidate.IsNull() && !nativeBench.IsNull()) {
            // No fallback could build — the partial native blend is still
            // better than failing the whole op.
            candidate = nativeBench;
            std::fprintf(stderr, "[Chamfer] keeping the partial native blend "
                         "(no fallback available).\n");
        }
        // LAST RESORT before failing: if the caller asked for EXACTLY the
        // parameters of a previously-successful build on EXACTLY the same
        // input body, that stored result IS the answer — adopt it. This is
        // the "put the value back" case: rebuilding at the original value
        // fails because the new blend is everywhere coincident with features
        // built on the original bevel (worst case for the boolean fallback),
        // yet the correct output already exists on the op.
        if (candidate.IsNull()) {
            for (auto it = m_storedResults.rbegin();
                 it != m_storedResults.rend(); ++it) {
                if (it->base.IsNull() || it->result.IsNull()) continue;
                if (!m_previousShape.IsEqual(it->base)) continue;
                if (std::abs(m_distance - it->d) > 1e-9 ||
                    std::abs(m_distance2 - it->d2) > 1e-9) continue;
                candidate = it->result;
                m_generatedFaces = it->genFaces;
                std::fprintf(stderr, "[Chamfer] rebuild failed at known-good "
                             "params — adopting the stored result (same "
                             "input body, same values, d=%.2f/%.2f)\n",
                             m_distance, m_distance2);
                break;
            }
        }
        if (candidate.IsNull()) {
            std::fprintf(stderr, "[Chamfer] MakeChamfer failed (d=%.2f/%.2f)\n",
                         m_distance, m_distance2);
            return false;
        }
        if (candidate.IsNull()) {
            std::fprintf(stderr, "[Chamfer] result shape is null (d=%.2f).\n",
                         m_distance);
            return false;
        }

        // IsDone() is necessary but NOT sufficient — see FilletOp::execute.
        // Chamfering many adjacent edges at once can yield a result OCCT
        // reports as done but is topologically INVALID (self-intersections,
        // dropped faces): the "faces disappear" bug. BRepCheck_Analyzer is the
        // authoritative validity test; reject anything it flags so a corrupt
        // body never reaches the document/history.
        if (!BRepCheck_Analyzer(candidate).IsValid()) {
            std::fprintf(stderr,
                "[Chamfer] result failed BRepCheck_Analyzer (d=%.2f, %zu edges) "
                "— invalid topology, refusing to commit.\n",
                m_distance, m_edges.size());
            return false;
        }

        // Same narrow defence as FilletOp::execute: bbox-must-not-grow
        // and volume must be strictly > 0. The old volume-cap (output ≤
        // input × 1.01) wrongly rejected concave chamfers which add a
        // sliver of material in the corner.
        {
            // AddOptimal walks the actual geometry rather than tolerance-
            // padded extents — see FilletOp::execute for the full story.
            Bnd_Box bbIn, bbOut;
            BRepBndLib::AddOptimal(m_previousShape, bbIn);
            BRepBndLib::AddOptimal(candidate,       bbOut);
            if (!bbIn.IsVoid() && !bbOut.IsVoid()) {
                Standard_Real ix0, iy0, iz0, ix1, iy1, iz1;
                Standard_Real ox0, oy0, oz0, ox1, oy1, oz1;
                bbIn .Get(ix0, iy0, iz0, ix1, iy1, iz1);
                bbOut.Get(ox0, oy0, oz0, ox1, oy1, oz1);
                const double slop = 1.01;
                if (ox1 - ox0 > (ix1 - ix0) * slop ||
                    oy1 - oy0 > (iy1 - iy0) * slop ||
                    oz1 - oz0 > (iz1 - iz0) * slop) {
                    return false;
                }
            }

            GProp_GProps gpOut;
            BRepGProp::VolumeProperties(candidate, gpOut);
            if (gpOut.Mass() < 1e-6) {
                std::fprintf(stderr, "[Chamfer] zero-volume result\n");
                return false;
            }
        }

        // (Ledger capture + generated-face collection happen inside tryBuild,
        // against whichever builder actually produced the accepted shape.)

        // Update the body with the chamfered shape (kept on the op too, so
        // serializeParams can index the generated faces against the result).
        m_resultShape = candidate;
        // Record this execute's naming so the NEXT run never guesses:
        // reference face + each edge as its adjacent faces' lineage ids.
        if (!sharedRef.IsNull()) {
            if (const auto* ids = materializr::topo::idsFor(inLineage, sharedRef))
                if (!ids->empty()) m_refFaceId = ids->front();
        }
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

        // Face lineage: carry the input body's ancestry through, then stamp
        // this chamfer's bevel faces with STABLE ids — reused across
        // re-executes (replay/edit) so downstream references and saves stay
        // consistent; minted fresh only when the bevel count changes.
        {
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

bool ChamferOp::undo(Document& doc) {
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

std::string ChamferOp::description() const {
    if (m_distance2 > 0.0)
        return "Chamfer D" + materializr::numStr(m_distance) + "/" +
               materializr::numStr(m_distance2) + " on " +
               std::to_string(m_edges.size()) + " edge(s)";
    return "Chamfer D" + materializr::numStr(m_distance) + " on " +
           std::to_string(m_edges.size()) + " edge(s)";
}

void ChamferOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Chamfer"));
    ImGui::Separator();

    materializr::inputNumber(materializr::tr("Distance"), &m_distance, 0.1, 1.0, "%g");
    bool asym = (m_distance2 > 0.0);
    if (ImGui::Checkbox(materializr::tr("Two distances"), &asym))
        m_distance2 = asym ? m_distance : -1.0;
    if (m_distance2 > 0.0)
        materializr::inputNumber(materializr::tr("Distance 2"), &m_distance2, 0.1, 1.0, "%g");

    ImGui::Text(materializr::tr("Edges: %d selected"), static_cast<int>(m_edges.size()));
    ImGui::Text(materializr::tr("Body ID: %d"), m_bodyId);
}

OperationDiff ChamferOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}

std::string ChamferOp::serializeParams() const {
    // Same persistent sub-shape scheme as FilletOp: edges indexed into the
    // INPUT shape, generated faces into the RESULT (see SubShapeIndex.h).
    std::string blob;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "body=%d;distance=%.6f", m_bodyId, m_distance);
    blob += buf;
    if (m_distance2 > 0.0) {
        std::snprintf(buf, sizeof(buf), ";distance2=%.6f", m_distance2);
        blob += buf;
    }
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
    if (m_refFaceId >= 0) blob += ";refid=" + std::to_string(m_refFaceId);
    if (!m_edgeFaceIdPairs.empty()) {
        blob += ";edgefaces=";
        for (size_t i = 0; i < m_edgeFaceIdPairs.size(); ++i)
            blob += (i ? "," : "") + std::to_string(m_edgeFaceIdPairs[i].first)
                  + ":" + std::to_string(m_edgeFaceIdPairs[i].second);
    }
    if (!m_genFaceIds.empty()) {
        blob += ";genids=";
        for (size_t i = 0; i < m_genFaceIds.size(); ++i)
            blob += (i ? "," : "") + std::to_string(m_genFaceIds[i]);
    }
    std::string anc = EdgeAnchor::serialize(m_edgeAnchors);
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

bool ChamferOp::deserializeParams(const std::string& blob) {
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
            if (!materializr::topo::parseRefList(rest, m_edgeRefs))
                return false;
            any = true;
            break;
        }
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "distance") { m_distance = std::atof(val.c_str()); any = true; }
        else if (key == "distance2"){ m_distance2 = std::atof(val.c_str()); any = true; }
        else if (key == "body")     { m_bodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "edges")    { m_edgeIndices = SubShapeIndex::parse(val); any = true; }
        else if (key == "gen")      { m_genFaceIndices = SubShapeIndex::parse(val); any = true; }
        else if (key == "genids")   { m_genFaceIds = SubShapeIndex::parse(val); any = true; }
        else if (key == "refid")    { m_refFaceId = std::atoi(val.c_str()); any = true; }
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
        else if (key == "anchor")   { EdgeAnchor::parse(val, m_edgeAnchors); any = true; }
        pos = end + 1;
    }
    return any;
}

bool ChamferOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_bodyId < 0 || m_edgeIndices.empty()) return false;

    m_previousShape.Nullify();
    m_resultShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    for (const auto& [id, shp] : state.modifiedAfter)
        if (id == m_bodyId) { m_resultShape = shp; break; }
    if (m_previousShape.IsNull()) return false;

    std::vector<TopoDS_Shape> resolved;
    if (!SubShapeIndex::resolveAll(m_previousShape, m_edgeIndices,
                                   TopAbs_EDGE, resolved)) {
        return false;
    }
    m_edges.clear();
    for (const auto& s : resolved) m_edges.push_back(TopoDS::Edge(s));

    m_generatedFaces.clear();
    if (!m_resultShape.IsNull() && !m_genFaceIndices.empty()) {
        std::vector<TopoDS_Shape> gen;
        if (SubShapeIndex::resolveAll(m_resultShape, m_genFaceIndices,
                                      TopAbs_FACE, gen)) {
            m_generatedFaces = std::move(gen);
        }
    }
    // Fallback: a save without generated-face indices (a fill chamfer whose
    // `gen=` was dropped during heavy undo/redo churn) leaves the history-hover
    // highlight blank. Recover the bevel faces geometrically — the faces the
    // chamfer added to the result — so the step still previews.
    if (m_generatedFaces.empty() && !m_resultShape.IsNull() &&
        !m_previousShape.IsNull())
        m_generatedFaces = facesCreatedVsPrev(m_resultShape, m_previousShape);
    // Seed the known-good cache with the LOADED build (entry 0, never
    // evicted): the original save is the answer for "put the value back".
    rememberResult(m_previousShape, m_resultShape);
    return true;
}

void ChamferOp::refreshGeneratedFaces(const TopoDS_Shape& currentBody,
                                      const materializr::topo::FaceIdMap* lineage) {
    if (currentBody.IsNull()) return;
    // Lineage first (exact, split-aware): every current face whose ancestry
    // contains one of this chamfer's bevel ids IS a piece of this chamfer —
    // including pieces a downstream boolean cut the bevel into (#51), which
    // no geometric matcher can trace. Falls through to geometry when the
    // body has no lineage (old saves, non-propagating downstream ops).
    if (lineage && !m_genFaceIds.empty()) {
        std::vector<TopoDS_Shape> mine;
        for (const auto& e : *lineage)
            for (int id : e.ids)
                if (std::find(m_genFaceIds.begin(), m_genFaceIds.end(), id) !=
                    m_genFaceIds.end()) { mine.push_back(e.face); break; }
        if (!mine.empty()) { m_generatedFaces = std::move(mine); return; }
    }
    if (m_genFaceIndices.empty()) return;
    // The saved ordinal indices are only meaningful against THIS chamfer's own
    // result shape (SubShapeIndex's documented limitation). Resolving them
    // straight against the final body drifts onto unrelated faces the moment a
    // downstream op reorders the face map — which made a chamfer claim faces
    // all over the part, and let an earlier fillet's drift steal a chamfer's
    // bevel (#49). Resolve against the result shape to get the TRUE bevel
    // faces as geometry, then map each to the current body by centre + surface
    // type (a bevel doesn't move when an unrelated downstream op runs).
    std::vector<TopoDS_Shape> truth;
    if (m_resultShape.IsNull() ||
        !SubShapeIndex::resolveAll(m_resultShape, m_genFaceIndices,
                                   TopAbs_FACE, truth) ||
        truth.empty())
        return; // keep whatever m_generatedFaces we already have

    auto surfType = [](const TopoDS_Face& f) -> int {
        try { return static_cast<int>(BRepAdaptor_Surface(f).GetType()); }
        catch (...) { return -1; }
    };
    std::vector<TopoDS_Shape> mapped;
    for (const auto& gf : truth) {
        gp_Pnt gc;
        if (!faceCenter(TopoDS::Face(gf), gc)) continue;
        const int gt = surfType(TopoDS::Face(gf));
        TopoDS_Shape best;
        double bestD = 1e-3; // a carried-through face keeps its centre exactly
        for (TopExp_Explorer ex(currentBody, TopAbs_FACE); ex.More(); ex.Next()) {
            const TopoDS_Face& cf = TopoDS::Face(ex.Current());
            if (surfType(cf) != gt) continue;
            gp_Pnt cc;
            if (!faceCenter(cf, cc)) continue;
            double dd = cc.Distance(gc);
            if (dd < bestD) { bestD = dd; best = cf; }
        }
        if (!best.IsNull()) mapped.push_back(best);
    }
    if (!mapped.empty()) m_generatedFaces = std::move(mapped);
}

bool ChamferOp::ownsFace(const TopoDS_Shape& face) const {
    return ownsFaceScore(face) > 0;
}

int ChamferOp::ownsFaceScore(const TopoDS_Shape& face) const {
    if (face.IsNull() || face.ShapeType() != TopAbs_FACE) return 0;
    // (No plane rejection here, unlike FilletOp: a chamfer bevel IS a flat
    // plane — rejecting planes would make chamfers un-editable.)
    for (const auto& f : m_generatedFaces) {
        if (f.IsSame(face)) return 2;   // exact identity on the live body
    }
    // Weaker geometric fallback (post-rebuild) — an exact owner wins over it (#49).
    gp_Pnt q;
    if (!faceCenter(TopoDS::Face(face), q)) return 0;
    for (const auto& f : m_generatedFaces) {
        gp_Pnt p;
        if (faceCenter(TopoDS::Face(f), p) && p.Distance(q) < 1e-4) return 1;
    }
    return 0;
}
