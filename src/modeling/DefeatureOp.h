#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <TopTools_ListOfShape.hxx>
#include <vector>
#include <string>

// Remove selected face(s) from a body and heal the surrounding faces back
// together (OCCT BRepAlgoAPI_Defeaturing). The headline use is taking a baked
// fillet/chamfer back to a sharp edge so it can be re-applied — but it also
// cleans up unwanted rounds/holes on imported STEP geometry.
class DefeatureOp : public Operation {
public:
    DefeatureOp();
    ~DefeatureOp() override = default;

    void setBody(int id);
    void addFace(const TopoDS_Face& face);
    void clearFaces();

    int getBodyId() const { return m_bodyId; }

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Remove Feature"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "defeature"; }
    OperationDiff captureDiff() const override;
    std::vector<int> plannedBodyIds() const override { return {m_bodyId}; }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    int m_bodyId = -1;
    TopTools_ListOfShape m_faces;
    TopoDS_Shape m_previousShape;
    // Removed faces persist as ordinal indices into the INPUT shape's canonical
    // face map (see SubShapeIndex.h) so the step reloads as a real editable op.
    std::vector<int> m_faceIndices;
};
