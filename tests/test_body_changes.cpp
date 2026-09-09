// changedBodies() names exactly the bodies an edit touched, so a preview
// re-tessellates those and not the rest of the document.
#include "core/BodyChanges.h"
#include "core/Document.h"

#include <gtest/gtest.h>

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopLoc_Location.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <set>
#include <vector>

using materializr::BodyChangeScope;
using materializr::BodySnapshot;
using materializr::changedBodies;
using materializr::snapshotBodies;

namespace {

TopoDS_Shape box(double s) { return BRepPrimAPI_MakeBox(s, s, s).Shape(); }

struct Doc {
    Document doc;
    int a, b, c;
    Doc() : a(doc.addBody(box(10.0), "a")), b(doc.addBody(box(20.0), "b")), c(doc.addBody(box(30.0), "c")) {}
};

} // namespace

TEST(BodyChanges, NothingChangedNamesNothing) {
    Doc d;
    const BodySnapshot before = snapshotBodies(d.doc);
    EXPECT_TRUE(changedBodies(before, d.doc).empty());
}

TEST(BodyChanges, AReplacedShapeNamesThatBodyOnly) {
    Doc d;
    const BodySnapshot before = snapshotBodies(d.doc);
    d.doc.updateBody(d.b, box(25.0));
    EXPECT_EQ(changedBodies(before, d.doc), std::vector<int>{d.b});
}

TEST(BodyChanges, AMovedBodyCounts) {
    // Same TShape, new Location: the renderer bakes the location into the
    // vertices, so a move is a change. A TShape-pointer compare would miss it.
    Doc d;
    const BodySnapshot before = snapshotBodies(d.doc);
    gp_Trsf t;
    t.SetTranslation(gp_Vec(5.0, 0.0, 0.0));
    d.doc.updateBody(d.a, d.doc.getBody(d.a).Moved(TopLoc_Location(t)));
    EXPECT_EQ(changedBodies(before, d.doc), std::vector<int>{d.a});
}

TEST(BodyChanges, HidingCounts) {
    // The partial rebuild removes a hidden body only when it is marked.
    Doc d;
    const BodySnapshot before = snapshotBodies(d.doc);
    d.doc.setBodyVisible(d.c, false);
    EXPECT_EQ(changedBodies(before, d.doc), std::vector<int>{d.c});
}

TEST(BodyChanges, RemovedAndAddedBodiesCount) {
    Doc d;
    const BodySnapshot before = snapshotBodies(d.doc);
    d.doc.removeBody(d.a);
    const int n = d.doc.addBody(box(40.0), "n");
    std::vector<int> want{d.a, n};
    std::sort(want.begin(), want.end());
    EXPECT_EQ(changedBodies(before, d.doc), want);
}

TEST(BodyChanges, ScopeMarksOnExitAndFallsBackWithoutAPerBodyMark) {
    Doc d;
    std::set<int> marked;
    {
        BodyChangeScope scope(d.doc, [&](int id) { marked.insert(id); });
        d.doc.updateBody(d.b, box(21.0));
        EXPECT_TRUE(marked.empty()); // nothing until the scope closes
    }
    EXPECT_EQ(marked, std::set<int>{d.b});
    bool all = false;
    {
        BodyChangeScope scope(d.doc, nullptr, [&] { all = true; });
        d.doc.updateBody(d.a, box(11.0));
    }
    EXPECT_TRUE(all);
}
