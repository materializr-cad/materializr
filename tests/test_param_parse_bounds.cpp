// Regression tests for the untrusted length/index fields in operation parameter
// blobs. A parameter blob arrives inside any shared .materializr file and inside
// autosave/recovery snapshots, so every value it supplies that SIZES or INDEXES a
// container is attacker-controlled.
//
// Three defects are covered here, all found while hardening the deserializers:
//
//  1. `(size_t)std::atoll("-1")` is SIZE_MAX, and the guard that followed it,
//     `colon + 1 + n > blob.size()`, WRAPS to `colon > blob.size()` — false — so
//     the bound passed and an arbitrary blob tail reached an OCCT reader.
//
//  2. topo::readTok used the same wrapping bound to advance its cursor. A length
//     of "-3" lands the new cursor exactly where it started, so Ref::parse spins
//     forever appending names: a hang plus unbounded growth. RefParseTerminates
//     does not merely fail if that regresses — it HANGS, and CTest's timeout
//     turns that into a red build, which is the intended signal.
//
//  3. h<N>/p<N> keys and PushPullOp's `count` were fed straight to resize()/
//     assign(), so "p2000000000=" asked for a ~200 GB allocation and idx ==
//     INT_MAX made `idx + 1` signed-overflow.

#include "modeling/ParamParse.h"
#include "modeling/TopoName.h"
#include "modeling/BoundaryFillOp.h"
#include "modeling/LoftOp.h"
#include "modeling/PushPullOp.h"
#include "modeling/ExtrudeOp.h"

#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

using materializr::readLenPrefix;
using materializr::readLenRecord;
using materializr::parseWholeInt;
using materializr::parseIndexKey;

// ── The helper itself ───────────────────────────────────────────────────────

TEST(ParamParse, AcceptsAWellFormedLength) {
    const std::string s = "5:ABCDE";
    std::size_t n = 0, payload = 0;
    ASSERT_TRUE(readLenPrefix(s, 0, 1, n, payload));
    EXPECT_EQ(n, 5u);
    EXPECT_EQ(s.substr(payload, n), "ABCDE");
}

TEST(ParamParse, RejectsNegativeLengthInsteadOfWrapping) {
    // The original defect: -1 became SIZE_MAX and the additive guard wrapped.
    const std::string s = "-1:ABCDE";
    std::size_t n = 0, payload = 0;
    EXPECT_FALSE(readLenPrefix(s, 0, 2, n, payload));
}

TEST(ParamParse, RejectsLengthOverrunningTheBuffer) {
    const std::string s = "99:ABC";
    std::size_t n = 0, payload = 0;
    EXPECT_FALSE(readLenPrefix(s, 0, 2, n, payload));
}

TEST(ParamParse, RejectsJunkAndSignsAndOverflow) {
    std::size_t n = 0, payload = 0;
    const std::string junk = "1x:AAAA";
    EXPECT_FALSE(readLenPrefix(junk, 0, 2, n, payload));
    const std::string plus = "+1:AAAA";
    EXPECT_FALSE(readLenPrefix(plus, 0, 2, n, payload));
    const std::string empty = ":AAAA";
    EXPECT_FALSE(readLenPrefix(empty, 0, 0, n, payload));
    // 20 nines overflows size_t; from_chars reports it instead of wrapping.
    const std::string huge = "99999999999999999999:AAAA";
    EXPECT_FALSE(readLenPrefix(huge, 0, 20, n, payload));
}

TEST(ParamParse, RecordCursorAlwaysAdvances) {
    // A zero-length record still has to move the cursor past "0:".
    const std::string s = "0:0:";
    std::size_t pos = 0;
    std::string tok;
    ASSERT_TRUE(readLenRecord(s, pos, tok));
    EXPECT_EQ(pos, 2u);
    ASSERT_TRUE(readLenRecord(s, pos, tok));
    EXPECT_EQ(pos, 4u);
    EXPECT_FALSE(readLenRecord(s, pos, tok));   // end of buffer
}

