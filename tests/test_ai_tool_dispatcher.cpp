#include "ai/AiToolDispatcher.h"
#include "core/Document.h"
#include "core/History.h"
#include "core/SelectionManager.h"
#include "plugin/PluginContext.h"

#include <gtest/gtest.h>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

using namespace materializr::ai;
using materializr::PluginContext;

namespace {
// A PluginContext with just enough bound to run executeTool: Document +
// History, plus an optional SelectionManager for the get_selection tests -
// every other test leaves it null, matching the old behaviour exactly.
PluginContext makeCtx(Document& doc, History& hist, SelectionManager* sel = nullptr) {
    PluginContext ctx;
    ctx._bind(&doc, &hist, sel, nullptr, nullptr, nullptr, nullptr, nullptr);
    return ctx;
}
double bboxSizeX(Document& doc, int bodyId) {
    Bnd_Box box;
    BRepBndLib::Add(doc.getBody(bodyId), box);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    return x1 - x0;
}
// PrimitiveOp's worldPnt maps user Z (up) -> world Y and user Y (depth) ->
// world Z, so a box's world-space Y extent is its HEIGHT and Z extent its DEPTH.
void bboxWorldYZ(Document& doc, int bodyId, double& sizeY, double& sizeZ) {
    Bnd_Box box;
    BRepBndLib::Add(doc.getBody(bodyId), box);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    sizeY = y1 - y0;
    sizeZ = z1 - z0;
}
void bboxWorldOrigin(Document& doc, int bodyId, double& x, double& y, double& z) {
    Bnd_Box box;
    BRepBndLib::Add(doc.getBody(bodyId), box);
    double x1, y1, z1;
    box.Get(x, y, z, x1, y1, z1);
}
double volumeOf(Document& doc, int bodyId) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(doc.getBody(bodyId), g);
    return g.Mass();
}
int faceCountOf(Document& doc, int bodyId) {
    int n = 0;
    for (TopExp_Explorer ex(doc.getBody(bodyId), TopAbs_FACE); ex.More(); ex.Next()) ++n;
    return n;
}
} // namespace

TEST(AiToolDispatcher, AddBoxCreatesABodyWithTheGivenDimensions) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    nlohmann::json args = {{"width", 20.0}, {"height", 15.0}, {"depth", 10.0}};
    ToolResult r = executeTool(ctx, "add_box", args);

    ASSERT_TRUE(r.ok) << r.message;
    ASSERT_EQ(doc.getAllBodyIds().size(), 1u);
    int id = doc.getAllBodyIds().front();
    EXPECT_NEAR(bboxSizeX(doc, id), 20.0, 1e-6);
}

TEST(AiToolDispatcher, AddBoxDefaultsPositionToTheOrigin) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    nlohmann::json args = {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}};
    ToolResult r = executeTool(ctx, "add_box", args);
    ASSERT_TRUE(r.ok) << r.message;

    Bnd_Box box;
    BRepBndLib::Add(doc.getBody(doc.getAllBodyIds().front()), box);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(x0, 0.0, 1e-6);
    EXPECT_NEAR(y0, 0.0, 1e-6);
    EXPECT_NEAR(z0, 0.0, 1e-6);
}

TEST(AiToolDispatcher, RejectsANonPositiveBoxDimensionWithoutTouchingTheDocument) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    nlohmann::json args = {{"width", -5.0}, {"height", 10.0}, {"depth", 10.0}};
    ToolResult r = executeTool(ctx, "add_box", args);

    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.message.empty());
    EXPECT_TRUE(doc.getAllBodyIds().empty())
        << "a rejected tool call must not create a body";
}

TEST(AiToolDispatcher, MoveBodyRejectsAnUnknownBodyId) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    nlohmann::json args = {{"body_id", 999}, {"dx", 1.0}, {"dy", 0.0}, {"dz", 0.0}};
    ToolResult r = executeTool(ctx, "move_body", args);
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, BooleanOpRejectsAnUnknownMode) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    ToolResult a = executeTool(ctx, "add_box", {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}});
    ToolResult b = executeTool(ctx, "add_box", {{"width", 5.0}, {"height", 5.0}, {"depth", 5.0}});
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    auto ids = doc.getAllBodyIds();
    ASSERT_EQ(ids.size(), 2u);

    nlohmann::json args = {{"target_body_id", ids[0]}, {"tool_body_id", ids[1]}, {"mode", "explode"}};
    ToolResult r = executeTool(ctx, "boolean_op", args);
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, BooleanOpUnionMergesTwoBodiesIntoOne) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    ToolResult a = executeTool(ctx, "add_box", {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}});
    ToolResult b = executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}, {"x", 5.0}});
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    auto ids = doc.getAllBodyIds();
    ASSERT_EQ(ids.size(), 2u);

    ToolResult r = executeTool(ctx, "boolean_op",
        {{"target_body_id", ids[0]}, {"tool_body_id", ids[1]}, {"mode", "union"}});
    EXPECT_TRUE(r.ok) << r.message;
    EXPECT_EQ(doc.getAllBodyIds().size(), 1u)
        << "the tool body must be consumed by a union";
}

