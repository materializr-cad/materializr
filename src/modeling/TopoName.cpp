#include "TopoName.h"
#include "EdgeAnchor.h"
#include "FaceAnchor.h"
#include "GenerationLedger.h"
#include "core/Document.h"
#include "ParamParse.h"
#include "modeling/Sketch.h"

#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <cstdlib>
#include <string>

namespace materializr {
namespace topo {

// ── Ref serialization ───────────────────────────────────────────────────────
// Length-prefixed tokens ("<n>:<bytes>") so any payload survives verbatim, and
// unknown schemes round-trip untouched (forward compatibility). A Ref is just
// scheme/payload token pairs concatenated.
namespace {
void writeTok(std::string& out, const std::string& s) {
    out += std::to_string(s.size());
    out += ':';
    out += s;
}
// Reads one token at pos; returns false at end/parse error.
//
// The length is parsed unsigned with full-consumption checking and bounded by
// subtraction (see ParamParse.h). The old `atoll` + `start + n > b.size()` form
// was defeated by its own wrap: a "-3" length made `pos` land back where it
// started, so Ref::parse() below looped forever appending names.
bool readTok(const std::string& b, size_t& pos, std::string& out) {
    return materializr::readLenRecord(b, pos, out);
}
} // namespace

std::string Ref::serialize() const {
    std::string out;
    for (const auto& nm : names) { writeTok(out, nm.scheme); writeTok(out, nm.payload); }
    return out;
}

bool parseRefList(const std::string& blob, std::vector<Ref>& out) {
    size_t pos = 0;
    std::string tok;
    while (materializr::readLenRecord(blob, pos, tok)) {
        // Checked BEFORE the push_back it guards, like every other budget here.
        if (out.size() >= materializr::kMaxRefsPerList) {
            out.clear();   // refuse whole, never hand back a truncated list
            return false;
        }
        out.push_back(Ref::parse(tok));
    }
    return true;
}

Ref Ref::parse(const std::string& blob) {
    Ref r;
    size_t pos = 0;
    std::string scheme, payload;
    while (readTok(blob, pos, scheme) && readTok(blob, pos, payload))
        r.names.push_back({ scheme, payload });
    return r;
}

// ── Registry ────────────────────────────────────────────────────────────────
Registry& Registry::instance() {
    static Registry r;
    return r;
}

void Registry::add(Strategy s) {
    m_strategies.push_back(std::move(s));
    std::stable_sort(m_strategies.begin(), m_strategies.end(),
                     [](const Strategy& a, const Strategy& b) {
                         return a.priority > b.priority; // most robust first
                     });
}

const Strategy* Registry::forScheme(const std::string& scheme) const {
    for (const auto& s : m_strategies) if (s.scheme == scheme) return &s;
    return nullptr;
}

// ── Built-in strategies ─────────────────────────────────────────────────────
namespace {

// FaceAnchor::SketchRef and EdgeAnchor::SketchRef are the same type
// (std::pair<int, const Sketch*>), so one list feeds both.
std::vector<FaceAnchor::SketchRef> sketchRefs(const Document* doc) {
    std::vector<FaceAnchor::SketchRef> refs;
    if (!doc) return refs;
    for (int sid : doc->getAllSketchIds()) {
        // During a cascade replay (History::editStep after a sketch edit) the
        // LIVE sketch is rolled back through its SketchEditOp snapshots, so it
        // holds a stale mid-replay state; the FINAL state is pinned as an
        // override. Prefer the override so anchors resolve against the geometry
        // the body was actually rebuilt from — otherwise a face gets matched
        // against stale sketch elements (opening vanishes / body skews).
        // Mirrors FilletOp.cpp:68.
        if (auto ov = doc->cascadeSketchOverride(sid)) refs.push_back({ sid, ov.get() });
        else if (auto sk = doc->getSketch(sid)) refs.push_back({ sid, sk.get() });
    }
    return refs;
}

// "sketchface" — generative naming via FaceAnchor. Robust to dimension edits
// (re-finds the face from the sketch element's current position). Faces only.
Strategy sketchFaceStrategy() {
    Strategy s;
    s.scheme = "sketchface";
    s.priority = 80;
    s.mint = [](const TopoDS_Shape& sub, const Context& ctx) -> std::string {
        if (ctx.type != TopAbs_FACE || sub.ShapeType() != TopAbs_FACE) return "";
        std::vector<TopoDS_Face> one{ TopoDS::Face(sub) };
        auto anchors = FaceAnchor::compute(one, sketchRefs(ctx.doc));
        if (anchors.empty() || anchors[0].kind == FaceAnchor::Anchor::None) return "";
        return FaceAnchor::serialize(anchors);
    };
    s.resolve = [](const std::string& payload, const Context& ctx) -> TopoDS_Shape {
        std::vector<FaceAnchor::Anchor> anchors;
        if (!FaceAnchor::parse(payload, anchors)) return {};
        std::vector<TopoDS_Face> out;
        if (!FaceAnchor::resolve(anchors, sketchRefs(ctx.doc), ctx.shape, out) ||
            out.empty())
            return {};
        return out[0];
    };
    s.resolveBatch = [](const std::vector<std::string>& payloads, const Context& ctx,
                        std::vector<TopoDS_Shape>& out) -> bool {
        std::vector<FaceAnchor::Anchor> anchors;
        for (const auto& p : payloads) {
            std::vector<FaceAnchor::Anchor> one;
            if (!FaceAnchor::parse(p, one) || one.empty()) return false;
            anchors.push_back(one[0]);
        }
        std::vector<TopoDS_Face> faces;
        if (!FaceAnchor::resolve(anchors, sketchRefs(ctx.doc), ctx.shape, faces))
            return false;
        out.assign(faces.begin(), faces.end());
        return out.size() == payloads.size();
    };
    return s;
}

// "sketchedge" — the existing (working) EdgeAnchor, hosted behind the registry.
// Single-edge mint/resolve; resolveBatch delegates to EdgeAnchor's native
// distinct-claim over the whole edge set. FilletOp/ChamferOp keep their own
// direct EdgeAnchor use for now; when they cut over, their on-disk `anchor=`
// blob simply becomes this scheme's payload (same format), so files stay
// compatible. Edges only.
Strategy sketchEdgeStrategy() {
    Strategy s;
    s.scheme = "sketchedge";
    s.priority = 80;
    s.mint = [](const TopoDS_Shape& sub, const Context& ctx) -> std::string {
        if (ctx.type != TopAbs_EDGE || sub.ShapeType() != TopAbs_EDGE) return "";
        std::vector<TopoDS_Edge> one{ TopoDS::Edge(sub) };
        auto anchors = EdgeAnchor::compute(one, sketchRefs(ctx.doc));
        if (anchors.empty() || anchors[0].kind == EdgeAnchor::Anchor::None) return "";
        return EdgeAnchor::serialize(anchors);
    };
    s.resolve = [](const std::string& payload, const Context& ctx) -> TopoDS_Shape {
        std::vector<EdgeAnchor::Anchor> anchors;
        if (!EdgeAnchor::parse(payload, anchors)) return {};
        std::vector<TopoDS_Edge> out;
        if (!EdgeAnchor::resolve(anchors, sketchRefs(ctx.doc), ctx.shape, out) ||
            out.empty())
            return {};
        return out[0];
    };
    s.resolveBatch = [](const std::vector<std::string>& payloads, const Context& ctx,
                        std::vector<TopoDS_Shape>& out) -> bool {
        std::vector<EdgeAnchor::Anchor> anchors;
        for (const auto& p : payloads) {
            std::vector<EdgeAnchor::Anchor> one;
            if (!EdgeAnchor::parse(p, one) || one.empty()) return false;
            anchors.push_back(one[0]);
        }
        std::vector<TopoDS_Edge> edges;
        if (!EdgeAnchor::resolve(anchors, sketchRefs(ctx.doc), ctx.shape, edges))
            return false;
        out.assign(edges.begin(), edges.end());
        return out.size() == payloads.size();
    };
    return s;
}

// "ordinal" — the universal fallback: 1-based index into
// TopExp::MapShapes(shape, type). Always mintable, resolves reliably against
// the SAME (BREP-roundtripped) shape; fails when upstream edits shift indices,
// at which point a higher-priority scheme in the Ref should have carried it.
Strategy ordinalStrategy() {
    Strategy s;
    s.scheme = "ordinal";
    s.priority = 10;
    s.rebuildSafe = false;  // an index into a REBUILT shape lands the wrong
                            // sub-shape whenever the structure shifted
    s.mint = [](const TopoDS_Shape& sub, const Context& ctx) -> std::string {
        if (ctx.shape.IsNull() || sub.IsNull()) return "";
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(ctx.shape, ctx.type, map);
        int idx = map.FindIndex(sub);
        return idx > 0 ? std::to_string(idx) : "";
    };
    s.resolve = [](const std::string& payload, const Context& ctx) -> TopoDS_Shape {
        if (ctx.shape.IsNull()) return {};
        int idx = std::atoi(payload.c_str());
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(ctx.shape, ctx.type, map);
        if (idx < 1 || idx > map.Extent()) return {};
        return map.FindKey(idx);
    };
    return s;
}

// "gen" — generation-map lineage. Names a sub-shape by its DERIVATION: which
// input sub-shape (itself named, recursively) generated/modified it, and its
// position in that input's output list. Stable across parameter edits because
// the derivation structure is invariant. The most robust scheme (priority 100)
// and the one that will eventually cover blend/boolean/loft faces no sketch
// scheme can. Requires the current op to publish ctx.gen; null -> unavailable,
// and the geometric schemes carry the ref instead.
Strategy genStrategy() {
    Strategy s;
    s.scheme = "gen";
    s.priority = 100;
    // Payload: <role>|<outIdx>|<inputIdx>|<inputName>. inputIdx selects which
    // of the op's inputs the deriving sub-shape belongs to (a boolean has two:
    // target + tool; a seam edge derives from a face of each).
    s.mint = [](const TopoDS_Shape& sub, const Context& ctx) -> std::string {
        if (!ctx.gen) return "";
        auto search = [&](const TopTools_IndexedDataMapOfShapeListOfShape& map,
                          char role) -> std::string {
            for (int i = 1; i <= map.Extent(); ++i) {
                const TopoDS_Shape& inSub = map.FindKey(i);
                int idx = 0;
                // Range-based, not TopTools_ListIteratorOfListOfShape — vcpkg
                // OCCT drops that standalone header on Windows.
                for (const TopoDS_Shape& outSub : map.FindFromIndex(i)) {
                    if (!outSub.IsSame(sub)) { ++idx; continue; }
                    const int which = ctx.gen->inputOf(inSub);
                    if (which < 0) return "";
                    // Name the INPUT sub-shape (recursively) against its own
                    // input shape — sketch-anchored inputs are edit-stable.
                    Context ic;
                    ic.doc = ctx.doc;
                    ic.shape = ctx.gen->inputs[which].shape;
                    ic.type = ctx.gen->inputs[which].type;
                    Ref inRef = mint(inSub, ic);
                    if (inRef.empty()) return "";
                    return std::string(1, role) + "|" + std::to_string(idx) +
                           "|" + std::to_string(which) + "|" + inRef.serialize();
                }
            }
            return "";
        };
        std::string r = search(ctx.gen->generated, 'G');
        if (r.empty()) r = search(ctx.gen->modified, 'M');
        return r;
    };
    s.resolve = [](const std::string& payload, const Context& ctx) -> TopoDS_Shape {
        if (!ctx.gen) return {};
        const size_t p1 = payload.find('|');
        if (p1 == std::string::npos) return {};
        const size_t p2 = payload.find('|', p1 + 1);
        if (p2 == std::string::npos) return {};
        const size_t p3 = payload.find('|', p2 + 1);
        if (p3 == std::string::npos) return {};
        const char role = payload[0];
        const int idx = std::atoi(payload.substr(p1 + 1, p2 - p1 - 1).c_str());
        const int which = std::atoi(payload.substr(p2 + 1, p3 - p2 - 1).c_str());
        if (which < 0 || which >= static_cast<int>(ctx.gen->inputs.size()))
            return {};
        const Ref inRef = Ref::parse(payload.substr(p3 + 1));   // rest = input name
        Context ic;
        ic.doc = ctx.doc;
        ic.shape = ctx.gen->inputs[which].shape;
        ic.type = ctx.gen->inputs[which].type;
        TopoDS_Shape inSub;
        if (!resolve(inRef, ic, inSub)) return {};
        const auto& map = (role == 'G') ? ctx.gen->generated : ctx.gen->modified;
        if (!map.Contains(inSub)) return {};
        int i = 0;
        for (const TopoDS_Shape& outSub : map.FindFromKey(inSub))
            if (i++ == idx) return outSub;
        return {};
    };
    return s;
}

} // namespace

Registry::Registry() {
    // Built-ins, lowest-to-highest doesn't matter — add() keeps them sorted.
    add(ordinalStrategy());
    add(sketchFaceStrategy());
    add(sketchEdgeStrategy());
    add(genStrategy());
    // Future: add(importIdStrategy()) — strictly additive.
}

// ── mint / resolve ──────────────────────────────────────────────────────────
Ref mint(const TopoDS_Shape& sub, const Context& ctx) {
    Ref r;
    for (const auto& s : Registry::instance().strategies()) {
        if (!s.mint) continue;
        std::string payload = s.mint(sub, ctx);
        if (!payload.empty()) r.names.push_back({ s.scheme, payload });
    }
    // strategies() is priority-sorted, so names come out best-first already.
    return r;
}

bool resolve(const Ref& ref, const Context& ctx, TopoDS_Shape& out) {
    for (const auto& nm : ref.names) {
        const Strategy* s = Registry::instance().forScheme(nm.scheme);
        if (!s || !s->resolve) continue;   // unknown scheme (newer file) — skip
        if (ctx.crossRebuild && !s->rebuildSafe) continue;
        TopoDS_Shape found = s->resolve(nm.payload, ctx);
        if (!found.IsNull()) { out = found; return true; }
    }
    return false;
}

namespace {
bool allDistinct(const std::vector<TopoDS_Shape>& v) {
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].IsNull()) return false;
        for (size_t j = i + 1; j < v.size(); ++j)
            if (v[i].IsSame(v[j])) return false;
    }
    return true;
}
} // namespace

