#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/History.h"
#include "core/Operation.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <gp_Dir.hxx>

// ─── Helpers ────────────────────────────────────────────────────────────────

static TopoDS_Shape makeTestBox(double x = 10, double y = 10, double z = 10) {
    return BRepPrimAPI_MakeBox(x, y, z).Shape();
}

static TopoDS_Shape makeRectProfile(double w, double h) {
    gp_Pnt p1(0, 0, 0), p2(w, 0, 0), p3(w, h, 0), p4(0, h, 0);
    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(p1, p2));
    wire.Add(BRepBuilderAPI_MakeEdge(p2, p3));
    wire.Add(BRepBuilderAPI_MakeEdge(p3, p4));
    wire.Add(BRepBuilderAPI_MakeEdge(p4, p1));
    return BRepBuilderAPI_MakeFace(wire.Wire()).Shape();
}

// ─── Extrude Operation (NewBody mode) ───────────────────────────────────────

class ExtrudeNewBodyOp : public Operation {
public:
    ExtrudeNewBodyOp(const TopoDS_Shape& profile, double distance)
        : m_profile(profile), m_distance(distance) {}

    bool execute(Document& doc) override {
        gp_Vec direction(0, 0, m_distance);
        BRepPrimAPI_MakePrism prism(m_profile, direction);
        if (!prism.IsDone()) return false;

        m_createdId = doc.addBody(prism.Shape(), "Extrude");
        return true;
    }

    bool undo(Document& doc) override {
        doc.removeBody(m_createdId);
        m_createdId = -1;
        return true;
    }

    std::string name() const override { return "Extrude (New Body)"; }
    std::string description() const override { return "Extrude profile into new body"; }
    void renderProperties() override {}
    std::string typeId() const override { return "extrude_new_body"; }

    int createdId() const { return m_createdId; }

private:
    TopoDS_Shape m_profile;
    double m_distance;
    int m_createdId = -1;
};

// ─── Extrude Operation (Subtract mode) ─────────────────────────────────────

class ExtrudeSubtractOp : public Operation {
public:
    ExtrudeSubtractOp(int targetBodyId, const TopoDS_Shape& profile, double distance)
        : m_targetBodyId(targetBodyId), m_profile(profile), m_distance(distance) {}

    bool execute(Document& doc) override {
        // Save the original shape for undo
        m_originalShape = doc.getBody(m_targetBodyId);

        // Create the tool (extruded profile)
        gp_Vec direction(0, 0, m_distance);
        BRepPrimAPI_MakePrism prism(m_profile, direction);
        if (!prism.IsDone()) return false;

        // Perform boolean subtraction
        BRepAlgoAPI_Cut cut(m_originalShape, prism.Shape());
        if (!cut.IsDone()) return false;

        doc.updateBody(m_targetBodyId, cut.Shape());
        return true;
    }

    bool undo(Document& doc) override {
        doc.updateBody(m_targetBodyId, m_originalShape);
        return true;
    }

    std::string name() const override { return "Extrude (Subtract)"; }
    std::string description() const override { return "Extrude cut from body"; }
    void renderProperties() override {}
    std::string typeId() const override { return "extrude_subtract"; }

private:
    int m_targetBodyId;
    TopoDS_Shape m_profile;
    double m_distance;
    TopoDS_Shape m_originalShape;
};

// ─── Fillet Operation ───────────────────────────────────────────────────────

class FilletOp : public Operation {
public:
    FilletOp(int bodyId, double radius)
        : m_bodyId(bodyId), m_radius(radius) {}

    bool execute(Document& doc) override {
        m_originalShape = doc.getBody(m_bodyId);

        BRepFilletAPI_MakeFillet fillet(m_originalShape);

        // Find and fillet the first edge
        TopExp_Explorer explorer(m_originalShape, TopAbs_EDGE);
        if (!explorer.More()) return false;

        TopoDS_Edge edge = TopoDS::Edge(explorer.Current());
        fillet.Add(m_radius, edge);

        fillet.Build();
        if (!fillet.IsDone()) return false;

        doc.updateBody(m_bodyId, fillet.Shape());
        return true;
    }

    bool undo(Document& doc) override {
        doc.updateBody(m_bodyId, m_originalShape);
        return true;
    }