TEST(AiToolDispatcher, DeleteBodyRemovesItFromTheDocument) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box", {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int id = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "delete_body", {{"body_id", id}});
    EXPECT_TRUE(r.ok) << r.message;
    EXPECT_TRUE(doc.getAllBodyIds().empty());
}

TEST(AiToolDispatcher, DuplicateBodyCreatesASecondBodyOffsetFromTheOriginal) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box", {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int original = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "duplicate_body", {{"body_id", original}});
    ASSERT_TRUE(r.ok) << r.message;
    ASSERT_EQ(doc.getAllBodyIds().size(), 2u);
    int copy = doc.getAllBodyIds().back();

    Bnd_Box box;
    BRepBndLib::Add(doc.getBody(copy), box);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    // Default offset is dx=20 (user X, unaffected by the Y/Z swap) - the
    // original sat at world x0=0, so the copy should land at world x0=20.
    EXPECT_NEAR(x0, 20.0, 1e-6);
    EXPECT_NE(copy, original);
}

TEST(AiToolDispatcher, MirrorBodyCreatesANewMirroredCopyByDefault) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}, {"x", 10.0}}).ok);
    int original = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "mirror_body", {{"body_id", original}, {"axis", "+x"}});
    ASSERT_TRUE(r.ok) << r.message;
    ASSERT_EQ(doc.getAllBodyIds().size(), 2u) << "keep_original defaults to true";
    int mirrored = doc.getAllBodyIds().back();

    Bnd_Box box;
    BRepBndLib::Add(doc.getBody(mirrored), box);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    // Original spans world x [10,20]; mirrored across the YZ plane (x=0)
    // should span [-20,-10].
    EXPECT_NEAR(x0, -20.0, 1e-6);
    EXPECT_NEAR(x1, -10.0, 1e-6);
}

TEST(AiToolDispatcher, MirrorBodyCanReplaceTheOriginalInPlace) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}, {"x", 10.0}}).ok);
    int original = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "mirror_body",
        {{"body_id", original}, {"axis", "+x"}, {"keep_original", false}});
    ASSERT_TRUE(r.ok) << r.message;
    ASSERT_EQ(doc.getAllBodyIds().size(), 1u) << "no new body when keep_original is false";

    Bnd_Box box;
    BRepBndLib::Add(doc.getBody(original), box);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(x0, -20.0, 1e-6);
}

TEST(AiToolDispatcher, MirrorBodyRejectsAnUnknownAxis) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box", {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int id = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "mirror_body", {{"body_id", id}, {"axis", "diagonal"}});
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, PatternBodyLinearCreatesTheRequestedNumberOfCopiesIncludingTheOriginal) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box", {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int id = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "pattern_body",
        {{"body_id", id}, {"type", "linear"}, {"count", 3},
         {"spacing_x", 15.0}, {"spacing_y", 0.0}, {"spacing_z", 0.0}});
    EXPECT_TRUE(r.ok) << r.message;
    EXPECT_EQ(doc.getAllBodyIds().size(), 3u);
}

TEST(AiToolDispatcher, PatternBodyRadialCreatesTheRequestedNumberOfCopiesIncludingTheOriginal) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box", {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int id = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "pattern_body",
        {{"body_id", id}, {"type", "radial"}, {"count", 4},
         {"axis_x", 0.0}, {"axis_y", 0.0}, {"axis_z", 1.0}});
    EXPECT_TRUE(r.ok) << r.message;
    EXPECT_EQ(doc.getAllBodyIds().size(), 4u);
}

TEST(AiToolDispatcher, PatternBodyRejectsAnUnknownType) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box", {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int id = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "pattern_body",
        {{"body_id", id}, {"type", "zigzag"}, {"count", 3}});
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, UnknownToolNameIsRejected) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ToolResult r = executeTool(ctx, "delete_universe", {});
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, AddBoxWithDifferentHeightAndDepthLandsOnTheCorrectWorldAxes) {
    // Regression for the height/depth swap: setBoxExtents(x,y,z) is W/D/H, so
    // the dispatcher must call it as (w, d, h), not (w, h, d).
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    nlohmann::json args = {{"width", 5.0}, {"height", 20.0}, {"depth", 30.0}};
    ToolResult r = executeTool(ctx, "add_box", args);
    ASSERT_TRUE(r.ok) << r.message;

    double sizeY, sizeZ;
    bboxWorldYZ(doc, doc.getAllBodyIds().front(), sizeY, sizeZ);
    EXPECT_NEAR(sizeY, 20.0, 1e-6) << "world Y must be the requested height";
    EXPECT_NEAR(sizeZ, 30.0, 1e-6) << "world Z must be the requested depth";
}

