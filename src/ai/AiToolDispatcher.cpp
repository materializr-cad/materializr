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
#include "../modeling/ExtrudeOp.h"
#include "../modeling/PushPullOp.h"
#include "../modeling/DeleteOp.h"
#include "../modeling/CopyOp.h"
#include "../modeling/MirrorOp.h"
#include "../modeling/PatternOp.h"
#include "../modeling/LoftOp.h"

#include <BRepTools.hxx>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Circle.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Plane.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <cstdio>
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

// User (x,y,z) -> world (x,z,y): Z is up, Y is depth - the same convention
// add_box/move_body/rotate_body already use (see PrimitiveOp.cpp's worldPnt()
// and moveBody's comment above). open_face is specified in this same
// user-facing convention, so it's remapped here before searching the body.
gp_Dir userDirToWorld(double ux, double uy, double uz) {
    return gp_Dir(ux, uz, uy);
}

// Same remap as userDirToWorld, for a POSITION rather than a direction -
// used by fillet_edge/chamfer_edge to turn the model's "approximately here"
// point into world space before searching for the nearest edge.
gp_Pnt userPntToWorld(double ux, double uy, double uz) {
    return gp_Pnt(ux, uz, uy);
}

// Inverse of userPntToWorld (the remap is its own inverse: swap Y/Z back).
// Used by list_bodies (and add_* below) to report existing/just-created
// geometry's position back to the model in the same convention every tool's
// x/y/z arguments already use.
void worldPntToUser(const gp_Pnt& w, double& ux, double& uy, double& uz) {
    ux = w.X();
    uy = w.Z();
    uz = w.Y();
}

// World bbox of `shape`, reported in the user-facing convention: centre
// position and size. Shared by list_bodies and add_* (Reported live: a
// model that just created a body has no way to know where it actually
// landed - add_box's origin argument is a CORNER, not a centre, and even
// for the centre-anchored primitives the model still has to do the mental
// math itself unless told the result directly - so every add_* tool below
// hands this back immediately instead of making the model spend a
// follow-up list_bodies call just to find out what it built).
bool describeBodyBox(const TopoDS_Shape& shape, std::string& out) {
    if (shape.IsNull()) return false;
    Bnd_Box box;
    try { BRepBndLib::Add(shape, box); } catch (...) { return false; }
    if (box.IsVoid()) return false;
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    double cx, cy, cz;
    worldPntToUser(gp_Pnt((x0 + x1) / 2.0, (y0 + y1) / 2.0, (z0 + z1) / 2.0), cx, cy, cz);
    char line[192];
    std::snprintf(line, sizeof(line),
        "centered at (x=%.1f, y=%.1f, z=%.1f)mm, size (width=%.1f, depth=%.1f, height=%.1f)mm",
        cx, cy, cz, x1 - x0, z1 - z0, y1 - y0);
    out = line;
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
    std::string msg = "Created body " + std::to_string(newId);
    std::string geom;
    if (describeBodyBox(ctx.document().getBody(newId), geom)) msg += ": " + geom;
    return {true, msg};
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

ToolResult deleteBody(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int bodyId;
    if (!requireBodyId(ctx.document(), args, "body_id", bodyId, err))
        return {false, err};
    auto op = std::make_unique<DeleteOp>();
    op->setBodyId(bodyId);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "the operation failed to execute"};
    ctx.markMeshesDirty();
    return {true, "Deleted body " + std::to_string(bodyId)};
}

ToolResult duplicateBody(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int bodyId;
    double dx, dy, dz;
    if (!requireBodyId(ctx.document(), args, "body_id", bodyId, err) ||
        !optionalNumber(args, "dx", dx, 20.0, err) ||
        !optionalNumber(args, "dy", dy, 0.0, err) ||
        !optionalNumber(args, "dz", dz, 0.0, err))
        return {false, err};
    auto op = std::make_unique<CopyOp>();
    op->setSourceBodyId(bodyId);
    // Same raw-world-coordinate remap as move_body's dx/dy/dz - see its
    // comment above (CopyOp::setOffset passes straight through, like
    // TransformOp::setTranslation, unlike PrimitiveOp's origin remap).
    op->setOffset(dx, dz, dy);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "the operation failed to execute"};
    ctx.markMeshesDirty();
    int newId = ctx.document().getAllBodyIds().back();
    std::string msg = "Created body " + std::to_string(newId) + " by duplicating body " +
                      std::to_string(bodyId);
    std::string geom;
    if (describeBodyBox(ctx.document().getBody(newId), geom)) msg += ": " + geom;
    return {true, msg};
}

