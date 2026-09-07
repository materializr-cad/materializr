#include "core/Units.h"
#include "MoveHoleOp.h"
#include "SubShapeIndex.h"
#include "../core/Verbose.h"
#include <cstdio>
#include <cstdlib>

#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepLib.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepGProp.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <string>
#include <vector>
#include <GProp_GProps.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include "UnifyTolerance.h"
#include <TopTools_MapOfShape.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <Geom_Surface.hxx>
#include <Geom_Plane.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
// The TopTools_ListIteratorOfListOfShape typedef comes from TopTools_ListOfShape.hxx;
// the standalone <...ListIteratorOfListOfShape.hxx> shim header was removed in OCCT
// 8.0 (the vcpkg/MSVC Windows build), so include the list header instead.
#include <TopTools_ListOfShape.hxx>
#include <gp_Trsf.hxx>
#include <gp_Pnt.hxx>
#include <cstdio>
#include <cmath>

namespace {

bool isPlanar(const TopoDS_Face& f) {
    Handle(Geom_Surface) s = BRep_Tool::Surface(f);
    return !s.IsNull() && s->IsKind(STANDARD_TYPE(Geom_Plane));
}

bool wireHasEdge(const TopoDS_Wire& w, const TopoDS_Edge& e) {
    for (TopExp_Explorer ex(w, TopAbs_EDGE); ex.More(); ex.Next())
        if (ex.Current().IsSame(e)) return true;
    return false;
}

// Outward normal of a planar face at its parametric centre.
gp_Vec faceNormal(const TopoDS_Face& f) {
    BRepGProp_Face gf(f);
    double u1, u2, v1, v2;
    gf.Bounds(u1, u2, v1, v2);
    gp_Pnt p; gp_Vec n;
    gf.Normal(0.5 * (u1 + u2), 0.5 * (v1 + v2), p, n);
    if (n.Magnitude() > 1e-12) n.Normalize();
    return n;
}

// Centre of mass of a closed wire — the rim's centre, however shaped.
gp_Pnt wireCentre(const TopoDS_Wire& w) {
    GProp_GProps g;
    BRepGProp::LinearProperties(w, g);
    return g.CentreOfMass();
}

} // namespace



