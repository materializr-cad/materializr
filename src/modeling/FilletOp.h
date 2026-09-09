#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include "EdgeAnchor.h"
#include "GenerationLedger.h"
#include "TopoName.h"
#include <TopoDS_Shape.hxx>
#include <TopoDS_Edge.hxx>
#include <vector>
#include <string>

class FilletOp : public Operation {
public:
    FilletOp();
    ~FilletOp() override = default;

    // Parameters
    void setBody(int bodyId);
    void setEdges(const std::vector<TopoDS_Edge>& edges);
    void setRadius(double radius);
    std::vector<TopoDS_Shape*> shapeParams() override {
        std::vector<TopoDS_Shape*> out;
        for (TopoDS_Shape& s : m_edges) out.push_back(&s);
        return out;
    }

    // Generative edge tracking (experiment/generative-edges): remember which
    // SKETCH generated this body so a filleted CORNER edge can be re-found by
    // the sketch VERTEX it sits over - surviving a dimension edit that
    // relocates the corner, where ordinal/carrier matching fails. -1 = unknown
    // (falls back to today's behaviour).
    void setSourceSketch(int sketchId) { m_sourceSketchId = sketchId; }
    int  getSourceSketch() const { return m_sourceSketchId; }

    // Getters
    int getBodyId() const { return m_bodyId; }
    double getRadius() const { return m_radius; }
    const std::vector<TopoDS_Edge>& getEdges() const { return m_edges; }
    // The blend faces this fillet produced on the live body - what ownsFace
    // matches and what the history-step preview highlights.
    const std::vector<TopoDS_Shape>& getGeneratedFaces() const { return m_generatedFaces; }
    // Body shape from the last execute()'s pre-state - needed by the
    // interactive edit-by-clicking-face flow so it can preview an updated
    // radius against the body as it stood BEFORE this fillet was applied.
    const TopoDS_Shape& getPreviousShape() const { return m_previousShape; }

    // Operation interface
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Fillet"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "fillet"; }
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
    double m_radius = 1.0;
    TopoDS_Shape m_previousShape; // for undo
    // Fillet (blend) faces produced by the last execute(), so a clicked face can
    // be mapped back to this op for re-editing.
    std::vector<TopoDS_Shape> m_generatedFaces;
    // Stable lineage ids of this fillet's blend faces (FaceLineage.h).
    std::vector<int> m_genFaceIds;
    // The filleted result, captured by execute(). serializeParams indexes the
    // generated faces against it (they're sub-shapes of the result, not the
    // input); rehydrate restores it from the reload's after-state.
    TopoDS_Shape m_resultShape;
    // Sub-shape indices parsed from a saved project (see SubShapeIndex.h);
    // resolved against the before/after shapes in rehydrateFromReload.
    std::vector<int> m_edgeIndices;
    std::vector<int> m_genFaceIndices;

    // Generative anchors: one per m_edges entry, tagging the sketch feature
    // (corner vertex / rim line) each edge came from (see EdgeAnchor.h).
    int m_sourceSketchId = -1;
    std::vector<EdgeAnchor::Anchor> m_edgeAnchors;
    // Topological names of the filleted edges - the LAST-RESORT resolution
    // after rebindEdges and resolveAnchors both fail. That is exactly the
    // boolean-SEAM case (a seam edge sits over no sketch feature, so anchors
    // can't name it; its "gen" lineage name - via the producing boolean's
    // ledger published on the Document - can). Minted on the first valid
    // execute with the body's producing ledger in context.
    std::vector<materializr::topo::Ref> m_edgeRefs;

    // Lineage-FIRST edge naming (parity with ChamferOp, #52): each filleted
    // edge as its two adjacent faces' ancestry ids - resolvable from the
    // FaceIdMap alone, i.e. it survives a partial replay where the ledger
    // (runtime-only) is gone but the map was carried/restored.
    std::vector<std::pair<int,int>> m_edgeFaceIdPairs;
    // Input face lineage captured at execute; undo restores it so a PARTIAL
    // replay still has ancestry for the ops it re-runs.
    materializr::topo::FaceIdMap m_prevFaceIds;

    // Known-good builds: (input body, radius) → result (see ChamferOp's
    // StoredResult for the full story - the "put the value back" adoption).
    // Entry 0 = the loaded original, never evicted.
    struct StoredResult {
        TopoDS_Shape base, result;
        double r = -1.0;
        std::vector<TopoDS_Shape> genFaces;
    };
    std::vector<StoredResult> m_storedResults;
    void rememberResult(const TopoDS_Shape& base, const TopoDS_Shape& result);

    // Transactional edit-state rollback (see Operation::snapshotEditState).
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
        double radius = 0.0;
        bool valid = false;
    };
    EditSnap m_editSnap;

public:
    void snapshotEditState() override;
    void restoreEditState() override;

private:

    // Generation map of the last execute(): the input EDGE -> the blend FACE(S)
    // it produced. Lets the "gen" naming strategy name a blend face by the edge
    // that generated it (itself sketch-anchored, so edit-stable) - the general
    // kernel path for op-GENERATED faces no geometric scheme can name.
    materializr::topo::GenerationLedger m_ledger;

public:
    const materializr::topo::GenerationLedger& generationLedger() const {
        return m_ledger;
    }

    // Capture anchors NOW if they're missing - used to retrofit fillets loaded
    // from a project made before anchoring existed, while their edges are
    // still valid (before any edit breaks the rebind). Anchoring consults
    // every sketch in the document, so no source-sketch setup is needed.
    void ensureAnchors(Document& doc) {
        if (m_edgeAnchors.empty() && !m_edges.empty())
            computeAnchors(doc);
    }

private:
    // Capture anchors from the current m_edges + source sketch (first execute).
    void computeAnchors(Document& doc);
    // Replace m_edges by re-finding each anchor at the sketch feature's CURRENT
    // position in `base`. Returns false unless EVERY edge resolves.
    bool resolveAnchors(Document& doc, const TopoDS_Shape& base);
};