TEST(ParamParse, WholeIntRejectsTrailingJunk) {
    int v = 0;
    EXPECT_TRUE(parseWholeInt("12", v));
    EXPECT_EQ(v, 12);
    EXPECT_FALSE(parseWholeInt("12junk", v));   // std::atoi would return 12
    EXPECT_FALSE(parseWholeInt("", v));
    EXPECT_FALSE(parseWholeInt("99999999999999999999", v));
    // A sign parses fine — the type is signed. Range-checking is the caller's
    // job, and parseIndexKey rejects negatives itself (below).
    EXPECT_TRUE(parseWholeInt("-5", v));
    EXPECT_EQ(v, -5);
}

TEST(ParamParse, IndexKeyRejectsTrailingJunk) {
    EXPECT_EQ(parseIndexKey("h12", 1), 12);
    EXPECT_EQ(parseIndexKey("h12junk", 1), -1);  // must not read as 12
    EXPECT_EQ(parseIndexKey("h", 1), -1);
    EXPECT_EQ(parseIndexKey("h-1", 1), -1);
}

// ── The hang ────────────────────────────────────────────────────────────────

TEST(TopoNameRefParse, RefParseTerminates) {
    // "-3" is the exact cycle: colon at pos+2, payload start at pos+3, and
    // start + (size_t)(-3) == pos, so the pre-fix cursor never moved and
    // Ref::parse looped forever pushing names. If this regresses it hangs.
    const materializr::topo::Ref r =
        materializr::topo::Ref::parse("-3:AAAA");
    EXPECT_TRUE(r.names.empty());
}

TEST(TopoNameRefParse, WrappingLengthsDoNotRewindTheCursor) {
    for (const char* blob : {"-1:AAAA", "-2:AAAA", "-3:AAAA", "-4:AAAA",
                             "0:", "99:AA", "+1:AA", "x:AA"}) {
        const materializr::topo::Ref r = materializr::topo::Ref::parse(blob);
        EXPECT_TRUE(r.names.empty()) << "blob: " << blob;
    }
}

TEST(TopoNameRefParse, WellFormedRefStillRoundTrips) {
    materializr::topo::Ref in;
    in.names.push_back({ "ordinal", "7" });
    in.names.push_back({ "sketchface", "payload:with:colons" });
    const materializr::topo::Ref out =
        materializr::topo::Ref::parse(in.serialize());
    ASSERT_EQ(out.names.size(), 2u);
    EXPECT_EQ(out.names[0].scheme, "ordinal");
    EXPECT_EQ(out.names[0].payload, "7");
    EXPECT_EQ(out.names[1].scheme, "sketchface");
    EXPECT_EQ(out.names[1].payload, "payload:with:colons");
}

// ── The allocation-sizing fields ────────────────────────────────────────────

TEST(BoundaryFillBounds, HugeProfileIndexIsRejected) {
    BoundaryFillOp op;
    // Pre-fix: planes.resize(2000000001) — a ~200 GB request.
    EXPECT_FALSE(op.deserializeParams(
        "created=-1;np=2;p2000000000=0,0,0,1,0,0,0,1,0"));
    EXPECT_FALSE(op.deserializeParams(
        "created=-1;np=2;h2000000000=1"));
}

TEST(BoundaryFillBounds, IntMaxIndexIsRejectedNotOverflowed) {
    BoundaryFillOp op;
    // Pre-fix: idx + 1 on INT_MAX is signed overflow (UB).
    EXPECT_FALSE(op.deserializeParams("created=-1;np=2;h2147483647=1"));
}

TEST(BoundaryFillBounds, NonFinitePlaneIsRejected) {
    BoundaryFillOp op;
    EXPECT_FALSE(op.deserializeParams(
        "created=-1;np=2;p0=nan,0,0,1,0,0,0,1,0"));
    EXPECT_FALSE(op.deserializeParams(
        "created=-1;np=2;p0=inf,0,0,1,0,0,0,1,0"));
}

