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
