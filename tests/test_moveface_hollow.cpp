// MoveFaceOp on a SHELLED (hollow) body: the loft/shear rebuild can produce a
// topologically-"valid" but WRONG result - an inside-out solid (negative
// volume) on a slide, or a re-solidified body whose cavity was silently
// discarded on a tilt. Steve hit both in-app ("doing anything to a shelled
// body makes the shell disappear"). The op must REFUSE cleanly and leave the
// hollow body untouched.

#include "modeling/ShellOp.h"
#include "modeling/MoveFaceOp.h"
#include "core/Document.h"
#include "core/History.h"

#include <gtest/gtest.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp_Face.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <cmath>

namespace {

double vol(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}

TopoDS_Face faceWithNormal(const TopoDS_Shape& b, double nx, double ny, double nz) {
    for (TopExp_Explorer ex(b, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepGProp_Face p(f);
        double u1, u2, v1, v2; p.Bounds(u1, u2, v1, v2);
        gp_Pnt c; gp_Vec n; p.Normal(0.5 * (u1 + u2), 0.5 * (v1 + v2), c, n);
        if (n.Magnitude() < 1e-9) continue;
        n.Normalize();
        if (n.X() * nx + n.Y() * ny + n.Z() * nz > 0.99) return f;
    }
    return {};
}

// 20x10x10 box shelled to 1mm walls with the TOP open. Returns body id.
int makeOpenShellBox(Document& doc) {
    int body = doc.addBody(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 20, 10, 10).Shape(),
                           "box");
    ShellOp sh;
    sh.setBody(body);
    sh.setThickness(1.0);
    sh.addFaceToRemove(faceWithNormal(doc.getBody(body), 0, 0, 1));
    EXPECT_TRUE(sh.execute(doc));
    return body;
}

} // namespace

TEST(MoveFaceHollow, SlideRefusesInsteadOfInvertingShell) {
    Document doc;
    int body = makeOpenShellBox(doc);
    const double hollowV = vol(doc.getBody(body));
    ASSERT_LT(hollowV, 2000.0 * 0.5) << "setup: body is hollow";

    MoveFaceOp mv;
    mv.setBody(body);
    mv.setFace(faceWithNormal(doc.getBody(body), 1, 0, 0)); // a side wall
    mv.setKind(MoveFaceOp::Kind::Translate);
    mv.setMoveVector(gp_Vec(0, 2, 0));                      // in-plane slide
    EXPECT_FALSE(mv.execute(doc))
        << "sliding a hollow body's wall produced garbage before the guard - "
           "it must refuse until the loft engine handles cavities";
    EXPECT_NEAR(vol(doc.getBody(body)), hollowV, 1e-6)
        << "a refusal must leave the hollow body untouched";
}

TEST(MoveFaceHollow, TiltRefusesInsteadOfFillingCavity) {
    Document doc;
    int body = makeOpenShellBox(doc);
    const double hollowV = vol(doc.getBody(body));

    MoveFaceOp mv;
    mv.setBody(body);
    mv.setFace(faceWithNormal(doc.getBody(body), 1, 0, 0));
    mv.setKind(MoveFaceOp::Kind::Rotate);
    mv.setRotation(gp_Dir(0, 1, 0), 0.17);                  // ~10 degrees
    EXPECT_FALSE(mv.execute(doc))
        << "tilting a hollow body's wall silently re-solidified it before the "
           "guard - it must refuse until the loft engine handles cavities";
    EXPECT_NEAR(vol(doc.getBody(body)), hollowV, 1e-6)
        << "a refusal must leave the hollow body untouched";
}

