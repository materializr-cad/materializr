// PROBE: current boolean-path cost for an INTERNAL (hole) thread at realistic
// nut-ish sizes - decides whether holes need a swept-path variant at all.
#include "modeling/ThreadOp.h"
#include "core/Document.h"
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <chrono>
#include <cstdio>
static double vol(const TopoDS_Shape& s){GProp_GProps g;BRepGProp::VolumeProperties(s,g);return g.Mass();}
int main() {
    struct Case { double R, len, pitch, depth; const char* name; };
    Case cases[] = {
        {4.0, 8.0, 1.25, 0.6, "M8-nut-ish (6.4 turns)"},
        {6.0, 15.0, 1.75, 0.8, "M12 deeper (8.6 turns)"},
        {4.0, 30.0, 1.25, 0.6, "long tapped hole (24 turns)"},
    };
    for (const auto& c : cases) {
        Document doc;
        // Block with a through-hole of radius R along Z.
        double W = 4 * c.R;
        TopoDS_Shape block = BRepPrimAPI_MakeBox(
            gp_Pnt(-W/2, -W/2, 0), W, W, c.len).Shape();
        TopoDS_Shape drill = BRepPrimAPI_MakeCylinder(
            gp_Ax2(gp_Pnt(0,0,-1), gp_Dir(0,0,1)), c.R, c.len + 2).Shape();
        BRepAlgoAPI_Cut cut(block, drill); cut.Build();
        int body = doc.addBody(cut.Shape(), "block");
        ThreadOp t;
        t.setBody(body);
        t.setAxis(gp_Ax2(gp_Pnt(0,0,0), gp_Dir(0,0,1), gp_Dir(1,0,0)));
        t.setRadius(c.R); t.setLength(c.len);
        t.setPitch(c.pitch); t.setDepth(c.depth);
        t.setIsHole(true); t.setRightHanded(true);
        double v0 = vol(doc.getBody(body));
        auto t0 = std::chrono::steady_clock::now();
        bool ok = t.execute(doc);
        auto t1 = std::chrono::steady_clock::now();
        double v1 = vol(doc.getBody(body));
        std::printf("%-28s ok=%d  %6lldms  vol %.0f -> %.0f (cut %.1f%%)\n",
            c.name, ok?1:0,
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count(),
            v0, v1, 100.0*(v0-v1)/v0);
    }
    return 0;
}
