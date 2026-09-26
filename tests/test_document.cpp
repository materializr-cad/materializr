#include <gtest/gtest.h>
#include "core/Document.h"
#include <BRepPrimAPI_MakeBox.hxx>
#include <algorithm>
#include <vector>

// Helper: create a simple box shape for testing
static TopoDS_Shape makeTestBox(double x = 10, double y = 10, double z = 10) {
    return BRepPrimAPI_MakeBox(x, y, z).Shape();
}

TEST(DocumentTest, AddBodyIncreasesCount) {
    Document doc;
    EXPECT_EQ(doc.bodyCount(), 0);

    doc.addBody(makeTestBox(), "Box1");
    EXPECT_EQ(doc.bodyCount(), 1);

    doc.addBody(makeTestBox(5, 5, 5), "Box2");
    EXPECT_EQ(doc.bodyCount(), 2);
}

TEST(DocumentTest, RemoveBodyDecreasesCount) {
    Document doc;
    int id1 = doc.addBody(makeTestBox(), "Box1");
    int id2 = doc.addBody(makeTestBox(), "Box2");
    EXPECT_EQ(doc.bodyCount(), 2);

    doc.removeBody(id1);
    EXPECT_EQ(doc.bodyCount(), 1);

    doc.removeBody(id2);
    EXPECT_EQ(doc.bodyCount(), 0);
}

TEST(DocumentTest, GetBodyReturnsValidShape) {
    Document doc;
    int id = doc.addBody(makeTestBox(), "TestBox");

    const TopoDS_Shape& shape = doc.getBody(id);
    EXPECT_FALSE(shape.IsNull());
}

TEST(DocumentTest, GetBodyThrowsForInvalidId) {
    Document doc;
    EXPECT_THROW(doc.getBody(999), std::runtime_error);
}

TEST(DocumentTest, BodyNaming) {
    Document doc;
    int id = doc.addBody(makeTestBox(), "OriginalName");

    EXPECT_EQ(doc.getBodyName(id), "OriginalName");

    doc.setBodyName(id, "RenamedBody");
    EXPECT_EQ(doc.getBodyName(id), "RenamedBody");
}

TEST(DocumentTest, BodyDefaultNaming) {
    Document doc;
    int id = doc.addBody(makeTestBox());

    // Default name should not be empty
    std::string name = doc.getBodyName(id);
    EXPECT_FALSE(name.empty());
}

TEST(DocumentTest, BodyVisibility) {
    Document doc;
    int id = doc.addBody(makeTestBox(), "Box");

    // Bodies are visible by default
    EXPECT_TRUE(doc.isBodyVisible(id));

    doc.setBodyVisible(id, false);
    EXPECT_FALSE(doc.isBodyVisible(id));

    doc.setBodyVisible(id, true);
    EXPECT_TRUE(doc.isBodyVisible(id));
}

TEST(DocumentTest, GetAllBodyIds) {
    Document doc;
    int id1 = doc.addBody(makeTestBox(), "A");
    int id2 = doc.addBody(makeTestBox(), "B");
    int id3 = doc.addBody(makeTestBox(), "C");

    std::vector<int> ids = doc.getAllBodyIds();
    EXPECT_EQ(ids.size(), 3u);

    // All IDs should be present
    EXPECT_NE(std::find(ids.begin(), ids.end(), id1), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), id2), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), id3), ids.end());
}

TEST(DocumentTest, ClearRemovesAllBodies) {
    Document doc;
    doc.addBody(makeTestBox(), "A");
    doc.addBody(makeTestBox(), "B");
    doc.addBody(makeTestBox(), "C");
    EXPECT_EQ(doc.bodyCount(), 3);

    doc.clear();
    EXPECT_EQ(doc.bodyCount(), 0);
    EXPECT_TRUE(doc.getAllBodyIds().empty());
}

TEST(DocumentTest, UpdateBodyChangesShape) {
    Document doc;
    int id = doc.addBody(makeTestBox(10, 10, 10), "Box");

    TopoDS_Shape newShape = makeTestBox(20, 20, 20);
    doc.updateBody(id, newShape);

    const TopoDS_Shape& retrieved = doc.getBody(id);
    EXPECT_FALSE(retrieved.IsNull());
    // The shape should have been replaced (we verify it's not null;
    // exact geometry comparison would require OCCT tools)
}

TEST(DocumentTest, RemoveNonexistentBodyDoesNotCrash) {
    Document doc;
    doc.addBody(makeTestBox(), "Box");

    // Removing a non-existent body should not throw or crash
    doc.removeBody(999);
    EXPECT_EQ(doc.bodyCount(), 1);
}