// Which hole do these edges belong to, and what does the selection mean?
//
// The trap here is WHICH rim was grabbed. buildVoid reports entry/exit in its
// own order, decided by how it walked the body — not by what the user clicked.
// Tilt has to pin the OTHER rim, so a Tilt that trusted buildVoid's order would
// look like it worked and lean the wrong end of the bore. So match the picked
// edges against both mouth wires and record which one they came from.
MoveHoleOp::EdgePick MoveHoleOp::classifyRimEdges(
    const TopoDS_Shape& body, const std::vector<TopoDS_Edge>& picked) {
    EdgePick r;
    if (body.IsNull() || picked.empty()) return r;

    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, edgeFaces);

    // Every candidate wall the picked edges touch. More than one distinct hole
    // means an ambiguous selection — decline.
    TopoDS_Face wall;
    TopoDS_Shape voidSolid; gp_Vec entryN; bool pocket = false;
    TopoDS_Wire entryRim, exitRim;
    // Take the first wall any picked edge touches that buildVoid accepts. Do
    // NOT try to prove later edges belong to the same hole by comparing their
    // voids — buildVoid constructs a fresh solid per call, so IsSame is never
    // true even for the same hole, and a square hole (four separate wall faces)
    // declined every time. The rim-membership loop below is the real filter:
    // an edge from another hole simply isn't on THIS hole's rims.
    for (const TopoDS_Edge& e : picked) {
        if (!edgeFaces.Contains(e)) return r;
        if (!wall.IsNull()) break;
        for (const TopoDS_Shape& fs : edgeFaces.FindFromKey(e)) {
            const TopoDS_Face f = TopoDS::Face(fs);
            TopoDS_Shape v; gp_Vec n; bool p = false;
            TopoDS_Wire en, ex;
            if (!buildVoid(body, f, v, n, p, &en, &ex)) continue;
            wall = f; voidSolid = v; entryN = n; pocket = p;
            entryRim = en; exitRim = ex;
            break;
        }
    }
    if (wall.IsNull() || entryRim.IsNull() || exitRim.IsNull()) return r;

    auto wireEdges = [](const TopoDS_Wire& w) {
        std::vector<TopoDS_Edge> out;
        for (TopExp_Explorer ex(w, TopAbs_EDGE); ex.More(); ex.Next())
            out.push_back(TopoDS::Edge(ex.Current()));
        return out;
    };
    const std::vector<TopoDS_Edge> entryEdges = wireEdges(entryRim);
    const std::vector<TopoDS_Edge> exitEdges  = wireEdges(exitRim);
    auto contains = [](const std::vector<TopoDS_Edge>& set, const TopoDS_Edge& e) {
        for (const TopoDS_Edge& x : set) if (x.IsSame(e)) return true;
        return false;
    };

    size_t onEntry = 0, onExit = 0;
    for (const TopoDS_Edge& e : picked) {
        if (contains(entryEdges, e)) ++onEntry;
        else if (contains(exitEdges, e)) ++onExit;
        else return r;                    // not a rim edge at all: offer nothing
    }

    r.wall = wall;
    if (onEntry > 0 && onExit > 0) {
        r.mode = Mode::Slide;             // grabbed both ends = move the bore
        r.ok = true;
        return r;
    }
    // NOT `near`: <windef.h> defines near/far/small as empty macros, so on
    // MSVC `const bool near = …` becomes `const bool = …` (C2513). OCCT 7.9.3
    // leaks windows.h into this TU, so it is reachable here. Same trap that
    // broke latticeAnchor's parameter (TechHQ, #76).
    const bool grabbedEntry = onEntry > 0;
    r.nearIsEntry = grabbedEntry;
    const std::vector<TopoDS_Edge>& rimEdges =
        grabbedEntry ? entryEdges : exitEdges;
    const size_t got = grabbedEntry ? onEntry : onExit;

    // Can this rim be edge-moved at all? Only if every side is straight —
    // EdgeMove refuses arcs, and it would be perverse to offer a verb that is
    // guaranteed to decline. A round rim is a single circular edge, so this
    // only ever bites on curved rims made of several arcs (a slot), where
    // "one edge picked" must still mean the rim rather than one side.
    bool rimAllStraight = true;
    for (const TopoDS_Edge& e : rimEdges) {
        BRepAdaptor_Curve c(e);
        if (c.GetType() != GeomAbs_Line) { rimAllStraight = false; break; }
    }

    if (!rimAllStraight) {
        // Curved rim: any part of it means the rim. Tilt.
        r.mode = Mode::Tilt;
        r.ok = true;
    } else if (got == rimEdges.size()) {
        r.mode = Mode::Tilt;              // the whole rim
        r.ok = true;
    } else if (got == 1) {
        r.mode = Mode::EdgeMove;          // one straight side
        r.rimEdge = picked.front();
        r.ok = true;
    }
    // Several straight sides but not all: no sensible verb. Leave ok=false.
    return r;
}