ToolResult mirrorBody(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int bodyId;
    if (!requireBodyId(ctx.document(), args, "body_id", bodyId, err))
        return {false, err};
    if (!args.contains("axis") || !args["axis"].is_string())
        return {false, "missing required argument 'axis'"};
    std::string axisStr = args["axis"].get<std::string>();
    // The mirror plane always passes through the world origin, with its
    // normal along the given axis - +x and -x mirror the SAME plane (a
    // reflection through the origin doesn't care about sign), so only the
    // letter matters. MirrorOp's own XY/XZ/YZ plane names are OCCT-native
    // (WORLD) axes, not this app's user-facing ones - see PrimitiveOp.cpp's
    // worldPnt() comment for why those differ (user Z=up is world Y, user
    // Y=depth is world Z). Translating through the same +x/-x/+y/-y/+z/-z
    // vocabulary every other face/direction argument here already uses
    // keeps this tool from reintroducing that exact axis-swap mistake.
    MirrorPlane plane;
    if (axisStr == "+x" || axisStr == "-x") plane = MirrorPlane::YZ;       // normal world X
    else if (axisStr == "+y" || axisStr == "-y") plane = MirrorPlane::XY;  // normal world Z (user depth)
    else if (axisStr == "+z" || axisStr == "-z") plane = MirrorPlane::XZ; // normal world Y (user up)
    else return {false, "'axis' must be one of: +x,-x,+y,-y,+z,-z"};
    bool keepOriginal = true;
    if (args.contains("keep_original")) {
        if (!args["keep_original"].is_boolean())
            return {false, "'keep_original' must be a boolean"};
        keepOriginal = args["keep_original"].get<bool>();
    }
    auto op = std::make_unique<MirrorOp>();
    op->setBody(bodyId);
    op->setPlane(plane);
    op->setKeepOriginal(keepOriginal);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "mirror failed"};
    ctx.markMeshesDirty();
    if (!keepOriginal)
        return {true, "Mirrored body " + std::to_string(bodyId) + " across the '" +
                      axisStr + "' axis (replaced in place)"};
    int newId = ctx.document().getAllBodyIds().back();
    std::string msg = "Created body " + std::to_string(newId) + " by mirroring body " +
                      std::to_string(bodyId) + " across the '" + axisStr + "' axis";
    std::string geom;
    if (describeBodyBox(ctx.document().getBody(newId), geom)) msg += ": " + geom;
    return {true, msg};
}

ToolResult patternBody(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int bodyId, count;
    if (!requireBodyId(ctx.document(), args, "body_id", bodyId, err)) return {false, err};
    double countD;
    if (!requirePositive(args, "count", countD, err)) return {false, err};
    count = static_cast<int>(countD);
    if (countD != std::floor(countD) || count < 1)
        return {false, "'count' must be a positive whole number"};
    if (!args.contains("type") || !args["type"].is_string())
        return {false, "missing required argument 'type' ('linear' or 'radial')"};
    std::string typeStr = args["type"].get<std::string>();

    auto op = std::make_unique<PatternOp>();
    op->setBody(bodyId);
    op->setCount(count);
    if (typeStr == "linear") {
        op->setType(PatternType::Linear);
        double sx, sy, sz;
        if (!requireNumber(args, "spacing_x", sx, err) ||
            !requireNumber(args, "spacing_y", sy, err) ||
            !requireNumber(args, "spacing_z", sz, err))
            return {false, err};
        // Same raw-world-coordinate remap as move_body/duplicate_body's
        // dx/dy/dz - PatternOp::setLinearSpacing passes straight through.
        op->setLinearSpacing(sx, sz, sy);
    } else if (typeStr == "radial") {
        op->setType(PatternType::Radial);
        double ax, ay, az, ox, oy, oz, totalAngle;
        if (!requireNumber(args, "axis_x", ax, err) ||
            !requireNumber(args, "axis_y", ay, err) ||
            !requireNumber(args, "axis_z", az, err) ||
            !optionalNumber(args, "origin_x", ox, 0.0, err) ||
            !optionalNumber(args, "origin_y", oy, 0.0, err) ||
            !optionalNumber(args, "origin_z", oz, 0.0, err) ||
            !optionalNumber(args, "total_angle_degrees", totalAngle, 360.0, err))
            return {false, err};
        if (ax == 0.0 && ay == 0.0 && az == 0.0)
            return {false, "'axis_x,axis_y,axis_z' must not all be zero"};
        // Same remap again - PatternOp::setRadialAxis/setRadialOrigin also
        // pass straight through to world space.
        op->setRadialAxis(ax, az, ay);
        op->setRadialOrigin(ox, oz, oy);
        op->setTotalAngle(totalAngle);
    } else {
        return {false, "'type' must be one of: linear, radial"};
    }
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "pattern failed"};
    ctx.markMeshesDirty();
    return {true, "Created " + std::to_string(count) + "-copy " + typeStr +
                  " pattern of body " + std::to_string(bodyId) +
                  " (list_bodies to see the new ids)"};
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

// The face's own outward normal at its parametric midpoint - same
// computation largestFaceFacing already does internally, but that helper
// doesn't hand the normal back out, so loft_bodies (which needs it to
// decide whether a profile wire's winding needs reversing - see its call
// site) recomputes it here.
gp_Vec faceMidpointNormal(const TopoDS_Face& f) {
    BRepGProp_Face gf(f);
    Standard_Real u0, u1, v0, v1;
    gf.Bounds(u0, u1, v0, v1);
    gp_Pnt p;
    gp_Vec n;
    gf.Normal(0.5 * (u0 + u1), 0.5 * (v0 + v1), p, n);
    return n;
}

