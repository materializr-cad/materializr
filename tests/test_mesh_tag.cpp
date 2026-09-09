// MeshTag decides when ShapeRenderer may skip the mesher for a TShape. The
// rule must let a body with a face the mesher cannot triangulate keep its mesh
// across rebuilds, while never trusting a tag on a fresh, unmeshed shape.
#include "viewport/MeshTag.h"
#include "core/MeshParams.h"

#include <gtest/gtest.h>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>

using materializr::MeshTag;
using materializr::countUnmeshedFaces;
using materializr::makeMeshTag;
using materializr::meshTagCovers;

namespace {

constexpr float kDefl = 0.1f, kAng = 0.3f;

void mesh(const TopoDS_Shape& s) {
    BRepMesh_IncrementalMesh m(s, materializr::meshParams(kDefl, kAng, true));
}

// A self-intersecting planar wire: the mesher leaves this face bare, every time.
TopoDS_Face bowtieFace() {
    BRepBuilderAPI_MakePolygon p(gp_Pnt(0, 0, 0), gp_Pnt(10, 10, 0), gp_Pnt(10, 0, 0), gp_Pnt(0, 10, 0), true);
    return BRepBuilderAPI_MakeFace(p.Wire(), true).Face();
}

TopoDS_Shape boxWithBowtie() {
    TopoDS_Compound c;
    BRep_Builder bb;
    bb.MakeCompound(c);
    bb.Add(c, BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape());
    bb.Add(c, bowtieFace());
    return c;
}

} // namespace

TEST(MeshTag, CoversTheShapeItWasMadeFromAtTheSameQualityOnly) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    mesh(box);
    const MeshTag tag = makeMeshTag(box, kDefl, kAng);
    EXPECT_EQ(tag.faces, 6);
    EXPECT_EQ(tag.unmeshedFaces, 0);
    EXPECT_TRUE(meshTagCovers(tag, kDefl, kAng, box));
    EXPECT_FALSE(meshTagCovers(tag, 0.5f, kAng, box));   // coarser request
    EXPECT_FALSE(meshTagCovers(tag, 0.05f, kAng, box));  // finer request
    EXPECT_FALSE(meshTagCovers(tag, kDefl, 0.5f, box));  // other angular value
}

TEST(MeshTag, ABareFaceTheMesherCouldNotTriangulateIsExpected) {
    TopoDS_Shape s = boxWithBowtie();
    mesh(s);
    int faces = 0;
    EXPECT_EQ(countUnmeshedFaces(s, &faces), 1);
    EXPECT_EQ(faces, 7);
    const MeshTag tag = makeMeshTag(s, kDefl, kAng);
    EXPECT_EQ(tag.unmeshedFaces, 1);
    // Second, third, ... rebuild: the bowtie is still bare, the mesh is kept.
    EXPECT_TRUE(meshTagCovers(tag, kDefl, kAng, s));
    // A tag that believed every face was meshed does not cover it.
    MeshTag full = tag;
    full.unmeshedFaces = 0;
    EXPECT_FALSE(meshTagCovers(full, kDefl, kAng, s));
}

TEST(MeshTag, LosingAMeshedFaceIsNoticed) {
    TopoDS_Shape s = boxWithBowtie();
    mesh(s);
    const MeshTag tag = makeMeshTag(s, kDefl, kAng);
    BRepTools::Clean(s); // someone dropped the triangulations
    EXPECT_EQ(countUnmeshedFaces(s), 7);
    EXPECT_FALSE(meshTagCovers(tag, kDefl, kAng, s));
}

TEST(MeshTag, AFreshShapeIsNeverCovered) {
    // A new TShape can be allocated at the address of a tagged one that died.
    // Whatever the stale tag recorded, a shape with every face bare is meshed.
    TopoDS_Shape fresh = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    for (int recorded : {0, 1, 5, 6}) {
        MeshTag tag;
        tag.deflection = kDefl;
        tag.angularDeflection = kAng;
        tag.faces = 6;
        tag.unmeshedFaces = recorded;
        EXPECT_FALSE(meshTagCovers(tag, kDefl, kAng, fresh)) << "recorded " << recorded;
    }
}

TEST(MeshTag, ADifferentFaceCountIsNotCovered) {
    TopoDS_Shape s = boxWithBowtie();
    mesh(s);
    MeshTag tag = makeMeshTag(s, kDefl, kAng);
    tag.faces = 8; // the tag came from a shape with one more face
    EXPECT_FALSE(meshTagCovers(tag, kDefl, kAng, s));
}