// A real BoundaryFillOp with two square profiles, so serializeParams() emits a
// COMPLETE blob (head + ";brep=<len>:<payload>") and deserializeParams() can
// actually return true. Without this the forward-compat assertions below are
// vacuous: an incomplete blob fails for its own reasons no matter what the key
// parser does.
static BoundaryFillOp makeTwoProfileOp() {
    BoundaryFillOp op;
    for (int i = 0; i < 2; ++i) {
        const gp_Ax3 ax(gp_Pnt(0, 0, 0),
                        i == 0 ? gp_Dir(0, 0, 1) : gp_Dir(1, 0, 0));
        const gp_Pln pln(ax);
        BRepBuilderAPI_MakePolygon poly;
        const gp_Pnt o = ax.Location();
        const gp_Vec dx(ax.XDirection()), dy(ax.YDirection());
        poly.Add(o.Translated(-dx * 5 - dy * 5));
        poly.Add(o.Translated( dx * 5 - dy * 5));
        poly.Add(o.Translated( dx * 5 + dy * 5));
        poly.Add(o.Translated(-dx * 5 + dy * 5));
        poly.Close();
        op.addProfile(poly.Wire(), {}, pln);
    }
    return op;
}

TEST(BoundaryFillBounds, RoundTripsAndIgnoresUnknownKeys) {
    const std::string blob = makeTwoProfileOp().serializeParams();

    // Baseline: a real blob must deserialize. If this fails the two assertions
    // below prove nothing, so assert it hard.
    BoundaryFillOp base;
    ASSERT_TRUE(base.deserializeParams(blob)) << "blob: " << blob.substr(0, 120);

    // Forward compatibility: an unknown field a future version adds that merely
    // starts with 'h'/'p' must be IGNORED, not fatal. The deserializers document
    // themselves as tolerant ("Unknown keys are ignored"), so rejecting these
    // would make this build refuse a file written by a newer one.
    BoundaryFillOp withUnknown;
    EXPECT_TRUE(withUnknown.deserializeParams("phase=7;" + blob))
        << "an unknown key starting with 'p' must be ignored, not fatal";

    // But a MALFORMED index key is still rejected — that is the discriminating
    // half, and it is why this test is not vacuous.
    BoundaryFillOp withMalformed;
    EXPECT_FALSE(withMalformed.deserializeParams("h0junk=0;" + blob))
        << "a malformed index key must be rejected";
}

TEST(BoundaryFillBounds, DuplicateHoleKeyIsRejected) {
    const std::string blob = makeTwoProfileOp().serializeParams();
    // The real blob already carries h0= and h1=; repeating one used to silently
    // overwrite, which made the running hole total ambiguous.
    BoundaryFillOp op;
    EXPECT_FALSE(op.deserializeParams("h0=0;" + blob));
}

TEST(BoundaryFillBounds, PerProfileHoleCountIsBounded) {
    BoundaryFillOp op;
    EXPECT_FALSE(op.deserializeParams("created=-1;np=2;h0=999999"));
    BoundaryFillOp op2;
    EXPECT_FALSE(op2.deserializeParams("created=-1;np=2;h0=-1"));
}

TEST(LoftBounds, DuplicateHoleKeyIsRejected) {
    LoftOp op;
    EXPECT_FALSE(op.deserializeParams("np=2;h0=1;h0=2"));
}

TEST(LoftBounds, HugeHoleIndexIsRejected) {
    LoftOp op;
    // This site was weaker than BoundaryFill's twin: no digit guard at all, so
    // every "h*" key reached std::atoi.
    EXPECT_FALSE(op.deserializeParams("np=2;h2000000000=1"));
    EXPECT_FALSE(op.deserializeParams("np=2;h2147483647=1"));
}

TEST(PushPullBounds, HugeCountIsRejectedBeforeAllocating) {
    PushPullOp op;
    // The indexed writes below this were bounds-checked, but `count` is itself
    // the size of five assign() calls — those ran first.
    EXPECT_FALSE(op.deserializeParams("dist=1;count=2000000000"));
    EXPECT_FALSE(op.deserializeParams("dist=1;count=2147483647"));
}

TEST(PushPullBounds, ReasonableCountStillWorks) {
    PushPullOp op;
    EXPECT_TRUE(op.deserializeParams("dist=1;count=2"));
}

