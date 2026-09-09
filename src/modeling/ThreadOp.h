#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include "TopoName.h"
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>
#include <string>
#include <memory>
#include <functional>
#include <atomic>

// Cuts a helical V-groove screw thread into a cylindrical face - external
// (boss/bolt: groove cut inward from the surface) or internal (hole/nut:
// groove cut outward into the wall), chosen from which side the material is
// on. The thread is pure derived geometry (axis + radius + extent + pitch +
// depth), no sub-shape references, so reloaded steps rehydrate fully
// editable: pitch / depth / handedness recompute via editStep.
// Cross-section family swept along the helix. Standard is the shipped,
// validated profile (untouched - the "reference part" option). The others are
// the maker/printing generalization: coarser, printer-friendly, or custom.
enum class ThreadProfile {
    Standard = 0,    // current arc profile - bit-identical shipped behaviour
    Trapezoidal,     // ACME/leadscrew: straight flanks, flat crest+root
    Square,          // near-vertical walls, equal land/groove
    Buttress,        // asymmetric: one steep flank, one shallow (high axial load)
    Rounded,         // sinusoidal-ish, easiest to print / strongest crest
};

class ThreadOp : public Operation {
public:
    ThreadOp();
    ~ThreadOp() override = default;

    // Parameters. The axis is anchored at the threaded span's V_min end with
    // its direction pointing along the cylinder (same convention as the
    // cylindrical-face detector); `length` extends from there.
    void setBody(int id) { m_bodyId = id; }
    int  getBodyId() const { return m_bodyId; }
    void setAxis(const gp_Ax2& axis);
    // The thread's CURRENT axis - kept accurate through upstream edits by
    // the face-ref / coaxial re-resolution, so it IS the body's true axis
    // (sketch-on-cap anchors at it; fitted geometry can't be trusted there).
    const gp_Ax2& getAxis() const { return m_axis; }
    void setRadius(double r) { m_radius = r; }
    void setLength(double l) { m_length = l; }
    void setPitch(double p) { m_pitch = p; }
    void setDepth(double d) { m_depth = d; }
    void setIsHole(bool h) { m_isHole = h; }
    void setRightHanded(bool rh) { m_rightHanded = rh; }

    // Generalized-thread knobs (experiment). Profile picks the cross-section
    // family; clearance is the radial fit gap for PRINTED threads (crest
    // pulled IN on external / OUT on internal so a printed bolt+nut actually
    // assemble - nozzle over-extrusion means a geometrically-exact thread
    // binds); starts is the number of interleaved helical starts (bottle
    // caps / quarter-turn closures are multi-start). All default to the
    // shipped single-start Standard behaviour.
    void setProfile(ThreadProfile p) { m_profile = p; }
    ThreadProfile getProfile() const { return m_profile; }
    void setClearance(double c) { m_clearance = c; }
    void setStarts(int n) { m_starts = n < 1 ? 1 : n; }
    // Explicit groove width in mm, decoupling the cut from the pitch. Every
    // profile otherwise sizes its groove as a FRACTION of the pitch, so a
    // coarse pitch always means a wide groove - no way to ask for a narrow
    // groove on a long lead (a helical wire seat, a grip spiral, a cable
    // channel). 0 = automatic, i.e. the profile's own fraction, which is what
    // every existing thread and every saved file keeps. Applies to the
    // straight-flanked profiles (Trapezoidal / Square / Buttress); Standard
    // and Rounded are swept forms whose shape is defined differently.
    void setGrooveWidth(double w) { m_grooveWidth = w > 0.0 ? w : 0.0; }
    double getGrooveWidth() const { return m_grooveWidth; }
    // True for the profiles setGrooveWidth() actually affects.
    static bool profileTakesGrooveWidth(ThreadProfile p) {
        return p == ThreadProfile::Trapezoidal ||
               p == ThreadProfile::Square ||
               p == ThreadProfile::Buttress;
    }
    // The groove opening this profile uses when the width is automatic, as a
    // fraction of the pitch - so the UI can show what "automatic" resolves to.
    static double profileOpenFraction(ThreadProfile p);

    // Cooperative cancel for background workers: buildResult checks the token
    // between turns/chunks and wires it into the boolean cuts as an OCCT
    // UserBreak, so even a single long boolean aborts. Set a FRESH token per
    // job (the token is not serialized and never set on history-owned ops).
    void setCancelToken(std::shared_ptr<std::atomic<bool>> t) {
        m_cancelTok = std::move(t);
    }