    std::string name() const override { return "Fillet"; }
    std::string description() const override { return "Apply fillet to edge"; }
    void renderProperties() override {}
    std::string typeId() const override { return "fillet"; }

private:
    int m_bodyId;
    double m_radius;
    TopoDS_Shape m_originalShape;
};

// ─── Chamfer Operation ──────────────────────────────────────────────────────

class ChamferOp : public Operation {
public:
    ChamferOp(int bodyId, double distance)
        : m_bodyId(bodyId), m_distance(distance) {}

    bool execute(Document& doc) override {
        m_originalShape = doc.getBody(m_bodyId);

        BRepFilletAPI_MakeChamfer chamfer(m_originalShape);

        // Find the first edge and an adjacent face
        TopExp_Explorer edgeExplorer(m_originalShape, TopAbs_EDGE);
        if (!edgeExplorer.More()) return false;
        TopoDS_Edge edge = TopoDS::Edge(edgeExplorer.Current());

        // Find a face adjacent to this edge
        TopExp_Explorer faceExplorer(m_originalShape, TopAbs_FACE);
        if (!faceExplorer.More()) return false;
        TopoDS_Face face = TopoDS::Face(faceExplorer.Current());

        chamfer.Add(m_distance, m_distance, edge, face);

        chamfer.Build();
        if (!chamfer.IsDone()) return false;

        doc.updateBody(m_bodyId, chamfer.Shape());
        return true;
    }

    bool undo(Document& doc) override {
        doc.updateBody(m_bodyId, m_originalShape);
        return true;
    }

    std::string name() const override { return "Chamfer"; }
    std::string description() const override { return "Apply chamfer to edge"; }
    void renderProperties() override {}
    std::string typeId() const override { return "chamfer"; }

private:
    int m_bodyId;
    double m_distance;
    TopoDS_Shape m_originalShape;
};

// ─── Tests: ExtrudeNewBodyOp ────────────────────────────────────────────────

TEST(ExtrudeNewBodyTest, ExtrudeCreatesBody) {
    Document doc;
    TopoDS_Shape profile = makeRectProfile(10, 10);

    auto op = std::make_unique<ExtrudeNewBodyOp>(profile, 10.0);
    EXPECT_TRUE(op->execute(doc));
    EXPECT_EQ(doc.bodyCount(), 1);

    const TopoDS_Shape& body = doc.getBody(op->createdId());
    EXPECT_FALSE(body.IsNull());
}

TEST(ExtrudeNewBodyTest, UndoRemovesBody) {
    Document doc;
    TopoDS_Shape profile = makeRectProfile(10, 10);

    auto op = std::make_unique<ExtrudeNewBodyOp>(profile, 10.0);
    op->execute(doc);
    EXPECT_EQ(doc.bodyCount(), 1);

    op->undo(doc);
    EXPECT_EQ(doc.bodyCount(), 0);
}

TEST(ExtrudeNewBodyTest, ExtrudeViaHistory) {
    Document doc;
    History history;
    TopoDS_Shape profile = makeRectProfile(5, 8);

    auto op = std::make_unique<ExtrudeNewBodyOp>(profile, 15.0);
    EXPECT_TRUE(history.pushOperation(std::move(op), doc));
    EXPECT_EQ(doc.bodyCount(), 1);

    history.undo(doc);
    EXPECT_EQ(doc.bodyCount(), 0);

    history.redo(doc);
    EXPECT_EQ(doc.bodyCount(), 1);
}

// ─── Tests: ExtrudeSubtractOp ───────────────────────────────────────────────

