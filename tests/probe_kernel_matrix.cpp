// probe_kernel_matrix - one-line-per-case battery of kernel-sensitive modeling
// operations, for diffing behavior ACROSS OCCT VERSIONS (7.6.3 apt baseline vs
// source-built 7.9.3 vs 8.0.0). Each case runs in a forked child with a 90s
// alarm, so a crash or hang (e.g. the known MakeThickSolid arc-join hang)
// reports as its own line instead of killing the battery.
//
// Output grammar (stable, diffable):
//   CASE <name> | build=<0|1> valid=<0|1> solids=N faces=N edges=N vol=X [note]
//   CASE <name> | DIED signal=N          (crash: 11=segv, 6=abort, 14=timeout)
//
// Also includes app-level repros for two reported bugs:
//   bugB-prim-height-leak : creating cylinder #2 must not change #1's height
//   bugA-copy-tshape-share: does CopyOp share TShapes with the source?
//   bugA-copy-edit-follow : editStep() on the source primitive - does the copy
//                           follow through replay? (documents the semantics)

#include "core/Document.h"
#include "core/History.h"
#include "modeling/PrimitiveOp.h"
#include "modeling/CopyOp.h"
#include "modeling/BooleanOp.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Version.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <BRep_Tool.hxx>
#include <TopLoc_Location.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

// Document/History live in the global namespace; the ops in materializr::.

// ─── reporting helpers ──────────────────────────────────────────────────────

static int countKind(const TopoDS_Shape& s, TopAbs_ShapeEnum k) {
    int n = 0;
    for (TopExp_Explorer ex(s, k); ex.More(); ex.Next()) ++n;
    return n;
}

static double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return p.Mass();
}

static void report(const char* name, const TopoDS_Shape& s, const char* note = "") {
    if (s.IsNull()) {
        std::printf("CASE %-28s | build=0 %s\n", name, note);
        std::fflush(stdout);
        return;
    }
    const bool valid = BRepCheck_Analyzer(s).IsValid() == Standard_True;
    std::printf("CASE %-28s | build=1 valid=%d solids=%d faces=%d edges=%d vol=%.3f %s\n",
                name, valid ? 1 : 0,
                countKind(s, TopAbs_SOLID), countKind(s, TopAbs_FACE),
                countKind(s, TopAbs_EDGE), volumeOf(s), note);
    std::fflush(stdout);
}

static void reportFail(const char* name, const char* why) {
    std::printf("CASE %-28s | build=0 note=%s\n", name, why);
    std::fflush(stdout);
}

// Run one case in a forked child (alarm 90s). Crash/hang become a DIED line.
static void runCase(const char* name, const std::function<void()>& fn) {
    pid_t pid = fork();
    if (pid == 0) {
        alarm(90);
        try { fn(); }
        catch (const Standard_Failure& e) { reportFail(name, e.GetMessageString()); }
        catch (const std::exception& e)   { reportFail(name, e.what()); }
        catch (...)                       { reportFail(name, "unknown-exception"); }
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) {
        std::printf("CASE %-28s | DIED signal=%d%s\n", name, WTERMSIG(st),
                    WTERMSIG(st) == SIGALRM ? " (timeout=hang)" : "");
        std::fflush(stdout);
    }
}

// ─── geometry helpers ───────────────────────────────────────────────────────

static TopoDS_Shape box(double x, double y, double z,
                        double ox = 0, double oy = 0, double oz = 0) {
    return BRepPrimAPI_MakeBox(gp_Pnt(ox, oy, oz), x, y, z).Shape();
}

static TopoDS_Shape cyl(double r, double h,
                        double ox = 0, double oy = 0, double oz = 0) {
    gp_Ax2 ax(gp_Pnt(ox, oy, oz), gp_Dir(0, 0, 1));
    return BRepPrimAPI_MakeCylinder(ax, r, h).Shape();
}

static TopoDS_Shape rectFace(double w, double h) {
    gp_Pnt p1(0, 0, 0), p2(w, 0, 0), p3(w, h, 0), p4(0, h, 0);
    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(p1, p2));
    wire.Add(BRepBuilderAPI_MakeEdge(p2, p3));
    wire.Add(BRepBuilderAPI_MakeEdge(p3, p4));
    wire.Add(BRepBuilderAPI_MakeEdge(p4, p1));
    return BRepBuilderAPI_MakeFace(wire.Wire()).Shape();
}