// Parses the same +x/-x/+y/-y/+z/-z vocabulary shell_body's open_face uses,
// into a world-space direction. Shared by fillet_face_edges/chamfer_face_edges.
bool parseFaceDirection(const std::string& s, gp_Dir& out, std::string& err) {
    if (s == "+x") out = userDirToWorld(1, 0, 0);
    else if (s == "-x") out = userDirToWorld(-1, 0, 0);
    else if (s == "+y") out = userDirToWorld(0, 1, 0);
    else if (s == "-y") out = userDirToWorld(0, -1, 0);
    else if (s == "+z") out = userDirToWorld(0, 0, 1);
    else if (s == "-z") out = userDirToWorld(0, 0, -1);
    else { err = "'face' must be one of: +x,-x,+y,-y,+z,-z"; return false; }
    return true;
}

// The edges bounding `face`, exactly as they appear on it - fillet_face_edges/
// chamfer_face_edges' whole reason to exist: round/bevel a named side of a
// body (e.g. "the top edges") without the model having to guess a point near
// each individual edge the way fillet_edge/chamfer_edge require.
std::vector<TopoDS_Edge> edgesOfFace(const TopoDS_Face& face) {
    std::vector<TopoDS_Edge> edges;
    for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next())
        edges.push_back(TopoDS::Edge(ex.Current()));
    return edges;
}

ToolResult filletFaceEdges(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int bodyId;
    double radius;
    if (!requireBodyId(ctx.document(), args, "body_id", bodyId, err) ||
        !requirePositive(args, "radius", radius, err))
        return {false, err};
    if (!args.contains("face") || !args["face"].is_string())
        return {false, "missing required argument 'face'"};
    std::string faceStr = args["face"].get<std::string>();
    gp_Dir dir(0, 0, 1);
    if (!parseFaceDirection(faceStr, dir, err)) return {false, err};
    const TopoDS_Shape& body = ctx.document().getBody(bodyId);
    const TopoDS_Face face = largestFaceFacing(body, dir);
    if (face.IsNull())
        return {false, "no face on body " + std::to_string(bodyId) + " faces direction '" +
                       faceStr + "'"};
    std::vector<TopoDS_Edge> edges = edgesOfFace(face);
    if (edges.empty()) return {false, "that face has no edges"};
    auto op = std::make_unique<FilletOp>();
    op->setBody(bodyId);
    op->setEdges(edges);
    op->setRadius(radius);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "fillet failed - the radius is likely too large for this face's "
                       "smallest edge; try a smaller radius"};
    ctx.markMeshesDirty();
    return {true, "Filleted " + std::to_string(edges.size()) + " edge(s) of the '" +
                  faceStr + "' face of body " + std::to_string(bodyId) + " at radius " +
                  std::to_string(radius) + "mm"};
}

ToolResult chamferFaceEdges(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int bodyId;
    double distance;
    if (!requireBodyId(ctx.document(), args, "body_id", bodyId, err) ||
        !requirePositive(args, "distance", distance, err))
        return {false, err};
    if (!args.contains("face") || !args["face"].is_string())
        return {false, "missing required argument 'face'"};
    std::string faceStr = args["face"].get<std::string>();
    gp_Dir dir(0, 0, 1);
    if (!parseFaceDirection(faceStr, dir, err)) return {false, err};
    const TopoDS_Shape& body = ctx.document().getBody(bodyId);
    const TopoDS_Face face = largestFaceFacing(body, dir);
    if (face.IsNull())
        return {false, "no face on body " + std::to_string(bodyId) + " faces direction '" +
                       faceStr + "'"};
    std::vector<TopoDS_Edge> edges = edgesOfFace(face);
    if (edges.empty()) return {false, "that face has no edges"};
    auto op = std::make_unique<ChamferOp>();
    op->setBody(bodyId);
    op->setEdges(edges);
    op->setDistance(distance);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "chamfer failed - the distance is likely too large for this "
                       "face's smallest edge; try a smaller distance"};
    ctx.markMeshesDirty();
    return {true, "Chamfered " + std::to_string(edges.size()) + " edge(s) of the '" +
                  faceStr + "' face of body " + std::to_string(bodyId) + " at distance " +
                  std::to_string(distance) + "mm"};
}

// The curve's own parametric midpoint - not the endpoint average, which
// collapses a closed/periodic edge (a full circular rim, one vertex used at
// both ends) down to that single vertex instead of a point representative of
// the whole edge. Falls back to the endpoint average only for the rare edge
// with no 3D curve at all.
gp_Pnt edgeMidpoint(const TopoDS_Edge& e) {
    double f, l;
    Handle(Geom_Curve) c = BRep_Tool::Curve(e, f, l);
    if (!c.IsNull()) return c->Value(0.5 * (f + l));
    TopoDS_Vertex v1, v2;
    TopExp::Vertices(e, v1, v2);
    if (v1.IsNull() || v2.IsNull()) return gp_Pnt();
    return gp_Pnt(0.5 * (BRep_Tool::Pnt(v1).XYZ() + BRep_Tool::Pnt(v2).XYZ()));
}

