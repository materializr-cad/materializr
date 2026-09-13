#include "ai/AiToolDispatcher.h"
#include "core/Document.h"
#include "core/History.h"
#include "plugin/PluginContext.h"

#include <gtest/gtest.h>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>

using namespace materializr::ai;
using materializr::PluginContext;

namespace {
// A PluginContext with just enough bound to run executeTool: Document +
// History. The other _bind() parameters aren't touched by any tool.
PluginContext makeCtx(Document& doc, History& hist) {
    PluginContext ctx;
    ctx._bind(&doc, &hist, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
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