    // Recursion guard: the external-thread GRAFT fallback threads a clean
    // synthetic cylinder via a nested ThreadOp, which must take the direct
    // cut path and never graft again. Set false on the nested op.
    void setAllowGraft(bool a) { m_allowGraft = a; }
    // TEST hook: skip the direct/per-turn cut so the graft path always runs
    // (the real-world trigger - a body the union rebuilt so the helical cut
    // inverts - is hard to synthesize; this gives the graft deterministic
    // coverage). No effect in production, where nothing sets it.
    void setForceGraft(bool f) { m_forceGraft = f; }

    // Topological name of the target cylindrical face. When set, execute()
    // re-resolves it against the CURRENT body and re-derives axis + radius from
    // the cylinder's new geometry - so the thread FOLLOWS an upstream edit
    // (the cylinder moving or its diameter changing) instead of floating at
    // its original absolute position. Empty ref = today's absolute-param
    // behaviour. Additive: an old file has no ref and just keeps its params.
    void setTargetFaceRef(const materializr::topo::Ref& r) { m_faceRef = r; }
    const materializr::topo::Ref& targetFaceRef() const { return m_faceRef; }

    // Deferred re-cut hook, installed once by the app. When set, execute() on
    // the RECOMPUTE path (editStep / cascade replay - no worker-precomputed
    // result) hands the multi-second helix re-cut to this callback instead of
    // blocking the caller ("app goes not responding" when an upstream sketch
    // edit cascades through a Thread step). The callback returns true when it
    // took ownership: execute() then returns success with the body left at
    // its PRE-thread state, and the hook re-cuts on a worker thread and
    // updates the body when the result lands. Returning false (or no hook -
    // headless tests / CLI) keeps the synchronous path.
    static void setAsyncRecutHook(std::function<bool(ThreadOp&, Document&)> h);

    // The heavy geometry (helix sweep + boolean cut), as a pure function of
    // the input body - no Document access, so the popup can run it on a
    // worker thread while the UI keeps pumping events. Returns a null shape
    // on failure. execute() uses it directly for the synchronous paths
    // (editStep recompute, redo).
    TopoDS_Shape buildResult(const TopoDS_Shape& body) const;
    // Hand execute() a result that buildResult already produced on a worker
    // thread; consumed on the next execute() so redo/edit recompute normally.
    void setPrecomputedResult(const TopoDS_Shape& s) { m_precomputed = s; }

    // Reflow propagation: a thread retargeted at a body the reflowed op
    // created (e.g. the other half of a split). Pure parameters copy over;
    // execution state resets so the clone recomputes fresh.
    std::unique_ptr<Operation> cloneForBody(int bodyId) const override {
        auto c = std::make_unique<ThreadOp>(*this);
        c->m_bodyId = bodyId;
        c->m_previousShape.Nullify();
        c->m_precomputed.Nullify();
        return c;
    }

    // Operation interface
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Thread"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "thread"; }
    OperationDiff captureDiff() const override;
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    int m_bodyId = -1;
    gp_Ax2 m_axis;
    double m_radius = 5.0;
    double m_length = 10.0;
    double m_pitch = 1.0;
    double m_depth = 0.6;
    bool m_isHole = false;     // false: external (boss), true: internal (hole)
    bool m_rightHanded = true;
    ThreadProfile m_profile = ThreadProfile::Standard;
    double m_clearance = 0.0;   // radial fit gap (mm); 0 = geometrically exact
    double m_grooveWidth = 0.0; // explicit groove width (mm); 0 = from pitch
    int m_starts = 1;           // interleaved helical starts
    bool m_allowGraft = true;   // false on the nested op the graft spawns
    bool m_forceGraft = false;  // test-only: skip the direct cut, always graft
    std::shared_ptr<std::atomic<bool>> m_cancelTok; // per-job, not serialized

    TopoDS_Shape m_previousShape; // for undo
    TopoDS_Shape m_precomputed;   // see setPrecomputedResult()
    materializr::topo::Ref m_faceRef; // target cylinder face name (see setter)

    // Axis components for (de)serialisation; m_axis is rebuilt from these in
    // execute() so a reloaded op recomputes identically.
    double m_axOX = 0, m_axOY = 0, m_axOZ = 0;
    double m_axDX = 0, m_axDY = 0, m_axDZ = 1;
    double m_axXX = 1, m_axXY = 0, m_axXZ = 0;
};