// Slide one straight side of a rim, letting its neighbours follow — the 3D
// equivalent of dragging a line in a sketch.
//
// The geometry is only line-line intersection: translate the grabbed side's
// infinite line, then re-intersect it with each neighbour's line to get the two
// new corners. Every other vertex is untouched. That is the whole trick, and it
// is why this is worth doing for straight-sided holes.
//
// It is also why arcs are refused. Extending a line to meet an ARC has two
// solutions and no obvious right answer, and on a slot the far rim's arcs would
// have to move in step or the loft pairs a straight side against a curve. That
// is a different feature, so this declines rather than guesses (Steve's call).
bool MoveHoleOp::editRimWire(const TopoDS_Wire& rim, const TopoDS_Edge& edge,
                             const gp_Vec& move, TopoDS_Wire& out,
                             std::string* why) {
    auto fail = [&](const char* msg) {
        if (why) *why = msg;
        return false;
    };
    if (rim.IsNull() || edge.IsNull()) return fail("no rim edge selected");

    // Walk the rim in order, collecting its corner points, and require every
    // side to be a straight segment.
    std::vector<gp_Pnt> pts;
    std::vector<TopoDS_Edge> edges;
    for (BRepTools_WireExplorer wx(rim); wx.More(); wx.Next()) {
        const TopoDS_Edge& e = wx.Current();
        BRepAdaptor_Curve c(e);
        if (c.GetType() != GeomAbs_Line)
            return fail("this hole has a curved side — moving one edge of it "
                        "would have to move the curves too, which isn't "
                        "supported yet. Try the whole-hole move instead.");
        edges.push_back(e);
        pts.push_back(BRep_Tool::Pnt(wx.CurrentVertex()));
    }
    const size_t n = pts.size();
    if (n < 3) return fail("the rim is too simple to reshape");

    // Which side was grabbed?
    size_t k = n;
    for (size_t i = 0; i < edges.size(); ++i)
        if (edges[i].IsSame(edge)) { k = i; break; }
    if (k == n) return fail("that edge isn't part of this hole's rim");

    // pts[i] starts edges[i]; so edge k runs pts[k] -> pts[k+1].
    const size_t iA = k, iB = (k + 1) % n;
    const size_t iPrev = (k + n - 1) % n, iNext = (iB + 1) % n;

    // 2D-safe line intersection in 3D: all four points are coplanar (a rim), so
    // solve for the parameter along each neighbour where it meets the moved line.
    auto intersect = [&](const gp_Pnt& nOuter, const gp_Pnt& nInner,
                         const gp_Pnt& mA, const gp_Pnt& mB, gp_Pnt& hit) {
        const gp_Vec d1(nOuter, nInner);      // neighbour direction
        const gp_Vec d2(mA, mB);              // moved side direction
        const gp_Vec cross = d1.Crossed(d2);
        if (cross.Magnitude() < 1e-12) return false;   // parallel: no corner
        const gp_Vec r(nOuter, mA);
        const double t = r.Crossed(d2).Dot(cross) / cross.SquareMagnitude();
        hit = nOuter.Translated(d1 * t);
        return true;
    };

    const gp_Pnt mA = pts[iA].Translated(move);
    const gp_Pnt mB = pts[iB].Translated(move);
    gp_Pnt newA, newB;
    if (!intersect(pts[iPrev], pts[iA], mA, mB, newA) ||
        !intersect(pts[iNext], pts[iB], mB, mA, newB))
        return fail("that side is parallel to the one next to it — there's no "
                    "corner for it to meet");

    // Refuse a move that turns the profile inside out or eats a whole side: each
    // neighbour must still run the same way it did before.
    auto sameSense = [](const gp_Pnt& fixed, const gp_Pnt& was, const gp_Pnt& now) {
        const gp_Vec a(fixed, was), b(fixed, now);
        return a.Magnitude() > 1e-9 && b.Magnitude() > 1e-9 && a.Dot(b) > 0.0;
    };
    if (!sameSense(pts[iPrev], pts[iA], newA) ||
        !sameSense(pts[iNext], pts[iB], newB))
        return fail("that would fold the hole through itself");

    std::vector<gp_Pnt> moved = pts;
    moved[iA] = newA;
    moved[iB] = newB;

    try {
        BRepBuilderAPI_MakePolygon poly;
        for (size_t i = 0; i < n; ++i) poly.Add(moved[i]);
        poly.Close();
        if (!poly.IsDone()) return fail("couldn't rebuild the hole outline");
        out = poly.Wire();
        return true;
    } catch (...) { return fail("couldn't rebuild the hole outline"); }
}