TEST(AiToolDispatcher, MoveBodyRoundTripsBackToTheOriginalPosition) {
    // add_sphere and move_body must agree on which axis is "up" vs "depth".
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    nlohmann::json addArgs = {{"radius", 2.0}, {"x", 1.0}, {"y", 10.0}, {"z", 3.0}};
    ToolResult added = executeTool(ctx, "add_sphere", addArgs);
    ASSERT_TRUE(added.ok) << added.message;
    int id = doc.getAllBodyIds().front();

    double ox0, oy0, oz0;
    bboxWorldOrigin(doc, id, ox0, oy0, oz0);

    nlohmann::json moveArgs = {{"body_id", id}, {"dx", 4.0}, {"dy", -6.0}, {"dz", 2.0}};
    ToolResult moved = executeTool(ctx, "move_body", moveArgs);
    ASSERT_TRUE(moved.ok) << moved.message;

    // Sanity-check the intermediate position uses the same user->world
    // convention as add_sphere: world = (ox+dx, oz+dz, oy+dy).
    double oxm, oym, ozm;
    bboxWorldOrigin(doc, id, oxm, oym, ozm);
    EXPECT_NEAR(oxm, ox0 + 4.0, 1e-6);
    EXPECT_NEAR(oym, oy0 + 2.0, 1e-6);
    EXPECT_NEAR(ozm, oz0 - 6.0, 1e-6);

    nlohmann::json undoArgs = {{"body_id", id}, {"dx", -4.0}, {"dy", 6.0}, {"dz", -2.0}};
    ToolResult undone = executeTool(ctx, "move_body", undoArgs);
    ASSERT_TRUE(undone.ok) << undone.message;

    double ox1, oy1, oz1;
    bboxWorldOrigin(doc, id, ox1, oy1, oz1);
    EXPECT_NEAR(ox1, ox0, 1e-6);
    EXPECT_NEAR(oy1, oy0, 1e-6);
    EXPECT_NEAR(oz1, oz0, 1e-6);
}

TEST(AiToolDispatcher, MoveBodyRejectsAFractionalBodyId) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 1.0}, {"height", 1.0}, {"depth", 1.0}}).ok);

    nlohmann::json args = {{"body_id", 1.9}, {"dx", 0.0}, {"dy", 0.0}, {"dz", 0.0}};
    ToolResult r = executeTool(ctx, "move_body", args);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.message.find("whole number"), std::string::npos) << r.message;
}

TEST(AiToolDispatcher, MoveBodyRejectsAnOutOfRangeBodyId) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    nlohmann::json args = {{"body_id", 1e20}, {"dx", 0.0}, {"dy", 0.0}, {"dz", 0.0}};
    ToolResult r = executeTool(ctx, "move_body", args);
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, RotateBodyAboutUpAxisMatchesTheHandedTransformOpRotation) {
    // rotate_body swaps axis_y/axis_z (op->setRotation(ax, az, ay, -angle))
    // to match the user->world axis convention, and negates the angle to
    // undo the reflection that swap introduces (see rotateBody's comment).
    // With axis_z=1 the world rotation axis is (0,1,0); TransformOp applies
    // gp_Trsf::SetRotation(axis, angleRad) which for axis Y implements the
    // standard right-hand rotation matrix x'=x*cos(t)+z*sin(t),
    // z'=-x*sin(t)+z*cos(t). Here t = -angle_degrees (in radians), so a
    // world point (x,y,z) maps to (-z,y,x) for a 90 degree request.
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    nlohmann::json addArgs = {{"width", 2.0}, {"height", 2.0}, {"depth", 2.0}, {"x", 10.0}};
    ASSERT_TRUE(executeTool(ctx, "add_box", addArgs).ok);
    int id = doc.getAllBodyIds().front();

    nlohmann::json rotateArgs = {{"body_id", id}, {"axis_x", 0.0}, {"axis_y", 0.0},
                                 {"axis_z", 1.0}, {"angle_degrees", 90.0}};
    ToolResult r = executeTool(ctx, "rotate_body", rotateArgs);
    ASSERT_TRUE(r.ok) << r.message;

    double x, y, z;
    bboxWorldOrigin(doc, id, x, y, z);
    EXPECT_NEAR(x, -2.0, 1e-6);
    EXPECT_NEAR(y, 0.0, 1e-6);
    EXPECT_NEAR(z, 10.0, 1e-6);
}