// ── Real BREP payload round-trip ────────────────────────────────────────────
// The ops that carry a ";brep=<len>:<raw>" payload had their length parsing
// rewritten. A brep blob is far larger than the toy strings above and contains
// newlines and colons of its own, so it is the case most likely to be broken by
// a stricter parser — and a break here is SILENT: the profile just goes missing
// and the op quietly falls back to different geometry.
TEST(ExtrudeRoundTrip, BrepProfileSurvivesSerializeDeserialize) {
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(0, 0, 0));
    poly.Add(gp_Pnt(10, 0, 0));
    poly.Add(gp_Pnt(10, 10, 0));
    poly.Add(gp_Pnt(0, 10, 0));
    poly.Close();
    ASSERT_TRUE(poly.IsDone());

    ExtrudeOp out;
    out.setProfile(poly.Wire());
    const std::string blob = out.serializeParams();
    ASSERT_NE(blob.find(";brep="), std::string::npos)
        << "fixture produced no brep payload; the test would be vacuous";

    // No public profile accessor, so assert via re-serialization. Comparing the
    // brep SECTIONS byte for byte, not merely checking one exists: a truncated
    // payload that still parsed into some smaller shape would satisfy a
    // presence check while having quietly lost geometry.
    ExtrudeOp in;
    in.deserializeParams(blob);
    const std::string again = in.serializeParams();
    ASSERT_NE(again.find(";brep="), std::string::npos)
        << "the brep payload did not survive the length-prefix parsing";
    EXPECT_EQ(again.substr(again.find(";brep=")),
              blob.substr(blob.find(";brep=")))
        << "the brep payload round-tripped but changed";
}

// ── The ref-list count budget ───────────────────────────────────────────────
// readLenRecord bounds each RECORD's length, but nothing bounded how MANY
// records a list could hold. "0:" is a well-formed zero-length record in two
// bytes, so a run of them yields one Ref per two input bytes — the one
// untrusted-input path in this area that had no count budget.

TEST(RefListBounds, RejectsAnUnboundedRunOfRecords) {
    std::string blob;
    const std::size_t over = materializr::kMaxRefsPerList + 16;
    blob.reserve(over * 2);
    for (std::size_t i = 0; i < over; ++i) blob += "0:";

    std::vector<materializr::topo::Ref> refs;
    EXPECT_FALSE(materializr::topo::parseRefList(blob, refs));
    EXPECT_TRUE(refs.empty())
        << "a refused list must not hand back a truncated set of refs";
}

TEST(RefListBounds, AcceptsAListWithinBudget) {
    // The discriminating half: the budget must not reject ordinary lists.
    std::vector<materializr::topo::Ref> refs;
    EXPECT_TRUE(materializr::topo::parseRefList("0:0:0:", refs));
    EXPECT_EQ(refs.size(), 3u);
}

TEST(RefListBounds, MalformedTailStillEndsTheListWithoutFailing) {
    // Forward compatibility is deliberately preserved: a trailing record this
    // build cannot parse ends the list, it does not fail the whole blob.
    std::vector<materializr::topo::Ref> refs;
    EXPECT_TRUE(materializr::topo::parseRefList("0:0:junk", refs));
    EXPECT_EQ(refs.size(), 2u);
}

// ── LoftOp hole children are type-checked ───────────────────────────────────

TEST(LoftBounds, NonWireHoleShapeIsRejectedNotThrown) {
    // BoundaryFillOp's twin loop checks ShapeType on every HOLE child; LoftOp's
    // checked only the profile wire. A compound whose hole slot holds a vertex
    // therefore reached TopoDS::Wire() and threw Standard_TypeMismatch straight
    // out of deserializeParams (the try/catch covers only BRepTools::Read).
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(0, 0, 0));
    poly.Add(gp_Pnt(10, 0, 0));
    poly.Add(gp_Pnt(10, 10, 0));
    poly.Close();
    ASSERT_TRUE(poly.IsDone());

    TopoDS_Compound comp;
    BRep_Builder bb;
    bb.MakeCompound(comp);
    bb.Add(comp, poly.Wire());                                 // profile: a wire
    bb.Add(comp, BRepBuilderAPI_MakeVertex(gp_Pnt(1, 1, 0)));  // hole: NOT a wire

    std::ostringstream os;
    BRepTools::Write(comp, os);
    const std::string payload = os.str();
    ASSERT_FALSE(payload.empty()) << "fixture wrote no brep; test would be vacuous";

    LoftOp op;
    const std::string blob =
        "np=1;h0=1;brep=" + std::to_string(payload.size()) + ":" + payload;
    EXPECT_FALSE(op.deserializeParams(blob));
}
