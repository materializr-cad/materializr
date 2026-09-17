#include "AiToolDispatcher.h"
#include "../plugin/PluginContext.h"
#include "../core/Document.h"
#include "../core/History.h"
#include "../modeling/PrimitiveOp.h"
#include "../modeling/TransformOp.h"
#include "../modeling/BooleanOp.h"
#include "../modeling/FilletOp.h"
#include "../modeling/ChamferOp.h"
#include "../modeling/ShellOp.h"

#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <limits>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr { namespace ai {

namespace {

// nlohmann::json::value(key, default) already does exactly what an optional
// numeric arg needs; this wraps the REQUIRED case so a missing key is a
// deliberate error rather than silently defaulting to 0.
bool requireNumber(const nlohmann::json& args, const char* key, double& out,
                   std::string& err) {
    if (!args.contains(key) || !args[key].is_number()) {
        err = std::string("missing or non-numeric required argument '") + key + "'";
        return false;
    }
    out = args[key].get<double>();
    return true;
}
bool requirePositive(const nlohmann::json& args, const char* key, double& out,
                     std::string& err) {
    if (!requireNumber(args, key, out, err)) return false;
    if (out <= 0.0) {
        err = std::string("'") + key + "' must be positive, got " + std::to_string(out);
        return false;
    }
    return true;
}
bool requireBodyId(Document& doc, const nlohmann::json& args, const char* key,
                   int& out, std::string& err) {
    double raw;
    if (!requireNumber(args, key, raw, err)) return false;
    if (!std::isfinite(raw) ||
        raw < static_cast<double>(std::numeric_limits<int>::min()) ||
        raw > static_cast<double>(std::numeric_limits<int>::max())) {
        err = std::string("'") + key + "' is out of range";
        return false;
    }
    if (raw != std::floor(raw)) {
        err = std::string("'") + key + "' must be a whole number, got " +
              std::to_string(raw);
        return false;
    }
    out = static_cast<int>(raw);
    for (int id : doc.getAllBodyIds()) if (id == out) return true;
    err = std::string("no body with id ") + std::to_string(out);
    return false;
}
// Distinguishes "absent" (use fallback) from "present but wrong type" (reject) -
// optNumber's single-return-value shape couldn't tell those apart, silently
// defaulting a malformed call instead of rejecting it.
bool optionalNumber(const nlohmann::json& args, const char* key, double& out,
                    double fallback, std::string& err) {
    if (!args.contains(key)) { out = fallback; return true; }
    if (!args[key].is_number()) {
        err = std::string("'") + key + "' must be a number";
        return false;
    }
    out = args[key].get<double>();
    return true;
}

ToolResult addPrimitive(PluginContext& ctx, PrimitiveOp::Kind kind,
                        const nlohmann::json& args) {
    std::string err;
    auto op = std::make_unique<PrimitiveOp>();
    op->setKind(kind);
    switch (kind) {
        case PrimitiveOp::Kind::Box: {
            double w, h, d;
            if (!requirePositive(args, "width", w, err) ||
                !requirePositive(args, "height", h, err) ||
                !requirePositive(args, "depth", d, err))
                return {false, err};
            // PrimitiveOp::setBoxExtents(x,y,z) takes W(x)/D(y)/H(z) - depth in the
            // middle slot, height last - so the call order is (w, d, h), not (w, h, d).
            op->setBoxExtents(w, d, h);
            break;
        }
        case PrimitiveOp::Kind::Cylinder: {
            double r, h;
            if (!requirePositive(args, "radius", r, err) ||
                !requirePositive(args, "height", h, err))
                return {false, err};
            op->setRadius(r);
            op->setHeight(h);
            break;
        }
        case PrimitiveOp::Kind::Sphere: {
            double r;
            if (!requirePositive(args, "radius", r, err)) return {false, err};
            op->setRadius(r);
            break;
        }
        case PrimitiveOp::Kind::Cone: {
            double br, tr, h;
            if (!requirePositive(args, "bottom_radius", br, err) ||
                !requireNumber(args, "top_radius", tr, err) ||
                !requirePositive(args, "height", h, err))
                return {false, err};
            if (tr < 0.0) return {false, "'top_radius' must not be negative"};
            op->setRadius(br);
            op->setTopRadius(tr);
            op->setHeight(h);
            break;
        }
        case PrimitiveOp::Kind::Torus: {
            double major, minor;
            if (!requirePositive(args, "major_radius", major, err) ||
                !requirePositive(args, "minor_radius", minor, err))
                return {false, err};
            if (major <= minor)
                return {false, "'major_radius' must be greater than 'minor_radius'"};
            op->setRadius(major);
            op->setMinorRadius(minor);
            break;
        }
    }
    double x, y, z;
    if (!optionalNumber(args, "x", x, 0.0, err) ||
        !optionalNumber(args, "y", y, 0.0, err) ||
        !optionalNumber(args, "z", z, 0.0, err))
        return {false, err};
    op->setOrigin(x, y, z);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "the operation failed to execute"};
    // pushOperation only touches the Document - without this, the new body
    // never reaches the renderer (same class of bug as BooleanPlugin's
    // partial-subtract gap: nothing marks the viewport dirty on its own).
    ctx.markMeshesDirty();
    // PrimitiveOp appends the new body, so its id is the last one - see Document::addBody.
    int newId = ctx.document().getAllBodyIds().back();
    return {true, "Created body " + std::to_string(newId)};
}