TEST(AiToolDispatcher, AddBoxRejectsANonNumericOptionalPosition) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    nlohmann::json args = {{"width", 5.0}, {"height", 5.0}, {"depth", 5.0}, {"x", "100"}};
    ToolResult r = executeTool(ctx, "add_box", args);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(doc.getAllBodyIds().empty())
        << "a malformed optional argument must not silently default";
}

TEST(AiToolDispatcher, FilletAllEdgesRoundsABoxAndRemovesVolume) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 20.0}, {"height", 20.0}, {"depth", 20.0}}).ok);
    int id = doc.getAllBodyIds().front();
    const double v0 = volumeOf(doc, id);
    const int f0 = faceCountOf(doc, id);

    ToolResult r = executeTool(ctx, "fillet_all_edges", {{"body_id", id}, {"radius", 2.0}});
    ASSERT_TRUE(r.ok) << r.message;
    EXPECT_LT(volumeOf(doc, id), v0) << "rounding a box's edges must remove material";
    EXPECT_GT(faceCountOf(doc, id), f0) << "every rounded edge adds a blend face";
}

TEST(AiToolDispatcher, FilletAllEdgesRejectsANonPositiveRadius) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int id = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "fillet_all_edges", {{"body_id", id}, {"radius", 0.0}});
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, FilletAllEdgesRejectsARadiusTooLargeForTheBody) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 4.0}, {"height", 4.0}, {"depth", 4.0}}).ok);
    int id = doc.getAllBodyIds().front();
    const double v0 = volumeOf(doc, id);

    // A radius bigger than half the smallest edge can't fit - OCCT either
    // fails outright or (worse, if unchecked) silently produces something
    // degenerate. Either way the document must not end up mutated on failure.
    ToolResult r = executeTool(ctx, "fillet_all_edges", {{"body_id", id}, {"radius", 50.0}});
    if (r.ok) {
        // Some OCCT versions clamp rather than fail; if it succeeded the
        // volume must still be sane (not collapsed to ~0 or negative).
        EXPECT_GT(volumeOf(doc, id), 0.0);
    } else {
        EXPECT_NEAR(volumeOf(doc, id), v0, 1e-6)
            << "a refused fillet must leave the body unchanged";
    }
}

TEST(AiToolDispatcher, ChamferAllEdgesBevelsABoxAndRemovesVolume) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 20.0}, {"height", 20.0}, {"depth", 20.0}}).ok);
    int id = doc.getAllBodyIds().front();
    const double v0 = volumeOf(doc, id);
    const int f0 = faceCountOf(doc, id);

    ToolResult r = executeTool(ctx, "chamfer_all_edges", {{"body_id", id}, {"distance", 2.0}});
    ASSERT_TRUE(r.ok) << r.message;
    EXPECT_LT(volumeOf(doc, id), v0) << "bevelling a box's edges must remove material";
    EXPECT_GT(faceCountOf(doc, id), f0) << "every chamfered edge adds a bevel face";
}

TEST(AiToolDispatcher, ChamferAllEdgesRejectsANonPositiveDistance) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int id = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "chamfer_all_edges", {{"body_id", id}, {"distance", -1.0}});
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, FilletEdgeRoundsOnlyTheNearestEdge) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 20.0}, {"height", 20.0}, {"depth", 20.0}}).ok);
    int id = doc.getAllBodyIds().front();
    const double v0 = volumeOf(doc, id);
    const int f0 = faceCountOf(doc, id);

    // A point near one corner - the top-front-right vertical edge, in the
    // same user X/Y/Z convention add_box uses.
    ToolResult r = executeTool(ctx, "fillet_edge",
        {{"body_id", id}, {"radius", 2.0}, {"x", 20.0}, {"y", 20.0}, {"z", 10.0}});
    ASSERT_TRUE(r.ok) << r.message;
    EXPECT_LT(volumeOf(doc, id), v0) << "rounding an edge must remove some material";
    EXPECT_EQ(faceCountOf(doc, id), f0 + 1)
        << "exactly one edge rounded should add exactly one blend face";
}