// The oblique void: a ruled loft from the PINNED far rim to the MOVED near rim.
//
// Ruled, not smoothed, because a hole's walls are straight — a smoothed loft
// would bow them. Lofting rim-to-rim is also why this is shape-agnostic: it
// never looks at what the profile is, so a square or slotted hole tilts by the
// same code as a round one. (Polygons need the two wires to correspond vertex
// for vertex or the loft twists; that is the open question for non-round holes,
// not the approach itself.)
//
// Both sections OVERSHOOT the faces they pass through. Ending the loft exactly
// on a face plane is a coincident-face boolean, and it does not cut cleanly:
// measured on a Ø10 hole it left a 14 mm-wide opening, the silhouette of the
// whole void rather than a hole. The overshoot runs along the TILTED axis so
// the angle the user dragged is preserved rather than subtly flattened.
TopoDS_Shape MoveHoleOp::buildTiltedVoid(const TopoDS_Wire& entryRim,
                                         const TopoDS_Wire& exitRim,
                                         const gp_Vec& move) {
    if (entryRim.IsNull() || exitRim.IsNull()) return {};
    try {
        const gp_Pnt cEntry = wireCentre(entryRim);
        const gp_Pnt cExit  = wireCentre(exitRim);
        gp_Pnt cEntryMoved = cEntry.Translated(move);
        gp_Vec axis(cExit, cEntryMoved);
        if (axis.Magnitude() < 1e-9) return {};
        axis.Normalize();

        // Overshoot proportional to the bore length, floored so a very short
        // hole still breaks its surfaces.
        const double span = gp_Vec(cExit, cEntry).Magnitude();
        const double over = std::max(0.5, span * 0.05);

        gp_Trsf tExit;  tExit.SetTranslation(-axis * over);
        gp_Trsf tEntry; tEntry.SetTranslation(move + axis * over);
        TopoDS_Wire wExit = TopoDS::Wire(
            BRepBuilderAPI_Transform(exitRim, tExit, true).Shape());
        TopoDS_Wire wEntry = TopoDS::Wire(
            BRepBuilderAPI_Transform(entryRim, tEntry, true).Shape());

        BRepOffsetAPI_ThruSections loft(/*isSolid=*/Standard_True,
                                        /*ruled=*/Standard_True);
        loft.AddWire(wExit);
        loft.AddWire(wEntry);
        loft.Build();
        if (!loft.IsDone() || loft.Shape().IsNull()) return {};
        return loft.Shape();
    } catch (const Standard_Failure& e) {
        std::fprintf(stderr, "[MoveHole] tilt loft failed: %s\n",
                     e.GetMessageString() ? e.GetMessageString() : "?");
        return {};
    } catch (...) { return {}; }
}

