// A DISABLED Thread step must not make its body count as threaded.
//
// isBodyThreaded() is the "this body has threads, be careful" gate: it puts
// Push/Pull on the ghost-preview path and turns Resize Cylindrical's live
// preview off, because running a real boolean against a helicoid every frame
// is unusable. Its sibling isBodyShelled() skips disabled steps; this one
// never did - it filtered on typeId alone. So disabling a Thread step (the
// History context menu's "Disable", right next to "Delete") left every later
// op still treating the body as threaded and degrading its preview, for a
// thread that is not in the model.
//
// reflowInsertionIndex() already skips disabled steps, so the two disagreed
// about the same history: no reflow, yet still "threaded".
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/History.h"
#include "modeling/ThreadOp.h"
#include "modeling/ShellOp.h"
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <gp_Ax2.hxx>
#include <memory>
#include <cstdio>

using namespace materializr;

namespace {
constexpr double R = 10.0, L = 9.0; // 3 coarse turns - fast

std::unique_ptr<ThreadOp> makeThread(int bodyId) {
    auto t = std::make_unique<ThreadOp>();
    t->setBody(bodyId);
    t->setAxis(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    t->setRadius(R);
    t->setLength(L);
    t->setPitch(3.0);
    t->setDepth(1.2);
    t->setIsHole(false);
    return t;
}
double vol(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}
} // namespace

TEST(DisabledThread, DisablingTheStepUnthreadsTheBody) {
    Document doc;
    History hist;
    TopoDS_Shape rod = BRepPrimAPI_MakeCylinder(R, L).Shape();
    const double vRod = vol(rod);
    int rodId = doc.addBody(rod, "rod");

    ASSERT_TRUE(hist.pushOperation(makeThread(rodId), doc));
    ASSERT_LT(vol(doc.getBody(rodId)), vRod);      // thread really cut
    EXPECT_TRUE(hist.isBodyThreaded(rodId));

    const int threadIdx = hist.currentStep();
    ASSERT_GE(threadIdx, 0);

    // Disable it: the geometry goes back to a plain rod...
    hist.setStepEnabled(threadIdx, false, doc);
    EXPECT_NEAR(vol(doc.getBody(rodId)), vRod, vRod * 1e-6);
    // ...so nothing downstream should still be treating it as threaded.
    EXPECT_FALSE(hist.isBodyThreaded(rodId));
    // The reflow gate already agreed - pin that the two stay consistent.
    auto probe = makeThread(rodId);          // any op planning this body
    EXPECT_EQ(hist.reflowInsertionIndex(*probe), -1);

    // Re-enabling restores both the geometry and the flag.
    hist.setStepEnabled(threadIdx, true, doc);
    EXPECT_LT(vol(doc.getBody(rodId)), vRod);
    EXPECT_TRUE(hist.isBodyThreaded(rodId));
}

// The contract this was measured against: Shell already behaved.
TEST(DisabledThread, ShellGateAlreadySkipsDisabledSteps) {
    Document doc;
    History hist;
    TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape();
    int boxId = doc.addBody(box, "box");

    // Shell needs an open face - the +Z top.
    TopoDS_Face top;
    for (TopExp_Explorer fx(box, TopAbs_FACE); fx.More(); fx.Next()) {
        GProp_GProps g; BRepGProp::SurfaceProperties(fx.Current(), g);
        if (std::abs(g.CentreOfMass().Z() - 20.0) < 1e-6) {
            top = TopoDS::Face(fx.Current()); break;
        }
    }
    ASSERT_FALSE(top.IsNull());

    auto sh = std::make_unique<ShellOp>();
    sh->setBody(boxId);
    sh->setThickness(2.0);
    sh->addFaceToRemove(top);
    ASSERT_TRUE(hist.pushOperation(std::move(sh), doc));
    EXPECT_TRUE(hist.isBodyShelled(boxId));

    const int shellIdx = hist.currentStep();
    hist.setStepEnabled(shellIdx, false, doc);
    EXPECT_FALSE(hist.isBodyShelled(boxId));   // already correct today
}