// THE REAL PATH - History::pushOperation. A face transform on a shelled body
// AUTO-REFLOWS beneath the Shell step: it applies to the pre-shell solid and
// the shell re-runs on the moved body ("the order flipped"), so the very
// operations the direct guards refuse SUCCEED through history - hollow body,
// moved geometry, no corruption.
TEST(MoveFaceHollow, SlideReflowsBeneathShellThroughHistory) {
    Document doc;
    History hist;
    int body = doc.addBody(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 20, 10, 10).Shape(),
                           "box");
    auto sh = std::make_unique<ShellOp>();
    sh->setBody(body);
    sh->setThickness(1.0);
    sh->addFaceToRemove(faceWithNormal(doc.getBody(body), 0, 0, 1)); // open top
    ASSERT_TRUE(hist.pushOperation(std::move(sh), doc));
    const double hollowV = vol(doc.getBody(body));
    ASSERT_LT(hollowV, 2000.0 * 0.5) << "setup: hollow";

    auto mv = std::make_unique<MoveFaceOp>();
    mv->setBody(body);
    mv->setFace(faceWithNormal(doc.getBody(body), 1, 0, 0)); // a side wall
    mv->setKind(MoveFaceOp::Kind::Translate);
    mv->setMoveVector(gp_Vec(0, 2, 0));                      // in-plane slide
    ASSERT_TRUE(hist.pushOperation(std::move(mv), doc))
        << "the slide must reflow beneath the shell and land";

    const TopoDS_Shape& result = doc.getBody(body);
    EXPECT_LT(vol(result), 2000.0 * 0.5)
        << "body must STILL be hollow (shell re-ran on the moved solid)";
    EXPECT_NEAR(vol(result), hollowV, hollowV * 0.15)
        << "sheared shell keeps ~the same wall volume";
    // History order flipped: [Move Face, Shell].
    ASSERT_EQ(hist.stepCount(), 2);
    EXPECT_EQ(hist.getStep(0)->typeId(), "moveface");
    EXPECT_EQ(hist.getStep(1)->typeId(), "shell");
}

TEST(MoveFaceHollow, TiltReflowsBeneathShellThroughHistory) {
    Document doc;
    History hist;
    int body = doc.addBody(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 20, 10, 10).Shape(),
                           "box");
    auto sh = std::make_unique<ShellOp>();
    sh->setBody(body);
    sh->setThickness(1.0);
    sh->addFaceToRemove(faceWithNormal(doc.getBody(body), 0, 0, 1));
    ASSERT_TRUE(hist.pushOperation(std::move(sh), doc));
    const double hollowV = vol(doc.getBody(body));

    auto mv = std::make_unique<MoveFaceOp>();
    mv->setBody(body);
    mv->setFace(faceWithNormal(doc.getBody(body), 1, 0, 0));
    mv->setKind(MoveFaceOp::Kind::Rotate);
    mv->setRotation(gp_Dir(0, 1, 0), 0.17);                  // ~10 degrees
    ASSERT_TRUE(hist.pushOperation(std::move(mv), doc))
        << "the tilt must reflow beneath the shell and land";
    EXPECT_LT(vol(doc.getBody(body)), 2000.0 * 0.6)
        << "tilted body must STILL be hollow - no silently filled cavity";
    ASSERT_EQ(hist.stepCount(), 2);
    EXPECT_EQ(hist.getStep(0)->typeId(), "moveface");
    EXPECT_EQ(hist.getStep(1)->typeId(), "shell");
    (void)hollowV;
}

// The guards must NOT block legitimate solid-body moves.
TEST(MoveFaceHollow, SolidBodySlideStillWorks) {
    Document doc;
    int body = doc.addBody(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 20, 10, 10).Shape(),
                           "solid");
    const double v0 = vol(doc.getBody(body));

    MoveFaceOp mv;
    mv.setBody(body);
    mv.setFace(faceWithNormal(doc.getBody(body), 0, 0, 1)); // top cap
    mv.setKind(MoveFaceOp::Kind::Translate);
    mv.setMoveVector(gp_Vec(3, 0, 0));                      // shear +X
    EXPECT_TRUE(mv.execute(doc)) << "plain solid slide must keep working";
    EXPECT_NEAR(vol(doc.getBody(body)), v0, v0 * 0.05)
        << "a shear preserves volume";
}
