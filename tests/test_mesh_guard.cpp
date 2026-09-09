// Imported meshes are REFERENCE bodies: modelling operations decline them.
//
// Steve, 2026-08-03: a union on two imported STLs appeared to hang the app.
// It wasn't hung - measured on his own file, the fuse of two 4,881-face mesh
// solids completed in 101 seconds and produced an 8,745-face result, on the
// main thread, with no progress and no cancel. The output would have been
// another mesh: no analytic faces to fillet, sketch on, or edit afterwards.
//
// So the rule is a boundary, not a performance fix - an import is there to
// measure and trace against. These tests pin the two halves of it: which bodies
// count as references, and that a mixed selection is caught (the case where the
// refusal is easiest to get wrong, because most of the selection is fine).

#include "core/Document.h"
#include "core/MeshGuard.h"

#include <gtest/gtest.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <string>

namespace {

struct Doc {
    Document doc;
    int modelled = -1, imported = -1;
    Doc() {
        modelled = doc.addBody(BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape(), "modelled");
        imported = doc.addBody(BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape(), "Imported_STL");
        doc.setBodyMesh(imported, true);   // what StlIO::import does
    }
};

} // namespace

TEST(MeshGuard, ModelledBodiesAreNotFlagged) {
    Doc f;
    EXPECT_TRUE(materializr::meshBodiesAmong(f.doc, {f.modelled}).empty());
}

TEST(MeshGuard, ImportedMeshIsFlagged) {
    Doc f;
    auto meshes = materializr::meshBodiesAmong(f.doc, {f.imported});
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes[0], f.imported);
}

// The one that matters most: a selection of mostly-fine bodies with one import
// in it must still be refused, or the op runs against the mesh anyway.
TEST(MeshGuard, MixedSelectionIsFlagged) {
    Doc f;
    auto meshes = materializr::meshBodiesAmong(f.doc, {f.modelled, f.imported});
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes[0], f.imported);
}

// The message has to name the operation and tell the user what the import IS
// good for - a bare refusal reads as a missing feature.
TEST(MeshGuard, RefusalMessageNamesTheOpAndTheAlternative) {
    const std::string all = materializr::meshRefusalMessage("Fillet", 2, 2);
    EXPECT_NE(all.find("Fillet"), std::string::npos);
    EXPECT_NE(all.find("Sketch on it"), std::string::npos);

    const std::string mixed = materializr::meshRefusalMessage("A boolean", 1, 2);
    EXPECT_NE(mixed.find("A boolean"), std::string::npos);
    EXPECT_NE(mixed.find("out of the selection"), std::string::npos);
}

// The flag has to survive a round-trip, or the guard silently stops applying
// after save/load and the ops come back.
TEST(MeshGuard, FlagIsQueryablePerBody) {
    Doc f;
    EXPECT_TRUE(f.doc.isBodyMesh(f.imported));
    EXPECT_FALSE(f.doc.isBodyMesh(f.modelled));
    f.doc.setBodyMesh(f.imported, false);
    EXPECT_FALSE(f.doc.isBodyMesh(f.imported));
}