bool MoveHoleOp::buildVoid(const TopoDS_Shape& body, const TopoDS_Face& seedWall,
                           TopoDS_Shape& voidOut, gp_Vec& entryNormal,
                           bool& isPocket, TopoDS_Wire* entryOpening,
                           TopoDS_Wire* exitOpening) {
    isPocket = false;
    if (body.IsNull() || seedWall.IsNull()) return false;

    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, edgeFaces);

    // Gather the hole's ACTUAL interior faces (walls, cones, counterbore steps —
    // any segment), and the two outer MOUTHS the bore opens through. This is
    // section-agnostic AND profile-agnostic: it reconstructs the exact void by
    // its real faces, so a countersink (cone+shank) or a counterbore (two
    // cylinders + a step annulus) rebuilds correctly, not just a constant prism.
    //
    // BFS from the clicked wall. For each face's edge, the adjacent face is a
    // MOUTH if it's planar and the edge lies on one of its INNER wires (the bore
    // pierces it → that inner wire is the opening). Otherwise it's another
    // interior face of the hole (another wall, a cone, or a step — whose own
    // outer boundary the edge sits on) → keep walking. A pocket floor would also
    // be gathered as an interior face, leaving only ONE mouth, which we reject.
    std::vector<TopoDS_Face> walls;
    TopTools_MapOfShape inSet;
    std::vector<std::pair<TopoDS_Face, TopoDS_Wire>> mouths; // (cap face, opening loop)
    TopTools_MapOfShape mouthSeen;

    std::vector<TopoDS_Face> stack;
    stack.push_back(seedWall);
    inSet.Add(seedWall);
    walls.push_back(seedWall);
    while (!stack.empty()) {
        TopoDS_Face W = stack.back(); stack.pop_back();
        for (TopExp_Explorer ex(W, TopAbs_EDGE); ex.More(); ex.Next()) {
            const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
            if (!edgeFaces.Contains(e)) continue;
            const TopTools_ListOfShape& fl = edgeFaces.FindFromKey(e);
            for (TopTools_ListIteratorOfListOfShape it(fl); it.More(); it.Next()) {
                TopoDS_Face f = TopoDS::Face(it.Value());
                if (f.IsSame(W) || inSet.Contains(f)) continue;
                // Mouth? planar + the edge is on one of f's inner wires.
                TopoDS_Wire opening;
                if (isPlanar(f)) {
                    TopoDS_Wire outer = BRepTools::OuterWire(f);
                    if (!wireHasEdge(outer, e)) {
                        for (TopoDS_Iterator wi(f); wi.More(); wi.Next()) {
                            if (wi.Value().ShapeType() != TopAbs_WIRE) continue;
                            TopoDS_Wire w = TopoDS::Wire(wi.Value());
                            if (w.IsSame(outer)) continue;
                            if (wireHasEdge(w, e)) { opening = w; break; }
                        }
                    }
                }
                if (!opening.IsNull()) {
                    if (!mouthSeen.Contains(f)) {
                        mouthSeen.Add(f);
                        mouths.emplace_back(f, opening);
                    }
                } else {
                    inSet.Add(f);
                    walls.push_back(f);
                    stack.push_back(f);
                }
            }
        }
    }

    // A through-hole opens at exactly two mouths. Exactly ONE mouth (+ a gathered
    // floor) = a real blind hole / pocket: recognized but unsupported, so flag it
    // and let the caller explain. ZERO mouths means the clicked face isn't a hole
    // wall at all (a plain outer face / cube side); >2 is an unrecognized profile.
    // In BOTH of those, leave isPocket false so the caller falls through to
    // ordinary Move Face instead of falsely refusing it as a "pocket".
    if (mouths.size() != 2) {
        isPocket = (mouths.size() == 1);
        // Verbose-only: buildVoid is also a per-frame PROBE (the toolbar's
        // rim-edge gate), and an unconditional print here flooded the journal
        // with one refusal per candidate face per frame while a pocket's edge
        // was selected. The interactive paths toast their own explanations.
        if (materializr::isVerbose())
            std::fprintf(stderr, "[MoveHole] not a through-hole: %zu mouths%s\n",
                         mouths.size(), isPocket ? " (blind/pocket)" : "");
        return false;
    }
    entryNormal = faceNormal(mouths[0].first);
    if (entryNormal.Magnitude() < 1e-9) return false;
    if (entryOpening) *entryOpening = mouths[0].second; // the hole's top rim
    if (exitOpening)  *exitOpening  = mouths[1].second; // the far rim (Tilt pins it)

    // Sew the interior faces + a cap over each mouth opening into a closed shell,
    // then a solid — the exact hole void, whatever its axial profile. Caps reuse
    // the mouths' real inner-wire edges, so they sew to the walls seamlessly.
    BRepBuilderAPI_Sewing sew(1e-6);
    for (const auto& w : walls) sew.Add(w);
    for (const auto& m : mouths) {
        BRepBuilderAPI_MakeFace mf(m.second, Standard_True /*only plane*/);
        if (!mf.IsDone()) {
            std::fprintf(stderr, "[MoveHole] could not cap a mouth\n");
            return false;
        }
        sew.Add(mf.Face());
    }
    sew.Perform();
    TopoDS_Shape sewn = sew.SewedShape();
    if (sewn.IsNull()) { std::fprintf(stderr, "[MoveHole] sewing failed\n"); return false; }

    TopExp_Explorer shx(sewn, TopAbs_SHELL);
    if (!shx.More()) { std::fprintf(stderr, "[MoveHole] no closed shell\n"); return false; }
    BRepBuilderAPI_MakeSolid ms(TopoDS::Shell(shx.Current()));
    if (!ms.IsDone()) { std::fprintf(stderr, "[MoveHole] make solid failed\n"); return false; }
    TopoDS_Solid solid = ms.Solid();
    BRepLib::OrientClosedSolid(solid); // normalize so it's a positive-volume void
    if (!BRepCheck_Analyzer(solid).IsValid()) {
        std::fprintf(stderr, "[MoveHole] void solid invalid\n");
        return false;
    }
    voidOut = solid;
    return true;
}

