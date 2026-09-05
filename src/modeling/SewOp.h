#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Shape.hxx>
#include <string>
#include <vector>

// Sew — stitch loose surfaces back into one body, and into a SOLID when they
// enclose something.
//
// This is the rung that was missing under Patch. A patch fills one opening; a
// space bounded by several openings needs several, and until now there was no
// way to put them together: BRepBuilderAPI_Sewing runs inside five different
// operations and was reachable from the UI in none of them. So "bound a space
// with surfaces, then make it solid" — which is how surface modelling works
// everywhere else — dead-ended at a pile of separate faces.
//
// Takes any mix of bodies. Every face of every one of them goes into one sew,
// and what comes back depends only on whether the faces actually close:
//
//   * closed  -> a SOLID, which is the point of the tool.
//   * open    -> the sewn shell, plus a count of the edges still hanging loose.
//                Strictly better than what went in (the pieces are joined where
//                they do meet) and the count says how far off the rest is.
//
// The result lands on the FIRST selected body's id, so its name, colour and
// folder survive; the others are deleted and restored by undo.
//
// TOLERANCE is a ladder, not a setting. Sewing wants the tightest value that
// closes: too loose and distinct edges within that distance are merged into one,
// which welds geometry the user did not ask to be welded. So try tight, and only
// reach for a looser one when the tight pass leaves the shell open — and report
// which rung it took, since a millimetre-scale sew is worth knowing about.
class SewOp : public Operation {
public:
    SewOp();
    ~SewOp() override = default;

    void setBodies(const std::vector<int>& ids) { m_bodyIds = ids; }
    const std::vector<int>& getBodies() const { return m_bodyIds; }

    // ── Results of the last successful execute() ──
    bool madeSolid() const { return m_madeSolid; }
    int freeEdgesLeft() const { return m_freeEdges; }   // 0 when it closed
    double toleranceUsed() const { return m_tolUsed; }
    int facesSewn() const { return m_facesSewn; }

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Sew"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "sew"; }
    OperationDiff captureDiff() const override;
    std::vector<int> plannedBodyIds() const override { return m_bodyIds; }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    std::vector<int> m_bodyIds;          // first one keeps its identity

    // Undo state: the kept body's shape before, and every consumed body's shape
    // and name so putBody can reinstate it under its own id.
    TopoDS_Shape m_previousShape;
    struct Consumed {
        int id = -1;
        TopoDS_Shape shape;
        std::string name;
        bool visible = true;
    };
    std::vector<Consumed> m_consumed;

    bool m_madeSolid = false;
    int m_freeEdges = 0;
    int m_facesSewn = 0;
    double m_tolUsed = 0.0;
};
