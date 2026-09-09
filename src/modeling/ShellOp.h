#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include "TopoName.h"
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <TopTools_ListOfShape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <string>
#include <vector>

class ShellOp : public Operation {
public:
    ShellOp();
    ~ShellOp() override = default;

    // Parameters
    void setBody(int id);
    void setThickness(double t);
    void addFaceToRemove(const TopoDS_Face& face);
    void clearFacesToRemove();

    // Getters
    int getBodyId() const { return m_bodyId; }
    double getThickness() const { return m_thickness; }

    // Distinct radii (ascending) of the body's rounded faces - cylinders and
    // tori, i.e. fillets/rounds and round holes. A shell fails outright when
    // the wall thickness nears one of these (offsetting a round by ~its own
    // radius is singular); the interactive panel uses this to explain WHY.
    static std::vector<double> roundedFaceRadii(const TopoDS_Shape& body);

    // Operation interface
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Shell"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "shell"; }
    OperationDiff captureDiff() const override;
    std::vector<int> plannedBodyIds() const override { return {m_bodyId}; }
    std::vector<TopoDS_Shape*> shapeParams() override {
        std::vector<TopoDS_Shape*> out;
        for (TopTools_ListIteratorOfListOfShape it(m_facesToRemove); it.More(); it.Next())
            out.push_back(&it.ChangeValue());
        return out;
    }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    int m_bodyId = -1;
    double m_thickness = 1.0;
    TopTools_ListOfShape m_facesToRemove;
    TopoDS_Shape m_previousShape;
    // Indices of the removed (opened) faces within the input shape's
    // canonical face map, parsed from a saved project (see SubShapeIndex.h).
    std::vector<int> m_faceIndices;

    // Geometric identity of each opened face - its outward normal and a point
    // on it - so the face can be re-found on a REGENERATED body (a sketch edit
    // upstream rebuilds the body, leaving the stored TopoDS_Face handles stale;
    // matching a raw handle would silently drop the opening and the shell would
    // vanish on the next edit). Captured on the first valid execute.
    struct FaceAnchor { gp_Dir normal; gp_Pnt point; };
    std::vector<FaceAnchor> m_faceAnchors;

    // Capture m_faceAnchors from the current m_facesToRemove (they must be
    // valid against `shape`). No-op if already captured.
    void captureFaceAnchors(const TopoDS_Shape& shape);
    // Replace any m_facesToRemove entries that are no longer sub-shapes of
    // `shape` by re-finding the best-matching face (normal + nearest plane).
    // Returns false only if an anchor can't be matched at all.
    bool rebindFaces(const TopoDS_Shape& shape);

    // Topological names of the opened faces - the robust path tried before the
    // geometric normal+point rebind. Sketch-anchored, so an opened face SURVIVES
    // a dimension edit that MOVES it (which normal+point can't follow). Captured
    // on the first valid execute; serialized additively as `facerefs=`.
    std::vector<materializr::topo::Ref> m_faceRefs;
    void captureFaceRefs(const Document& doc, const TopoDS_Shape& shape);
    // Resolve m_faceRefs against `shape`; on full success sets m_facesToRemove
    // and returns true. False -> caller falls back to rebindFaces.
    bool resolveFacesTopo(const Document& doc, const TopoDS_Shape& shape);
};
