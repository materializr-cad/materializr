// Rung 3: a live op (ThreadOp) wired to a topo::Ref. The thread stores the
// topological name of its target cylinder face; on rebuild it re-resolves that
// face and re-derives its axis/radius from the cylinder's CURRENT geometry, so
// it FOLLOWS an upstream edit instead of cutting where the cylinder used to be.
//
// Decisive check via volume: a real thread cuts helical grooves, so a threaded
// body has LESS volume than the plain cylinder. With the face ref the grooves
// land on the MOVED cylinder (vol < plain); without it (control) the helix
// misses the moved cylinder and nothing is cut (vol ~= plain).

#include "modeling/TopoName.h"
#include "modeling/ThreadOp.h"
#include "core/Document.h"
#include "modeling/Sketch.h"
#include "modeling/ExtrudeOp.h"

#include <gtest/gtest.h>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <memory>

using materializr::Sketch;
using namespace materializr;

namespace {

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}

// The lateral cylindrical face of a body.
TopoDS_Face cylFaceOf(const TopoDS_Shape& body) {
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        if (BRepAdaptor_Surface(f).GetType() == GeomAbs_Cylinder) return f;
    }
    return {};
}

// Circle sketch (centre id, radius) -> cylinder via ExtrudeOp. Returns body id;
// leaves `ext` set up so the caller can move the centre and re-execute.
int makeCylinder(Document& doc, std::shared_ptr<Sketch> sk, ExtrudeOp& ext,
                 double height) {
    int sid = doc.addSketch(sk);
    ext.setSketchSource(sid);
    ext.setDistance(height);
    EXPECT_TRUE(ext.rebuildProfileFromSketch(doc));
    EXPECT_TRUE(ext.execute(doc));
    return doc.getAllBodyIds().front();
}

void configThread(ThreadOp& t, int body, double radius, double height) {
    t.setBody(body);
    t.setAxis(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    t.setRadius(radius);
    t.setLength(height);
    t.setPitch(2.0);
    t.setDepth(0.8);
    t.setIsHole(false);
    t.setRightHanded(true);
}

} // namespace

// With the face ref, the thread follows the cylinder when the sketch moves it.
TEST(TopoThread, FollowsCylinderMove) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    int c = sk->addPoint({0.0f, 0.0f});
    sk->addCircle(c, 8.0);
    ExtrudeOp ext;
    int body = makeCylinder(doc, sk, ext, 6.0);

    // Name the cylinder face and thread it.
    topo::Context ctx; ctx.doc = &doc; ctx.shape = doc.getBody(body);
    ctx.type = TopAbs_FACE;
    TopoDS_Face cyl = cylFaceOf(doc.getBody(body));
    ASSERT_FALSE(cyl.IsNull());
    topo::Ref ref = topo::mint(cyl, ctx);
    ASSERT_FALSE(ref.empty());
    EXPECT_EQ(ref.names.front().scheme, "sketchface");  // Cyl anchor, robust

    ThreadOp thr;
    configThread(thr, body, 8.0, 6.0);
    thr.setTargetFaceRef(ref);
    ASSERT_TRUE(thr.execute(doc));
    const double threadedAtOrigin = volumeOf(doc.getBody(body));

    // MOVE the cylinder: centre 0 -> (30,0). Re-derive the base with the same
    // extrude op, then re-run the thread (recompute path).
    sk->movePoint(c, {30.0f, 0.0f});
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    const double plainMoved = volumeOf(doc.getBody(body));

    ASSERT_TRUE(thr.execute(doc)) << "thread must re-cut on the moved cylinder";
    const double threadedMoved = volumeOf(doc.getBody(body));

    // Grooves were actually cut on the MOVED cylinder.
    EXPECT_LT(threadedMoved, plainMoved * 0.999)
        << "thread should remove groove volume from the moved cylinder";
    // And it's a real thread comparable to the original (same size cylinder).
    EXPECT_NEAR(threadedMoved, threadedAtOrigin, plainMoved * 0.02)
        << "same-size cylinder should thread to ~the same volume, just relocated";
}

// Control: WITHOUT the face ref the thread keeps its original absolute axis,
// so after the move the helix misses the cylinder and cuts nothing.
TEST(TopoThread, WithoutRefMissesMovedCylinder) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    int c = sk->addPoint({0.0f, 0.0f});
    sk->addCircle(c, 8.0);
    ExtrudeOp ext;
    int body = makeCylinder(doc, sk, ext, 6.0);

    ThreadOp thr;
    configThread(thr, body, 8.0, 6.0);        // NO setTargetFaceRef
    ASSERT_TRUE(thr.execute(doc));

    sk->movePoint(c, {30.0f, 0.0f});
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    const double plainMoved = volumeOf(doc.getBody(body));

    thr.execute(doc);  // may succeed as a no-op cut or fail; either way:
    const double afterMoved = volumeOf(doc.getBody(body));
    EXPECT_GE(afterMoved, plainMoved * 0.999)
        << "without a ref the helix stays at the origin and cuts no grooves "
           "from the cylinder now at (30,0)";
}

// With the app's async hook installed, a RECOMPUTE-path execute defers: it
// returns success immediately with the body left unthreaded (the hook owns
// the re-cut). The initial precomputed path and hook-less headless runs stay
// synchronous.
TEST(TopoThread, AsyncHookDefersRecompute) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    int c = sk->addPoint({0.0f, 0.0f});
    sk->addCircle(c, 8.0);
    ExtrudeOp ext;
    int body = makeCylinder(doc, sk, ext, 6.0);
    const double plain = volumeOf(doc.getBody(body));

    ThreadOp thr;
    configThread(thr, body, 8.0, 6.0);

    int calls = 0;
    ThreadOp::setAsyncRecutHook(
        [&](ThreadOp& op, Document&) { ++calls; EXPECT_EQ(&op, &thr); return true; });
    ASSERT_TRUE(thr.execute(doc)) << "deferred execute reports success";
    EXPECT_EQ(calls, 1);
    EXPECT_NEAR(volumeOf(doc.getBody(body)), plain, 1e-6)
        << "body untouched - the hook owns the re-cut";
    ThreadOp::setAsyncRecutHook(nullptr);   // restore sync for other tests
}

// Long rods: >40 turns builds via phase-aligned chunked sweeps glued at
// planar interfaces (a single MakePipeShell degrades past ~40 turns).
TEST(TopoThread, LongRodChunkedSweep) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    int c = sk->addPoint({0.0f, 0.0f});
    sk->addCircle(c, 5.0);
    ExtrudeOp ext;
    int body = makeCylinder(doc, sk, ext, 150.0);   // 75 turns at pitch 2
    const double plain = volumeOf(doc.getBody(body));

    ThreadOp thr;
    configThread(thr, body, 5.0, 150.0);
    ASSERT_TRUE(thr.execute(doc));
    const double threaded = volumeOf(doc.getBody(body));
    EXPECT_LT(threaded, plain * 0.95) << "grooves cut along the whole rod";
    EXPECT_GT(threaded, plain * 0.75) << "not gouged";
}
