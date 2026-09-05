// Sew: loose surfaces become one body, and a solid when they close.
//
// The reason this exists is the rung it fills. A patch fills one opening; a
// space bounded by several needs several, and BRepBuilderAPI_Sewing ran inside
// five operations while being reachable from the UI in none of them. So the
// tests that matter most are the two ends of that story: six faces that enclose
// a box come back as a SOLID with a volume, and faces that don't quite meet
// come back joined with an honest count of what's still open.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "modeling/SewOp.h"
#include "modeling/OperationFactory.h"

#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

#include <cmath>
#include <vector>

namespace {

double vol(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

bool isSolid(const TopoDS_Shape& s) {
    return !s.IsNull() && TopExp_Explorer(s, TopAbs_SOLID).More();
}

int freeEdges(const TopoDS_Shape& s) {
    TopTools_IndexedDataMapOfShapeListOfShape anc;
    TopExp::MapShapesAndAncestors(s, TopAbs_EDGE, TopAbs_FACE, anc);
    int n = 0;
    for (int i = 1; i <= anc.Extent(); ++i) {
        if (BRep_Tool::Degenerated(TopoDS::Edge(anc.FindKey(i)))) continue;
        if (anc.FindFromIndex(i).Extent() == 1) ++n;
    }
    return n;
}

// The faces of a box, each added to `doc` as its own one-face surface body —
// what a user ends up with after patching a space shut one opening at a time.
std::vector<int> boxAsLooseFaces(Document& doc, double size, int skip = -1) {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(size, size, size).Shape();
    std::vector<int> ids;
    int i = 0;
    for (TopExp_Explorer ex(box, TopAbs_FACE); ex.More(); ex.Next(), ++i) {
        if (i == skip) continue;
        ids.push_back(doc.addBody(ex.Current(), "Surface"));
    }
    return ids;
}

} // namespace

TEST(Sew, SixLooseFacesBecomeASolid) {
    Document doc;
    const std::vector<int> ids = boxAsLooseFaces(doc, 10.0);
    ASSERT_EQ(ids.size(), 6u);
    ASSERT_EQ(doc.getAllBodyIds().size(), 6u);

    SewOp op;
    op.setBodies(ids);
    ASSERT_TRUE(op.execute(doc));

    EXPECT_TRUE(op.madeSolid());
    EXPECT_EQ(op.freeEdgesLeft(), 0);
    EXPECT_EQ(op.facesSewn(), 6);
    // One body left, and it has a volume — which is the entire point.
    EXPECT_EQ(doc.getAllBodyIds().size(), 1u);
    const TopoDS_Shape r = doc.getBody(ids.front());
    EXPECT_TRUE(isSolid(r));
    EXPECT_NEAR(vol(r), 1000.0, 1e-6);
}

TEST(Sew, TheResultKeepsTheFirstBodysIdentity) {
    Document doc;
    const std::vector<int> ids = boxAsLooseFaces(doc, 10.0);
    doc.setBodyName(ids.front(), "Housing");

    SewOp op;
    op.setBodies(ids);
    ASSERT_TRUE(op.execute(doc));
    EXPECT_EQ(doc.getBodyName(ids.front()), "Housing")
        << "the kept body's name, colour and folder have to survive";
}

TEST(Sew, AnOpenSetJoinsAnywayAndSaysWhatsLeft) {
    // Five faces of a box: they join along every shared edge, but the missing
    // lid leaves four edges open. Reporting that count is what tells the user
    // how many gaps are left to patch.
    Document doc;
    const std::vector<int> ids = boxAsLooseFaces(doc, 10.0, /*skip=*/0);
    ASSERT_EQ(ids.size(), 5u);

    SewOp op;
    op.setBodies(ids);
    ASSERT_TRUE(op.execute(doc));

    EXPECT_FALSE(op.madeSolid());
    EXPECT_EQ(op.freeEdgesLeft(), 4);
    EXPECT_EQ(doc.getAllBodyIds().size(), 1u) << "still joined into one body";
    EXPECT_FALSE(isSolid(doc.getBody(ids.front())));
    EXPECT_EQ(freeEdges(doc.getBody(ids.front())), 4);
}

TEST(Sew, UndoPutsEveryBodyBack) {
    Document doc;
    const std::vector<int> ids = boxAsLooseFaces(doc, 10.0);
    SewOp op;
    op.setBodies(ids);
    ASSERT_TRUE(op.execute(doc));
    ASSERT_EQ(doc.getAllBodyIds().size(), 1u);

    ASSERT_TRUE(op.undo(doc));
    EXPECT_EQ(doc.getAllBodyIds().size(), 6u);
    // Back under their ORIGINAL ids, so a later step naming one still resolves.
    for (int id : ids) EXPECT_FALSE(doc.getBody(id).IsNull()) << "body " << id;
}

TEST(Sew, DiffReportsTheConsumedBodiesAsDeleted) {
    Document doc;
    const std::vector<int> ids = boxAsLooseFaces(doc, 10.0);
    SewOp op;
    op.setBodies(ids);
    ASSERT_TRUE(op.execute(doc));

    const OperationDiff d = op.captureDiff();
    ASSERT_EQ(d.modifiedBefore.size(), 1u);
    EXPECT_EQ(d.modifiedBefore[0].first, ids.front());
    EXPECT_EQ(d.deletedBefore.size(), 5u) << "the other five were consumed";
}

TEST(Sew, ASingleClosedShellBecomesASolid) {
    // No second selection to make: a shell that already closes — one that came
    // in from STEP, or the body a last patch just finished — is a valid Sew.
    Document doc;
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    BRepBuilderAPI_Sewing sew(1e-6);
    for (TopExp_Explorer ex(box, TopAbs_FACE); ex.More(); ex.Next()) sew.Add(ex.Current());
    sew.Perform();
    const int id = doc.addBody(sew.SewedShape(), "Shell");
    ASSERT_FALSE(isSolid(doc.getBody(id)));

    SewOp op;
    op.setBodies({id});
    ASSERT_TRUE(op.execute(doc));
    EXPECT_TRUE(op.madeSolid());
    EXPECT_NEAR(vol(doc.getBody(id)), 1000.0, 1e-6);
}

TEST(Sew, RefusesWhenThereIsNothingToDo) {
    // One body that is already a solid. Sewing it to nothing returns itself, and
    // spending a history step on that is worse than declining.
    Document doc;
    const int id = doc.addBody(BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape(), "Box");
    SewOp op;
    op.setBodies({id});
    EXPECT_FALSE(op.execute(doc));
    EXPECT_NEAR(vol(doc.getBody(id)), 1000.0, 1e-6) << "and it left the body alone";
}

TEST(Sew, ParamsRoundTrip) {
    Document doc;
    const std::vector<int> ids = boxAsLooseFaces(doc, 10.0);
    SewOp op;
    op.setBodies(ids);
    ASSERT_TRUE(op.execute(doc));

    auto back = OperationFactory::create("sew");
    ASSERT_NE(back, nullptr) << "the factory has to know the type id";
    ASSERT_TRUE(back->deserializeParams(op.serializeParams()));
    EXPECT_EQ(static_cast<SewOp*>(back.get())->getBodies(), ids);
}

TEST(Sew, RefusesAGarbageBlob) {
    SewOp op;
    EXPECT_FALSE(op.deserializeParams(""));
    EXPECT_FALSE(op.deserializeParams("n=2;ids="));            // no ids
    EXPECT_FALSE(op.deserializeParams("n=2;ids=1,2,3"));       // count mismatch
    EXPECT_FALSE(op.deserializeParams("n=999999999;ids=1"));   // absurd count
    EXPECT_FALSE(op.deserializeParams("n=1;ids=-4"));          // negative id
}
