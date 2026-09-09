#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include "EdgeAnchor.h"
#include "GenerationLedger.h"
#include "TopoName.h"
#include <TopoDS_Shape.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <vector>
#include <string>

class ChamferOp : public Operation {
public:
    ChamferOp();
    ~ChamferOp() override = default;

    // Parameters
    void setBody(int bodyId);
    void setEdges(const std::vector<TopoDS_Edge>& edges);
    void setDistance(double distance);
    std::vector<TopoDS_Shape*> shapeParams() override {
        std::vector<TopoDS_Shape*> out;
        for (TopoDS_Shape& s : m_edges) out.push_back(&s);
        return out;
    }
    // Second setback (along the OTHER face of each edge). <= 0 means symmetric:
    // both faces use setDistance(). > 0 makes an asymmetric chamfer.
    void setDistance2(double distance) { m_distance2 = distance; }

    // Generative edge tracking (see FilletOp / EdgeAnchor.h): source sketch so
    // a chamfered corner/rim edge follows a later dimension edit.
    void setSourceSketch(int sketchId) { m_sourceSketchId = sketchId; }
    int  getSourceSketch() const { return m_sourceSketchId; }

    // Getters
    int getBodyId() const { return m_bodyId; }
    double getDistance() const { return m_distance; }
    double getDistance2() const { return m_distance2; }
    bool isAsymmetric() const { return m_distance2 > 0.0; }

    // A single face adjacent to EVERY edge in `edges` (the loop's shared face),
    // or a null face if they don't all border one common face. Used as the
    // distance-1 reference so an asymmetric chamfer is consistent across a
    // multi-edge selection. For a single edge, returns one of its two faces.
    static TopoDS_Face sharedReferenceFace(const TopoDS_Shape& body,
                                           const std::vector<TopoDS_Edge>& edges);
    const std::vector<TopoDS_Edge>& getEdges() const { return m_edges; }
    // The bevel faces this chamfer produced on the live body - what ownsFace
    // matches and what the history-step preview highlights.
    const std::vector<TopoDS_Shape>& getGeneratedFaces() const { return m_generatedFaces; }
    // Body shape from the last execute()'s pre-state - used by the interactive
    // edit-by-clicking-face flow to preview an updated distance against the
    // body as it stood BEFORE this chamfer was applied.
    const TopoDS_Shape& getPreviousShape() const { return m_previousShape; }

    // Operation interface
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Chamfer"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "chamfer"; }
    bool ownsFace(const TopoDS_Shape& face) const override;
    int ownsFaceScore(const TopoDS_Shape& face) const override;
    OperationDiff captureDiff() const override;
    std::vector<int> plannedBodyIds() const override { return {m_bodyId}; }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;
    // Re-resolve generated-face indices against the body's CURRENT shape (e.g.
    // after downstream transforms moved the geometry). Called by the loader
    // after all ops are rehydrated so ownsFace() works on the final body.
    void refreshGeneratedFaces(const TopoDS_Shape& currentBody,
                               const materializr::topo::FaceIdMap* lineage = nullptr);

private:
    int m_bodyId = -1;
    std::vector<TopoDS_Edge> m_edges;
    double m_distance = 1.0;
    double m_distance2 = -1.0; // <=0 = symmetric (use m_distance for both faces)
    TopoDS_Shape m_previousShape; // for undo
    // Chamfer faces produced by the last execute(), so a clicked face can be
    // mapped back to this op for re-editing.
    std::vector<TopoDS_Shape> m_generatedFaces;
    // Result shape + parsed sub-shape indices - same reload scheme as
    // FilletOp (see SubShapeIndex.h).
    TopoDS_Shape m_resultShape;
    std::vector<int> m_edgeIndices;
    std::vector<int> m_genFaceIndices;
    // Stable lineage ids of this chamfer's bevel faces (FaceLineage.h) -
    // minted at first execute, reused on re-execute, serialized (genids=).
    std::vector<int> m_genFaceIds;
    // Deterministic replay (topo naming, #52): the asymmetric reference
    // face's lineage id, and each edge named by its two adjacent faces' ids.
    // Persisted (refid= / edgefaces=); resolved against the input body's
    // lineage map BEFORE any geometric guessing.
    int m_refFaceId = -1;
    std::vector<std::pair<int,int>> m_edgeFaceIdPairs;
    // Input face lineage captured at execute; undo restores it so a PARTIAL
    // replay (editStep starting after the map's producer) still has ancestry
    // for the ops it re-runs.
    materializr::topo::FaceIdMap m_prevFaceIds;

    // Known-good builds: (input body, params) → result, kept for the loaded
    // original plus recent in-session successes. When a REBUILD at exact
    // previously-successful values on the exact same input fails - the "put
    // it back to 15" case, where the new blend is everywhere coincident with
    // features built on the original bevel, the worst case for the boolean
    // fallback - adopt the stored result outright: identical input +
    // identical params ⇒ that stored shape IS the answer. A single slot
    // wasn't enough: a successful 16-edit overwrote the original 15 answer,
    // so "back to 15" had nothing to adopt. Bounded; entry 0 (the loaded
    // original) is never evicted. Shape handles are cheap (shared TShapes).
    struct StoredResult {
        TopoDS_Shape base, result;
        double d = -1.0, d2 = -2.0;
        std::vector<TopoDS_Shape> genFaces;
    };
    std::vector<StoredResult> m_storedResults;
    void rememberResult(const TopoDS_Shape& base, const TopoDS_Shape& result);

    // Transactional edit-state rollback (see Operation::snapshotEditState):
    // everything a doomed replay's execute() may have rewritten.
    struct EditSnap {
        std::vector<TopoDS_Edge> edges;
        std::vector<EdgeAnchor::Anchor> anchors;
        std::vector<materializr::topo::Ref> refs;
        std::vector<std::pair<int,int>> pairs;
        materializr::topo::FaceIdMap prevFaceIds;
        TopoDS_Shape previousShape, resultShape;
        std::vector<TopoDS_Shape> generatedFaces;
        std::vector<int> genFaceIds;
        std::vector<StoredResult> storedResults;
        double distance = 0.0, distance2 = -1.0;
        int refFaceId = -1;
        bool valid = false;
    };
    EditSnap m_editSnap;

public:
    void snapshotEditState() override;
    void restoreEditState() override;

private:

    // Generative anchors (EdgeAnchor.h) - same scheme as FilletOp.
    int m_sourceSketchId = -1;
    std::vector<EdgeAnchor::Anchor> m_edgeAnchors;
    // Topological names of the chamfered edges - LAST-RESORT resolution after
    // rebindEdges and resolveAnchors both fail (the boolean-SEAM case; a seam
    // sits over no sketch feature so anchors can't name it, its gen-lineage
    // ref via the body's producing ledger can). Mirrors FilletOp.
    std::vector<materializr::topo::Ref> m_edgeRefs;

    // Generation map (input edge -> chamfer bevel face) - lets the "gen"
    // naming strategy name a bevel face by its generating edge (edit-stable).
    materializr::topo::GenerationLedger m_ledger;
public:
    const materializr::topo::GenerationLedger& generationLedger() const { return m_ledger; }
private:
    void computeAnchors(Document& doc);
    bool resolveAnchors(Document& doc, const TopoDS_Shape& base);

public:
    // Retrofit anchors for a chamfer loaded from a pre-anchoring project.
    // Anchoring consults every sketch in the document (see FilletOp).
    void ensureAnchors(Document& doc) {
        if (m_edgeAnchors.empty() && !m_edges.empty())
            computeAnchors(doc);
    }
};