bool resolveSet(const std::vector<Ref>& refs, const Context& ctx,
                std::vector<TopoDS_Shape>& out) {
    out.clear();
    if (refs.empty()) return false;

    // Best-first: the highest-priority scheme that EVERY ref carries and whose
    // batch resolver claims distinct sub-shapes for all of them.
    for (const auto& s : Registry::instance().strategies()) {
        if (!s.resolveBatch) continue;
        if (ctx.crossRebuild && !s.rebuildSafe) continue;
        std::vector<std::string> payloads;
        payloads.reserve(refs.size());
        bool allHave = true;
        for (const auto& r : refs) {
            const std::string* p = nullptr;
            for (const auto& nm : r.names)
                if (nm.scheme == s.scheme) { p = &nm.payload; break; }
            if (!p) { allHave = false; break; }
            payloads.push_back(*p);
        }
        if (!allHave) continue;
        std::vector<TopoDS_Shape> batch;
        if (s.resolveBatch(payloads, ctx, batch) &&
            batch.size() == refs.size() && allDistinct(batch)) {
            out = std::move(batch);
            return true;
        }
    }

    // Fallback: per-ref best-first resolve, then require distinctness. Sketch
    // single-resolve lacks cross-ref claiming, so the distinctness check is
    // what keeps a colliding fallback all-or-nothing (never silently wrong).
    std::vector<TopoDS_Shape> got;
    got.reserve(refs.size());
    for (const auto& r : refs) {
        TopoDS_Shape one;
        if (!resolve(r, ctx, one)) return false;
        got.push_back(one);
    }
    if (!allDistinct(got)) return false;
    out = std::move(got);
    return true;
}

} // namespace topo
} // namespace materializr
