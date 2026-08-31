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

#include <gtest/gtest.h>

#include <string>

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

TEST(BoundaryFillBounds, UnknownKeyStartingWithHOrPIsIgnoredNotFatal) {
    // Forward compatibility: an unknown field a future version adds that merely
    // starts with 'h'/'p' ("phase=...") must fall through and be ignored, the
    // way it always was. The bounds checks apply only to real h<digits>/p<digits>
    // keys. Tightening this into the branch body (rather than its condition)
    // would make an older build reject the whole operation.
    BoundaryFillOp op;
    // Well-formed apart from the unknown key: parsing must not fail on it. The
    // op still returns false overall (no brep payload here), so assert on the
    // discriminator instead: the same blob WITHOUT the unknown key behaves
    // identically.
    const bool withUnknown =
        op.deserializeParams("created=-1;np=2;phase=7;h0=0");
    BoundaryFillOp op2;
    const bool withoutUnknown =
        op2.deserializeParams("created=-1;np=2;h0=0");
    EXPECT_EQ(withUnknown, withoutUnknown);
}

TEST(LoftBounds, UnknownKeyStartingWithHIsIgnoredNotFatal) {
    LoftOp op;
    const bool withUnknown = op.deserializeParams("np=2;hint=3;h0=0");
    LoftOp op2;
    const bool withoutUnknown = op2.deserializeParams("np=2;h0=0");
    EXPECT_EQ(withUnknown, withoutUnknown);
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