TEST(AiToolDispatcher, FilletEdgeReportsTheActualEdgeLocationFound) {
    // Steve reported the model "seeing" a body via list_bodies but failing
    // to fillet it - nearestEdge never reports "no edge close enough" (it
    // always returns SOMETHING), so a Y/Z-swapped point silently rounds the
    // wrong edge instead of failing where the mistake would be obvious. The
    // message reporting back where the edge ACTUALLY was is the fix: a
    // point dead-on this edge's midpoint must echo that same point back.
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 20.0}, {"height", 20.0}, {"depth", 20.0}}).ok);
    int id = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "fillet_edge",
        {{"body_id", id}, {"radius", 2.0}, {"x", 20.0}, {"y", 20.0}, {"z", 10.0}});
    ASSERT_TRUE(r.ok) << r.message;
    EXPECT_NE(r.message.find("edge found at"), std::string::npos) << r.message;
    EXPECT_NE(r.message.find("(20.0, 20.0, 10.0)"), std::string::npos) << r.message;
}

TEST(AiToolDispatcher, FilletEdgeRoundsALessVolumeThanFilletAllEdges) {
    // Same box, same radius: one edge should remove much less material than
    // all twelve - a cheap sanity check that only one edge was actually
    // touched, not silently all of them.
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 20.0}, {"height", 20.0}, {"depth", 20.0}}).ok);
    int oneEdgeId = doc.getAllBodyIds().front();
    const double v0 = volumeOf(doc, oneEdgeId);
    ASSERT_TRUE(executeTool(ctx, "fillet_edge",
        {{"body_id", oneEdgeId}, {"radius", 2.0}, {"x", 20.0}, {"y", 20.0}, {"z", 10.0}}).ok);
    const double removedByOne = v0 - volumeOf(doc, oneEdgeId);

    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 20.0}, {"height", 20.0}, {"depth", 20.0}, {"x", 50.0}}).ok);
    int allEdgesId = doc.getAllBodyIds().back();
    ASSERT_TRUE(executeTool(ctx, "fillet_all_edges",
        {{"body_id", allEdgesId}, {"radius", 2.0}}).ok);
    const double removedByAll = v0 - volumeOf(doc, allEdgesId);

    EXPECT_LT(removedByOne, removedByAll * 0.5);
}

TEST(AiToolDispatcher, FilletEdgeRejectsANonPositiveRadius) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int id = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "fillet_edge",
        {{"body_id", id}, {"radius", 0.0}, {"x", 0.0}, {"y", 0.0}, {"z", 0.0}});
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, ChamferEdgeBevelsOnlyTheNearestEdge) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 20.0}, {"height", 20.0}, {"depth", 20.0}}).ok);
    int id = doc.getAllBodyIds().front();
    const double v0 = volumeOf(doc, id);
    const int f0 = faceCountOf(doc, id);

    ToolResult r = executeTool(ctx, "chamfer_edge",
        {{"body_id", id}, {"distance", 2.0}, {"x", 20.0}, {"y", 20.0}, {"z", 10.0}});
    ASSERT_TRUE(r.ok) << r.message;
    EXPECT_LT(volumeOf(doc, id), v0) << "bevelling an edge must remove some material";
    EXPECT_EQ(faceCountOf(doc, id), f0 + 1)
        << "exactly one edge chamfered should add exactly one bevel face";
}

TEST(AiToolDispatcher, ChamferEdgeRejectsANonPositiveDistance) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int id = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "chamfer_edge",
        {{"body_id", id}, {"distance", -1.0}, {"x", 0.0}, {"y", 0.0}, {"z", 0.0}});
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, ExtrudeRectCreatesANewBodyWithTheGivenVolume) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    ToolResult r = executeTool(ctx, "extrude_rect",
        {{"width", 10.0}, {"depth", 5.0}, {"distance", 20.0}});
    ASSERT_TRUE(r.ok) << r.message;
    ASSERT_EQ(doc.getAllBodyIds().size(), 1u);
    EXPECT_NEAR(volumeOf(doc, doc.getAllBodyIds().front()), 10.0 * 5.0 * 20.0, 1e-3);
}

TEST(AiToolDispatcher, ExtrudeRectSubtractModeCutsAPocketIntoAnExistingBody) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 20.0}, {"height", 20.0}, {"depth", 20.0}}).ok);
    int id = doc.getAllBodyIds().front();
    const double v0 = volumeOf(doc, id);

    // A 4x4x10 pocket straight down from above the box's centre.
    ToolResult r = executeTool(ctx, "extrude_rect",
        {{"width", 4.0}, {"depth", 4.0}, {"distance", -10.0},
         {"x", 10.0}, {"y", 10.0}, {"z", 25.0},
         {"mode", "subtract"}, {"target_body_id", id}});
    ASSERT_TRUE(r.ok) << r.message;
    EXPECT_EQ(doc.getAllBodyIds().size(), 1u) << "subtract must not create a new body";
    EXPECT_LT(volumeOf(doc, id), v0) << "a subtract-mode extrude must remove material";
}

