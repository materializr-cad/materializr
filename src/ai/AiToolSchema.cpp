#include "AiToolSchema.h"

namespace materializr { namespace ai {

namespace {
ToolParam num(const char* name, const char* desc, bool required = true) {
    return {name, ToolParamType::Number, required, desc};
}
ToolParam str(const char* name, const char* desc, bool required = true) {
    return {name, ToolParamType::String, required, desc};
}
// x, y, z default to the world origin - see AiToolDispatcher (Task 5) for
// where the default is actually applied when the model omits them.
std::vector<ToolParam> withOrigin(std::vector<ToolParam> params) {
    params.push_back(num("x", "World X position in mm (default 0).", false));
    params.push_back(num("y", "World Y position in mm (default 0).", false));
    params.push_back(num("z", "World Z position in mm (default 0).", false));
    return params;
}
// Position (profile centre) plus an extrude direction, both optional - a
// plain vertical extrude (dir 0,0,1, matching add_box's own up axis) is the
// common case; a custom direction is what makes extrude_rect/extrude_circle
// more than just add_box/add_cylinder with extra steps, letting a cut or
// addition go in at ANY orientation, not just the three principal axes.
std::vector<ToolParam> withOriginAndDirection(std::vector<ToolParam> params) {
    params = withOrigin(std::move(params));
    params.push_back(num("dir_x", "Extrude direction X component (default 0).", false));
    params.push_back(num("dir_y", "Extrude direction Y component (default 0).", false));
    params.push_back(num("dir_z", "Extrude direction Z component (default 1, i.e. "
                                  "straight up, same convention as add_box).", false));
    return params;
}
// mode + target_body_id, shared by extrude_rect/extrude_circle: create a new
// body, or combine into an existing one the same way boolean_op does.
std::vector<ToolParam> withExtrudeMode(std::vector<ToolParam> params) {
    params.push_back(str("mode", "One of: new_body (default), union, subtract, intersect. "
                                 "union/subtract/intersect need target_body_id.", false));
    params.push_back(num("target_body_id", "The body to combine into, for union/subtract/"
                                           "intersect. Ignored for new_body.", false));
    return params;
}
} // namespace

const std::vector<ToolDef>& allTools() {
    static const std::vector<ToolDef> kTools = {
        {"add_box", "Create a rectangular box body.",
         withOrigin({num("width", "Size along X in mm."),
                     num("height", "Size along the up axis (Z) in mm."),
                     num("depth", "Size along the horizontal depth axis (Y) in mm.")})},
        {"add_cylinder", "Create a cylindrical body.",
         withOrigin({num("radius", "Radius in mm."),
                     num("height", "Height in mm.")})},
        {"add_sphere", "Create a spherical body.",
         withOrigin({num("radius", "Radius in mm.")})},
        {"add_cone", "Create a conical body (top_radius 0 for a sharp point).",
         withOrigin({num("bottom_radius", "Base radius in mm."),
                     num("top_radius", "Top radius in mm; 0 for a point."),
                     num("height", "Height in mm.")})},
        {"add_torus", "Create a torus (ring) body.",
         withOrigin({num("major_radius", "Distance from centre to tube centre, in mm."),
                     num("minor_radius", "Tube radius in mm.")})},
        {"move_body", "Translate an existing body.",
         {num("body_id", "The id of the body to move."),
          num("dx", "Move along X in mm."),
          num("dy", "Move along Y in mm."),
          num("dz", "Move along Z in mm.")}},
        {"rotate_body", "Rotate an existing body about an axis through the world origin.",
         {num("body_id", "The id of the body to rotate."),
          num("axis_x", "Rotation axis X component."),
          num("axis_y", "Rotation axis Y component."),
          num("axis_z", "Rotation axis Z component."),
          num("angle_degrees", "Rotation angle in degrees.")}},
        {"scale_body", "Uniformly scale an existing body.",
         {num("body_id", "The id of the body to scale."),
          num("factor", "Scale factor, e.g. 2.0 doubles the size.")}},
        {"boolean_op", "Combine two bodies with a boolean operation.",
         {num("target_body_id", "The body kept after the operation."),
          num("tool_body_id", "The body combined into the target."),
          str("mode", "One of: union, subtract, intersect.")}},
        {"fillet_all_edges", "Round every edge of a body with a constant radius.",
         {num("body_id", "The id of the body to fillet."),
          num("radius", "Fillet radius in mm. Must be small enough to fit the body's "
                        "smallest edge/face - if it fails, try a smaller radius.")}},
        {"chamfer_all_edges", "Bevel every edge of a body by a constant distance.",
         {num("body_id", "The id of the body to chamfer."),
          num("distance", "Chamfer distance in mm, measured along each adjoining face.")}},
        {"fillet_edge", "Round a SINGLE edge of a body - whichever one is nearest the "
                        "given point - with a constant radius. For a whole-body round, "
                        "use fillet_all_edges instead.",
         {num("body_id", "The id of the body to fillet."),
          num("radius", "Fillet radius in mm."),
          num("x", "Approximate X position near the edge to fillet, in mm - same "
                   "X/Y/Z convention as add_box (Z is up, Y is depth). Doesn't need to "
                   "be exact, just closer to the intended edge than to any other."),
          num("y", "Approximate Y position near the edge, in mm."),
          num("z", "Approximate Z position near the edge, in mm.")}},
        {"chamfer_edge", "Bevel a SINGLE edge of a body - whichever one is nearest the "
                         "given point - by a constant distance. For a whole-body bevel, "
                         "use chamfer_all_edges instead.",
         {num("body_id", "The id of the body to chamfer."),
          num("distance", "Chamfer distance in mm, measured along each adjoining face."),
          num("x", "Approximate X position near the edge to chamfer, in mm - same "
                   "X/Y/Z convention as add_box (Z is up, Y is depth). Doesn't need to "
                   "be exact, just closer to the intended edge than to any other."),
          num("y", "Approximate Y position near the edge, in mm."),
          num("z", "Approximate Z position near the edge, in mm.")}},
        {"push_pull_face", "Push or pull a single face of an existing body - whichever "
                          "face is nearest the given point - along its own normal, "
                          "adding or removing material. This is how to nudge one wall, "
                          "widen a pocket, or extend one side of a body you already have.",
         {num("body_id", "The id of the body to edit."),
          num("distance", "Distance in mm. Positive extends the face outward (adds "
                          "material); negative pushes it inward (removes material)."),
          num("x", "Approximate X position near the face to push/pull, in mm - same "
                   "X/Y/Z convention as add_box. Doesn't need to be exact, just closer "
                   "to the intended face than to any other."),
          num("y", "Approximate Y position near the face, in mm."),
          num("z", "Approximate Z position near the face, in mm.")}},
        {"extrude_rect", "Create a rectangular profile and extrude it along a chosen "
                         "direction - a box at any orientation, or (with mode=subtract) "
                         "a rectangular pocket/slot cut into an existing body at any "
                         "orientation, e.g. through the SIDE of a part.",
         withExtrudeMode(withOriginAndDirection(
             {num("width", "Profile width in mm, along the extrude direction's local X."),
              num("depth", "Profile depth in mm, along the extrude direction's local Y."),
              num("distance", "Extrude distance in mm along the direction (can be "
                              "negative to extrude the other way).")}))},
        {"extrude_circle", "Create a circular profile and extrude it along a chosen "
                           "direction - a cylinder at any orientation, or (with "
                           "mode=subtract) a round hole drilled into an existing body at "
                           "any orientation, e.g. through the SIDE of a part, which "
                           "add_cylinder + boolean_op cannot do without a separate rotate.",
         withExtrudeMode(withOriginAndDirection(
             {num("radius", "Profile radius in mm."),
              num("distance", "Extrude distance in mm along the direction (can be "
                              "negative to extrude the other way).")}))},
        {"shell_body", "Hollow out a body to a constant wall thickness, optionally "
                       "leaving one face open so the inside is reachable.",
         {num("body_id", "The id of the body to shell."),
          num("thickness", "Wall thickness in mm."),
          str("open_face", "Which face to remove, by the direction its outward normal "
                           "points, in the same X/Y/Z convention as add_box (Z is up, Y "
                           "is depth): '+x','-x','+y','-y','+z' (top, most common for an "
                           "open-top container),'-z', or 'none' for a fully closed hollow "
                           "shell. If more than one face on the body points that way, the "
                           "largest one is removed.", false)}},
        {"capture_view", "Take a screenshot of the current 3D view (whatever camera angle "
                         "is currently on screen - this does not move the camera) and see "
                         "it as an image. Use this to check your own progress, e.g. after a "
                         "few modeling steps, or to compare against a loaded reference mesh "
                         "before deciding what to do next. Takes no arguments.",
         {}},
        {"list_bodies", "List every body already in the document, with its id, name, "
                        "position, and size. ALWAYS call this before editing, moving, "
                        "combining with, or otherwise targeting something that isn't a "
                        "body you just created yourself in this conversation - body_id/"
                        "target_body_id arguments elsewhere only work with a real id from "
                        "here (or from a body you created earlier in this chat). Takes no "
                        "arguments.",
         {}},
    };
    return kTools;
}

const std::string& systemPrompt() {
    static const std::string kPrompt =
        "You are a CAD modeling assistant embedded in materializr, a parametric "
        "solid-modeling app. You build and edit geometry ONLY through the tools "
        "provided - there is no other way to affect the document.\n"
        "\n"
        "COORDINATE CONVENTION - read carefully: every x/y/z argument on every "
        "tool here (add_box, move_body, fillet_edge, list_bodies, all of them) "
        "uses X = width (left-right), Y = depth (front-back), Z = up (vertical). "
        "Always use Z for vertical and Y for depth in tool arguments - never the "
        "other way around, and never assume Y is vertical the way it is in "
        "Unity/Maya/OpenGL. If a fillet/chamfer/push-pull call keeps missing the "
        "edge or face you intended, the most likely cause is having swapped Y "
        "and Z when computing the point.\n"
        "\n"
        "Before editing, moving, or combining anything that isn't a body you just "
        "created earlier in THIS conversation, call list_bodies first - its "
        "reported positions/sizes already use the X/Y/Z convention above, so you "
        "can use them directly. Use capture_view to look at your own progress "
        "after a few steps, especially before deciding a multi-step edit is done.";
    return kPrompt;
}

namespace {
const char* typeName(ToolParamType t) {
    return t == ToolParamType::String ? "string" : "number";
}
// Shared by both formatters: the JSON Schema "object" body every provider
// wraps identically (properties + required list), only the outer envelope
// differs (Anthropic's input_schema vs OpenAI's function.parameters).
nlohmann::json paramsToJsonSchema(const std::vector<ToolParam>& params) {
    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json required = nlohmann::json::array();
    for (const auto& p : params) {
        properties[p.name] = {{"type", typeName(p.type)}, {"description", p.description}};
        if (p.required) required.push_back(p.name);
    }
    return {{"type", "object"}, {"properties", properties}, {"required", required}};
}
} // namespace

nlohmann::json toolsToAnthropicJson(const std::vector<ToolDef>& tools) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& t : tools)
        out.push_back({{"name", t.name},
                       {"description", t.description},
                       {"input_schema", paramsToJsonSchema(t.params)}});
    return out;
}

nlohmann::json toolsToOpenAiJson(const std::vector<ToolDef>& tools) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& t : tools)
        out.push_back({{"type", "function"},
                       {"function", {{"name", t.name},
                                     {"description", t.description},
                                     {"parameters", paramsToJsonSchema(t.params)}}}});
    return out;
}

} } // namespace materializr::ai