// The edge whose midpoint is closest to `target` - how fillet_edge/
// chamfer_edge pick ONE edge out of a body without needing a "list edges"
// companion tool: the model gives an approximate world position (which it
// usually already knows, having just specified the body's own dimensions),
// the same way a person would click near the edge they mean.
TopoDS_Edge nearestEdge(const TopoDS_Shape& body, const gp_Pnt& target) {
    TopoDS_Edge best;
    double bestD2 = std::numeric_limits<double>::max();
    for (TopExp_Explorer ex(body, TopAbs_EDGE); ex.More(); ex.Next()) {
        const TopoDS_Edge e = TopoDS::Edge(ex.Current());
        if (BRep_Tool::Degenerated(e)) continue;
        const double d2 = target.SquareDistance(edgeMidpoint(e));
        if (d2 < bestD2) { bestD2 = d2; best = e; }
    }
    return best;
}

ToolResult filletEdge(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int bodyId;
    double radius, ux, uy, uz;
    if (!requireBodyId(ctx.document(), args, "body_id", bodyId, err) ||
        !requirePositive(args, "radius", radius, err) ||
        !requireNumber(args, "x", ux, err) ||
        !requireNumber(args, "y", uy, err) ||
        !requireNumber(args, "z", uz, err))
        return {false, err};
    const TopoDS_Shape& body = ctx.document().getBody(bodyId);
    const TopoDS_Edge edge = nearestEdge(body, userPntToWorld(ux, uy, uz));
    if (edge.IsNull()) return {false, "body " + std::to_string(bodyId) + " has no edges"};
    // Report back the edge ACTUALLY found, not just the point that was
    // searched from - nearestEdge always returns SOMETHING (never "no edge
    // close enough"), so a badly-off point (most often a Y/Z mixup - see the
    // system prompt) silently fillets the wrong edge instead of failing
    // where the mistake would be obvious. Comparing the requested point
    // against this is the fastest way to catch that from the tool result
    // alone, without a screenshot.
    double fx, fy, fz;
    worldPntToUser(edgeMidpoint(edge), fx, fy, fz);
    char foundAt[96];
    std::snprintf(foundAt, sizeof(foundAt), "(%.1f, %.1f, %.1f)", fx, fy, fz);
    auto op = std::make_unique<FilletOp>();
    op->setBody(bodyId);
    op->setEdges({edge});
    op->setRadius(radius);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, std::string("fillet failed on the edge found nearest (") +
                       std::to_string(ux) + ", " + std::to_string(uy) + ", " +
                       std::to_string(uz) + ") - that edge is actually at " + foundAt +
                       " - the radius is likely too large for it or its neighbouring "
                       "faces; try a smaller radius, or call list_bodies/capture_view "
                       "if that location doesn't look like the edge you meant"};
    ctx.markMeshesDirty();
    return {true, "Filleted the edge of body " + std::to_string(bodyId) +
                  " nearest (" + std::to_string(ux) + ", " + std::to_string(uy) + ", " +
                  std::to_string(uz) + ") - edge found at " + foundAt +
                  " - at radius " + std::to_string(radius) + "mm"};
}

ToolResult chamferEdge(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int bodyId;
    double distance, ux, uy, uz;
    if (!requireBodyId(ctx.document(), args, "body_id", bodyId, err) ||
        !requirePositive(args, "distance", distance, err) ||
        !requireNumber(args, "x", ux, err) ||
        !requireNumber(args, "y", uy, err) ||
        !requireNumber(args, "z", uz, err))
        return {false, err};
    const TopoDS_Shape& body = ctx.document().getBody(bodyId);
    const TopoDS_Edge edge = nearestEdge(body, userPntToWorld(ux, uy, uz));
    if (edge.IsNull()) return {false, "body " + std::to_string(bodyId) + " has no edges"};
    // See filletEdge's identical comment - report the edge ACTUALLY found,
    // since nearestEdge never fails to find one.
    double fx, fy, fz;
    worldPntToUser(edgeMidpoint(edge), fx, fy, fz);
    char foundAt[96];
    std::snprintf(foundAt, sizeof(foundAt), "(%.1f, %.1f, %.1f)", fx, fy, fz);
    auto op = std::make_unique<ChamferOp>();
    op->setBody(bodyId);
    op->setEdges({edge});
    op->setDistance(distance);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, std::string("chamfer failed on the edge found nearest (") +
                       std::to_string(ux) + ", " + std::to_string(uy) + ", " +
                       std::to_string(uz) + ") - that edge is actually at " + foundAt +
                       " - the distance is likely too large for it or its "
                       "neighbouring faces; try a smaller distance, or call "
                       "list_bodies/capture_view if that location doesn't look like "
                       "the edge you meant"};
    ctx.markMeshesDirty();
    return {true, "Chamfered the edge of body " + std::to_string(bodyId) +
                  " nearest (" + std::to_string(ux) + ", " + std::to_string(uy) + ", " +
                  std::to_string(uz) + ") - edge found at " + foundAt +
                  " - at distance " + std::to_string(distance) + "mm"};
}