TEST(ExtrudeSubtractTest, SubtractModifiesBody) {
    Document doc;

    // Create a box body first
    int boxId = doc.addBody(makeTestBox(20, 20, 20), "BaseBox");
    TopoDS_Shape originalShape = doc.getBody(boxId);

    // Create a smaller profile positioned for subtraction
    // (a small rectangle at the center of the top face)
    gp_Pnt p1(5, 5, 20), p2(15, 5, 20), p3(15, 15, 20), p4(5, 15, 20);
    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(p1, p2));
    wire.Add(BRepBuilderAPI_MakeEdge(p2, p3));
    wire.Add(BRepBuilderAPI_MakeEdge(p3, p4));
    wire.Add(BRepBuilderAPI_MakeEdge(p4, p1));
    TopoDS_Shape cutProfile = BRepBuilderAPI_MakeFace(wire.Wire()).Shape();

    auto op = std::make_unique<ExtrudeSubtractOp>(boxId, cutProfile, -10.0);
    EXPECT_TRUE(op->execute(doc));

    // Body count should remain the same (modified, not added)
    EXPECT_EQ(doc.bodyCount(), 1);

    // The shape should be different from the original
    const TopoDS_Shape& modifiedShape = doc.getBody(boxId);
    EXPECT_FALSE(modifiedShape.IsNull());
    // Shape has changed (can't do deep comparison easily, but it should not be identical)
    EXPECT_FALSE(modifiedShape.IsEqual(originalShape));
}

TEST(ExtrudeSubtractTest, UndoRestoresOriginalShape) {
    Document doc;

    int boxId = doc.addBody(makeTestBox(20, 20, 20), "BaseBox");
    TopoDS_Shape originalShape = doc.getBody(boxId);

    gp_Pnt p1(5, 5, 20), p2(15, 5, 20), p3(15, 15, 20), p4(5, 15, 20);
    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(p1, p2));
    wire.Add(BRepBuilderAPI_MakeEdge(p2, p3));
    wire.Add(BRepBuilderAPI_MakeEdge(p3, p4));
    wire.Add(BRepBuilderAPI_MakeEdge(p4, p1));
    TopoDS_Shape cutProfile = BRepBuilderAPI_MakeFace(wire.Wire()).Shape();

    auto op = std::make_unique<ExtrudeSubtractOp>(boxId, cutProfile, -10.0);
    op->execute(doc);
    op->undo(doc);

    const TopoDS_Shape& restoredShape = doc.getBody(boxId);
    EXPECT_TRUE(restoredShape.IsEqual(originalShape));
}

// ─── Tests: FilletOp ────────────────────────────────────────────────────────

TEST(FilletTest, FilletChangesShape) {
    Document doc;
    int boxId = doc.addBody(makeTestBox(20, 20, 20), "Box");
    TopoDS_Shape originalShape = doc.getBody(boxId);

    auto op = std::make_unique<FilletOp>(boxId, 1.0);
    EXPECT_TRUE(op->execute(doc));

    const TopoDS_Shape& filletedShape = doc.getBody(boxId);
    EXPECT_FALSE(filletedShape.IsNull());
    EXPECT_FALSE(filletedShape.IsEqual(originalShape));
}

TEST(FilletTest, UndoRestoresOriginal) {
    Document doc;
    int boxId = doc.addBody(makeTestBox(20, 20, 20), "Box");
    TopoDS_Shape originalShape = doc.getBody(boxId);

    auto op = std::make_unique<FilletOp>(boxId, 1.0);
    op->execute(doc);
    op->undo(doc);

    const TopoDS_Shape& restoredShape = doc.getBody(boxId);
    EXPECT_TRUE(restoredShape.IsEqual(originalShape));
}

TEST(FilletTest, FilletViaHistory) {
    Document doc;
    History history;

    int boxId = doc.addBody(makeTestBox(20, 20, 20), "Box");
    TopoDS_Shape originalShape = doc.getBody(boxId);

    auto op = std::make_unique<FilletOp>(boxId, 2.0);
    EXPECT_TRUE(history.pushOperation(std::move(op), doc));

    const TopoDS_Shape& filletedShape = doc.getBody(boxId);
    EXPECT_FALSE(filletedShape.IsEqual(originalShape));

    history.undo(doc);
    const TopoDS_Shape& restoredShape = doc.getBody(boxId);
    EXPECT_TRUE(restoredShape.IsEqual(originalShape));
}

// ─── Tests: ChamferOp ───────────────────────────────────────────────────────

TEST(ChamferTest, ChamferChangesShape) {
    Document doc;
    int boxId = doc.addBody(makeTestBox(20, 20, 20), "Box");
    TopoDS_Shape originalShape = doc.getBody(boxId);

    auto op = std::make_unique<ChamferOp>(boxId, 1.0);
    EXPECT_TRUE(op->execute(doc));

    const TopoDS_Shape& chamferedShape = doc.getBody(boxId);
    EXPECT_FALSE(chamferedShape.IsNull());
    EXPECT_FALSE(chamferedShape.IsEqual(originalShape));
}

