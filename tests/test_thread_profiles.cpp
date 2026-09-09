// Generalized thread profiles: every cross-section family must build a VALID
// solid, external AND internal, at a coarse printed-thread pitch. Guards the
// ThreadOp profile generalization (Standard V + the maker set: Trapezoidal,
// Square, Buttress, Rounded) and the fit-clearance path.
#include <gtest/gtest.h>

#include "modeling/ThreadOp.h"

#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <gp_Ax2.hxx>

using namespace materializr;

namespace {
double vol(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}
// A tube (outer cylinder minus a coaxial bore) so the inner face can be
// threaded internally.
TopoDS_Shape tube(double rOuter, double rBore, double len) {
    TopoDS_Shape solid = BRepPrimAPI_MakeCylinder(rOuter, len).Shape();
    TopoDS_Shape bore  = BRepPrimAPI_MakeCylinder(rBore,  len).Shape();
    return BRepAlgoAPI_Cut(solid, bore).Shape();
}
void configure(ThreadOp& t, double r, double len, ThreadProfile p, double clr) {
    t.setAxis(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    t.setRadius(r);
    t.setLength(len);
    t.setPitch(3.0);       // coarse - the printed-thread case
    t.setDepth(1.2);
    t.setProfile(p);
    t.setClearance(clr);
}
} // namespace

TEST(ThreadProfiles, ExternalEachProfileValid) {
    const double R = 10.0, L = 9.0;  // 3 turns - fast
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(R, L).Shape();
    for (int i = 0; i <= static_cast<int>(ThreadProfile::Rounded); ++i) {
        ThreadOp t;
        configure(t, R, L, static_cast<ThreadProfile>(i), 0.0);
        t.setIsHole(false);
        TopoDS_Shape rod = t.buildResult(cyl);
        ASSERT_FALSE(rod.IsNull()) << "profile " << i << " built no solid";
        EXPECT_TRUE(BRepCheck_Analyzer(rod).IsValid()) << "profile " << i;
        // A thread removes material (or, for a rounded rope groove, only a
        // little) - never grows the body.
        EXPECT_LT(vol(rod), vol(cyl) + 1e-3) << "profile " << i;
        EXPECT_GT(vol(rod), 0.5 * vol(cyl)) << "profile " << i;
    }
}

TEST(ThreadProfiles, InternalEachProfileValid) {
    const double R = 10.0, L = 9.0;
    TopoDS_Shape t8 = tube(16.0, R, L);   // bore radius R = thread radius
    ASSERT_FALSE(t8.IsNull());
    for (int i = 0; i <= static_cast<int>(ThreadProfile::Rounded); ++i) {
        ThreadOp t;
        configure(t, R, L, static_cast<ThreadProfile>(i), 0.0);
        t.setIsHole(true);
        TopoDS_Shape res = t.buildResult(t8);
        ASSERT_FALSE(res.IsNull()) << "internal profile " << i << " built nothing";
        EXPECT_TRUE(BRepCheck_Analyzer(res).IsValid()) << "internal profile " << i;
    }
}

TEST(ThreadProfiles, ClearanceThinsExternalThread) {
    // A fit clearance pulls the crest in, so the cleared thread has strictly
    // less material than the exact one (it fits its mate).
    const double R = 10.0, L = 9.0;
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(R, L).Shape();
    auto build = [&](double clr) {
        ThreadOp t;
        configure(t, R, L, ThreadProfile::Trapezoidal, clr);
        t.setIsHole(false);
        return t.buildResult(cyl);
    };
    TopoDS_Shape exact = build(0.0), cleared = build(0.4);
    ASSERT_FALSE(exact.IsNull());
    ASSERT_FALSE(cleared.IsNull());
    EXPECT_LT(vol(cleared), vol(exact))
        << "clearance should remove more material (thinner thread)";
}

TEST(ThreadProfiles, MultiStartExternalValid) {
    // Bottle-cap style: 3 interleaved helixes, crest spacing = pitch, each
    // helix advancing 3 x pitch per turn. Same groove density per axial mm
    // as a single start (at any fixed angle a groove passes every pitch), so
    // the removed volume lands near the single-start figure - but the shape
    // must genuinely DIFFER (steeper helixes), which the asymmetric cut
    // check asserts.
    const double R = 10.0, L = 9.0;
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(R, L).Shape();
    auto build = [&](ThreadProfile p, int starts) {
        ThreadOp t;
        configure(t, R, L, p, 0.0);
        t.setIsHole(false);
        t.setStarts(starts);
        return t.buildResult(cyl);
    };
    for (ThreadProfile p : {ThreadProfile::Trapezoidal, ThreadProfile::Rounded}) {
        TopoDS_Shape one = build(p, 1);
        TopoDS_Shape three = build(p, 3);
        ASSERT_FALSE(one.IsNull()) << "profile " << (int)p;
        ASSERT_FALSE(three.IsNull()) << "3-start profile " << (int)p;
        EXPECT_TRUE(BRepCheck_Analyzer(three).IsValid())
            << "3-start profile " << (int)p;
        const double v1 = vol(one), v3 = vol(three), vc = vol(cyl);
        // Same crest density -> comparable removal (loose band).
        EXPECT_LT(v3, vc + 1e-3) << "3-start grew the body";
        EXPECT_GT(v3, 0.5 * vc) << "3-start gutted the body";
        EXPECT_NEAR(v3, v1, 0.35 * (vc - v1) + 1e-3)
            << "3-start removal wildly off the single-start figure";
        // The shapes must differ: material present in the single-start rod
        // but absent from the 3-start rod exists (steeper groove walls).
        TopoDS_Shape diff = BRepAlgoAPI_Cut(one, three).Shape();
        ASSERT_FALSE(diff.IsNull());
        EXPECT_GT(vol(diff), 1e-2)
            << "3-start result identical to single start - starts ignored?";
    }
}

// An explicit groove width decouples the cut from the pitch: normally the
// groove is a fixed FRACTION of the pitch, so a coarse pitch forces a wide
// groove. Steve's case is a 2mm-wide, 1mm-deep groove on an 11.5mm pitch -
// a helical wire seat, where the automatic width would be 5.75mm.
TEST(ThreadProfiles, ExplicitGrooveWidthIsIndependentOfPitch) {
    const double R = 7.5, L = 46.0, P = 11.5;   // 15mm rod, 4 turns
    TopoDS_Shape rod = BRepPrimAPI_MakeCylinder(R, L).Shape();
    auto build = [&](double width) {
        ThreadOp t;
        configure(t, R, L, ThreadProfile::Square, 0.0);
        t.setPitch(P);
        t.setDepth(1.0);
        t.setIsHole(false);
        t.setGrooveWidth(width);
        return t.buildResult(rod);
    };

    TopoDS_Shape narrow = build(2.0);
    TopoDS_Shape autoW  = build(0.0);   // Square = 0.50 * pitch = 5.75mm
    ASSERT_FALSE(narrow.IsNull()) << "explicit 2mm groove built nothing";
    ASSERT_FALSE(autoW.IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(narrow).IsValid());

    // The requested groove really is narrower - not silently snapped back to
    // the profile's fraction. Removed volume scales with width, so a 2mm cut
    // must remove FAR less than the 5.75mm automatic one.
    const double vRod = vol(rod);
    const double removedNarrow = vRod - vol(narrow);
    const double removedAuto   = vRod - vol(autoW);
    EXPECT_GT(removedNarrow, 0.0) << "the explicit-width cut removed nothing";
    EXPECT_LT(removedNarrow, 0.6 * removedAuto)
        << "explicit width was ignored (removed " << removedNarrow
        << " vs automatic " << removedAuto << ")";

    // Sanity against the analytic figure: a 2mm x 1mm square groove swept at
    // mid-depth radius, over L/P turns. Loose band - real ends taper.
    const double turns = L / P;
    const double analytic = 2.0 * 1.0 * 2.0 * M_PI * (R - 0.5) * turns;
    EXPECT_GT(removedNarrow, 0.4 * analytic);
    EXPECT_LT(removedNarrow, 1.6 * analytic);

    // 0 stays "automatic" for every existing thread and saved file.
    EXPECT_NEAR(removedAuto, vRod - vol(build(0.0)), 1e-6);
}

// A width wider than the pitch would leave no crest between turns; it clamps
// instead of producing a shredded body.
TEST(ThreadProfiles, GrooveWidthClampsToLeaveACrest) {
    const double R = 7.5, L = 30.0, P = 5.0;
    TopoDS_Shape rod = BRepPrimAPI_MakeCylinder(R, L).Shape();
    auto build = [&](double width) {
        ThreadOp t;
        configure(t, R, L, ThreadProfile::Square, 0.0);
        t.setPitch(P);
        t.setDepth(1.0);
        t.setIsHole(false);
        t.setGrooveWidth(width);
        return t.buildResult(rod);
    };
    TopoDS_Shape huge = build(50.0);        // absurd - 10x the pitch
    TopoDS_Shape atCap = build(0.9 * P);    // the cap itself
    ASSERT_FALSE(huge.IsNull()) << "over-wide groove rejected instead of clamped";
    EXPECT_TRUE(BRepCheck_Analyzer(huge).IsValid());
    EXPECT_NEAR(vol(huge), vol(atCap), 1e-3)
        << "clamped width should cut the same groove as asking for the cap";
}

TEST(ThreadProfiles, MultiStartRoundedDepthCap) {
    // Multi-start Rounded cuts with the rope tool, whose radius (= depth)
    // caps at 0.45·pitch. A requested depth above that cap must clamp to it
    // - same solid as asking for the cap exactly - not get rejected by
    // volume/probe gates measuring a deeper groove than the tool cuts.
    const double R = 10.0, L = 9.0, P = 3.0;
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(R, L).Shape();
    auto build = [&](double depth) {
        ThreadOp t;
        configure(t, R, L, ThreadProfile::Rounded, 0.0);
        t.setDepth(depth);
        t.setIsHole(false);
        t.setStarts(3);
        return t.buildResult(cyl);
    };
    TopoDS_Shape atCap  = build(0.45 * P);
    TopoDS_Shape beyond = build(0.60 * P);   // above the rope cap, below 0.65P
    ASSERT_FALSE(atCap.IsNull());
    ASSERT_FALSE(beyond.IsNull()) << "over-deep rounded multi-start rejected "
                                     "instead of clamped";
    EXPECT_TRUE(BRepCheck_Analyzer(beyond).IsValid());
    EXPECT_NEAR(vol(beyond), vol(atCap), 1e-3)
        << "clamped depth should cut the identical groove";
}

TEST(ThreadProfiles, MultiStartInternalValid) {
    // 2-start internal (nut side of a fast-acting pair).
    const double R = 10.0, L = 9.0;
    TopoDS_Shape t8 = tube(16.0, R, L);
    ASSERT_FALSE(t8.IsNull());
    ThreadOp t;
    configure(t, R, L, ThreadProfile::Trapezoidal, 0.0);
    t.setIsHole(true);
    t.setStarts(2);
    TopoDS_Shape res = t.buildResult(t8);
    ASSERT_FALSE(res.IsNull()) << "2-start internal built nothing";
    EXPECT_TRUE(BRepCheck_Analyzer(res).IsValid());
    // An internal thread carves the bore wider: volume strictly shrinks.
    EXPECT_LT(vol(res), vol(t8) + 1e-3);
}