// The face whose centre of mass is closest to `target` - the same
// nearest-point selection trick as nearestEdge, one level up. Works for
// planar or curved faces alike since GProp's centre of mass is a property of
// the whole face, not a parametric sample.
TopoDS_Face nearestFace(const TopoDS_Shape& body, const gp_Pnt& target) {
    TopoDS_Face best;
    double bestD2 = std::numeric_limits<double>::max();
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face f = TopoDS::Face(ex.Current());
        GProp_GProps g;
        try { BRepGProp::SurfaceProperties(f, g); } catch (...) { continue; }
        const double d2 = target.SquareDistance(g.CentreOfMass());
        if (d2 < bestD2) { bestD2 = d2; best = f; }
    }
    return best;
}

ToolResult pushPullFace(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int bodyId;
    double distance, ux, uy, uz;
    if (!requireBodyId(ctx.document(), args, "body_id", bodyId, err) ||
        !requireNumber(args, "distance", distance, err) ||
        !requireNumber(args, "x", ux, err) ||
        !requireNumber(args, "y", uy, err) ||
        !requireNumber(args, "z", uz, err))
        return {false, err};
    if (distance == 0.0) return {false, "'distance' must not be zero"};
    const TopoDS_Shape& body = ctx.document().getBody(bodyId);
    const TopoDS_Face face = nearestFace(body, userPntToWorld(ux, uy, uz));
    if (face.IsNull()) return {false, "body " + std::to_string(bodyId) + " has no faces"};
    auto op = std::make_unique<PushPullOp>();
    op->setTargets({{face, bodyId}});
    op->setDistance(distance);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "push/pull failed - a negative distance larger than the body's "
                       "own depth there will do this; try a smaller magnitude"};
    ctx.markMeshesDirty();
    return {true, "Pushed/pulled the face of body " + std::to_string(bodyId) +
                  " nearest (" + std::to_string(ux) + ", " + std::to_string(uy) + ", " +
                  std::to_string(uz) + ") by " + std::to_string(distance) +
                  "mm (positive = outward, negative = cut inward)"};
}

// u,v basis for the plane perpendicular to `dir`, and the resulting Geom_Ax3
// - shared by buildRectFace/buildCircleFace so both profile builders agree on
// where "in-plane X/Y" point for a given extrude direction.
gp_Ax3 profileFrame(const gp_Pnt& center, const gp_Dir& dir) {
    return gp_Ax3(center, dir);
}

// A rectangular face of the given width (along the frame's local X) and
// depth (local Y), centred at `center`, with outward normal exactly `dir` -
// so ExtrudeOp (which always extrudes along the profile FACE's own normal,
// regardless of its ExtrudeDirection setting - see its execute()) extrudes
// this along `dir` with no further coordination needed.
TopoDS_Face buildRectFace(const gp_Pnt& center, const gp_Dir& dir, double width, double depth) {
    const gp_Ax3 ax = profileFrame(center, dir);
    const gp_XYZ hw = ax.XDirection().XYZ() * (width / 2.0);
    const gp_XYZ hd = ax.YDirection().XYZ() * (depth / 2.0);
    const gp_Pnt p1(center.XYZ() - hw - hd), p2(center.XYZ() + hw - hd),
                p3(center.XYZ() + hw + hd), p4(center.XYZ() - hw + hd);
    BRepBuilderAPI_MakeWire mw;
    mw.Add(BRepBuilderAPI_MakeEdge(p1, p2));
    mw.Add(BRepBuilderAPI_MakeEdge(p2, p3));
    mw.Add(BRepBuilderAPI_MakeEdge(p3, p4));
    mw.Add(BRepBuilderAPI_MakeEdge(p4, p1));
    if (!mw.IsDone()) return {};
    Handle(Geom_Plane) plane = new Geom_Plane(ax);
    BRepBuilderAPI_MakeFace mf(plane, mw.Wire(), Standard_True);
    if (!mf.IsDone()) return {};
    return mf.Face();
}

// Same idea as buildRectFace but circular - outward normal exactly `dir`.
TopoDS_Face buildCircleFace(const gp_Pnt& center, const gp_Dir& dir, double radius) {
    const gp_Ax3 ax = profileFrame(center, dir);
    const gp_Circ circ(gp_Ax2(center, dir, ax.XDirection()), radius);
    const TopoDS_Edge circEdge = BRepBuilderAPI_MakeEdge(circ);
    BRepBuilderAPI_MakeWire mw(circEdge);
    if (!mw.IsDone()) return {};
    Handle(Geom_Plane) plane = new Geom_Plane(ax);
    BRepBuilderAPI_MakeFace mf(plane, mw.Wire(), Standard_True);
    if (!mf.IsDone()) return {};
    return mf.Face();
}

