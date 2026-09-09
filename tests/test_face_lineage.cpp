// Face lineage (#49/#51): a chamfer's bevel faces stay traceable to their op
// after a downstream boolean SPLITS them - the case no geometric matcher can
// handle - and the lineage survives save/load. Old saves (no lineage section)
// keep today's geometric-fallback behaviour untouched.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "io/ProjectIO.h"
#include "modeling/BooleanOp.h"
#include "modeling/ChamferOp.h"
#include "modeling/FaceLineage.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <BRep_Tool.hxx>
#include <gp_Pnt.hxx>

#include <cstdio>
#include <fstream>
#include <string>

using namespace materializr;

namespace {

std::string tmpPath(const char* name) {
    return std::string(::testing::TempDir()) + name;
}

// The top-front long edge of an axis-aligned box (y = maxY, z = 0 plane side):
// both vertices at y==h and z==0.
TopoDS_Edge topFrontEdge(const TopoDS_Shape& box, double h) {
    for (TopExp_Explorer ex(box, TopAbs_EDGE); ex.More(); ex.Next()) {
        const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
        TopExp_Explorer vx(e, TopAbs_VERTEX);
        bool ok = true;
        int nv = 0;
        for (; vx.More(); vx.Next(), ++nv) {
            gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()));
            if (std::abs(p.Y() - h) > 1e-9 || std::abs(p.Z()) > 1e-9) ok = false;
        }
        if (ok && nv == 2) return e;
    }
    return TopoDS_Edge();
}

// Box 60x10x10 at origin, chamfer its top-front edge (two distances), then
// subtract a narrow box crossing the middle - the bevel splits in two.
struct Scenario {
    Document doc;
    int bodyId = -1;
    ChamferOp chamfer;
    bool build() {
        bodyId = doc.addBody(BRepPrimAPI_MakeBox(60.0, 10.0, 10.0).Shape(), "Bar");
        TopoDS_Edge e = topFrontEdge(doc.getBody(bodyId), 10.0);
        if (e.IsNull()) return false;
        chamfer.setBody(bodyId);
        chamfer.setEdges({e});
        chamfer.setDistance(2.0);
        chamfer.setDistance2(4.0);           // the two-distance case from #51
        if (!chamfer.execute(doc)) return false;
        if (chamfer.getGeneratedFaces().size() != 1) return false;

        // Cutter: a 4mm-wide slab through the bar's middle, full height/depth.
        gp_Pnt corner(28.0, -1.0, -1.0);
        int tool = doc.addBody(
            BRepPrimAPI_MakeBox(corner, 4.0, 12.0, 12.0).Shape(), "Cutter");
        BooleanOp cut;
        cut.setTargetBodyId(bodyId);
        cut.setToolBodyId(tool);
        cut.setMode(BooleanMode::Subtract);
        return cut.execute(doc);
    }
};

} // namespace

TEST(FaceLineage, BevelPiecesAllTraceBackAfterBooleanSplit) {
    Scenario s;
    ASSERT_TRUE(s.build());

    const auto* lineage = s.doc.bodyFaceIds(s.bodyId);
    ASSERT_NE(lineage, nullptr) << "boolean must propagate the lineage map";

    s.chamfer.refreshGeneratedFaces(s.doc.getBody(s.bodyId), lineage);
    // The cut crossed the bevel: BOTH pieces must trace to the chamfer.
    ASSERT_EQ(s.chamfer.getGeneratedFaces().size(), 2u)
        << "split bevel pieces lost their lineage";
    for (const auto& f : s.chamfer.getGeneratedFaces())
        EXPECT_EQ(s.chamfer.ownsFaceScore(f), 2);
}

TEST(FaceLineage, SurvivesSaveLoad) {
    Scenario s;
    ASSERT_TRUE(s.build());
    ASSERT_NE(s.doc.bodyFaceIds(s.bodyId), nullptr);

    // The op's own ids persist in its params (genids=…).
    EXPECT_NE(s.chamfer.serializeParams().find("genids="), std::string::npos);

    const std::string path = tmpPath("lineage.mzr");
    ASSERT_TRUE(ProjectIO::save(path, s.doc, nullptr).success);

    Document back;
    ASSERT_TRUE(ProjectIO::load(path, back, nullptr).success);
    std::remove(path.c_str());
    const auto* lineage = back.bodyFaceIds(s.bodyId);
    ASSERT_NE(lineage, nullptr) << "FACEIDS section didn't round-trip";

    // A rehydrated chamfer (params only) finds both bevel pieces via lineage.
    ChamferOp again;
    ASSERT_TRUE(again.deserializeParams(s.chamfer.serializeParams()));
    again.refreshGeneratedFaces(back.getBody(s.bodyId), lineage);
    EXPECT_EQ(again.getGeneratedFaces().size(), 2u);

    // The id counter also round-trips (no id reuse in the next session).
    EXPECT_GE(back.faceIdCounter(), s.doc.faceIdCounter());
}

TEST(FaceLineage, OldSaveWithoutLineageFallsBackGracefully) {
    // Unsplit chamfer, lineage map deliberately DROPPED before refresh - the
    // old-save condition. The geometric fallback must still find the bevel.
    Document doc;
    int bid = doc.addBody(BRepPrimAPI_MakeBox(60.0, 10.0, 10.0).Shape(), "Bar");
    TopoDS_Edge e = topFrontEdge(doc.getBody(bid), 10.0);
    ASSERT_FALSE(e.IsNull());
    ChamferOp ch;
    ch.setBody(bid);
    ch.setEdges({e});
    ch.setDistance(2.0);
    ASSERT_TRUE(ch.execute(doc));

    ch.refreshGeneratedFaces(doc.getBody(bid), /*lineage=*/nullptr);
    ASSERT_EQ(ch.getGeneratedFaces().size(), 1u);
    EXPECT_GT(ch.ownsFaceScore(ch.getGeneratedFaces().front()), 0);
}
