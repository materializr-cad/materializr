#pragma once
// Unified sub-shape naming layer (experiment/face-anchors).
//
// The whole point of this file is to reach FULL topological-naming coverage
// incrementally WITHOUT ever backtracking. It does that by decoupling three
// things that the current per-op fallback chains conflate:
//
//   1. WHAT a name is        -> topo::Name  (a scheme tag + opaque payload)
//   2. HOW MANY names a       -> topo::Ref   (an ordered list of Names; a
//      sub-shape carries                        sub-shape keeps EVERY name it
//                                               can mint, best-first)
//   3. WHO can mint/resolve   -> topo::Strategy in a priority registry
//      a given scheme
//
// An operation stores a topo::Ref and never knows or cares which scheme named
// its edge/face. Adding coverage = registering another Strategy:
//   - today:   "sketchface" (FaceAnchor), "ordinal" (universal fallback)
//   - next:    "sketchedge" (wrap the existing EdgeAnchor), curved walls
//   - later:   "gen"    — generation-map lineage (the general kernel)
//              "import" — stable ids stamped on imported/primitive faces
// New schemes are STRICTLY ADDITIVE: resolve() tries a Ref's names best-first,
// and Ref serialization preserves names whose scheme this build doesn't know
// (an older binary round-trips a newer file without dropping its richer name).
//
// This never destabilises the working EdgeAnchor/fillet path — that keeps its
// own resolution for now; when it migrates here, its on-disk `anchor=` blob
// becomes the "sketchedge" payload, so files stay compatible.

#include <TopoDS_Shape.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <functional>
#include <string>
#include <vector>

class Document;   // global namespace (see core/Document.h)

namespace materializr {
namespace topo {

struct GenerationLedger;   // GenerationLedger.h

// One naming attempt for one sub-shape: a scheme tag + that scheme's opaque
// payload (meaningful only to the strategy that owns the scheme).
struct Name {
    std::string scheme;
    std::string payload;
};

// A persistent reference to ONE sub-shape, holding every name that could be
// minted for it (kept sorted best-first). Belt-and-suspenders on purpose: a
// face may carry a "gen" lineage name AND a "sketchface" name AND an "ordinal"
// index; resolve tries them in that order so the most robust available scheme
// wins and weaker ones are the safety net.
struct Ref {
    std::vector<Name> names;

    bool empty() const { return names.empty(); }

    // Length-prefixed, delimiter-safe, and forward-compatible: names whose
    // scheme this build can't resolve still round-trip untouched.
    std::string serialize() const;
    static Ref parse(const std::string& blob);

};

// Parses a back-to-back run of length-prefixed Ref records ("<n>:<bytes>...")
// into `out`, stopping at the first malformed record. Shared by the ops that
// serialize a ref LIST (fillet/chamfer edgerefs, shell/taper facerefs), which
// each carried a byte-identical copy of this loop.
void parseRefList(const std::string& blob, std::vector<Ref>& out);

// Everything a strategy needs to mint OR resolve, for BOTH edges and faces.
// `shape` is the shape to name a sub-shape INTO (at mint) or resolve a name
// AGAINST (at resolve) — an op's input shape for edge/face refs. `doc` gives
// strategies the sketches/bodies they need. The future "gen" strategy will
// read an op-derivation ledger added here; defining the struct now means that
// addition won't change any op signature.
struct Context {
    const Document* doc = nullptr;
    TopoDS_Shape    shape;
    TopAbs_ShapeEnum type = TopAbs_FACE;   // FACE or EDGE
    // The current op execution's generation maps, when available — the "gen"
    // strategy mints/resolves through this. Null for ops that don't (yet)
    // publish their derivation; those fall back to the geometric schemes.
    const GenerationLedger* gen = nullptr;
    // TRUE when resolving against a shape that may have been REBUILT since the
    // ref was minted (an op re-finding its targets after an upstream edit).
    // Strategies that are only meaningful on the IDENTICAL shape — ordinal,
    // whose index "succeeds" on any same-count shape and silently lands the
    // WRONG sub-shape when the structure shifted — are skipped, so a resolve
    // failure surfaces to the op's own geometric fallback instead of a
    // confident wrong answer preempting it. Leave false for save/load, where
    // the shape is bit-identical and ordinal is exact.
    bool crossRebuild = false;
};

// A naming scheme. `mint` returns an empty string when it cannot name `sub`;
// `resolve` returns a null shape when it cannot find the payload's sub-shape.
// `resolveBatch` (optional) resolves a whole set of this scheme's payloads to
// DISTINCT sub-shapes at once — the set-level distinct-claim that
// EdgeAnchor/FaceAnchor already do (two fragments of one sketch line at the
// same height must land on different edges). Schemes without a natural batch
// (e.g. ordinal, whose indices are already unique) leave it null and
// resolveSet falls back to per-payload resolve().
struct Strategy {
    std::string scheme;
    int         priority = 0;   // higher = more robust; minted + tried first
    // FALSE for schemes whose payload is only meaningful against the identical
    // shape (ordinal). Such schemes are skipped when ctx.crossRebuild is set.
    bool        rebuildSafe = true;
    std::function<std::string(const TopoDS_Shape& sub, const Context&)> mint;
    std::function<TopoDS_Shape(const std::string& payload, const Context&)> resolve;
    std::function<bool(const std::vector<std::string>& payloads, const Context&,
                       std::vector<TopoDS_Shape>& out)> resolveBatch;
};

// The registry. Built-in strategies (ordinal, sketchface) self-register on
// first use; new schemes call add() (e.g. at startup). Priority-ordered.
class Registry {
public:
    static Registry& instance();
    void add(Strategy s);                       // keeps the list priority-sorted
    const std::vector<Strategy>& strategies() const { return m_strategies; }
    const Strategy* forScheme(const std::string& scheme) const;
private:
    Registry();
    std::vector<Strategy> m_strategies;
};

// Mint a Ref for `sub`: ask every registered strategy, keep each name it can
// mint (best-first). An empty Ref means nothing could name it (caller keeps
// today's behaviour — e.g. bake to ReplayOp).
Ref mint(const TopoDS_Shape& sub, const Context& ctx);

// Resolve a Ref against ctx.shape: try its names best-first, return the first
// that a known scheme resolves. Returns false (out untouched) on total miss.
bool resolve(const Ref& ref, const Context& ctx, TopoDS_Shape& out);

// Resolve a SET of Refs (an op's N edges/faces) to DISTINCT sub-shapes. Tries
// each scheme best-first: the highest-priority scheme that EVERY ref carries
// AND whose batch resolver claims distinct sub-shapes for all of them wins;
// otherwise falls to the next scheme, and finally to a per-ref best-first
// resolve with a distinctness check. All-or-nothing: returns false (out
// cleared) unless every ref resolves to a distinct sub-shape — a partial or
// colliding result would drive an op onto the wrong geometry. This is the
// op-facing entry point (FilletOp's edge set, ShellOp's open faces, …).
bool resolveSet(const std::vector<Ref>& refs, const Context& ctx,
                std::vector<TopoDS_Shape>& out);

} // namespace topo
} // namespace materializr
