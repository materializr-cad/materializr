#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Vec.hxx>
#include <vector>
#include <string>

// Slide a THROUGH-HOLE laterally across the face it pierces - round, square,
// polygon, any straight prismatic section. Reuses the Move button + gizmo, but
// under the hood it's a boolean re-cut, NOT a face loft: it fills the hole back
// to solid where it was and cuts an identical hole at the new position
//   result = (body ∪ void) − translate(void, move)
// where `void` is the exact hole solid (the opening profile extruded through the
// body). No opposite-cap heuristic, so it never fills cavities the way a face
// loft can. Pockets (blind holes) are detected and REFUSED for now - moving a
// pocket is a separate feature (the cap/depth need their own handling).
class MoveHoleOp : public Operation {
public:
    MoveHoleOp() = default;
    ~MoveHoleOp() override = default;

    void setBody(int bodyId) { m_bodyId = bodyId; }
    // A wall face of the hole the user clicked (one cylindrical face for a round
    // hole, one of the flats for a square/polygon hole - the rest are gathered).
    void setSeedWall(const TopoDS_Face& wall) { m_seedWall = wall; }
    void setMoveVector(const gp_Vec& v) { m_move = v; }

    // Slide moves the whole hole: both rims travel together and the bore stays
    // parallel to where it was. Tilt pins the FAR rim and moves only the near
    // one, so the bore goes oblique - "move the top edge and leave the bottom
    // where it is". Same re-cut either way; only the replacement void differs,
    // which is why they share an op.
    // EdgeMove reshapes ONE straight side of the near rim: the grabbed edge
    // slides and its two neighbours extend or shrink to meet it, exactly like
    // dragging a line in a sketch. The far rim keeps its shape, so the bore
    // becomes a loft between two different profiles. Refused unless every side
    // of the rim is straight - see editRimWire.
    enum class Mode { Slide, Tilt, EdgeMove };
    void setMode(Mode m) { m_mode = m; }
    // Which rim edge EdgeMove drags. Ignored by the other modes.
    void setRimEdge(const TopoDS_Edge& e) { m_rimEdge = e; }
    Mode mode() const { return m_mode; }
    // Which mouth the user actually grabbed. buildVoid names the two rims
    // "entry" and "exit" by its OWN walk order, which has nothing to do with
    // which one was clicked - so Tilt and EdgeMove must be told, or they move
    // the far rim and pin the near one (looks plausible, leans the wrong way).
    void setNearIsEntry(bool nearIsEntry) { m_nearIsEntry = nearIsEntry; }
    bool nearIsEntry() const { return m_nearIsEntry; }

    int getBodyId() const { return m_bodyId; }
    gp_Vec getMoveVector() const { return m_move; }
    // True if the last execute() refused because the selected hole is a pocket.
    bool wasPocket() const { return m_wasPocket; }
    const TopoDS_Shape& getPreviousShape() const { return m_previousShape; }

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Move Hole"; }
    std::string description() const override;
    void renderProperties() override {}
    std::string typeId() const override { return "move_hole"; }
    std::vector<int> plannedBodyIds() const override { return {m_bodyId}; }
    OperationDiff captureDiff() const override;
    // Reload support: the seed wall persists as an ordinal index into the input
    // shape's canonical face map (SubShapeIndex.h), the move as plain numbers -
    // so a move-hole reloads as a real, editable op instead of baked geometry.
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

    // Build the exact hole-void solid from a clicked wall face. Returns false if
    // the selection isn't a recognizable through-hole; sets isPocket=true (and
    // returns false) when it's a blind pocket. `entryNormal` is the outward
    // normal of the face the hole opens through (the plane the move slides in).
    // Static so the interactive layer can validate/preview a selection cheaply.
    // `entryOpening` (optional) receives the entry mouth's loop - the hole's top
    // rim - for the interactive move highlight. `exitOpening` (optional) receives
    // the far mouth's loop, which Tilt pins while the entry rim moves. Both were
    // always collected; only the entry one used to be handed back.
    static bool buildVoid(const TopoDS_Shape& body, const TopoDS_Face& seedWall,
                          TopoDS_Shape& voidOut, gp_Vec& entryNormal,
                          bool& isPocket, TopoDS_Wire* entryOpening = nullptr,
                          TopoDS_Wire* exitOpening = nullptr);

    // The oblique replacement void: a ruled loft from the pinned far rim to the
    // moved near rim, overshooting both faces. Null when it can't be built.
    // Slide one straight side of `rim` by `move`, re-meeting its neighbours at
    // their new intersections. Returns false - and changes nothing - when the
    // edit isn't safe to make:
    //   * any side of the rim is an arc (a slot, a rounded pocket). Extending a
    //     line to meet an arc is a different, two-solution problem, and the far
    //     rim would need its arcs moved in step; refused rather than guessed.
    //   * a neighbour is parallel to the moved side, so there is no corner.
    //   * the move turns the profile inside out, or collapses a side to nothing.
    // `why` receives a user-facing reason.
    static bool editRimWire(const TopoDS_Wire& rim, const TopoDS_Edge& edge,
                            const gp_Vec& move, TopoDS_Wire& out,
                            std::string* why = nullptr);

    // What a set of selected rim EDGES means. The verb comes from the
    // selection, the way a face selection already turns Move into Move Face:
    //   whole rim (a circle is one edge)  -> Tilt   (pin the far rim)
    //   one straight side of a rim        -> EdgeMove
    //   edges on both rims                -> Slide  (the whole bore)
    // Anything else - an edge that isn't a hole rim, or edges from more than
    // one hole - resolves to None, and the caller offers nothing at all rather
    // than falling back to a body move (Steve's call, 2026-08-03).
    struct EdgePick {
        bool ok = false;
        Mode mode = Mode::Slide;
        TopoDS_Face wall;       // seed wall for buildVoid
        TopoDS_Edge rimEdge;    // the dragged side, for EdgeMove
        bool nearIsEntry = true; // which mouth the picked rim is
    };
    // Resolve selected edges against `body`. Cheap enough for a per-frame
    // toolbar gate (it only runs buildVoid once a candidate wall is found).
    static EdgePick classifyRimEdges(const TopoDS_Shape& body,
                                     const std::vector<TopoDS_Edge>& picked);

    static TopoDS_Shape buildTiltedVoid(const TopoDS_Wire& entryRim,
                                        const TopoDS_Wire& exitRim,
                                        const gp_Vec& move);

private:
    int m_bodyId = -1;
    TopoDS_Face m_seedWall;
    gp_Vec m_move{0.0, 0.0, 0.0};
    Mode m_mode = Mode::Slide;
    TopoDS_Edge m_rimEdge;
    bool m_nearIsEntry = true;
    bool m_wasPocket = false;
    TopoDS_Shape m_previousShape; // for undo
    // Seed wall as ordinal index/indices into the input shape (parsed on load,
    // resolved against the reloaded previous shape in rehydrateFromReload).
    std::vector<int> m_seedWallIndices;
    // Same trick for EdgeMove's dragged rim edge, so a reshaped hole reloads as
    // a reshape instead of silently degrading to a slide.
    std::vector<int> m_rimEdgeIndices;
};
