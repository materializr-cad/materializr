// probe_validity_fuzz - deterministic randomized validity sweep over the fragile
// kernel operations. Same seed => IDENTICAL case stream on every OCCT version,
// so a divergence in valid/invalid/crash counts between kernels is a real kernel
// behavior difference (not noise). Each case is fork-isolated (60s alarm) so a
// segfault or hang is RECORDED, not fatal.
//
// Usage: probe_validity_fuzz [N=1500] [seed=1]
// Output: per-op tallies + a final machine-readable SUMMARY line for diffing:
//   SUMMARY n=N valid=.. invalid=.. empty=.. exception=.. crash=.. hang=..
//
// "invalid" = op reported done (or a shape came back non-null) but
// BRepCheck_Analyzer rejects it - i.e. a well-formedness bug in the kernel.

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <Bnd_Box.hxx>
#include <Standard_Version.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pnt.hxx>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

// outcome codes shared parent<->child via exit status
enum Outcome { OK_VALID = 0, OK_INVALID = 1, EMPTY = 2, EXCEPTION = 3 };

static int nSolids(const TopoDS_Shape& s) {
    int n = 0;
    for (TopExp_Explorer e(s, TopAbs_SOLID); e.More(); e.Next()) ++n;
    return n;
}

static Outcome classify(const TopoDS_Shape& s) {
    if (s.IsNull() || nSolids(s) == 0) return EMPTY;
    return BRepCheck_Analyzer(s).IsValid() ? OK_VALID : OK_INVALID;
}

// ── random primitive generators (seeded per-iteration) ──────────────────────
static TopoDS_Shape randPrim(std::mt19937& rng) {
    std::uniform_real_distribution<double> d(2.0, 40.0);
    std::uniform_real_distribution<double> o(-15.0, 15.0);
    std::uniform_int_distribution<int> kind(0, 3);
    gp_Pnt p(o(rng), o(rng), o(rng));
    switch (kind(rng)) {
        case 0: return BRepPrimAPI_MakeBox(p, d(rng), d(rng), d(rng)).Shape();
        case 1: return BRepPrimAPI_MakeCylinder(gp_Ax2(p, gp_Dir(0, 0, 1)), d(rng) * 0.5, d(rng)).Shape();
        case 2: return BRepPrimAPI_MakeSphere(p, d(rng) * 0.5).Shape();
        default: {
            double r1 = d(rng) * 0.5, r2 = d(rng) * 0.3;
            return BRepPrimAPI_MakeCone(gp_Ax2(p, gp_Dir(0, 0, 1)), r1, r2, d(rng)).Shape();
        }
    }
}

// perform one randomized op; returns its resulting shape (may throw/be null)
static TopoDS_Shape doOp(int opcat, std::mt19937& rng, const char** label) {
    switch (opcat) {
        case 0: { *label = "fuse";
            return BRepAlgoAPI_Fuse(randPrim(rng), randPrim(rng)).Shape(); }
        case 1: { *label = "cut";
            return BRepAlgoAPI_Cut(randPrim(rng), randPrim(rng)).Shape(); }
        case 2: { *label = "common";
            return BRepAlgoAPI_Common(randPrim(rng), randPrim(rng)).Shape(); }
        case 3: { *label = "fillet";
            TopoDS_Shape b = randPrim(rng);
            std::uniform_real_distribution<double> r(0.2, 5.0);
            std::bernoulli_distribution pick(0.5);
            BRepFilletAPI_MakeFillet f(b);
            for (TopExp_Explorer e(b, TopAbs_EDGE); e.More(); e.Next())
                if (pick(rng)) f.Add(r(rng), TopoDS::Edge(e.Current()));
            if (f.NbContours() == 0) return TopoDS_Shape();
            return f.Shape(); }
        case 4: { *label = "chamfer";
            TopoDS_Shape b = randPrim(rng);
            std::uniform_real_distribution<double> r(0.2, 4.0);
            std::bernoulli_distribution pick(0.5);
            BRepFilletAPI_MakeChamfer c(b);
            for (TopExp_Explorer e(b, TopAbs_EDGE); e.More(); e.Next())
                if (pick(rng)) c.Add(r(rng), TopoDS::Edge(e.Current()));
            if (c.NbContours() == 0) return TopoDS_Shape();
            return c.Shape(); }
        case 5: { *label = "shell";
            TopoDS_Shape b = randPrim(rng);
            std::uniform_real_distribution<double> t(0.3, 4.0);
            std::bernoulli_distribution pick(0.4);
            TopTools_ListOfShape faces;
            for (TopExp_Explorer e(b, TopAbs_FACE); e.More(); e.Next())
                if (pick(rng)) faces.Append(e.Current());
            if (faces.IsEmpty()) return TopoDS_Shape();
            BRepOffsetAPI_MakeThickSolid mk;
            mk.MakeThickSolidByJoin(b, faces, -t(rng), 1e-3);
            return mk.Shape(); }
        case 6: { *label = "offset";
            std::uniform_real_distribution<double> t(0.2, 3.0);
            BRepOffsetAPI_MakeOffsetShape o;
            o.PerformByJoin(randPrim(rng), t(rng), 1e-3);
            return o.Shape(); }
        default: { *label = "chain";   // cut then fillet the result - 2-stage
            TopoDS_Shape b = BRepAlgoAPI_Cut(randPrim(rng), randPrim(rng)).Shape();
            if (b.IsNull() || nSolids(b) == 0) return b;
            std::uniform_real_distribution<double> r(0.2, 2.0);
            std::bernoulli_distribution pick(0.3);
            BRepFilletAPI_MakeFillet f(b);
            for (TopExp_Explorer e(b, TopAbs_EDGE); e.More(); e.Next())
                if (pick(rng)) f.Add(r(rng), TopoDS::Edge(e.Current()));
            if (f.NbContours() == 0) return b;
            return f.Shape(); }
    }
}