// The invariant Application::markFolderBodiesDirty rests on.
//
// That helper marks exactly getBodiesInFolder(fid) after a folder colour or
// visibility edit, on the understanding that those edits touch the folder's
// members and nothing else. If a future change made folders nest, or gave a
// body a colour that overrides its folder's, the helper would start
// under-marking and bodies would keep stale colours on screen until something
// forced a full rebuild. Pin the assumption here, where it is cheap, rather
// than in Application, which no test binary can construct.
TEST(DocumentTest, FolderColourAndVisibilityCascadeToExactlyTheFoldersMembers) {
    Document doc;
    const int fid = doc.addFolder("group");
    const int inA = doc.addBody(makeTestBox(), "in-a");
    const int inB = doc.addBody(makeTestBox(), "in-b");
    const int out = doc.addBody(makeTestBox(), "outside");
    doc.setBodyFolder(inA, fid);
    doc.setBodyFolder(inB, fid);

    std::vector<int> members = doc.getBodiesInFolder(fid);
    std::sort(members.begin(), members.end());
    std::vector<int> want{inA, inB};
    std::sort(want.begin(), want.end());
    ASSERT_EQ(members, want) << "getBodiesInFolder is what the marking uses";

    const glm::vec3 before = doc.getBodyColor(out);
    const glm::vec3 tint(0.1f, 0.7f, 0.3f);
    doc.setFolderColor(fid, tint);
    EXPECT_EQ(doc.getBodyColor(inA), tint);
    EXPECT_EQ(doc.getBodyColor(inB), tint);
    EXPECT_EQ(doc.getBodyColor(out), before)
        << "a folder colour reached a body outside the folder";

    doc.setFolderVisible(fid, false);
    EXPECT_FALSE(doc.isBodyVisible(inA));
    EXPECT_FALSE(doc.isBodyVisible(inB));
    EXPECT_TRUE(doc.isBodyVisible(out))
        << "a folder visibility change reached a body outside the folder";

    // Root bodies are folderId -1, which is why the helper guards on that:
    // an unguarded -1 would mark every body at the root.
    EXPECT_EQ(doc.getBodyFolder(out), -1);
}

// Items-panel drag-and-drop: moveBody repositions within getAllBodyIds()'s
// order (the thing the panel actually renders in), not just folder membership.
TEST(DocumentTest, MoveBodyReordersWithoutChangingFolder) {
    Document doc;
    const int a = doc.addBody(makeTestBox(), "a");
    const int b = doc.addBody(makeTestBox(), "b");
    const int c = doc.addBody(makeTestBox(), "c");
    ASSERT_EQ(doc.getAllBodyIds(), (std::vector<int>{a, b, c}));

    // Drag c to sit before a.
    doc.moveBody(c, -1, a);
    EXPECT_EQ(doc.getAllBodyIds(), (std::vector<int>{c, a, b}));
    EXPECT_EQ(doc.getBodyFolder(c), -1);

    // beforeBodyId < 0 moves to the true end.
    doc.moveBody(c, -1, -1);
    EXPECT_EQ(doc.getAllBodyIds(), (std::vector<int>{a, b, c}));
}

TEST(DocumentTest, MoveBodyIntoFolderReparentsAndPositions) {
    Document doc;
    const int fid = doc.addFolder("group");
    const int inFolder = doc.addBody(makeTestBox(), "in-folder");
    doc.setBodyFolder(inFolder, fid);
    const int atRoot = doc.addBody(makeTestBox(), "at-root");

    // Drag the root body to just before the folder's member - it should
    // both re-parent into the folder AND land ahead of that member.
    doc.moveBody(atRoot, fid, inFolder);
    EXPECT_EQ(doc.getBodyFolder(atRoot), fid);
    EXPECT_EQ(doc.getBodiesInFolder(fid), (std::vector<int>{atRoot, inFolder}));
    EXPECT_TRUE(doc.getBodiesInFolder(-1).empty());

    // Dropping on the folder header itself (beforeBodyId = -1) appends to
    // the folder's end instead of its start.
    const int another = doc.addBody(makeTestBox(), "another");
    doc.moveBody(another, fid, -1);
    EXPECT_EQ(doc.getBodiesInFolder(fid), (std::vector<int>{atRoot, inFolder, another}));
}

TEST(DocumentTest, MoveBodyIgnoresUnknownIdsAndSelfDrop) {
    Document doc;
    const int a = doc.addBody(makeTestBox(), "a");
    const int b = doc.addBody(makeTestBox(), "b");

    doc.moveBody(a, -1, a);      // dropped onto itself - no-op
    EXPECT_EQ(doc.getAllBodyIds(), (std::vector<int>{a, b}));

    doc.moveBody(999, -1, a);    // unknown dragged id - no-op
    EXPECT_EQ(doc.getAllBodyIds(), (std::vector<int>{a, b}));

    doc.moveBody(a, 999, b);     // unknown target folder - no-op
    EXPECT_EQ(doc.getBodyFolder(a), -1);
    EXPECT_EQ(doc.getAllBodyIds(), (std::vector<int>{a, b}));
}

TEST(DocumentTest, MoveFolderReorders) {
    Document doc;
    const int f1 = doc.addFolder("one");
    const int f2 = doc.addFolder("two");
    const int f3 = doc.addFolder("three");
    ASSERT_EQ(doc.getAllFolderIds(), (std::vector<int>{f1, f2, f3}));

    doc.moveFolder(f3, f1);
    EXPECT_EQ(doc.getAllFolderIds(), (std::vector<int>{f3, f1, f2}));

    doc.moveFolder(f1, -1); // to the end
    EXPECT_EQ(doc.getAllFolderIds(), (std::vector<int>{f3, f2, f1}));
}