TEST(AiToolDispatcher, ExtrudeRectRejectsAModeWithoutATargetBodyId) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);

    ToolResult r = executeTool(ctx, "extrude_rect",
        {{"width", 2.0}, {"depth", 2.0}, {"distance", 5.0}, {"mode", "subtract"}});
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, ExtrudeCircleAlongACustomDirectionExtendsAlongThatAxis) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    // dir_x=1 (all else default 0) -> world X, not the default up axis.
    ToolResult r = executeTool(ctx, "extrude_circle",
        {{"radius", 2.0}, {"distance", 30.0}, {"dir_x", 1.0}, {"dir_y", 0.0}, {"dir_z", 0.0}});
    ASSERT_TRUE(r.ok) << r.message;
    int id = doc.getAllBodyIds().front();

    Bnd_Box box;
    BRepBndLib::Add(doc.getBody(id), box);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(x1 - x0, 30.0, 1e-3) << "world X extent must match the extrude distance";
    EXPECT_NEAR(y1 - y0, 4.0, 1e-3) << "world Y/Z extents must match the circle's diameter";
    EXPECT_NEAR(z1 - z0, 4.0, 1e-3);
}

TEST(AiToolDispatcher, ExtrudeRectRejectsAZeroDistance) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ToolResult r = executeTool(ctx, "extrude_rect",
        {{"width", 5.0}, {"depth", 5.0}, {"distance", 0.0}});
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, ExtrudePolygonCreatesANewBodyWithTheGivenCrossSectionArea) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    // Right triangle (0,0)-(10,0)-(0,10): area 50, extruded 4mm -> volume 200.
    ToolResult r = executeTool(ctx, "extrude_polygon",
        {{"points", "[[0,0],[10,0],[0,10]]"}, {"distance", 4.0}});
    ASSERT_TRUE(r.ok) << r.message;
    ASSERT_EQ(doc.getAllBodyIds().size(), 1u);
    EXPECT_NEAR(volumeOf(doc, doc.getAllBodyIds().front()), 50.0 * 4.0, 1e-3);
}

TEST(AiToolDispatcher, ExtrudePolygonRejectsFewerThanThreePoints) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ToolResult r = executeTool(ctx, "extrude_polygon",
        {{"points", "[[0,0],[10,0]]"}, {"distance", 4.0}});
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, ExtrudePolygonRejectsMalformedJson) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ToolResult r = executeTool(ctx, "extrude_polygon",
        {{"points", "not json"}, {"distance", 4.0}});
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, LoftBodiesCreatesANewBodyConnectingTwoOtherBodies) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    // A 10x10x10 box at the origin, and a smaller 4x4x4 box floating above it.
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 4.0}, {"height", 4.0}, {"depth", 4.0},
         {"x", 3.0}, {"y", 3.0}, {"z", 20.0}}).ok);
    auto ids = doc.getAllBodyIds();
    ASSERT_EQ(ids.size(), 2u);

    ToolResult r = executeTool(ctx, "loft_bodies",
        {{"from_body_id", ids[0]}, {"from_face", "+z"},
         {"to_body_id", ids[1]}, {"to_face", "-z"}});
    ASSERT_TRUE(r.ok) << r.message;
    ASSERT_EQ(doc.getAllBodyIds().size(), 3u) << "loft must add a new body, not consume either source";
    int loftId = doc.getAllBodyIds().back();
    EXPECT_GT(volumeOf(doc, loftId), 0.0);
}

TEST(AiToolDispatcher, LoftBodiesRejectsTheSameBodyForBothEnds) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box", {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int id = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "loft_bodies",
        {{"from_body_id", id}, {"from_face", "+z"}, {"to_body_id", id}, {"to_face", "-z"}});
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, PushPullFacePositiveDistanceAddsMaterial) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 20.0}, {"height", 20.0}, {"depth", 20.0}}).ok);
    int id = doc.getAllBodyIds().front();
    const double v0 = volumeOf(doc, id);

    // (10, 20, 10) in user space -> world (10, 10, 20): the centre of the
    // face at world Z=20, one of the cube's six faces, unambiguously.
    ToolResult r = executeTool(ctx, "push_pull_face",
        {{"body_id", id}, {"distance", 5.0}, {"x", 10.0}, {"y", 20.0}, {"z", 10.0}});
    ASSERT_TRUE(r.ok) << r.message;
    EXPECT_GT(volumeOf(doc, id), v0);
}