TEST(ChamferTest, UndoRestoresOriginal) {
    Document doc;
    int boxId = doc.addBody(makeTestBox(20, 20, 20), "Box");
    TopoDS_Shape originalShape = doc.getBody(boxId);

    auto op = std::make_unique<ChamferOp>(boxId, 1.0);
    op->execute(doc);
    op->undo(doc);

    const TopoDS_Shape& restoredShape = doc.getBody(boxId);
    EXPECT_TRUE(restoredShape.IsEqual(originalShape));
}

TEST(ChamferTest, ChamferViaHistory) {
    Document doc;
    History history;

    int boxId = doc.addBody(makeTestBox(20, 20, 20), "Box");
    TopoDS_Shape originalShape = doc.getBody(boxId);

    auto op = std::make_unique<ChamferOp>(boxId, 1.5);
    EXPECT_TRUE(history.pushOperation(std::move(op), doc));

    const TopoDS_Shape& chamferedShape = doc.getBody(boxId);
    EXPECT_FALSE(chamferedShape.IsEqual(originalShape));

    history.undo(doc);
    const TopoDS_Shape& restoredShape = doc.getBody(boxId);
    EXPECT_TRUE(restoredShape.IsEqual(originalShape));
}

#include "modeling/PlaneTransformOp.h"
#include "modeling/AxisTransformOp.h"
#include <gp_Ax3.hxx>

// Plane/axis gizmo transforms must round-trip their params - without this a
// BRAND-NEW file containing one plane move reloaded with frozen steps and the
// misleading "restored from an older save" amber banner (Steve's mug).
TEST(TransformOps, PlaneAndAxisTransformParamsRoundTrip) {
    PlaneTransformOp::Entry pe;
    pe.planeId = 7;
    pe.before = gp_Pln(gp_Ax3(gp_Pnt(1, 2, 3), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    pe.after  = gp_Pln(gp_Ax3(gp_Pnt(4, 5, 6), gp_Dir(0, 1, 0), gp_Dir(0, 0, 1)));
    PlaneTransformOp p("Rotate Plane", {pe});
    PlaneTransformOp p2;
    ASSERT_TRUE(p2.deserializeParams(p.serializeParams()));
    EXPECT_EQ(p2.name(), "Rotate Plane");
    Operation::ReloadState rs;
    Document dummy;
    EXPECT_TRUE(p2.rehydrateFromReload(rs, dummy)) << "must come back EDITABLE";

    AxisTransformOp::Entry ae;
    ae.axisId = 3;
    ae.beforeOrigin = gp_Pnt(0, 0, 0); ae.beforeDir = gp_Dir(0, 0, 1);
    ae.afterOrigin  = gp_Pnt(9, 8, 7); ae.afterDir  = gp_Dir(1, 0, 0);
    AxisTransformOp a("Move Axis", {ae});
    AxisTransformOp a2;
    ASSERT_TRUE(a2.deserializeParams(a.serializeParams()));
    EXPECT_TRUE(a2.rehydrateFromReload(rs, dummy));
}

#include "modeling/ReplayOp.h"
// A reloaded SKETCH-only step (no body snapshots) must NOT be a frozen feature
// - it's inert history and shouldn't raise the amber warning (Steve: a benign
// "Remove sketch element" kept prompting it). A reloaded BODY step still is.
TEST(ReloadWarning, SketchOnlyReloadIsNotFrozenFeature) {
    ReplayOp sketchOnly("sketchedit", "Sketch Edit", "Remove sketch element",
                        {}, {}, /*fromReload=*/true);
    EXPECT_TRUE(sketchOnly.isReloaded());
    EXPECT_FALSE(sketchOnly.isFrozenFeature()) << "sketch-only reload must not warn";

    TopoDS_Shape dummy;  // a null shape is enough; the flag checks emptiness
    ReplayOp::BodyState before{{1, dummy}}, after{{1, dummy}};
    ReplayOp bodyFeature("fillet", "Fillet", "Fillet R2", before, after, true);
    EXPECT_TRUE(bodyFeature.isFrozenFeature()) << "baked body feature still warns";
}