ToolResult moveBody(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int bodyId;
    double dx, dy, dz;
    if (!requireBodyId(ctx.document(), args, "body_id", bodyId, err) ||
        !requireNumber(args, "dx", dx, err) ||
        !requireNumber(args, "dy", dy, err) ||
        !requireNumber(args, "dz", dz, err))
        return {false, err};
    auto op = std::make_unique<TransformOp>();
    op->setBodyId(bodyId);
    op->setType(TransformType::Translate);
    // TransformOp::setTranslation passes its args straight through as raw world
    // coordinates, unlike PrimitiveOp::setOrigin, which remaps user (x,y,z) to
    // world (x,z,y) - see PrimitiveOp.cpp's worldPnt() comment (user Z "up" ->
    // world Y, user Y "depth" -> world Z). Swap dy/dz here so move_body uses the
    // SAME user-space convention as add_* - do not "fix" this back to (dx,dy,dz).
    op->setTranslation(dx, dz, dy);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "the operation failed to execute"};
    ctx.markMeshesDirty();
    return {true, "Moved body " + std::to_string(bodyId)};
}

ToolResult rotateBody(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int bodyId;
    double ax, ay, az, angle;
    if (!requireBodyId(ctx.document(), args, "body_id", bodyId, err) ||
        !requireNumber(args, "axis_x", ax, err) ||
        !requireNumber(args, "axis_y", ay, err) ||
        !requireNumber(args, "axis_z", az, err) ||
        !requireNumber(args, "angle_degrees", angle, err))
        return {false, err};
    if (ax == 0.0 && ay == 0.0 && az == 0.0)
        return {false, "the rotation axis must not be the zero vector"};
    auto op = std::make_unique<TransformOp>();
    op->setBodyId(bodyId);
    op->setType(TransformType::Rotate);
    // Same user-to-world axis swap as moveBody's setTranslation (see its comment)
    // applied to the rotation axis, so an AI-issued axis of (0,0,1) ("rotate
    // around up") means world Y, consistent with add_*'s origin convention.
    // The (x,y,z)->(x,z,y) map has determinant -1 (a reflection, not a pure
    // rotation), so swapping only the axis flips the rotation's handedness;
    // negating the angle restores the sense the user intended.
    op->setRotation(ax, az, ay, -angle);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "the operation failed to execute"};
    ctx.markMeshesDirty();
    return {true, "Rotated body " + std::to_string(bodyId)};
}

ToolResult scaleBody(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int bodyId;
    double factor;
    if (!requireBodyId(ctx.document(), args, "body_id", bodyId, err) ||
        !requirePositive(args, "factor", factor, err))
        return {false, err};
    auto op = std::make_unique<TransformOp>();
    op->setBodyId(bodyId);
    op->setType(TransformType::Scale);
    op->setScale(factor);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "the operation failed to execute"};
    ctx.markMeshesDirty();
    return {true, "Scaled body " + std::to_string(bodyId)};
}

