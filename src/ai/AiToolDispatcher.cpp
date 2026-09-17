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

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRep_Tool.hxx>
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

// Same remap as userDirToWorld, for a POSITION rather than a direction -
// used by fillet_edge/chamfer_edge to turn the model's "approximately here"
// point into world space before searching for the nearest edge.
gp_Pnt userPntToWorld(double ux, double uy, double uz) {
    return gp_Pnt(ux, uz, uy);
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
    auto op = std::make_unique<FilletOp>();
    op->setBody(bodyId);
    op->setEdges({edge});
    op->setRadius(radius);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "fillet failed - the radius is likely too large for this edge "
                       "or its neighbouring faces; try a smaller radius"};
    ctx.markMeshesDirty();
    return {true, "Filleted the edge of body " + std::to_string(bodyId) +
                  " nearest (" + std::to_string(ux) + ", " + std::to_string(uy) + ", " +
                  std::to_string(uz) + ") at radius " + std::to_string(radius) + "mm"};
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
    auto op = std::make_unique<ChamferOp>();
    op->setBody(bodyId);
    op->setEdges({edge});
    op->setDistance(distance);
    if (!ctx.history().pushOperation(std::move(op), ctx.document()))
        return {false, "chamfer failed - the distance is likely too large for this "
                       "edge or its neighbouring faces; try a smaller distance"};
    ctx.markMeshesDirty();
    return {true, "Chamfered the edge of body " + std::to_string(bodyId) +
                  " nearest (" + std::to_string(ux) + ", " + std::to_string(uy) + ", " +
                  std::to_string(uz) + ") at distance " + std::to_string(distance) + "mm"};
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
        return {true, "Created body " + std::to_string(newId) + " by extrusion"};
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
    if (toolName == "fillet_edge") return filletEdge(ctx, args);
    if (toolName == "chamfer_edge") return chamferEdge(ctx, args);
    if (toolName == "shell_body") return shellBody(ctx, args);
    if (toolName == "push_pull_face") return pushPullFace(ctx, args);
    if (toolName == "extrude_rect") return extrudeRect(ctx, args);
    if (toolName == "extrude_circle") return extrudeCircle(ctx, args);
    return {false, "unknown tool '" + toolName + "'"};
}

} } // namespace materializr::ai