// Same idea as buildRectFace/buildCircleFace but an arbitrary polygon: `pts`
// are LOCAL 2D coordinates in the profile's own frame (same local-X/local-Y
// axes buildRectFace's width/depth already use), one edge per consecutive
// pair, closed from the last point back to the first. extrude_polygon's
// answer to "I need a custom cross-section, not just a rect or circle."
TopoDS_Face buildPolygonFace(const gp_Pnt& origin, const gp_Dir& dir,
                             const std::vector<std::pair<double, double>>& pts) {
    const gp_Ax3 ax = profileFrame(origin, dir);
    std::vector<gp_Pnt> worldPts;
    worldPts.reserve(pts.size());
    for (const auto& p : pts) {
        const gp_XYZ off = ax.XDirection().XYZ() * p.first + ax.YDirection().XYZ() * p.second;
        worldPts.push_back(gp_Pnt(origin.XYZ() + off));
    }
    BRepBuilderAPI_MakeWire mw;
    for (size_t i = 0; i < worldPts.size(); ++i)
        mw.Add(BRepBuilderAPI_MakeEdge(worldPts[i], worldPts[(i + 1) % worldPts.size()]));
    if (!mw.IsDone()) return {};
    Handle(Geom_Plane) plane = new Geom_Plane(ax);
    BRepBuilderAPI_MakeFace mf(plane, mw.Wire(), Standard_True);
    if (!mf.IsDone()) return {};
    return mf.Face();
}

bool parseExtrudeMode(const std::string& s, ExtrudeMode& out, std::string& err) {
    if (s == "new_body") { out = ExtrudeMode::NewBody; return true; }
    if (s == "union") { out = ExtrudeMode::Union; return true; }
    if (s == "subtract") { out = ExtrudeMode::Subtract; return true; }
    if (s == "intersect") { out = ExtrudeMode::Intersect; return true; }
    err = "'mode' must be one of: new_body, union, subtract, intersect";
    return false;
}

// Shared by extrude_rect/extrude_circle: everything except the profile shape
// itself (built by the caller).
ToolResult extrudeProfile(PluginContext& ctx, const nlohmann::json& args,
                          const TopoDS_Face& profile, double distance) {
    std::string err;
    std::string modeStr = args.value("mode", std::string("new_body"));
    ExtrudeMode mode;
    if (!parseExtrudeMode(modeStr, mode, err)) return {false, err};
    int targetId = -1;
    if (mode != ExtrudeMode::NewBody) {
        if (!requireBodyId(ctx.document(), args, "target_body_id", targetId, err))
            return {false, "mode '" + modeStr + "' needs a valid 'target_body_id': " + err};
    }
    if (profile.IsNull()) return {false, "failed to build the extrude profile"};

    auto op = std::make_unique<ExtrudeOp>();
    op->setProfile(profile);
    op->setDistance(distance);
    op->setMode(mode);
    if (mode != ExtrudeMode::NewBody) op->setTargetBody(targetId);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "extrude failed"};
    ctx.markMeshesDirty();
    if (mode == ExtrudeMode::NewBody) {
        int newId = ctx.document().getAllBodyIds().back();
        std::string msg = "Created body " + std::to_string(newId) + " by extrusion";
        std::string geom;
        if (describeBodyBox(ctx.document().getBody(newId), geom)) msg += ": " + geom;
        return {true, msg};
    }
    return {true, "Extruded (" + modeStr + ") into body " + std::to_string(targetId)};
}

ToolResult extrudeRect(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    double width, depth, distance;
    if (!requirePositive(args, "width", width, err) ||
        !requirePositive(args, "depth", depth, err) ||
        !requireNumber(args, "distance", distance, err))
        return {false, err};
    if (distance == 0.0) return {false, "'distance' must not be zero"};
    double ux, uy, uz, ddx, ddy, ddz;
    if (!optionalNumber(args, "x", ux, 0.0, err) ||
        !optionalNumber(args, "y", uy, 0.0, err) ||
        !optionalNumber(args, "z", uz, 0.0, err) ||
        !optionalNumber(args, "dir_x", ddx, 0.0, err) ||
        !optionalNumber(args, "dir_y", ddy, 0.0, err) ||
        !optionalNumber(args, "dir_z", ddz, 1.0, err))
        return {false, err};
    gp_Dir dir;
    try { dir = userDirToWorld(ddx, ddy, ddz); }
    catch (...) { return {false, "'dir_x,dir_y,dir_z' must not all be zero"}; }
    const TopoDS_Face profile = buildRectFace(userPntToWorld(ux, uy, uz), dir, width, depth);
    return extrudeProfile(ctx, args, profile, distance);
}