struct Ext { double x, y, z; };
static Ext extents(const TopoDS_Shape& s) {
    Bnd_Box b;
    BRepBndLib::Add(s, b);
    double x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    return {x1 - x0, y1 - y0, z1 - z0};   // primitives: height = world Y
}

// ─── the battery ────────────────────────────────────────────────────────────

int main() {
    std::printf("KERNEL %s\n", OCC_VERSION_COMPLETE);
    std::fflush(stdout);

    // - booleans, the fragile corners first -
    runCase("fuse-overlap", [] {
        report("fuse-overlap", BRepAlgoAPI_Fuse(box(10, 10, 10), box(10, 10, 10, 5, 5, 5)).Shape());
    });
    runCase("fuse-coincident-face", [] {   // exact shared face - classic fragility
        report("fuse-coincident-face", BRepAlgoAPI_Fuse(box(10, 10, 10), box(10, 10, 10, 10, 0, 0)).Shape());
    });
    runCase("fuse-identical", [] {         // self-coincident everything
        report("fuse-identical", BRepAlgoAPI_Fuse(box(10, 10, 10), box(10, 10, 10)).Shape());
    });
    runCase("cut-through-hole", [] {
        report("cut-through-hole", BRepAlgoAPI_Cut(box(20, 20, 10), cyl(4, 30, 10, 10, -5)).Shape());
    });
    runCase("cut-coplanar-wall", [] {      // tool wall coincident with target wall
        report("cut-coplanar-wall", BRepAlgoAPI_Cut(box(20, 20, 20), box(10, 20, 20, 10, 0, 0)).Shape());
    });
    runCase("cut-tangent-cyl", [] {        // cylinder tangent to the box wall
        report("cut-tangent-cyl", BRepAlgoAPI_Cut(box(20, 20, 10), cyl(5, 20, 5, 0, -5)).Shape());
    });
    runCase("cut-thin-sliver", [] {        // 0.05mm remaining wall
        report("cut-thin-sliver", BRepAlgoAPI_Cut(box(20, 20, 10), box(19.95, 20, 10, 0, 0, 0)).Shape());
    });
    runCase("cut-consumes-all", [] {       // tool swallows the whole body
        TopoDS_Shape r = BRepAlgoAPI_Cut(box(10, 10, 10), box(30, 30, 30, -10, -10, -10)).Shape();
        char note[64];
        std::snprintf(note, sizeof(note), "note=solids-left=%d", countKind(r, TopAbs_SOLID));
        report("cut-consumes-all", r, note);
    });
    runCase("common-box-sphere", [] {
        report("common-box-sphere", BRepAlgoAPI_Common(box(10, 10, 10), BRepPrimAPI_MakeSphere(gp_Pnt(5, 5, 5), 6).Shape()).Shape());
    });
    runCase("section-curve", [] {          // section plane through a fused pair
        TopoDS_Shape fused = BRepAlgoAPI_Fuse(box(10, 10, 10), cyl(4, 20, 5, 5, -5)).Shape();
        TopoDS_Shape sec = BRepAlgoAPI_Section(fused, gp_Pln(gp_Pnt(0, 0, 5), gp_Dir(0, 0, 1))).Shape();
        char note[64];
        std::snprintf(note, sizeof(note), "note=section-edges=%d", countKind(sec, TopAbs_EDGE));
        report("section-curve", sec, note);
    });

    // - fillet / chamfer -
    runCase("fillet-one-edge", [] {
        TopoDS_Shape b = box(10, 10, 10);
        BRepFilletAPI_MakeFillet f(b);
        TopExp_Explorer ex(b, TopAbs_EDGE);
        f.Add(2.0, TopoDS::Edge(ex.Current()));
        report("fillet-one-edge", f.Shape());
    });
    runCase("fillet-all-edges", [] {
        TopoDS_Shape b = box(10, 10, 10);
        BRepFilletAPI_MakeFillet f(b);
        for (TopExp_Explorer ex(b, TopAbs_EDGE); ex.More(); ex.Next())
            f.Add(1.0, TopoDS::Edge(ex.Current()));
        report("fillet-all-edges", f.Shape());
    });
    runCase("fillet-boolean-seam", [] {    // fillet the seam edge of a fused pair
        TopoDS_Shape fused = BRepAlgoAPI_Fuse(box(10, 10, 10), box(10, 10, 10, 0, 0, 10)).Shape();
        BRepFilletAPI_MakeFillet f(fused);
        // seam edges live at z=10; grab every edge whose bbox is flat at z=10
        for (TopExp_Explorer ex(fused, TopAbs_EDGE); ex.More(); ex.Next()) {
            Bnd_Box eb;
            BRepBndLib::Add(ex.Current(), eb);
            double x0, y0, z0, x1, y1, z1;
            eb.Get(x0, y0, z0, x1, y1, z1);
            if (std::fabs(z0 - 10.0) < 1e-6 && std::fabs(z1 - 10.0) < 1e-6)
                f.Add(1.5, TopoDS::Edge(ex.Current()));
        }
        report("fillet-boolean-seam", f.NbContours() ? f.Shape() : TopoDS_Shape(),
               f.NbContours() ? "" : "note=no-seam-edges-found");
    });
    runCase("fillet-radius-too-big", [] {  // r > face - must fail gracefully, not crash
        TopoDS_Shape b = box(10, 10, 10);
        BRepFilletAPI_MakeFillet f(b);
        TopExp_Explorer ex(b, TopAbs_EDGE);
        f.Add(9.0, TopoDS::Edge(ex.Current()));
        try { report("fillet-radius-too-big", f.Shape()); }
        catch (const Standard_Failure& e) { reportFail("fillet-radius-too-big", e.GetMessageString()); }
    });
    runCase("chamfer-box", [] {
        TopoDS_Shape b = box(10, 10, 10);
        BRepFilletAPI_MakeChamfer c(b);
        for (TopExp_Explorer ex(b, TopAbs_EDGE); ex.More(); ex.Next())
            c.Add(1.5, TopoDS::Edge(ex.Current()));
        report("chamfer-box", c.Shape());
    });
    runCase("chamfer-through-fillet", [] { // known OCCT limit on 7.x - did 8.0 fix it?
        TopoDS_Shape b = box(20, 20, 10);
        BRepFilletAPI_MakeFillet f(b);
        for (TopExp_Explorer ex(b, TopAbs_EDGE); ex.More(); ex.Next()) {
            Bnd_Box eb; BRepBndLib::Add(ex.Current(), eb);
            double x0, y0, z0, x1, y1, z1; eb.Get(x0, y0, z0, x1, y1, z1);
            // the four vertical edges
            if (std::fabs((z1 - z0) - 10.0) < 1e-6) f.Add(3.0, TopoDS::Edge(ex.Current()));
        }
        TopoDS_Shape filleted = f.Shape();
        BRepFilletAPI_MakeChamfer c(filleted);
        for (TopExp_Explorer ex(filleted, TopAbs_EDGE); ex.More(); ex.Next()) {
            Bnd_Box eb; BRepBndLib::Add(ex.Current(), eb);
            double x0, y0, z0, x1, y1, z1; eb.Get(x0, y0, z0, x1, y1, z1);
            if (std::fabs(z0 - 10.0) < 1e-6 && std::fabs(z1 - 10.0) < 1e-6)
                c.Add(1.0, TopoDS::Edge(ex.Current()));   // top rim crosses the fillets
        }
        report("chamfer-through-fillet", c.Shape());
    });

    // - shell / offset -
    runCase("shell-open-top", [] {
        TopoDS_Shape b = box(20, 20, 10);
        TopTools_ListOfShape faces;
        for (TopExp_Explorer ex(b, TopAbs_FACE); ex.More(); ex.Next()) {
            Bnd_Box fb; BRepBndLib::Add(ex.Current(), fb);
            double x0, y0, z0, x1, y1, z1; fb.Get(x0, y0, z0, x1, y1, z1);
            if (std::fabs(z0 - 10.0) < 1e-6 && std::fabs(z1 - 10.0) < 1e-6)
                faces.Append(ex.Current());
        }
        BRepOffsetAPI_MakeThickSolid t;
        t.MakeThickSolidByJoin(b, faces, -2.0, 1e-3);
        report("shell-open-top", t.Shape());
    });
    runCase("shell-hits-fillet-radius", [] {   // the documented arc-join HANG trap
        TopoDS_Shape b = box(20, 20, 20);
        BRepFilletAPI_MakeFillet f(b);
        for (TopExp_Explorer ex(b, TopAbs_EDGE); ex.More(); ex.Next())
            f.Add(2.0, TopoDS::Edge(ex.Current()));
        TopoDS_Shape filleted = f.Shape();
        TopTools_ListOfShape faces;
        TopExp_Explorer fx(filleted, TopAbs_FACE);
        faces.Append(fx.Current());
        BRepOffsetAPI_MakeThickSolid t;
        t.MakeThickSolidByJoin(filleted, faces, -2.0, 1e-3);  // wall == fillet radius
        report("shell-hits-fillet-radius", t.Shape());
    });
    runCase("offset-outward", [] {
        BRepOffsetAPI_MakeOffsetShape o;
        o.PerformByJoin(box(10, 10, 10), 2.0, 1e-3);
        report("offset-outward", o.Shape());
    });

    // - swept / lofted -
    runCase("revolve-270deg", [] {
        TopoDS_Shape prof = rectFace(5, 12);
        gp_Trsf mv; mv.SetTranslation(gp_Vec(8, 0, 0));
        prof = BRepBuilderAPI_Transform(prof, mv).Shape();
        report("revolve-270deg",
               BRepPrimAPI_MakeRevol(prof, gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)),
                                     270.0 * M_PI / 180.0).Shape());
    });
    runCase("loft-square-circle", [] {
        BRepOffsetAPI_ThruSections loft(Standard_True /*solid*/);
        gp_Pnt p1(0, 0, 0), p2(20, 0, 0), p3(20, 20, 0), p4(0, 20, 0);
        BRepBuilderAPI_MakeWire w1;
        w1.Add(BRepBuilderAPI_MakeEdge(p1, p2)); w1.Add(BRepBuilderAPI_MakeEdge(p2, p3));
        w1.Add(BRepBuilderAPI_MakeEdge(p3, p4)); w1.Add(BRepBuilderAPI_MakeEdge(p4, p1));
        gp_Circ c(gp_Ax2(gp_Pnt(10, 10, 25), gp_Dir(0, 0, 1)), 6);
        TopoDS_Edge circE = BRepBuilderAPI_MakeEdge(c);
        BRepBuilderAPI_MakeWire w2(circE);
        loft.AddWire(w1.Wire());
        loft.AddWire(w2.Wire());
        loft.Build();
        report("loft-square-circle", loft.Shape());
    });
    runCase("pipe-arc", [] {
        GC_MakeArcOfCircle arc(gp_Pnt(0, 0, 0), gp_Pnt(20, 0, 20), gp_Pnt(40, 0, 0));
        TopoDS_Edge spineE = BRepBuilderAPI_MakeEdge(arc.Value());
        BRepBuilderAPI_MakeWire spine(spineE);
        gp_Circ c(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)), 3);
        TopoDS_Shape prof = BRepBuilderAPI_MakeFace(
            BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(c)).Wire()).Shape();
        report("pipe-arc", BRepOffsetAPI_MakePipe(spine.Wire(), prof).Shape());
    });

    // - meshing (what the viewport tessellates; counts quantify kernel diffs) -
    runCase("mesh-cylinder", [] {
        TopoDS_Shape s = cyl(10, 20);
        BRepMesh_IncrementalMesh mesh(s, 0.1);
        int tris = 0;
        for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
            TopLoc_Location loc;
            auto tri = BRep_Tool::Triangulation(TopoDS::Face(ex.Current()), loc);
            if (!tri.IsNull()) tris += tri->NbTriangles();
        }
        char note[64];
        std::snprintf(note, sizeof(note), "note=triangles=%d", tris);
        report("mesh-cylinder", s, note);
    });
    runCase("mesh-boolean-result", [] {
        TopoDS_Shape s = BRepAlgoAPI_Cut(box(20, 20, 10), cyl(4, 30, 10, 10, -5)).Shape();
        BRepMesh_IncrementalMesh mesh(s, 0.1);
        int tris = 0;
        for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
            TopLoc_Location loc;
            auto tri = BRep_Tool::Triangulation(TopoDS::Face(ex.Current()), loc);
            if (!tri.IsNull()) tris += tri->NbTriangles();
        }
        char note[64];
        std::snprintf(note, sizeof(note), "note=triangles=%d", tris);
        report("mesh-boolean-result", s, note);
    });

    // - app-level: the two reported bugs -
    runCase("bugB-prim-height-leak", [] {
        Document doc;
        History hist;
        auto c1 = std::make_unique<materializr::PrimitiveOp>();
        c1->setKind(materializr::PrimitiveOp::Kind::Cylinder);
        c1->setRadius(15); c1->setHeight(20); c1->setName("Cyl1");
        hist.pushOperation(std::move(c1), doc);
        const auto ids1 = doc.getAllBodyIds();
        const Ext e1before = extents(doc.getBody(ids1.front()));

        auto c2 = std::make_unique<materializr::PrimitiveOp>();
        c2->setKind(materializr::PrimitiveOp::Kind::Cylinder);
        c2->setRadius(5); c2->setHeight(30); c2->setOrigin(50, 0, 0); c2->setName("Cyl2");
        hist.pushOperation(std::move(c2), doc);
        const Ext e1after = extents(doc.getBody(ids1.front()));

        const bool same = std::fabs(e1after.x - e1before.x) < 1e-6 &&
                          std::fabs(e1after.y - e1before.y) < 1e-6 &&
                          std::fabs(e1after.z - e1before.z) < 1e-6;
        std::printf("CASE %-28s | build=1 ext-before=%.1f/%.1f/%.1f ext-after=%.1f/%.1f/%.1f %s\n",
                    "bugB-prim-height-leak",
                    e1before.x, e1before.y, e1before.z,
                    e1after.x, e1after.y, e1after.z,
                    same ? "OK" : "LEAK!");
        std::fflush(stdout);
    });
    runCase("bugA-copy-tshape-share", [] {
        Document doc;
        History hist;
        auto p = std::make_unique<materializr::PrimitiveOp>();
        p->setKind(materializr::PrimitiveOp::Kind::Box);
        p->setBoxExtents(10, 10, 10); p->setName("Box");
        hist.pushOperation(std::move(p), doc);
        const int srcId = doc.getAllBodyIds().front();

        auto cp = std::make_unique<CopyOp>();
        cp->setSourceBodyId(srcId);
        cp->setOffset(30, 0, 0);
        hist.pushOperation(std::move(cp), doc);
        const auto ids = doc.getAllBodyIds();
        const int copyId = ids.back();

        const TopoDS_Shape a = doc.getBody(srcId);
        const TopoDS_Shape b = doc.getBody(copyId);
        const bool sharedRoot = (a.TShape() == b.TShape());
        bool partner = false;   // any face of A sharing a TShape with a face of B
        for (TopExp_Explorer ea(a, TopAbs_FACE); ea.More() && !partner; ea.Next())
            for (TopExp_Explorer eb(b, TopAbs_FACE); eb.More() && !partner; eb.Next())
                if (ea.Current().TShape() == eb.Current().TShape()) partner = true;
        std::printf("CASE %-28s | build=1 shared-root=%d shared-faces=%d %s\n",
                    "bugA-copy-tshape-share", sharedRoot ? 1 : 0, partner ? 1 : 0,
                    (sharedRoot || partner) ? "SHARED!" : "OK");
        std::fflush(stdout);
    });
    runCase("bugA-copy-edit-follow", [] {
        Document doc;
        History hist;
        auto p = std::make_unique<materializr::PrimitiveOp>();
        p->setKind(materializr::PrimitiveOp::Kind::Cylinder);
        p->setRadius(15); p->setHeight(20); p->setName("Cyl");
        hist.pushOperation(std::move(p), doc);
        const int srcId = doc.getAllBodyIds().front();

        auto cp = std::make_unique<CopyOp>();
        cp->setSourceBodyId(srcId);
        cp->setOffset(50, 0, 0);
        hist.pushOperation(std::move(cp), doc);
        const int copyId = doc.getAllBodyIds().back();
        const Ext cBefore = extents(doc.getBody(copyId));

        // Edit step 0 (the primitive) like the history panel would, then replay.
        auto* prim = const_cast<materializr::PrimitiveOp*>(
            dynamic_cast<const materializr::PrimitiveOp*>(hist.getStep(0)));
        if (!prim) { reportFail("bugA-copy-edit-follow", "step0-not-primitive"); return; }
        prim->setHeight(35);
        const bool edited = hist.editStep(0, doc);
        bool copyAlive = true;
        Ext cAfter{0, 0, 0};
        try { cAfter = extents(doc.getBody(copyId)); } catch (...) { copyAlive = false; }

        // Documents the replay semantics per kernel. Known app defect:
        // PrimitiveOp::execute uses addBody (new id per re-execution) instead
        // of addOrPutBody, so downstream CopyOp loses its source on replay -
        // expect editStep=0 + copy-alive=0 on every kernel until that's fixed.
        std::printf("CASE %-28s | build=1 editStep=%d copy-alive=%d "
                    "copy-y-before=%.2f copy-y-after=%.2f %s\n",
                    "bugA-copy-edit-follow", edited ? 1 : 0, copyAlive ? 1 : 0,
                    cBefore.y, cAfter.y,
                    !edited ? "REPLAY-BREAKS"
                            : (std::fabs(cAfter.y - cBefore.y) < 1e-6 ? "independent"
                                                                      : "FOLLOWS-EDIT"));
        std::fflush(stdout);
    });

    std::printf("MATRIX DONE\n");
    return 0;
}