bool MoveHoleOp::execute(Document& doc) {
    m_wasPocket = false;
    TopoDS_Shape body = doc.getBody(m_bodyId);
    if (body.IsNull() || m_seedWall.IsNull()) return false;
    m_previousShape = body;

    TopoDS_Shape voidSolid;
    gp_Vec entryNormal;
    TopoDS_Wire entryRim, exitRim;
    if (!buildVoid(body, m_seedWall, voidSolid, entryNormal, m_wasPocket,
                   &entryRim, &exitRim))
        return false; // pocket or unrecognized → caller toasts

    // Project the requested move onto the entry plane (a hole slides ACROSS its
    // face, never along the bore — that would just deepen/shorten it).
    gp_Vec move = m_move;
    double along = move.Dot(entryNormal);
    move -= entryNormal * along;
    if (move.Magnitude() < 1e-9) return false; // no in-plane motion

    try {
        // Fill the old hole back to solid, then cut the same void at the new spot.
        BRepAlgoAPI_Fuse fuse(body, voidSolid);
        fuse.Build();
        if (!fuse.IsDone() || fuse.Shape().IsNull()) return false;

        // buildVoid labels the mouths by its own walk order, so "entry" is not
        // necessarily the rim the user grabbed. Tilt/EdgeMove move the NEAR rim
        // and pin the far one; swap them when the pick says the near mouth is
        // the exit, or the hole leans away from the end that was dragged.
        const TopoDS_Wire& nearRim = m_nearIsEntry ? entryRim : exitRim;
        const TopoDS_Wire& farRim  = m_nearIsEntry ? exitRim  : entryRim;

        TopoDS_Shape movedVoid;
        if (m_mode == Mode::EdgeMove) {
            // Reshape the near rim, then loft it to the untouched far rim: the
            // bore runs between two different profiles. Same void recipe, so
            // the re-cut below is unchanged.
            TopoDS_Wire edited;
            std::string why;
            if (!editRimWire(nearRim, m_rimEdge, move, edited, &why)) {
                std::fprintf(stderr, "[MoveHole] edge move refused: %s\n",
                             why.c_str());
                return false;
            }
            movedVoid = buildTiltedVoid(edited, farRim, gp_Vec(0, 0, 0));
            if (movedVoid.IsNull()) {
                std::fprintf(stderr, "[MoveHole] edge move: loft failed\n");
                return false;
            }
        } else if (m_mode == Mode::Tilt) {
            movedVoid = buildTiltedVoid(nearRim, farRim, move);
            if (movedVoid.IsNull()) {
                std::fprintf(stderr, "[MoveHole] tilt: could not loft the "
                                     "oblique void\n");
                return false;
            }
        } else {
            gp_Trsf t; t.SetTranslation(move);
            movedVoid = BRepBuilderAPI_Transform(voidSolid, t, true).Shape();
        }

        BRepAlgoAPI_Cut cut(fuse.Shape(), movedVoid);
        cut.Build();
        if (!cut.IsDone() || cut.Shape().IsNull()) return false;

        TopoDS_Shape result = cut.Shape();
        // The fill-fuse leaves the patched disk as a separate face coplanar with
        // the original face, with a ghost circular edge between them. Merge same-
        // surface faces and drop the redundant seam so the old location looks
        // untouched (also tidies the new hole's edges).
        result = materializr::unifySameDomain(result, "MoveHole",
                                              /*concatBSplines=*/false);

        if (!BRepCheck_Analyzer(result).IsValid()) {
            std::fprintf(stderr, "[MoveHole] result invalid\n");
            return false;
        }
        doc.updateBody(m_bodyId, result);
        return true;
    } catch (...) {
        std::fprintf(stderr, "[MoveHole] OCCT exception\n");
        return false;
    }
}