TEST(AiToolDispatcher, PushPullFaceNegativeDistanceRemovesMaterial) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 20.0}, {"height", 20.0}, {"depth", 20.0}}).ok);
    int id = doc.getAllBodyIds().front();
    const double v0 = volumeOf(doc, id);

    ToolResult r = executeTool(ctx, "push_pull_face",
        {{"body_id", id}, {"distance", -5.0}, {"x", 10.0}, {"y", 20.0}, {"z", 10.0}});
    ASSERT_TRUE(r.ok) << r.message;
    EXPECT_LT(volumeOf(doc, id), v0);
}

TEST(AiToolDispatcher, PushPullFaceRejectsAZeroDistance) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int id = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "push_pull_face",
        {{"body_id", id}, {"distance", 0.0}, {"x", 5.0}, {"y", 10.0}, {"z", 5.0}});
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, ShellBodyWithNoOpenFaceHollowsOutMostOfTheVolume) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 20.0}, {"height", 20.0}, {"depth", 20.0}}).ok);
    int id = doc.getAllBodyIds().front();
    const double v0 = volumeOf(doc, id);

    ToolResult r = executeTool(ctx, "shell_body",
        {{"body_id", id}, {"thickness", 2.0}, {"open_face", "none"}});
    ASSERT_TRUE(r.ok) << r.message;
    // A 20mm cube shelled to 2mm walls leaves a 16mm^3 cavity: most of the
    // original 8000mm^3 is gone.
    EXPECT_LT(volumeOf(doc, id), v0 * 0.6);
    EXPECT_GT(volumeOf(doc, id), 0.0);
}

TEST(AiToolDispatcher, ShellBodyDefaultsToAFullyClosedShellWhenOpenFaceIsOmitted) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 20.0}, {"height", 20.0}, {"depth", 20.0}}).ok);
    int id = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "shell_body", {{"body_id", id}, {"thickness", 2.0}});
    EXPECT_TRUE(r.ok) << r.message;
}

TEST(AiToolDispatcher, ShellBodyWithAnOpenFaceStillHollowsTheBody) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 20.0}, {"height", 20.0}, {"depth", 20.0}}).ok);
    int id = doc.getAllBodyIds().front();
    const double v0 = volumeOf(doc, id);

    ToolResult r = executeTool(ctx, "shell_body",
        {{"body_id", id}, {"thickness", 2.0}, {"open_face", "+z"}});
    ASSERT_TRUE(r.ok) << r.message;
    EXPECT_LT(volumeOf(doc, id), v0 * 0.6);
}

TEST(AiToolDispatcher, ShellBodyRejectsAnInvalidOpenFaceString) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int id = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "shell_body",
        {{"body_id", id}, {"thickness", 1.0}, {"open_face", "sideways"}});
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, ShellBodyRejectsANonPositiveThickness) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int id = doc.getAllBodyIds().front();

    ToolResult r = executeTool(ctx, "shell_body", {{"body_id", id}, {"thickness", 0.0}});
    EXPECT_FALSE(r.ok);
}

TEST(AiToolDispatcher, CaptureViewFailsCleanlyWithNoCaptureCallbackBound) {
    // makeCtx() leaves the capture callback unset - the shape a real
    // Application always binds one, but a dispatcher call before that (or a
    // future headless caller) must fail cleanly, not crash.
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    ToolResult r = executeTool(ctx, "capture_view", {});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.imagePng.empty());
}

TEST(AiToolDispatcher, CaptureViewReturnsTheImageBytesFromTheBoundCallback) {
    Document doc;
    History hist;
    PluginContext ctx;
    ctx._bind(&doc, &hist, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {},
             [](std::vector<uint8_t>& out) {
                 out = {0x89, 'P', 'N', 'G'};
                 return true;
             });

    ToolResult r = executeTool(ctx, "capture_view", {});
    ASSERT_TRUE(r.ok) << r.message;
    EXPECT_EQ(r.imagePng, (std::vector<uint8_t>{0x89, 'P', 'N', 'G'}));
}

TEST(AiToolDispatcher, ListBodiesReportsAnEmptyDocumentCleanly) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    ToolResult r = executeTool(ctx, "list_bodies", {});
    ASSERT_TRUE(r.ok);
    EXPECT_NE(r.message.find("No bodies"), std::string::npos);
}

TEST(AiToolDispatcher, ListBodiesReportsIdNameAndPositionForEachBody) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 20.0}, {"depth", 30.0},
         {"x", 5.0}, {"y", 6.0}, {"z", 7.0}}).ok);
    int id = doc.getAllBodyIds().front();
    doc.setBodyName(id, "fuselage");

    ToolResult r = executeTool(ctx, "list_bodies", {});
    ASSERT_TRUE(r.ok);
    EXPECT_NE(r.message.find("id " + std::to_string(id)), std::string::npos);
    EXPECT_NE(r.message.find("\"fuselage\""), std::string::npos);
    // add_box's x/y/z is the corner, not the center - the reported centre
    // must reflect that (corner + half the extent along the matching axis),
    // not just echo the box's own creation args back unchanged.
    EXPECT_NE(r.message.find("x=10.0"), std::string::npos)
        << r.message; // 5 (corner) + 10/2 (half width)
}