ToolResult booleanOp(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int targetId, toolId;
    if (!requireBodyId(ctx.document(), args, "target_body_id", targetId, err) ||
        !requireBodyId(ctx.document(), args, "tool_body_id", toolId, err))
        return {false, err};
    if (targetId == toolId)
        return {false, "target_body_id and tool_body_id must be different bodies"};
    if (!args.contains("mode") || !args["mode"].is_string())
        return {false, "missing required argument 'mode'"};
    std::string modeStr = args["mode"].get<std::string>();
    BooleanMode mode;
    if (modeStr == "union") mode = BooleanMode::Union;
    else if (modeStr == "subtract") mode = BooleanMode::Subtract;
    else if (modeStr == "intersect") mode = BooleanMode::Intersect;
    else return {false, "'mode' must be one of: union, subtract, intersect"};

    auto op = std::make_unique<BooleanOp>();
    op->setTargetBodyId(targetId);
    op->setToolBodyId(toolId);
    op->setMode(mode);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "the operation failed to execute"};
    ctx.markMeshesDirty();
    return {true, "Combined bodies " + std::to_string(targetId) + " and " +
                  std::to_string(toolId) + " (" + modeStr + ")"};
}

ToolResult filletAllEdges(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int bodyId;
    double radius;
    if (!requireBodyId(ctx.document(), args, "body_id", bodyId, err) ||
        !requirePositive(args, "radius", radius, err))
        return {false, err};
    const TopoDS_Shape& body = ctx.document().getBody(bodyId);
    std::vector<TopoDS_Edge> edges;
    for (TopExp_Explorer ex(body, TopAbs_EDGE); ex.More(); ex.Next())
        edges.push_back(TopoDS::Edge(ex.Current()));
    if (edges.empty()) return {false, "body " + std::to_string(bodyId) + " has no edges"};
    auto op = std::make_unique<FilletOp>();
    op->setBody(bodyId);
    op->setEdges(edges);
    op->setRadius(radius);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "fillet failed - the radius is likely too large for this body's "
                       "smallest edge or face; try a smaller radius"};
    ctx.markMeshesDirty();
    return {true, "Filleted all edges of body " + std::to_string(bodyId) +
                  " at radius " + std::to_string(radius) + "mm"};
}

ToolResult chamferAllEdges(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int bodyId;
    double distance;
    if (!requireBodyId(ctx.document(), args, "body_id", bodyId, err) ||
        !requirePositive(args, "distance", distance, err))
        return {false, err};
    const TopoDS_Shape& body = ctx.document().getBody(bodyId);
    std::vector<TopoDS_Edge> edges;
    for (TopExp_Explorer ex(body, TopAbs_EDGE); ex.More(); ex.Next())
        edges.push_back(TopoDS::Edge(ex.Current()));
    if (edges.empty()) return {false, "body " + std::to_string(bodyId) + " has no edges"};
    auto op = std::make_unique<ChamferOp>();
    op->setBody(bodyId);
    op->setEdges(edges);
    op->setDistance(distance);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "chamfer failed - the distance is likely too large for this "
                       "body's smallest edge or face; try a smaller distance"};
    ctx.markMeshesDirty();
    return {true, "Chamfered all edges of body " + std::to_string(bodyId) +
                  " at distance " + std::to_string(distance) + "mm"};
}

// The face on `body` whose outward normal points most nearly along `dir`
// (world space), or a null face if none is even roughly aligned. Ties (more
// than one face facing that way) go to the largest by area - shell_body's
// schema documents this so the model isn't surprised by which one opens.
TopoDS_Face largestFaceFacing(const TopoDS_Shape& body, const gp_Dir& dir) {
    TopoDS_Face best;
    double bestArea = -1.0;
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face f = TopoDS::Face(ex.Current());
        try {
            BRepGProp_Face gf(f);
            Standard_Real u0, u1, v0, v1;
            gf.Bounds(u0, u1, v0, v1);
            gp_Pnt p;
            gp_Vec n;
            gf.Normal(0.5 * (u0 + u1), 0.5 * (v0 + v1), p, n);
            if (n.Magnitude() < 1e-9) continue;
            if (gp_Dir(n).Angle(dir) > 20.0 * M_PI / 180.0) continue; // not facing that way
            GProp_GProps g;
            BRepGProp::SurfaceProperties(f, g);
            if (g.Mass() > bestArea) { bestArea = g.Mass(); best = f; }
        } catch (...) { continue; }
    }
    return best;
}