static const char* CATS[] = {"fuse", "cut", "common", "fillet", "chamfer",
                             "shell", "offset", "chain"};
static const int NCAT = 8;

int main(int argc, char** argv) {
    const int N   = argc > 1 ? std::atoi(argv[1]) : 1500;
    const uint32_t seed = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 1u;

    std::printf("KERNEL %s   fuzz n=%d seed=%u\n", OCC_VERSION_COMPLETE, N, seed);
    std::fflush(stdout);

    // per-category tallies [cat][outcome 0..5]
    long tally[NCAT][6];
    std::memset(tally, 0, sizeof(tally));
    // outcome index: 0 valid,1 invalid,2 empty,3 exception,4 crash,5 hang
    long tot[6] = {0, 0, 0, 0, 0, 0};

    for (int i = 0; i < N; ++i) {
        const int cat = i % NCAT;   // even spread across categories
        int pipefd[2];
        if (pipe(pipefd) != 0) continue;
        pid_t pid = fork();
        if (pid == 0) {
            close(pipefd[0]);
            alarm(60);
            // Deterministic per-iteration seed: same stream on every kernel.
            std::mt19937 rng(seed * 2654435761u + static_cast<uint32_t>(i));
            unsigned char oc = EXCEPTION;
            const char* lbl = "?";
            try {
                TopoDS_Shape s = doOp(cat, rng, &lbl);
                oc = static_cast<unsigned char>(classify(s));
            } catch (...) { oc = EXCEPTION; }
            ssize_t w = write(pipefd[1], &oc, 1);
            (void)w;
            close(pipefd[1]);
            _exit(0);
        }
        close(pipefd[1]);
        unsigned char oc = 255;
        ssize_t r = read(pipefd[0], &oc, 1);
        close(pipefd[0]);
        int st = 0;
        waitpid(pid, &st, 0);
        int outcome;
        if (WIFSIGNALED(st)) outcome = (WTERMSIG(st) == SIGALRM) ? 5 : 4;  // hang / crash
        else if (r != 1 || oc > 3) outcome = 4;                            // died before write
        else outcome = oc;                                                 // 0..3
        tally[cat][outcome]++;
        tot[outcome]++;
        // progress heartbeat every 250
        if ((i + 1) % 250 == 0) {
            std::printf("... %d/%d (invalid=%ld crash=%ld hang=%ld)\n",
                        i + 1, N, tot[1], tot[4], tot[5]);
            std::fflush(stdout);
        }
    }

    std::printf("\n%-9s %7s %7s %7s %7s %6s %5s\n",
                "OP", "valid", "invalid", "empty", "excep", "crash", "hang");
    for (int c = 0; c < NCAT; ++c)
        std::printf("%-9s %7ld %7ld %7ld %7ld %6ld %5ld\n", CATS[c],
                    tally[c][0], tally[c][1], tally[c][2], tally[c][3],
                    tally[c][4], tally[c][5]);

    std::printf("\nSUMMARY n=%d valid=%ld invalid=%ld empty=%ld exception=%ld crash=%ld hang=%ld\n",
                N, tot[0], tot[1], tot[2], tot[3], tot[4], tot[5]);
    // exit nonzero if any hard problem (invalid geometry, crash, or hang)
    return (tot[1] + tot[4] + tot[5]) > 0 ? 2 : 0;
}