TEST(AiToolDispatcher, ListBodiesReportsMultipleBodies) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    ASSERT_TRUE(executeTool(ctx, "add_sphere", {{"radius", 5.0}}).ok);

    ToolResult r = executeTool(ctx, "list_bodies", {});
    ASSERT_TRUE(r.ok);
    for (int id : doc.getAllBodyIds())
        EXPECT_NE(r.message.find("id " + std::to_string(id)), std::string::npos);
}

TEST(AiToolDispatcher, GetSelectionReportsNothingSelectedWhenEmpty) {
    Document doc;
    History hist;
    SelectionManager sel;
    PluginContext ctx = makeCtx(doc, hist, &sel);

    ToolResult r = executeTool(ctx, "get_selection", {});
    ASSERT_TRUE(r.ok);
    EXPECT_NE(r.message.find("Nothing is currently selected"), std::string::npos);
}

TEST(AiToolDispatcher, GetSelectionReportsASelectedBody) {
    Document doc;
    History hist;
    SelectionManager sel;
    PluginContext ctx = makeCtx(doc, hist, &sel);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int id = doc.getAllBodyIds().front();
    doc.setBodyName(id, "fuselage");

    SelectionEntry entry;
    entry.type = SelectionType::Body;
    entry.bodyId = id;
    sel.select(entry);

    ToolResult r = executeTool(ctx, "get_selection", {});
    ASSERT_TRUE(r.ok);
    EXPECT_NE(r.message.find("BODY"), std::string::npos);
    EXPECT_NE(r.message.find("id " + std::to_string(id)), std::string::npos);
    EXPECT_NE(r.message.find("\"fuselage\""), std::string::npos);
}

TEST(AiToolDispatcher, GetSelectionReportsASelectedFaceAsATargetablePoint) {
    Document doc;
    History hist;
    SelectionManager sel;
    PluginContext ctx = makeCtx(doc, hist, &sel);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int id = doc.getAllBodyIds().front();

    TopExp_Explorer ex(doc.getBody(id), TopAbs_FACE);
    ASSERT_TRUE(ex.More());
    SelectionEntry entry;
    entry.type = SelectionType::Face;
    entry.bodyId = id;
    // TopoDS::Face returns a reference; assigning the call expression
    // directly into TopoDS_Shape hits an OCCT operator= overload-resolution
    // trap (SFINAE picks the templated overload's deduced reference type,
    // then fails the base-class conversion) - going through a named local
    // of the concrete type first, same as the app's own selection code
    // (Application_Viewport.cpp), sidesteps it.
    TopoDS_Face face = TopoDS::Face(ex.Current());
    entry.shape = face;
    sel.select(entry);

    ToolResult r = executeTool(ctx, "get_selection", {});
    ASSERT_TRUE(r.ok);
    EXPECT_NE(r.message.find("FACE"), std::string::npos);
    EXPECT_NE(r.message.find("id " + std::to_string(id)), std::string::npos);
    EXPECT_NE(r.message.find("push_pull_face"), std::string::npos) << r.message;
}

TEST(AiToolDispatcher, GetSelectionReportsASelectedEdgeAsATargetablePoint) {
    Document doc;
    History hist;
    SelectionManager sel;
    PluginContext ctx = makeCtx(doc, hist, &sel);
    ASSERT_TRUE(executeTool(ctx, "add_box",
        {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}).ok);
    int id = doc.getAllBodyIds().front();

    TopExp_Explorer ex(doc.getBody(id), TopAbs_EDGE);
    ASSERT_TRUE(ex.More());
    SelectionEntry entry;
    entry.type = SelectionType::Edge;
    entry.bodyId = id;
    // See the identical comment on the face test above.
    TopoDS_Edge edge = TopoDS::Edge(ex.Current());
    entry.shape = edge;
    sel.select(entry);

    ToolResult r = executeTool(ctx, "get_selection", {});
    ASSERT_TRUE(r.ok);
    EXPECT_NE(r.message.find("EDGE"), std::string::npos);
    EXPECT_NE(r.message.find("id " + std::to_string(id)), std::string::npos);
    EXPECT_NE(r.message.find("fillet_edge"), std::string::npos) << r.message;
}