ToolResult extrudeCircle(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    double radius, distance;
    if (!requirePositive(args, "radius", radius, err) ||
        !requireNumber(args, "distance", distance, err))
        return {false, err};
    if (distance == 0.0) return {false, "'distance' must not be zero"};
    double ux, uy, uz, ddx, ddy, ddz;
    if (!optionalNumber(args, "x", ux, 0.0, err) ||
        !optionalNumber(args, "y", uy, 0.0, err) ||
        !optionalNumber(args, "z", uz, 0.0, err) ||
        !optionalNumber(args, "dir_x", ddx, 0.0, err) ||
        !optionalNumber(args, "dir_y", ddy, 0.0, err) ||
        !optionalNumber(args, "dir_z", ddz, 1.0, err))
        return {false, err};
    gp_Dir dir;
    try { dir = userDirToWorld(ddx, ddy, ddz); }
    catch (...) { return {false, "'dir_x,dir_y,dir_z' must not all be zero"}; }
    const TopoDS_Face profile = buildCircleFace(userPntToWorld(ux, uy, uz), dir, radius);
    return extrudeProfile(ctx, args, profile, distance);
}

// 'points' rides as a JSON-array-shaped STRING (not a native array/object
// argument - the tool schema here only has number/string/boolean params, the
// same constraint tool_calls.arguments itself is under), e.g.
// "[[0,0],[10,0],[10,5],[0,5]]". Parsed once here; every caller gets the
// same validation and the same error message shape.
bool parsePointsArg(const nlohmann::json& args, std::vector<std::pair<double, double>>& out,
                    std::string& err) {
    if (!args.contains("points") || !args["points"].is_string()) {
        err = "missing required argument 'points' (a JSON array string of [x,y] pairs, "
              "e.g. \"[[0,0],[10,0],[10,5],[0,5]]\")";
        return false;
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(args["points"].get<std::string>());
    } catch (const nlohmann::json::parse_error&) {
        err = "'points' must be valid JSON, e.g. \"[[0,0],[10,0],[10,5],[0,5]]\"";
        return false;
    }
    if (!parsed.is_array() || parsed.size() < 3) {
        err = "'points' must be a JSON array of at least 3 [x,y] pairs";
        return false;
    }
    out.clear();
    for (const auto& p : parsed) {
        if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) {
            err = "each entry in 'points' must be a [x,y] pair of numbers";
            return false;
        }
        out.emplace_back(p[0].get<double>(), p[1].get<double>());
    }
    return true;
}

ToolResult extrudePolygon(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    double distance;
    if (!requireNumber(args, "distance", distance, err)) return {false, err};
    if (distance == 0.0) return {false, "'distance' must not be zero"};
    std::vector<std::pair<double, double>> pts;
    if (!parsePointsArg(args, pts, err)) return {false, err};
    double ux, uy, uz, ddx, ddy, ddz;
    if (!optionalNumber(args, "x", ux, 0.0, err) ||
        !optionalNumber(args, "y", uy, 0.0, err) ||
        !optionalNumber(args, "z", uz, 0.0, err) ||
        !optionalNumber(args, "dir_x", ddx, 0.0, err) ||
        !optionalNumber(args, "dir_y", ddy, 0.0, err) ||
        !optionalNumber(args, "dir_z", ddz, 1.0, err))
        return {false, err};
    gp_Dir dir;
    try { dir = userDirToWorld(ddx, ddy, ddz); }
    catch (...) { return {false, "'dir_x,dir_y,dir_z' must not all be zero"}; }
    const TopoDS_Face profile = buildPolygonFace(userPntToWorld(ux, uy, uz), dir, pts);
    if (profile.IsNull())
        return {false, "failed to build a face from 'points' - check they form a simple "
                       "(non-self-intersecting) closed polygon"};
    return extrudeProfile(ctx, args, profile, distance);
}

