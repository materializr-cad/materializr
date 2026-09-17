#include "ai/AiToolSchema.h"

#include <gtest/gtest.h>

using namespace materializr::ai;

TEST(AiToolSchema, AllToolsContainsExactlyTheExpectedTools) {
    const auto& tools = allTools();
    std::vector<std::string> names;
    for (const auto& t : tools) names.push_back(t.name);
    std::vector<std::string> expected = {
        "add_box", "add_cylinder", "add_sphere", "add_cone", "add_torus",
        "move_body", "rotate_body", "scale_body", "boolean_op",
        "fillet_all_edges", "chamfer_all_edges", "fillet_edge", "chamfer_edge",
        "push_pull_face", "extrude_rect", "extrude_circle", "shell_body",
        "capture_view"};
    EXPECT_EQ(names, expected);
}

TEST(AiToolSchema, EveryToolHasANonEmptyDescription) {
    for (const auto& t : allTools())
        EXPECT_FALSE(t.description.empty()) << "tool: " << t.name;
}

TEST(AiToolSchema, AnthropicJsonHasOneEntryPerToolWithInputSchema) {
    nlohmann::json j = toolsToAnthropicJson(allTools());
    ASSERT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), allTools().size());
    for (const auto& entry : j) {
        EXPECT_TRUE(entry.contains("name"));
        EXPECT_TRUE(entry.contains("input_schema"));
        EXPECT_EQ(entry["input_schema"]["type"], "object");
    }
}

TEST(AiToolSchema, OpenAiJsonWrapsEachToolInAFunctionEnvelope) {
    nlohmann::json j = toolsToOpenAiJson(allTools());
    ASSERT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), allTools().size());
    for (const auto& entry : j) {
        EXPECT_EQ(entry["type"], "function");
        EXPECT_TRUE(entry["function"].contains("name"));
        EXPECT_TRUE(entry["function"].contains("parameters"));
        EXPECT_EQ(entry["function"]["parameters"]["type"], "object");
    }
}

TEST(AiToolSchema, BooleanOpModeParamIsRequired) {
    for (const auto& t : allTools()) {
        if (t.name != "boolean_op") continue;
        bool found = false;
        for (const auto& p : t.params)
            if (p.name == "mode") { found = true; EXPECT_TRUE(p.required); }
        EXPECT_TRUE(found);
        return;
    }
    FAIL() << "boolean_op tool not found";
}

TEST(AiToolSchema, ShellBodyOpenFaceParamIsOptional) {
    for (const auto& t : allTools()) {
        if (t.name != "shell_body") continue;
        bool found = false;
        for (const auto& p : t.params)
            if (p.name == "open_face") { found = true; EXPECT_FALSE(p.required); }
        EXPECT_TRUE(found);
        return;
    }
    FAIL() << "shell_body tool not found";
}

TEST(AiToolSchema, FilletEdgePositionParamsAreRequired) {
    for (const auto& t : allTools()) {
        if (t.name != "fillet_edge") continue;
        for (const char* key : {"x", "y", "z"}) {
            bool found = false;
            for (const auto& p : t.params)
                if (p.name == key) { found = true; EXPECT_TRUE(p.required) << key; }
            EXPECT_TRUE(found) << key;
        }
        return;
    }
    FAIL() << "fillet_edge tool not found";
}

TEST(AiToolSchema, ExtrudeRectModeAndTargetBodyIdAreOptional) {
    for (const auto& t : allTools()) {
        if (t.name != "extrude_rect") continue;
        for (const char* key : {"mode", "target_body_id"}) {
            bool found = false;
            for (const auto& p : t.params)
                if (p.name == key) { found = true; EXPECT_FALSE(p.required) << key; }
            EXPECT_TRUE(found) << key;
        }
        return;
    }
    FAIL() << "extrude_rect tool not found";
}

TEST(AiToolSchema, ExtrudeCircleRadiusAndDistanceAreRequired) {
    for (const auto& t : allTools()) {
        if (t.name != "extrude_circle") continue;
        for (const char* key : {"radius", "distance"}) {
            bool found = false;
            for (const auto& p : t.params)
                if (p.name == key) { found = true; EXPECT_TRUE(p.required) << key; }
            EXPECT_TRUE(found) << key;
        }
        return;
    }
    FAIL() << "extrude_circle tool not found";
}