bool MoveHoleOp::undo(Document& doc) {
    if (m_previousShape.IsNull()) return false;
    doc.updateBody(m_bodyId, m_previousShape);
    return true;
}

OperationDiff MoveHoleOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}

std::string MoveHoleOp::serializeParams() const {
    // body + move vector as plain numbers; the seed wall as an ordinal index
    // into the INPUT shape's canonical face map (see SubShapeIndex.h).
    // The MODE travels too. Without it a tilted or reshaped hole reloads as a
    // plain slide — the geometry silently changes on reopen, which is exactly
    // what full replay exists to prevent. Old blobs have no mode= and default
    // to Slide, which is what they were.
    char buf[200];
    std::snprintf(buf, sizeof(buf), "body=%d;move=%.9g,%.9g,%.9g;mode=%d;near=%d",
                  m_bodyId, m_move.X(), m_move.Y(), m_move.Z(),
                  static_cast<int>(m_mode), m_nearIsEntry ? 1 : 0);
    std::string blob = buf;
    if (!m_previousShape.IsNull() && !m_seedWall.IsNull()) {
        std::vector<TopoDS_Shape> faces{m_seedWall};
        std::string idx = SubShapeIndex::serialize(m_previousShape, faces,
                                                   TopAbs_FACE);
        if (!idx.empty()) blob += ";wall=" + idx;
    }
    if (m_mode == Mode::EdgeMove && !m_previousShape.IsNull() &&
        !m_rimEdge.IsNull()) {
        std::vector<TopoDS_Shape> edges{m_rimEdge};
        std::string idx = SubShapeIndex::serialize(m_previousShape, edges,
                                                   TopAbs_EDGE);
        if (!idx.empty()) blob += ";rim=" + idx;
    }
    return blob;
}

bool MoveHoleOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if (key == "body") { m_bodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "move") {
            double x = 0, y = 0, z = 0;
            std::sscanf(val.c_str(), "%lf,%lf,%lf", &x, &y, &z);
            m_move = gp_Vec(x, y, z);
            any = true;
        } else if (key == "wall") {
            m_seedWallIndices = SubShapeIndex::parse(val);
            any = true;
        } else if (key == "mode") {
            const int m = std::atoi(val.c_str());
            if (m >= 0 && m <= static_cast<int>(Mode::EdgeMove))
                m_mode = static_cast<Mode>(m);
            any = true;
        } else if (key == "near") {
            m_nearIsEntry = std::atoi(val.c_str()) != 0;
            any = true;
        } else if (key == "rim") {
            m_rimEdgeIndices = SubShapeIndex::parse(val);
            any = true;
        }
        pos = end + 1;
    }
    return any;
}

bool MoveHoleOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_bodyId < 0) return false;

    m_previousShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    if (m_previousShape.IsNull()) return false;

    // The seed wall must resolve against the reloaded input shape, else the BFS
    // in buildVoid can't find the hole — decline so it falls back to a baked op.
    if (m_seedWallIndices.empty()) return false;
    std::vector<TopoDS_Shape> resolved;
    if (!SubShapeIndex::resolveAll(m_previousShape, m_seedWallIndices,
                                   TopAbs_FACE, resolved) ||
        resolved.empty()) {
        return false;
    }
    m_seedWall = TopoDS::Face(resolved[0]);

    // EdgeMove additionally needs the dragged rim edge back. If it won't
    // resolve, replaying as a slide would move the whole hole somewhere the
    // user never put it — decline instead and let it reload baked.
    if (m_mode == Mode::EdgeMove) {
        std::vector<TopoDS_Shape> edges;
        if (m_rimEdgeIndices.empty() ||
            !SubShapeIndex::resolveAll(m_previousShape, m_rimEdgeIndices,
                                       TopAbs_EDGE, edges) ||
            edges.empty()) {
            return false;
        }
        m_rimEdge = TopoDS::Edge(edges[0]);
    }
    return true;
}

std::string MoveHoleOp::description() const {
    double mag = m_move.Magnitude();
    char buf[48];
    std::snprintf(buf, sizeof(buf), "Move hole %s", materializr::fmtLength(mag).c_str());
    return buf;
}
