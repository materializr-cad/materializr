#include "ai/AiToolSchema.h"

#include <gtest/gtest.h>
#include <algorithm>

using namespace materializr::ai;

TEST(AiToolSchema, AllToolsContainsExactlyTheExpectedTools) {
    const auto& tools = allTools();
    std::vector<std::string> names;
    for (const auto& t : tools) names.push_back(t.name);
    std::vector<std::string> expected = {
        "add_box", "add_cylinder", "add_sphere", "add_cone", "add_torus",
        "move_body", "rotate_body", "scale_body", "boolean_op",
        "delete_body", "duplicate_body", "mirror_body", "pattern_body",
        "fillet_all_edges", "chamfer_all_edges", "fillet_face_edges",
        "chamfer_face_edges", "fillet_edge", "chamfer_edge",
        "push_pull_face", "extrude_rect", "extrude_circle", "extrude_polygon",
        "loft_bodies", "shell_body", "capture_view", "list_bodies",
        "get_selection",
        "separate_body", "align_body", "construction_axis", "construction_plane",
        "extrude_sketch", "describe_scene"};
    EXPECT_EQ(names, expected);
    EXPECT_EQ(tools.size(), expected.size());
}

TEST(AiToolSchema, AllNineOriginalToolNamesAreStillPresent) {
    const auto& tools = allTools();
    std::vector<std::string> names;
    for (const auto& t : tools) names.push_back(t.name);
    std::vector<std::string> original = {
        "add_box", "add_cylinder", "add_sphere", "add_cone", "add_torus",
        "move_body", "rotate_body", "scale_body", "boolean_op"};
    for (const auto& n : original) {
        EXPECT_NE(std::find(names.begin(), names.end(), n), names.end()) << "missing: " << n;
    }
}

namespace {
const ToolDef* findTool(const std::vector<ToolDef>& tools, const std::string& name) {
    for (const auto& t : tools) if (t.name == name) return &t;
    return nullptr;
}
void expectParamSplit(const ToolDef& t, const std::vector<std::string>& required,
                      const std::vector<std::string>& optional) {
    for (const auto& name : required) {
        bool found = false;
        for (const auto& p : t.params) if (p.name == name) { found = true; EXPECT_TRUE(p.required) << name; }
        EXPECT_TRUE(found) << "missing required param: " << name;
    }
    for (const auto& name : optional) {
        bool found = false;
        for (const auto& p : t.params) if (p.name == name) { found = true; EXPECT_FALSE(p.required) << name; }
        EXPECT_TRUE(found) << "missing optional param: " << name;
    }
}
} // namespace

TEST(AiToolSchema, MirrorBodyParamShapeMatchesTask1_2) {
    const auto* t = findTool(allTools(), "mirror_body");
    ASSERT_NE(t, nullptr);
    expectParamSplit(*t, {"body_id", "axis"}, {"keep_original"});
}

TEST(AiToolSchema, PatternBodyParamShapeMatchesBrief) {
    const auto* t = findTool(allTools(), "pattern_body");
    ASSERT_NE(t, nullptr);
    expectParamSplit(*t, {"body_id", "type", "count"},
                     {"spacing_x", "spacing_y", "spacing_z", "axis_x", "axis_y", "axis_z",
                      "origin_x", "origin_y", "origin_z", "total_angle_degrees"});
}

TEST(AiToolSchema, ConstructionAxisParamShapeMatchesBrief) {
    const auto* t = findTool(allTools(), "construction_axis");
    ASSERT_NE(t, nullptr);
    expectParamSplit(*t, {"type"}, {"p1_x", "p1_y", "p1_z", "p2_x", "p2_y", "p2_z", "name"});
}

TEST(AiToolSchema, ConstructionPlaneParamShapeMatchesBrief) {
    const auto* t = findTool(allTools(), "construction_plane");
    ASSERT_NE(t, nullptr);
    expectParamSplit(*t, {"type"}, {"offset", "name"});
}

TEST(AiToolSchema, ExtrudeSketchParamShapeMatchesPlan) {
    const auto* t = findTool(allTools(), "extrude_sketch");
    ASSERT_NE(t, nullptr);
    expectParamSplit(*t, {"sketch_id", "distance"},
                     {"region_indices", "symmetric", "mode", "target_body_id"});
}

TEST(AiToolSchema, ExtrudeSketchRegionIndicesSerializesAsAnIntegerArray) {
    // expectParamSplit only checks the ToolParam struct's required flag,
    // not what actually gets serialized - a broken paramsToJsonSchema
    // branch could still pass it. Assert the real wire shape both
    // providers receive.
    nlohmann::json anthropic = toolsToAnthropicJson(allTools());
    nlohmann::json openai = toolsToOpenAiJson(allTools());
    auto findSchema = [](const nlohmann::json& tools, bool isOpenAi) -> const nlohmann::json* {
        for (const auto& t : tools) {
            const std::string& name = isOpenAi ? t["function"]["name"].get_ref<const std::string&>()
                                                : t["name"].get_ref<const std::string&>();
            if (name == "extrude_sketch")
                return isOpenAi ? &t["function"]["parameters"] : &t["input_schema"];
        }
        return nullptr;
    };
    const nlohmann::json* a = findSchema(anthropic, false);
    const nlohmann::json* o = findSchema(openai, true);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(o, nullptr);
    for (const nlohmann::json* schema : {a, o}) {
        const auto& regionIndices = (*schema)["properties"]["region_indices"];
        EXPECT_EQ(regionIndices["type"], "array");
        EXPECT_EQ(regionIndices["items"]["type"], "integer");
    }
}

TEST(AiToolSchema, DescribeSceneParamsAreAllOptional) {
    const auto* t = findTool(allTools(), "describe_scene");
    ASSERT_NE(t, nullptr);
    expectParamSplit(*t, {},
                     {"after_body_id", "after_sketch_id", "after_axis_id", "after_plane_id"});
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
