#include "AiToolSchema.h"

namespace materializr { namespace ai {

namespace {
ToolParam num(const char* name, const char* desc, bool required = true) {
    return {name, ToolParamType::Number, required, desc};
}
ToolParam str(const char* name, const char* desc, bool required = true) {
    return {name, ToolParamType::String, required, desc};
}
ToolParam boolean(const char* name, const char* desc, bool required = true) {
    return {name, ToolParamType::Boolean, required, desc};
}
// x, y, z default to the world origin - see AiToolDispatcher (Task 5) for
// where the default is actually applied when the model omits them.
//
// `anchor`, when given, says what point on the shape (x,y,z) actually is -
// most of these primitives anchor at their centre (sphere, torus) or the
// centre of their base face (cylinder, cone), which is the assumption a
// model defaults to anyway and so doesn't need calling out. add_box is the
// one exception (its origin is a CORNER, not a centre) - see its own call
// below. Left null for every other caller, which keeps their wording
// unchanged.
std::vector<ToolParam> withOrigin(std::vector<ToolParam> params, const char* anchor = nullptr) {
    std::string xDesc = "World X position in mm (default 0).";
    if (anchor) xDesc = "World X position in mm (default 0) - " + std::string(anchor);
    params.push_back(num("x", xDesc.c_str(), false));
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
                     num("depth", "Size along the horizontal depth axis (Y) in mm.")},
                    "this is the box's MINIMUM CORNER, not its centre - the box "
                    "extends from here by width/depth/height in the +X/+Y/+Z "
                    "direction. To centre a box at a point, subtract half of "
                    "width/height/depth from that point's x/y/z first.")},
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
        {"boolean_op", "Combine two bodies with a boolean operation. Also how to "
                      "SUBTRACT one body from another: mode='subtract' removes "
                      "tool_body_id's volume from target_body_id.",
         {num("target_body_id", "The body kept after the operation."),
          num("tool_body_id", "The body combined into the target."),
          str("mode", "One of: union, subtract, intersect.")}},
        {"delete_body", "Permanently remove a body from the document. Always call "
                        "list_bodies first if the body wasn't created earlier in "
                        "THIS conversation.",
         {num("body_id", "The id of the body to delete.")}},
        {"duplicate_body", "Make a copy of an existing body, offset from the "
                           "original.",
         {num("body_id", "The id of the body to duplicate."),
          num("dx", "Offset of the copy along X in mm (default 20).", false),
          num("dy", "Offset of the copy along Y in mm (default 0).", false),
          num("dz", "Offset of the copy along Z in mm (default 0).", false)}},
        {"mirror_body", "Reflect a body across a plane through the world origin, "
                        "either as a new mirrored copy (default) or by flipping "
                        "the body itself in place.",
         {num("body_id", "The id of the body to mirror."),
          str("axis", "The mirror plane's normal direction, in the same X/Y/Z "
                      "convention as add_box (Z is up, Y is depth): one of "
                      "+x,-x,+y,-y,+z,-z. Sign doesn't matter - '+x' and '-x' "
                      "mirror the same plane."),
          boolean("keep_original", "If true (default), the mirror is a NEW body "
                                   "and the original is untouched. If false, the "
                                   "original body itself is replaced by its "
                                   "mirror image.", false)}},
        {"pattern_body", "Create multiple evenly-spaced or evenly-rotated copies "
                         "of a body. Afterwards call list_bodies to get the new "
                         "copies' ids - this tool doesn't return them directly.",
         {num("body_id", "The id of the body to pattern."),
          str("type", "One of: linear (evenly spaced along a straight line), "
                      "radial (evenly rotated around an axis)."),
          num("count", "Total number of copies, INCLUDING the original - 3 "
                       "means the original plus 2 new copies."),
          num("spacing_x", "LINEAR ONLY (required if type=linear): spacing "
                           "between copies along X in mm.", false),
          num("spacing_y", "LINEAR ONLY: spacing along Y in mm.", false),
          num("spacing_z", "LINEAR ONLY: spacing along Z in mm.", false),
          num("axis_x", "RADIAL ONLY (required if type=radial): rotation axis "
                        "X component.", false),
          num("axis_y", "RADIAL ONLY: rotation axis Y component.", false),
          num("axis_z", "RADIAL ONLY: rotation axis Z component.", false),
          num("origin_x", "RADIAL ONLY: a point the rotation axis passes "
                          "through, X in mm (default 0 - the world origin).", false),
          num("origin_y", "RADIAL ONLY: rotation axis point, Y in mm (default 0).", false),
          num("origin_z", "RADIAL ONLY: rotation axis point, Z in mm (default 0).", false),
          num("total_angle_degrees", "RADIAL ONLY: total angle the copies span, "
                                     "in degrees (default 360, i.e. a full ring).",
                                     false)}},
        {"fillet_all_edges", "Round every edge of a body with a constant radius.",
         {num("body_id", "The id of the body to fillet."),
          num("radius", "Fillet radius in mm. Must be small enough to fit the body's "
                        "smallest edge/face - if it fails, try a smaller radius.")}},
        {"chamfer_all_edges", "Bevel every edge of a body by a constant distance.",
         {num("body_id", "The id of the body to chamfer."),
          num("distance", "Chamfer distance in mm, measured along each adjoining face.")}},
        {"fillet_face_edges", "Round every edge bounding ONE face of a body - e.g. "
                              "'round the top edges' of a box means this, not "
                              "fillet_all_edges (which would round the bottom and side "
                              "edges too) and not fillet_edge (which only takes one edge "
                              "at a time and needs a guessed point). Prefer this whenever "
                              "the request names a face/side rather than a specific edge.",
         {num("body_id", "The id of the body to fillet."),
          num("radius", "Fillet radius in mm. Must be small enough to fit the face's "
                        "smallest edge - if it fails, try a smaller radius."),
          str("face", "Which face's bounding edges to round, by the direction its "
                      "outward normal points, in the same X/Y/Z convention as add_box "
                      "(Z is up, Y is depth): '+x','-x','+y','-y','+z' (top),'-z' "
                      "(bottom). If more than one face points that way, the largest one "
                      "is used.")}},
        {"chamfer_face_edges", "Bevel every edge bounding ONE face of a body - e.g. "
                               "'chamfer the top edges' of a box means this, not "
                               "chamfer_all_edges (which would bevel the bottom and side "
                               "edges too) and not chamfer_edge (which only takes one "
                               "edge at a time and needs a guessed point). Prefer this "
                               "whenever the request names a face/side rather than a "
                               "specific edge.",
         {num("body_id", "The id of the body to chamfer."),
          num("distance", "Chamfer distance in mm, measured along each adjoining face."),
          str("face", "Which face's bounding edges to bevel, by the direction its "
                      "outward normal points, in the same X/Y/Z convention as add_box "
                      "(Z is up, Y is depth): '+x','-x','+y','-y','+z' (top),'-z' "
                      "(bottom). If more than one face points that way, the largest one "
                      "is used.")}},
        {"fillet_edge", "Round a SINGLE edge of a body - whichever one is nearest the "
                        "given point - with a constant radius. For a whole-body round, "
                        "use fillet_all_edges instead; for every edge of one face (e.g. "
                        "'the top edges'), use fillet_face_edges instead - it needs no "
                        "guessed point.",
         {num("body_id", "The id of the body to fillet."),
          num("radius", "Fillet radius in mm."),
          num("x", "Approximate X position near the edge to fillet, in mm - same "
                   "X/Y/Z convention as add_box (Z is up, Y is depth). Doesn't need to "
                   "be exact, just closer to the intended edge than to any other."),
          num("y", "Approximate Y position near the edge, in mm."),
          num("z", "Approximate Z position near the edge, in mm.")}},
        {"chamfer_edge", "Bevel a SINGLE edge of a body - whichever one is nearest the "
                         "given point - by a constant distance. For a whole-body bevel, "
                         "use chamfer_all_edges instead; for every edge of one face (e.g. "
                         "'the top edges'), use chamfer_face_edges instead - it needs no "
                         "guessed point.",
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
        {"extrude_polygon", "Create a custom polygon profile and extrude it along a "
                            "chosen direction - for any cross-section that isn't a "
                            "plain rectangle or circle (use extrude_rect/extrude_circle "
                            "for those, they're simpler). Also how to build a custom "
                            "profile to loft between with loft_bodies.",
         withExtrudeMode(withOriginAndDirection(
             {str("points", "The profile's corners, as a JSON array of [x,y] pairs "
                            "ENCODED AS A STRING (this is the one tool argument here "
                            "that isn't a plain number/string/bool - it's a JSON array "
                            "written out as text), e.g. "
                            "\"[[0,0],[10,0],[10,5],[0,5]]\". At least 3 points, "
                            "listed in order around the polygon (clockwise or "
                            "counterclockwise, either works - don't jump between "
                            "opposite corners). Coordinates are LOCAL to the profile: "
                            "local x/y are the same axes extrude_rect's width/depth "
                            "use, relative to this tool's own x/y/z + dir_x/y/z."),
              num("distance", "Extrude distance in mm along the direction (can be "
                              "negative to extrude the other way).")}))},
        {"loft_bodies", "Create a new smooth solid connecting one face of an existing "
                        "body to a face of a DIFFERENT existing body - e.g. lofting "
                        "from a round body to a square one. Both source bodies are "
                        "left untouched; the loft is a separate new body (use "
                        "boolean_op afterward to fuse everything into one part if "
                        "that's wanted). Call list_bodies first if either body wasn't "
                        "created earlier in THIS conversation.",
         {num("from_body_id", "The id of the first body."),
          str("from_face", "Which face of from_body_id to start the loft from, by the "
                           "direction its outward normal points, in the same X/Y/Z "
                           "convention as add_box (Z is up, Y is depth): one of "
                           "+x,-x,+y,-y,+z,-z. If more than one face points that way, "
                           "the largest one is used."),
          num("to_body_id", "The id of the second body."),
          str("to_face", "Which face of to_body_id to end the loft at - same "
                        "direction convention as from_face."),
          boolean("solid", "true (default) for a solid loft; false for a thin loft "
                           "shell with no wall thickness.", false)}},
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
    switch (t) {
        case ToolParamType::String: return "string";
        case ToolParamType::Boolean: return "boolean";
        case ToolParamType::Number: return "number";
    }
    return "number";
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
