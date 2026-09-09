// Multi-body export: several separate solids must land in ONE file with their
// relative positions intact. That is the print-in-place case - a hinge, or one
// of those articulated toys - where the parts are deliberately disjoint and
// the SPACING between them is the whole design. Exercises the document-level
// exporter that Application::exportBodiesAs drives with a scratch document.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "io/StlExport.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <gp_Trsf.hxx>

#include <filesystem>
#include <string>

using namespace materializr;
namespace fs = std::filesystem;

namespace {
// Three separate 10mm cubes spread 25mm apart along X - disjoint solids
// standing in for the links of a print-in-place assembly.
Document threeSpacedCubes() {
    Document d;
    for (int i = 0; i < 3; ++i) {
        TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
        gp_Trsf t;
        t.SetTranslation(gp_Vec(i * 25.0, 0, 0));
        d.addBody(BRepBuilderAPI_Transform(box, t, true).Shape(),
                  "link" + std::to_string(i));
    }
    return d;
}
std::string tmp(const char* name) {
    return (fs::temp_directory_path() / name).string();
}
} // namespace

TEST(MultiBodyExport, EverySolidLandsInOneFile) {
    Document doc = threeSpacedCubes();
    const std::string path = tmp("mzr_multibody.stl");

    auto res = StlExport::exportFile(path, doc);
    ASSERT_TRUE(res.success) << res.errorMessage;
    // 3 cubes x 12 triangles - one body's worth would be 12.
    EXPECT_GE(res.triangleCount, 36);
    EXPECT_TRUE(fs::exists(path));
    fs::remove(path);
}

// The parts must keep their relative positions: an exporter that re-centred
// each body (or collapsed them to the origin) would destroy the assembly
// while still producing a plausible-looking file.
TEST(MultiBodyExport, RelativePositionsSurvive) {
    Document doc = threeSpacedCubes();
    Bnd_Box bb;
    for (int id : doc.getAllBodyIds())
        BRepBndLib::Add(doc.getBody(id), bb, Standard_True);
    double x0, y0, z0, x1, y1, z1;
    bb.Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(x0, 0.0, 1e-3);
    EXPECT_NEAR(x1, 60.0, 1e-3)   // 2 * 25mm spacing + the last 10mm cube
        << "the spread between parts was lost";
}

// Visibility is the subset control - hiding a body leaves it out of the
// export without deleting it, which is how you export part of an assembly.
TEST(MultiBodyExport, HiddenBodiesAreExcluded) {
    Document full = threeSpacedCubes();
    Document hidden = threeSpacedCubes();
    const auto ids = hidden.getAllBodyIds();
    ASSERT_EQ(ids.size(), 3u);
    hidden.setBodyVisible(ids[1], false);

    const std::string pAll = tmp("mzr_mb_all.stl");
    const std::string pSome = tmp("mzr_mb_some.stl");
    auto rAll = StlExport::exportFile(pAll, full);
    auto rSome = StlExport::exportFile(pSome, hidden);
    ASSERT_TRUE(rAll.success) << rAll.errorMessage;
    ASSERT_TRUE(rSome.success) << rSome.errorMessage;

    EXPECT_LT(rSome.triangleCount, rAll.triangleCount)
        << "the hidden body was exported anyway";
    fs::remove(pAll);
    fs::remove(pSome);
}

// All bodies hidden is a clear error, not a silently empty file the user
// only discovers in their slicer.
TEST(MultiBodyExport, NothingVisibleIsAnError) {
    Document doc = threeSpacedCubes();
    for (int id : doc.getAllBodyIds()) doc.setBodyVisible(id, false);
    const std::string path = tmp("mzr_mb_none.stl");
    auto res = StlExport::exportFile(path, doc);
    EXPECT_FALSE(res.success);
    EXPECT_FALSE(res.errorMessage.empty());
    fs::remove(path);
}