// User (x,y,z) -> world (x,z,y): Z is up, Y is depth - the same convention
// add_box/move_body/rotate_body already use (see PrimitiveOp.cpp's worldPnt()
// and moveBody's comment above). open_face is specified in this same
// user-facing convention, so it's remapped here before searching the body.
gp_Dir userDirToWorld(double ux, double uy, double uz) {
    return gp_Dir(ux, uz, uy);
}

ToolResult shellBody(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int bodyId;
    double thickness;
    if (!requireBodyId(ctx.document(), args, "body_id", bodyId, err) ||
        !requirePositive(args, "thickness", thickness, err))
        return {false, err};
    std::string openFace = "none";
    if (args.contains("open_face")) {
        if (!args["open_face"].is_string())
            return {false, "'open_face' must be a string"};
        openFace = args["open_face"].get<std::string>();
    }
    gp_Dir dir(0, 0, 1); // placeholder; only read when openFace != "none"
    if (openFace != "none") {
        if (openFace == "+x") dir = userDirToWorld(1, 0, 0);
        else if (openFace == "-x") dir = userDirToWorld(-1, 0, 0);
        else if (openFace == "+y") dir = userDirToWorld(0, 1, 0);
        else if (openFace == "-y") dir = userDirToWorld(0, -1, 0);
        else if (openFace == "+z") dir = userDirToWorld(0, 0, 1);
        else if (openFace == "-z") dir = userDirToWorld(0, 0, -1);
        else return {false, "'open_face' must be one of: +x,-x,+y,-y,+z,-z,none"};
    }
    auto op = std::make_unique<ShellOp>();
    op->setBody(bodyId);
    op->setThickness(thickness);
    if (openFace != "none") {
        const TopoDS_Shape& body = ctx.document().getBody(bodyId);
        const TopoDS_Face face = largestFaceFacing(body, dir);
        if (face.IsNull())
            return {false, "no face on body " + std::to_string(bodyId) +
                          " faces direction '" + openFace + "'"};
        op->addFaceToRemove(face);
    }
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "shell failed - the thickness is likely too large (it must be "
                       "smaller than the body's smallest wall/radius); try a smaller "
                       "thickness"};
    ctx.markMeshesDirty();
    return {true, "Shelled body " + std::to_string(bodyId) + " to " +
                  std::to_string(thickness) + "mm walls" +
                  (openFace == "none" ? " (fully closed)" : (", open on " + openFace))};
}

} // namespace

ToolResult executeTool(PluginContext& ctx, const std::string& toolName,
                       const nlohmann::json& args) {
    if (toolName == "add_box") return addPrimitive(ctx, PrimitiveOp::Kind::Box, args);
    if (toolName == "add_cylinder") return addPrimitive(ctx, PrimitiveOp::Kind::Cylinder, args);
    if (toolName == "add_sphere") return addPrimitive(ctx, PrimitiveOp::Kind::Sphere, args);
    if (toolName == "add_cone") return addPrimitive(ctx, PrimitiveOp::Kind::Cone, args);
    if (toolName == "add_torus") return addPrimitive(ctx, PrimitiveOp::Kind::Torus, args);
    if (toolName == "move_body") return moveBody(ctx, args);
    if (toolName == "rotate_body") return rotateBody(ctx, args);
    if (toolName == "scale_body") return scaleBody(ctx, args);
    if (toolName == "boolean_op") return booleanOp(ctx, args);
    if (toolName == "fillet_all_edges") return filletAllEdges(ctx, args);
    if (toolName == "chamfer_all_edges") return chamferAllEdges(ctx, args);
    if (toolName == "shell_body") return shellBody(ctx, args);
    return {false, "unknown tool '" + toolName + "'"};
}

} } // namespace materializr::ai
