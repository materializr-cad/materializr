// Regression (issue #21): a duplicated sketch must be INDEPENDENT of the source
// sketch's body. Application::duplicateSketch deep-copies the sketch, which
// carries the source's m_sourceBodyId AND its bound host face - so without
// intervention the copy cascaded edits into the original body, re-bound that
// host face (a stray filled region over the body), and aimed push/pull + extrude
// at the wrong body (no new solid; perimeter fused flat, holes inverted).
// DuplicateSketchOp::execute must sever both so the copy is a standalone sketch
// whose region comes from its OWN loops and whose ops make a NEW body.

#include "core/Document.h"
#include "modeling/Sketch.h"
#include "modeling/DuplicateSketchOp.h"

#include <gtest/gtest.h>
#include <gp_Pln.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <TopoDS_Face.hxx>
#include <glm/glm.hpp>
#include <memory>

using materializr::Sketch;

namespace {
// A closed square in the sketch plane - one region from the sketch's own loops.
void addSquare(Sketch& sk, float s) {
    int p0 = sk.addPoint(glm::vec2(0, 0));
    int p1 = sk.addPoint(glm::vec2(s, 0));
    int p2 = sk.addPoint(glm::vec2(s, s));
    int p3 = sk.addPoint(glm::vec2(0, s));
    sk.addLine(p0, p1); sk.addLine(p1, p2); sk.addLine(p2, p3); sk.addLine(p3, p0);
}
}  // namespace

TEST(DuplicateSketch, CopyIsSeveredFromSourceBody) {
    Document doc;

    // A sketch that looks like it was drawn on a body's face: own geometry, a
    // source-body id, and a bound host face (a plain planar face stands in for
    // the body's holed top face).
    auto src = std::make_shared<Sketch>();
    src->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    addSquare(*src, 40.0f);
    src->setSourceBody(7);
    TopoDS_Face host = BRepBuilderAPI_MakeFace(
        gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), -50, 50, -50, 50).Face();
    src->setSourceFace(host);
    int srcId = doc.addSketch(src, "Sketch");

    // Mirror Application::duplicateSketch: the deep copy carries the link...
    auto copy = std::make_shared<Sketch>(*src);
    ASSERT_EQ(copy->getSourceBody(), 7) << "deep copy inherits the source body link";
    ASSERT_FALSE(copy->getSourceFace().IsNull()) << "deep copy inherits the host face";

    // ...and the op severs it on execute.
    DuplicateSketchOp op;
    op.setCopy(copy, srcId, "Sketch copy");
    ASSERT_TRUE(op.execute(doc));

    EXPECT_EQ(copy->getSourceBody(), -1)
        << "a duplicate must not stay linked to the source's body (issue #21)";
    EXPECT_TRUE(copy->getSourceFace().IsNull())
        << "a duplicate must drop the inherited host face so its region is its own";
    EXPECT_FALSE(copy->isDetachedFromBody())
        << "a duplicate is a clean free sketch, not a detached-from-body one";

    // The original is untouched - it still drives its body.
    EXPECT_EQ(src->getSourceBody(), 7);
    EXPECT_FALSE(src->getSourceFace().IsNull());

    // The copy keeps its own drawn loops, so it still builds a region.
    EXPECT_GE(copy->buildRegions().size(), 1u) << "the copy keeps its own geometry";
}