ToolResult loftBodies(PluginContext& ctx, const nlohmann::json& args) {
    std::string err;
    int fromId, toId;
    if (!requireBodyId(ctx.document(), args, "from_body_id", fromId, err) ||
        !requireBodyId(ctx.document(), args, "to_body_id", toId, err))
        return {false, err};
    if (fromId == toId)
        return {false, "'from_body_id' and 'to_body_id' must be different bodies"};
    if (!args.contains("from_face") || !args["from_face"].is_string())
        return {false, "missing required argument 'from_face'"};
    if (!args.contains("to_face") || !args["to_face"].is_string())
        return {false, "missing required argument 'to_face'"};
    std::string fromFaceStr = args["from_face"].get<std::string>();
    std::string toFaceStr = args["to_face"].get<std::string>();
    gp_Dir fromDir(0, 0, 1), toDir(0, 0, 1);
    if (!parseFaceDirection(fromFaceStr, fromDir, err)) return {false, err};
    if (!parseFaceDirection(toFaceStr, toDir, err)) return {false, err};

    const TopoDS_Face fromFace = largestFaceFacing(ctx.document().getBody(fromId), fromDir);
    if (fromFace.IsNull())
        return {false, "no face on body " + std::to_string(fromId) + " faces direction '" +
                       fromFaceStr + "'"};
    const TopoDS_Face toFace = largestFaceFacing(ctx.document().getBody(toId), toDir);
    if (toFace.IsNull())
        return {false, "no face on body " + std::to_string(toId) + " faces direction '" +
                       toFaceStr + "'"};

    bool solid = true;
    if (args.contains("solid")) {
        if (!args["solid"].is_boolean()) return {false, "'solid' must be a boolean"};
        solid = args["solid"].get<bool>();
    }

    TopoDS_Wire fromWire = BRepTools::OuterWire(fromFace);
    TopoDS_Wire toWire = BRepTools::OuterWire(toFace);
    // The two faces almost always face TOWARD each other (a "top" paired
    // with a "bottom", mating sides, ...), so their outward normals are
    // close to ANTI-parallel - and OuterWire()'s natural winding on each
    // face (which follows that face's own normal via the right-hand rule)
    // then winds the two wires OPPOSITE ways as seen along the loft
    // direction, twisting the result into a self-intersecting bowtie.
    // Reverse one wire whenever the normals are more anti-parallel than
    // perpendicular, so ThruSections gets consistent winding instead.
    if (faceMidpointNormal(fromFace).Dot(faceMidpointNormal(toFace)) < 0.0)
        toWire.Reverse();
    auto op = std::make_unique<LoftOp>();
    op->addProfile(fromWire);
    op->addProfile(toWire);
    op->setSolid(solid);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "loft failed - the two faces may be too different in shape or "
                       "position for a clean loft (badly twisted or self-intersecting "
                       "result); try different faces or bodies"};
    ctx.markMeshesDirty();
    int newId = ctx.document().getAllBodyIds().back();
    std::string msg = "Created body " + std::to_string(newId) + " by lofting from body " +
                      std::to_string(fromId) + "'s '" + fromFaceStr + "' face to body " +
                      std::to_string(toId) + "'s '" + toFaceStr + "' face";
    std::string geom;
    if (describeBodyBox(ctx.document().getBody(newId), geom)) msg += ": " + geom;
    return {true, msg};
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

// Read-only, no arguments. Without this the model has no way to discover
// what already exists in the document - every other tool that targets a
// body (move_body, boolean_op, push_pull_face, fillet_edge, shell_body, ...)
// needs a numeric body_id/target_body_id, and the model can otherwise only
// know one by having created it itself earlier in THIS conversation. Asked
// to edit something that predates the chat (or that another tool call made
// without the model noticing the id), it had no way to find it - the
// reported real-world symptom this tool exists to fix.
ToolResult listBodies(PluginContext& ctx) {
    Document& doc = ctx.document();
    std::vector<int> ids = doc.getAllBodyIds();
    if (ids.empty()) return {true, "No bodies in the document yet."};

    std::string msg = "Bodies in the document (x/y/z in the usual "
                      "X=width/Y=depth/Z=up convention):\n";
    for (int id : ids) {
        std::string geom;
        if (!describeBodyBox(doc.getBody(id), geom)) continue;
        msg += "  id " + std::to_string(id) + " \"" + doc.getBodyName(id) + "\": " + geom +
               (doc.isBodyVisible(id) ? "" : " [hidden]") + "\n";
    }
    return {true, msg};
}

// The only read-only tool: no arguments, no Document/History mutation. The
// image rides in ToolResult::imagePng - AiSessionController carries it into
// a ChatMessage, and each LLM client shapes it into its own wire format (see
// AnthropicClient/OpenAiCompatibleClient buildRequestBody).
ToolResult captureView(PluginContext& ctx) {
    std::vector<uint8_t> png;
    if (!ctx.captureViewportPng(png) || png.empty())
        return {false, "failed to capture the viewport"};
    ToolResult result;
    result.ok = true;
    result.message = "Captured the current view";
    result.imagePng = std::move(png);
    return result;
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
    if (toolName == "delete_body") return deleteBody(ctx, args);
    if (toolName == "duplicate_body") return duplicateBody(ctx, args);
    if (toolName == "mirror_body") return mirrorBody(ctx, args);
    if (toolName == "pattern_body") return patternBody(ctx, args);
    if (toolName == "fillet_all_edges") return filletAllEdges(ctx, args);
    if (toolName == "chamfer_all_edges") return chamferAllEdges(ctx, args);
    if (toolName == "fillet_face_edges") return filletFaceEdges(ctx, args);
    if (toolName == "chamfer_face_edges") return chamferFaceEdges(ctx, args);
    if (toolName == "fillet_edge") return filletEdge(ctx, args);
    if (toolName == "chamfer_edge") return chamferEdge(ctx, args);
    if (toolName == "shell_body") return shellBody(ctx, args);
    if (toolName == "push_pull_face") return pushPullFace(ctx, args);
    if (toolName == "extrude_rect") return extrudeRect(ctx, args);
    if (toolName == "extrude_circle") return extrudeCircle(ctx, args);
    if (toolName == "extrude_polygon") return extrudePolygon(ctx, args);
    if (toolName == "loft_bodies") return loftBodies(ctx, args);
    if (toolName == "capture_view") return captureView(ctx);
    if (toolName == "list_bodies") return listBodies(ctx);
    return {false, "unknown tool '" + toolName + "'"};
}

} } // namespace materializr::ai
