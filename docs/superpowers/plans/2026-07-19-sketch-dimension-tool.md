# Sketch Dimension Tool Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Onshape-style dimension tool in sketch mode, bound to `d`: click entities, place a label, type a value - creating driving constraints (distance, length, diameter, angle, point-to-line).

**Architecture:** New `SketchToolMode::Dimension` state machine in `SketchTool` (picking + pair resolution, no mutations), a new solver constraint `DistancePointLine`, persisted label offsets on `Constraint`, and app-layer commit through `recordSketchMutation` reusing the existing `##DimEdit` popup for value entry.

**Tech Stack:** C++17, ImGui, glm, OCCT (sketch plane only), GoogleTest + ctest, Unix Makefiles build in `build/`.

**Spec:** `docs/superpowers/specs/2026-07-19-sketch-dimension-tool-design.md` - read it first.

## Global Constraints

- `ConstraintType` enum is **append-only** (serialization stores the int value). `DistancePointLine` goes after `Angle`, nothing reordered.
- K-line format: new fields append at the END of the line; reader must accept 6-field legacy lines.
- Repo root for all paths below: `materializr/` (the git repo). Build dir: `build/` (Unix Makefiles, already configured).
- Build: `cmake --build build -j 8 --target <target>`; full: `cmake --build build -j 8`.
- **ctest must run UNSANDBOXED** - sandboxed runs false-fail 5 file-IO suites on /tmp writes (see project memory).
- Existing `Angle` constraint semantics: **signed** angle of line B relative to line A, radians, wrapped to [-π, π] (`SketchSolver.cpp` `case ConstraintType::Angle`). Match it exactly.
- Existing UI convention: circle/arc dims display and edit as **diameter**, stored as radius in `Constraint::value`.
- Commits: normal messages, no Claude trailer (project policy).

---

### Task 1: `DistancePointLine` constraint in the solver

**Files:**
- Modify: `src/modeling/SketchConstraints.h`
- Modify: `src/modeling/SketchSolver.cpp` (three switch statements: `numEquations` in `solve()`, `computeError()`, `applyCorrection()`)
- Create: `tests/test_sketch_dimension.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Sketch::addPoint(glm::vec2)`, `Sketch::addLine(int,int)`, `Sketch::addConstraint(const Constraint&)` (assigns id, returns it), `SketchSolver::solve(Sketch&, int maxIterations, double tolerance)`.
- Produces: `ConstraintType::DistancePointLine` - entityA = point id, entityB = line id, value = perpendicular distance to the infinite line. Later tasks rely on this exact meaning.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_sketch_dimension.cpp`:

```cpp
// DistancePointLine: perpendicular distance from a point (entityA) to the
// infinite line carried by a sketch line (entityB). Solver drives the point
// and the line's endpoints apart/together along the line normal.

#include "modeling/Sketch.h"
#include "modeling/SketchSolver.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cmath>

using materializr::Constraint;
using materializr::ConstraintType;
using materializr::Sketch;
using materializr::SketchSolver;

namespace {

double pointLineDist(const Sketch& sk, int ptId, int lineId) {
    const auto* p = sk.getPoint(ptId);
    for (const auto& l : sk.getLines()) {
        if (l.id != lineId) continue;
        const auto* a = sk.getPoint(l.startPointId);
        const auto* b = sk.getPoint(l.endPointId);
        glm::vec2 d = b->pos - a->pos;
        glm::vec2 r = p->pos - a->pos;
        return std::abs(d.x * r.y - d.y * r.x) / glm::length(d);
    }
    return -1.0;
}

Constraint makeDPL(int ptId, int lineId, double value) {
    Constraint c{};
    c.id = 0;
    c.type = ConstraintType::DistancePointLine;
    c.entityA = ptId;
    c.entityB = lineId;
    c.value = value;
    return c;
}

} // namespace

TEST(DistancePointLine, ConvergesToTarget) {
    Sketch sk;
    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({10.0f, 0.0f});
    int ln = sk.addLine(a, b);
    int p = sk.addPoint({5.0f, 3.0f});
    sk.addConstraint(makeDPL(p, ln, 7.0));

    SketchSolver solver;
    EXPECT_TRUE(solver.solve(sk, 500, 1e-4));
    EXPECT_NEAR(pointLineDist(sk, p, ln), 7.0, 1e-3);
}

TEST(DistancePointLine, DegenerateLineDoesNotNaN) {
    Sketch sk;
    int a = sk.addPoint({2.0f, 2.0f});
    int b = sk.addPoint({2.0f, 2.0f}); // zero-length line
    int ln = sk.addLine(a, b);
    int p = sk.addPoint({5.0f, 3.0f});
    sk.addConstraint(makeDPL(p, ln, 4.0));

    SketchSolver solver;
    solver.solve(sk, 100, 1e-4); // must not crash or NaN, return value unspecified
    for (const auto& pt : sk.getPoints()) {
        EXPECT_TRUE(std::isfinite(pt.pos.x));
        EXPECT_TRUE(std::isfinite(pt.pos.y));
    }
}

TEST(DistancePointLine, MissingEntitiesAreInert) {
    Sketch sk;
    int p = sk.addPoint({1.0f, 1.0f});
    sk.addConstraint(makeDPL(p, 9999, 4.0)); // no such line
    SketchSolver solver;
    solver.solve(sk, 50, 1e-4);
    EXPECT_NEAR(sk.getPoint(p)->pos.x, 1.0f, 1e-6);
    EXPECT_NEAR(sk.getPoint(p)->pos.y, 1.0f, 1e-6);
}
```

Register in `tests/CMakeLists.txt` (append at the end, matching neighbors):

```cmake
# test_sketch_dimension - DistancePointLine solver, dimension pair resolution,
# and K-line label-offset persistence (Onshape-style dimension tool).
add_executable(test_sketch_dimension test_sketch_dimension.cpp)
target_link_libraries(test_sketch_dimension PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_sketch_dimension COMMAND test_sketch_dimension)
```

- [ ] **Step 2: Run tests, verify they fail to compile**

Run: `cmake --build build -j 8 --target test_sketch_dimension`
Expected: compile error - `DistancePointLine` is not a member of `ConstraintType`.

- [ ] **Step 3: Add the enum value and implement the solver cases**

`src/modeling/SketchConstraints.h` - append to the enum (comment style matches neighbors):

```cpp
    Concentric,    // two circles/arcs share same center
    Angle,         // fixed angle (radians) between two lines
    DistancePointLine // fixed perpendicular distance from a point to a line's infinite carrier
```

Do NOT add the label-offset fields yet (Task 2).

`src/modeling/SketchSolver.cpp` - three additions:

1. In `solve()`'s `numEquations` switch:

```cpp
            case ConstraintType::DistancePointLine:
                numEquations += 1;
                break;
```

2. In `computeError()` (after the `Angle` case):

```cpp
        case ConstraintType::DistancePointLine: {
            // entityA = point id, entityB = line id. Distance is to the
            // line's INFINITE carrier, matching how CAD dimensions read.
            const SketchPoint* p = sketch.getPoint(c.entityA);
            if (!p) return 0.0;
            for (const auto& line : sketch.getLines()) {
                if (line.id != c.entityB) continue;
                const SketchPoint* a = sketch.getPoint(line.startPointId);
                const SketchPoint* b = sketch.getPoint(line.endPointId);
                if (!a || !b) return 0.0;
                glm::vec2 dir = b->pos - a->pos;
                float len = glm::length(dir);
                if (len < 1e-10f) return 0.0; // degenerate line: inert, no NaN
                glm::vec2 rel = p->pos - a->pos;
                double dist = std::abs(static_cast<double>(dir.x) * rel.y -
                                       static_cast<double>(dir.y) * rel.x) / len;
                return dist - c.value;
            }
            return 0.0;
        }
```

3. In `applyCorrection()` (after the `Angle` case; split correction between the point and the line, mirroring the `Distance` case):

```cpp
        case ConstraintType::DistancePointLine: {
            const SketchPoint* p = sketch.getPoint(c.entityA);
            if (!p) return;
            for (const auto& line : sketch.getLines()) {
                if (line.id != c.entityB) continue;
                const SketchPoint* a = sketch.getPoint(line.startPointId);
                const SketchPoint* b = sketch.getPoint(line.endPointId);
                if (!a || !b) return;
                glm::vec2 dir = b->pos - a->pos;
                float len = glm::length(dir);
                if (len < 1e-10f) return;
                dir /= len;
                glm::vec2 n(-dir.y, dir.x); // unit normal
                float s = glm::dot(p->pos - a->pos, n); // signed distance
                if (s < 0.0f) { n = -n; s = -s; }       // n points line → point
                float corr = (static_cast<float>(c.value) - s) * 0.5f;
                sketch.movePoint(c.entityA, p->pos + n * corr);
                sketch.movePoint(line.startPointId, a->pos - n * corr);
                sketch.movePoint(line.endPointId,   b->pos - n * corr);
                return;
            }
            break;
        }
```

- [ ] **Step 4: Build and run the tests**

Run: `cmake --build build -j 8 --target test_sketch_dimension && ./build/tests/test_sketch_dimension`
(If the test binary lands elsewhere, find it: `find build -name test_sketch_dimension -type f`.)
Expected: all 3 tests PASS.

- [ ] **Step 5: Orphan-cleanup sanity check (no code expected)**

`Sketch::pruneOrphanPoints()` already drops any constraint whose `entityA`/`entityB` id vanished (generic id check over points AND elements - `src/modeling/Sketch.cpp:655`). Confirm by reading that function; `DistancePointLine` needs no special case. If that generic check has changed, add the new type to it.

- [ ] **Step 6: Commit**

```bash
git add src/modeling/SketchConstraints.h src/modeling/SketchSolver.cpp tests/test_sketch_dimension.cpp tests/CMakeLists.txt
git commit -m "sketch solver: DistancePointLine constraint (point to infinite line)"
```

---

### Task 2: Label offsets on `Constraint` + K-line persistence

**Files:**
- Modify: `src/modeling/SketchConstraints.h`
- Modify: `src/io/ProjectIO.cpp` (writer ~line 328, reader ~line 650)
- Modify: `tests/test_sketch_dimension.cpp` (add roundtrip tests)

**Interfaces:**
- Consumes: `ProjectIO::save(const std::string&, const Document&, const ProjectHistory* = nullptr)`, `ProjectIO::load(const std::string&, Document&, ProjectHistory* = nullptr)` (`src/io/ProjectIO.h:61-66`), `Document::addSketch(std::shared_ptr<Sketch>, const std::string&)`, `Document::getSketch(int)`.
- Produces: `Constraint::labelOffX` / `labelOffY` (`double`, default 0.0; sketch-space label offset from the auto anchor; `0,0` = auto placement). K-line format `K id type eA eB value valueY labelOffX labelOffY`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_sketch_dimension.cpp`:

```cpp
#include "core/Document.h"
#include "io/ProjectIO.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>

using materializr::ProjectIO;

namespace {

std::string tmpProjectPath(const char* name) {
    const char* t = std::getenv("TMPDIR");
    std::string dir = t ? t : "/tmp";
    if (!dir.empty() && dir.back() != '/') dir += '/';
    return dir + name;
}

} // namespace

TEST(DimensionPersistence, KLineRoundTripsTypeAndLabelOffsets) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    int a = sk->addPoint({0.0f, 0.0f});
    int b = sk->addPoint({10.0f, 0.0f});
    int ln = sk->addLine(a, b);
    int p = sk->addPoint({5.0f, 3.0f});
    Constraint c{};
    c.type = ConstraintType::DistancePointLine;
    c.entityA = p;
    c.entityB = ln;
    c.value = 3.0;
    c.labelOffX = 1.5;
    c.labelOffY = -2.25;
    sk->addConstraint(c);
    doc.addSketch(sk, "dim-test");

    std::string path = tmpProjectPath("dim_roundtrip.mzr");
    ASSERT_TRUE(ProjectIO::save(path, doc).success);

    Document loaded;
    ASSERT_TRUE(ProjectIO::load(path, loaded).success);
    std::remove(path.c_str());

    // First (only) sketch in the loaded doc.
    auto ids = loaded.getSketchIds();
    ASSERT_EQ(ids.size(), 1u);
    auto lsk = loaded.getSketch(ids[0]);
    ASSERT_TRUE(lsk);
    ASSERT_EQ(lsk->getConstraints().size(), 1u);
    const Constraint& lc = lsk->getConstraints()[0];
    EXPECT_EQ(lc.type, ConstraintType::DistancePointLine);
    EXPECT_DOUBLE_EQ(lc.value, 3.0);
    EXPECT_DOUBLE_EQ(lc.labelOffX, 1.5);
    EXPECT_DOUBLE_EQ(lc.labelOffY, -2.25);
}

TEST(DimensionPersistence, LegacySixFieldKLineDefaultsOffsetsToZero) {
    // Save with the new writer, then truncate every K line back to the legacy
    // 6-field form and reload - offsets must default to 0, load must succeed.
    Document doc;
    auto sk = std::make_shared<Sketch>();
    int a = sk->addPoint({0.0f, 0.0f});
    int b = sk->addPoint({4.0f, 0.0f});
    sk->addLine(a, b);
    Constraint c{};
    c.type = ConstraintType::Distance;
    c.entityA = a;
    c.entityB = b;
    c.value = 4.0;
    c.labelOffX = 9.0; // will be stripped below
    c.labelOffY = 9.0;
    sk->addConstraint(c);
    doc.addSketch(sk, "legacy");

    std::string path = tmpProjectPath("dim_legacy.mzr");
    ASSERT_TRUE(ProjectIO::save(path, doc).success);

    // Strip trailing fields from K lines: keep "K id type eA eB value valueY".
    std::ifstream in(path);
    std::stringstream out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("K ", 0) == 0) {
            std::istringstream ls(line);
            std::string tok, kept;
            for (int i = 0; i < 7 && (ls >> tok); ++i) { // "K" + 6 fields
                if (i) kept += ' ';
                kept += tok;
            }
            out << kept << '\n';
        } else {
            out << line << '\n';
        }
    }
    in.close();
    std::ofstream ow(path, std::ios::trunc);
    ow << out.str();
    ow.close();

    Document loaded;
    ASSERT_TRUE(ProjectIO::load(path, loaded).success);
    std::remove(path.c_str());
    auto lsk = loaded.getSketch(loaded.getSketchIds()[0]);
    ASSERT_EQ(lsk->getConstraints().size(), 1u);
    EXPECT_DOUBLE_EQ(lsk->getConstraints()[0].labelOffX, 0.0);
    EXPECT_DOUBLE_EQ(lsk->getConstraints()[0].labelOffY, 0.0);
}
```

Note: if `Document::getSketchIds()` doesn't exist under that name, find the real accessor with `grep -n "getSketchIds\|sketchIds\|getAllSketch" src/core/Document.h` and use it; keep the rest of the test identical.

- [ ] **Step 2: Run tests, verify they fail**

Run: `cmake --build build -j 8 --target test_sketch_dimension && ./build/tests/test_sketch_dimension --gtest_filter='DimensionPersistence.*'`
Expected: FAIL - `labelOffX` not a member (compile), then after Step 3's header change, offsets not persisted (runtime).

- [ ] **Step 3: Implement**

`src/modeling/SketchConstraints.h`, inside `struct Constraint` after `valueY`:

```cpp
    // Sketch-space offset of the dimension label from its auto-computed
    // anchor. (0,0) = legacy auto placement (pre-offset files and constraints
    // created without explicit label placement).
    double labelOffX = 0.0;
    double labelOffY = 0.0;
```

`src/io/ProjectIO.cpp` writer (~line 328) - the K line becomes:

```cpp
        for (const auto& c : cns) {
            ofs << "K " << c.id << " " << static_cast<int>(c.type) << " "
                << c.entityA << " " << c.entityB << " "
                << c.value << " " << c.valueY << " "
                << c.labelOffX << " " << c.labelOffY << "\n";
        }
```

Update the comment above it: the K line carries 8 fields, trailing two are label offsets, readers of older builds ignore trailing tokens.

`src/io/ProjectIO.cpp` reader (~line 655) - after the existing extraction:

```cpp
                s >> t >> c.id >> tval >> c.entityA >> c.entityB >> c.value >> c.valueY;
                c.type = static_cast<ConstraintType>(tval);
                // Label offsets are trailing optional fields (since the
                // dimension tool); legacy 6-field K lines default to auto
                // placement.
                if (!(s >> c.labelOffX >> c.labelOffY)) {
                    c.labelOffX = 0.0;
                    c.labelOffY = 0.0;
                }
```

- [ ] **Step 4: Run tests, verify they pass**

Run: `./build/tests/test_sketch_dimension`
Expected: all tests PASS (Task 1's included).

- [ ] **Step 5: Commit**

```bash
git add src/modeling/SketchConstraints.h src/io/ProjectIO.cpp tests/test_sketch_dimension.cpp
git commit -m "sketch: persist dimension label offsets on constraints (K-line v2)"
```

---

### Task 3: Dimension mode state machine in `SketchTool`

**Files:**
- Modify: `src/modeling/SketchTool.h` (mode enum, new public types + API)
- Modify: `src/modeling/SketchTool.cpp` (mode dispatch in `onMouseDown`/`onMouseMove`/`onCancel`, new methods)
- Modify: `tests/test_sketch_dimension.cpp` (pair-resolution tests)

**Interfaces:**
- Consumes: `ConstraintType::DistancePointLine` (Task 1), Sketch accessors, `findCoincidentPoint`.
- Produces (later tasks call these exact names):

```cpp
enum class SketchToolMode { None, Select, Line, Circle, Rectangle, Arc, Spline,
                            Polygon, Trim, Text, Svg, Mirror, Dimension }; // Dimension appended

enum class DimEntityKind { None, Point, Line, Circle, Arc };
struct DimPick { DimEntityKind kind = DimEntityKind::None; int id = -1; };

// A pick set resolved into the constraint it would create. measured is the
// current geometry value: mm for distances, RADIUS in mm for Radius (UI
// doubles it for display), SIGNED radians for Angle (line B rel. line A).
struct PendingDimension {
    ConstraintType type = ConstraintType::Distance;
    int entityA = -1, entityB = -1;
    double measured = 0.0;
    bool valid = false;
};

enum class DimPhase { PickFirst, PickSecondOrPlace, PlaceLabel };

// on SketchTool, public:
DimPhase getDimPhase() const;
DimPick getDimPickA() const;
const PendingDimension& getPendingDimension() const;
glm::vec2 getDimLabelPos() const;      // valid when dimReadyToCommit()
bool dimReadyToCommit() const;         // label placed; app commits + calls clearDimState()
void clearDimState();                  // back to PickFirst, pending invalidated
DimPick dimHitTest(glm::vec2 pos) const; // hover highlight for the viewport
static PendingDimension resolveDimension(const Sketch& sk, DimPick a, DimPick b);
```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_sketch_dimension.cpp`:

```cpp
#include "modeling/SketchTool.h"

using materializr::DimEntityKind;
using materializr::DimPick;
using materializr::PendingDimension;
using materializr::SketchTool;

namespace {

// 10-unit horizontal line at y=0 and a second line at `deg` degrees from it,
// plus a free point at (5,3). Returns ids via out-params.
struct DimFixture {
    Sketch sk;
    int pA, pB, lnAB;      // horizontal line
    int pC, pD, lnCD;      // rotated line
    int pFree;
    explicit DimFixture(float deg) {
        pA = sk.addPoint({0.0f, 0.0f});
        pB = sk.addPoint({10.0f, 0.0f});
        lnAB = sk.addLine(pA, pB);
        float r = deg * 3.14159265358979f / 180.0f;
        pC = sk.addPoint({0.0f, 5.0f});
        pD = sk.addPoint({10.0f * std::cos(r), 5.0f + 10.0f * std::sin(r)});
        lnCD = sk.addLine(pC, pD);
        pFree = sk.addPoint({5.0f, 3.0f});
    }
};

DimPick pick(DimEntityKind k, int id) { return DimPick{k, id}; }

} // namespace

TEST(DimensionResolve, CircleAloneIsRadius) {
    Sketch sk;
    int c = sk.addPoint({0.0f, 0.0f});
    int ci = sk.addCircle(c, 6.5);
    auto r = SketchTool::resolveDimension(sk, pick(DimEntityKind::Circle, ci), DimPick{});
    ASSERT_TRUE(r.valid);
    EXPECT_EQ(r.type, ConstraintType::Radius);
    EXPECT_EQ(r.entityA, ci);
    EXPECT_NEAR(r.measured, 6.5, 1e-9);
}

TEST(DimensionResolve, LineAloneIsEndpointDistance) {
    DimFixture f(30.0f);
    auto r = SketchTool::resolveDimension(f.sk, pick(DimEntityKind::Line, f.lnAB), DimPick{});
    ASSERT_TRUE(r.valid);
    EXPECT_EQ(r.type, ConstraintType::Distance);
    EXPECT_EQ(r.entityA, f.pA);
    EXPECT_EQ(r.entityB, f.pB);
    EXPECT_NEAR(r.measured, 10.0, 1e-6);
}

TEST(DimensionResolve, PointPointIsDistance) {
    DimFixture f(30.0f);
    auto r = SketchTool::resolveDimension(f.sk, pick(DimEntityKind::Point, f.pA),
                                          pick(DimEntityKind::Point, f.pFree));
    ASSERT_TRUE(r.valid);
    EXPECT_EQ(r.type, ConstraintType::Distance);
    EXPECT_NEAR(r.measured, std::sqrt(25.0 + 9.0), 1e-6);
}

TEST(DimensionResolve, PointLineEitherOrderIsDistancePointLine) {
    DimFixture f(30.0f);
    auto r1 = SketchTool::resolveDimension(f.sk, pick(DimEntityKind::Point, f.pFree),
                                           pick(DimEntityKind::Line, f.lnAB));
    auto r2 = SketchTool::resolveDimension(f.sk, pick(DimEntityKind::Line, f.lnAB),
                                           pick(DimEntityKind::Point, f.pFree));
    for (const auto& r : {r1, r2}) {
        ASSERT_TRUE(r.valid);
        EXPECT_EQ(r.type, ConstraintType::DistancePointLine);
        EXPECT_EQ(r.entityA, f.pFree);
        EXPECT_EQ(r.entityB, f.lnAB);
        EXPECT_NEAR(r.measured, 3.0, 1e-6);
    }
}

TEST(DimensionResolve, ParallelLinesGiveDistance_NonParallelGiveAngle) {
    DimFixture par(0.5f);   // inside the 1° parallel threshold
    auto rp = SketchTool::resolveDimension(par.sk, pick(DimEntityKind::Line, par.lnAB),
                                           pick(DimEntityKind::Line, par.lnCD));
    ASSERT_TRUE(rp.valid);
    EXPECT_EQ(rp.type, ConstraintType::DistancePointLine);
    EXPECT_EQ(rp.entityA, par.pC);   // second line's start point
    EXPECT_EQ(rp.entityB, par.lnAB); // measured against the first line

    DimFixture ang(30.0f);
    auto ra = SketchTool::resolveDimension(ang.sk, pick(DimEntityKind::Line, ang.lnAB),
                                           pick(DimEntityKind::Line, ang.lnCD));
    ASSERT_TRUE(ra.valid);
    EXPECT_EQ(ra.type, ConstraintType::Angle);
    EXPECT_EQ(ra.entityA, ang.lnAB);
    EXPECT_EQ(ra.entityB, ang.lnCD);
    EXPECT_NEAR(ra.measured, 30.0 * 3.14159265358979 / 180.0, 1e-4); // signed, B rel A
}

TEST(DimensionResolve, InvalidCombosAreInvalid) {
    Sketch sk;
    int c = sk.addPoint({0.0f, 0.0f});
    int ci = sk.addCircle(c, 2.0);
    int p = sk.addPoint({5.0f, 0.0f});
    // circle + point is out of scope (spec non-goal)
    auto r = SketchTool::resolveDimension(sk, pick(DimEntityKind::Circle, ci),
                                          pick(DimEntityKind::Point, p));
    EXPECT_FALSE(r.valid);
    // lone point
    auto r2 = SketchTool::resolveDimension(sk, pick(DimEntityKind::Point, p), DimPick{});
    EXPECT_FALSE(r2.valid);
    // dangling id
    auto r3 = SketchTool::resolveDimension(sk, pick(DimEntityKind::Line, 9999), DimPick{});
    EXPECT_FALSE(r3.valid);
}
```

Note on `resolveDimension` line-vs-line normalization: the tests pin **first-picked line = entityA / reference**; for the parallel case entityA is the SECOND line's start point measured against the FIRST line (`entityB` = first line id)… careful: `DistancePointLine` defines entityA = point, entityB = line. The test above encodes: pair (lineAB first, lineCD second) → point = `lnCD`'s start (`pC`), line = `lnAB`. That matches the spec ("first line's start point ↔ second line") mirrored - **the test is the source of truth here: point from the second-picked line, measured to the first-picked line**, which keeps the first pick as the reference entity for both the parallel and angle branches. (Deviation from spec wording, same geometry - note it in the commit message.)

- [ ] **Step 2: Run tests, verify they fail to compile**

Run: `cmake --build build -j 8 --target test_sketch_dimension`
Expected: compile error - `resolveDimension` / `DimPick` not declared.

- [ ] **Step 3: Implement in SketchTool**

`src/modeling/SketchTool.h`:
- Append `Dimension` to `SketchToolMode`.
- Add the `DimEntityKind` / `DimPick` / `PendingDimension` / `DimPhase` declarations (namespace scope, before `class SketchTool`) exactly as in **Interfaces** above.
- Public members on `SketchTool` exactly as in **Interfaces**.
- Private state:

```cpp
    // --- Dimension tool state ---
    DimPhase m_dimPhase = DimPhase::PickFirst;
    DimPick m_dimPickA;
    PendingDimension m_dimPending;
    glm::vec2 m_dimLabelPos{0.0f};
    bool m_dimReady = false;
    DimPick hitTestDimEntity(glm::vec2 pos) const;
    void handleDimensionTool(glm::vec2 pos);
```

`src/modeling/SketchTool.cpp`:

1. `setMode()`: entering or leaving `Dimension` calls `clearDimState()`.

2. `onMouseDown()` dispatch: add `case SketchToolMode::Dimension: handleDimensionTool(pos); break;` (use the RAW cursor like Trim - no snapping; see the comment at the top of `onMouseDown`).

3. `onMouseMove()`: in Dimension mode just record `m_currentPos = pos;` (the viewport reads it for the ghost label) and return before snapping logic.

4. `onCancel()`: in Dimension mode - if `m_dimPhase != DimPhase::PickFirst || m_dimReady`, `clearDimState()`; else `setMode(SketchToolMode::Select)`.

5. Hit test - same priority and tolerance as `handleSelectTool` (point → line → circle/arc, `tol = std::max(m_gridStep * 0.5f, 0.5f) * snapScale()`); factor the shared scan or duplicate it (~40 lines), returning `DimPick`:

```cpp
DimPick SketchTool::hitTestDimEntity(glm::vec2 pos) const {
    DimPick out;
    if (!m_sketch) return out;
    int nearPt = findCoincidentPoint(pos, -1);
    if (nearPt >= 0) {
        // Text glyph geometry is not dimensionable.
        const SketchPoint* p = m_sketch->getPoint(nearPt);
        if (p && !p->fromText) { out = {DimEntityKind::Point, nearPt}; }
        return out;
    }
    const float tol = std::max(m_gridStep * 0.5f, 0.5f) * snapScale();
    // Lines (segment distance), skipping fromText - same math as handleSelectTool.
    float bestD = 0.0f; int bestLine = -1;
    for (const auto& l : m_sketch->getLines()) {
        if (l.fromText) continue;
        const SketchPoint* a = m_sketch->getPoint(l.startPointId);
        const SketchPoint* b = m_sketch->getPoint(l.endPointId);
        if (!a || !b) continue;
        glm::vec2 ab = b->pos - a->pos;
        float len2 = glm::dot(ab, ab);
        if (len2 < 1e-12f) continue;
        float t = glm::clamp(glm::dot(pos - a->pos, ab) / len2, 0.0f, 1.0f);
        float d = glm::distance(a->pos + ab * t, pos);
        if (d < tol && (bestLine < 0 || d < bestD)) { bestLine = l.id; bestD = d; }
    }
    if (bestLine >= 0) return {DimEntityKind::Line, bestLine};
    // Circle then arc perimeters - same as handleSelectTool.
    float bestCd = 0.0f; int bestCircle = -1;
    for (const auto& c : m_sketch->getCircles()) {
        const SketchPoint* ctr = m_sketch->getPoint(c.centerPointId);
        if (!ctr) continue;
        float d = std::abs(glm::distance(pos, ctr->pos) - static_cast<float>(c.radius));
        if (d < tol && (bestCircle < 0 || d < bestCd)) { bestCircle = c.id; bestCd = d; }
    }
    if (bestCircle >= 0) return {DimEntityKind::Circle, bestCircle};
    float bestAd = 0.0f; int bestArc = -1;
    for (const auto& a : m_sketch->getArcs()) {
        const SketchPoint* ctr = m_sketch->getPoint(a.centerPointId);
        if (!ctr) continue;
        float d = std::abs(glm::distance(pos, ctr->pos) - static_cast<float>(a.radius));
        if (d < tol && (bestArc < 0 || d < bestAd)) { bestArc = a.id; bestAd = d; }
    }
    if (bestArc >= 0) return {DimEntityKind::Arc, bestArc};
    return out;
}
```

6. State machine:

```cpp
void SketchTool::handleDimensionTool(glm::vec2 pos) {
    if (!m_sketch) return;
    m_currentPos = pos;
    switch (m_dimPhase) {
        case DimPhase::PickFirst: {
            DimPick hit = hitTestDimEntity(pos);
            if (hit.kind == DimEntityKind::None) return;
            if (hit.kind == DimEntityKind::Circle || hit.kind == DimEntityKind::Arc) {
                m_dimPending = resolveDimension(*m_sketch, hit, DimPick{});
                if (m_dimPending.valid) { m_dimPickA = hit; m_dimPhase = DimPhase::PlaceLabel; }
                return;
            }
            m_dimPickA = hit;
            if (hit.kind == DimEntityKind::Line)
                m_dimPending = resolveDimension(*m_sketch, hit, DimPick{}); // tentative length
            else
                m_dimPending = PendingDimension{}; // lone point: no dim yet
            m_dimPhase = DimPhase::PickSecondOrPlace;
            return;
        }
        case DimPhase::PickSecondOrPlace: {
            DimPick hit = hitTestDimEntity(pos);
            if (hit.kind != DimEntityKind::None) {
                if (hit.kind == m_dimPickA.kind && hit.id == m_dimPickA.id) return; // same entity
                PendingDimension pair = resolveDimension(*m_sketch, m_dimPickA, hit);
                if (pair.valid) { m_dimPending = pair; m_dimPhase = DimPhase::PlaceLabel; }
                return; // invalid combo: ignore the click, picks unchanged
            }
            // Empty space: places the tentative single-entity dim (line length).
            if (m_dimPending.valid) { m_dimLabelPos = pos; m_dimReady = true; }
            return; // lone point + empty space: no-op, pick stays pending
        }
        case DimPhase::PlaceLabel: {
            m_dimLabelPos = pos;
            m_dimReady = true;
            return;
        }
    }
}

void SketchTool::clearDimState() {
    m_dimPhase = DimPhase::PickFirst;
    m_dimPickA = DimPick{};
    m_dimPending = PendingDimension{};
    m_dimReady = false;
}
```

7. `resolveDimension` (static, pure):

```cpp
PendingDimension SketchTool::resolveDimension(const Sketch& sk, DimPick a, DimPick b) {
    PendingDimension out;
    auto lineById = [&sk](int id) -> const SketchLine* {
        for (const auto& l : sk.getLines()) if (l.id == id) return &l;
        return nullptr;
    };
    auto lineEnds = [&sk, &lineById](int id, glm::vec2& s, glm::vec2& e) {
        const SketchLine* l = lineById(id);
        if (!l) return false;
        const SketchPoint* sp = sk.getPoint(l->startPointId);
        const SketchPoint* ep = sk.getPoint(l->endPointId);
        if (!sp || !ep) return false;
        s = sp->pos; e = ep->pos;
        return true;
    };
    auto perpDist = [](glm::vec2 p, glm::vec2 s, glm::vec2 e) -> double {
        glm::vec2 d = e - s;
        float len = glm::length(d);
        if (len < 1e-10f) return -1.0;
        glm::vec2 r = p - s;
        return std::abs(static_cast<double>(d.x) * r.y -
                        static_cast<double>(d.y) * r.x) / len;
    };

    // Single-entity dims.
    if (b.kind == DimEntityKind::None) {
        if (a.kind == DimEntityKind::Circle) {
            for (const auto& c : sk.getCircles())
                if (c.id == a.id) { out = {ConstraintType::Radius, a.id, -1, c.radius, true}; break; }
            return out;
        }
        if (a.kind == DimEntityKind::Arc) {
            for (const auto& ar : sk.getArcs())
                if (ar.id == a.id) { out = {ConstraintType::Radius, a.id, -1, ar.radius, true}; break; }
            return out;
        }
        if (a.kind == DimEntityKind::Line) {
            const SketchLine* l = lineById(a.id);
            glm::vec2 s, e;
            if (l && lineEnds(a.id, s, e))
                out = {ConstraintType::Distance, l->startPointId, l->endPointId,
                       static_cast<double>(glm::distance(s, e)), true};
            return out;
        }
        return out; // lone point: invalid
    }

    // Normalize point-first for the mixed pair.
    if (a.kind == DimEntityKind::Line && b.kind == DimEntityKind::Point) std::swap(a, b);

    if (a.kind == DimEntityKind::Point && b.kind == DimEntityKind::Point) {
        const SketchPoint* pa = sk.getPoint(a.id);
        const SketchPoint* pb = sk.getPoint(b.id);
        if (pa && pb)
            out = {ConstraintType::Distance, a.id, b.id,
                   static_cast<double>(glm::distance(pa->pos, pb->pos)), true};
        return out;
    }
    if (a.kind == DimEntityKind::Point && b.kind == DimEntityKind::Line) {
        const SketchPoint* p = sk.getPoint(a.id);
        glm::vec2 s, e;
        if (p && lineEnds(b.id, s, e)) {
            double d = perpDist(p->pos, s, e);
            if (d >= 0.0) out = {ConstraintType::DistancePointLine, a.id, b.id, d, true};
        }
        return out;
    }
    if (a.kind == DimEntityKind::Line && b.kind == DimEntityKind::Line) {
        glm::vec2 as, ae, bs, be;
        if (!lineEnds(a.id, as, ae) || !lineEnds(b.id, bs, be)) return out;
        glm::vec2 da = ae - as, db = be - bs;
        if (glm::length(da) < 1e-10f || glm::length(db) < 1e-10f) return out;
        // Signed angle of B relative to A, wrapped to [-π, π] - same
        // convention as the solver's Angle error term.
        double ang = std::atan2(db.y, db.x) - std::atan2(da.y, da.x);
        while (ang >  M_PI) ang -= 2.0 * M_PI;
        while (ang < -M_PI) ang += 2.0 * M_PI;
        // Parallel (or anti-parallel) within 1°: distance dim. The point is
        // the SECOND line's start, measured to the FIRST line, so the first
        // pick stays the reference for both branches.
        const double kParallelTol = 1.0 * M_PI / 180.0;
        double folded = std::min(std::abs(ang), M_PI - std::abs(ang));
        if (folded <= kParallelTol) {
            const SketchLine* lb = lineById(b.id);
            double d = perpDist(bs, as, ae);
            if (lb && d >= 0.0)
                out = {ConstraintType::DistancePointLine, lb->startPointId, a.id, d, true};
        } else {
            out = {ConstraintType::Angle, a.id, b.id, ang, true};
        }
        return out;
    }
    return out; // circle/arc pairs and circle+point: out of scope
}
```

Needs `#include <cmath>` and `M_PI` (already used elsewhere in the file's includes - verify).

Accessor bodies (header, inline): `getDimPhase`, `getDimPickA`, `getPendingDimension`, `getDimLabelPos`, `dimReadyToCommit` return the corresponding members; `dimHitTest(pos)` forwards to `hitTestDimEntity(pos)`.

- [ ] **Step 4: Run tests, verify they pass**

Run: `cmake --build build -j 8 --target test_sketch_dimension && ./build/tests/test_sketch_dimension`
Expected: all tests PASS.

- [ ] **Step 5: Full build (nothing else broke the mode enum switches)**

Run: `cmake --build build -j 8 2>&1 | tail -20`
Expected: clean build. If a switch over `SketchToolMode` warns/errs on the new enumerator, add a no-op `case SketchToolMode::Dimension:` there.

- [ ] **Step 6: Commit**

```bash
git add src/modeling/SketchTool.h src/modeling/SketchTool.cpp tests/test_sketch_dimension.cpp
git commit -m "sketch: Dimension tool state machine + pick resolution (line-line ref = first pick)"
```

---

### Task 4: App integration - `d` key, click routing, commit path

**Files:**
- Modify: `src/app/Application.h` (declare `applyPendingDimension()`)
- Modify: `src/app/Application.cpp` (key binding near the Ctrl+D handler at ~line 2340; `applyPendingDimension` near `applySketchConstraint` at ~line 4384)
- Modify: `src/app/Application_Viewport.cpp` (click routing branch at ~line 5805)

**Interfaces:**
- Consumes: `SketchTool` Dimension API (Task 3), `recordSketchMutation(fn)`, `m_activeSketch`, `m_dimEditingId` / `m_dimEditingBuf` / `m_dimEditingFocus` + `##DimEdit` popup (existing, `Application_Viewport.cpp` ~2344-2560).
- Produces: `void Application::applyPendingDimension();` - commits the tool's pending dimension as one undoable constraint add (with dedup-replace), then opens the existing value-edit popup on it.

- [ ] **Step 1: Key binding**

`src/app/Application.cpp`, immediately BEFORE the Ctrl+D duplicate block (~line 2340):

```cpp
    // Plain D - Dimension tool in sketch mode (Onshape-style). Ctrl+D stays
    // Duplicate (handled below); text-input focus swallows the key.
    if (m_inSketchMode && m_sketchTool && !io.KeyCtrl && !io.WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_D, false)) {
        m_sketchTool->setMode(SketchToolMode::Dimension);
    }
```

- [ ] **Step 2: Commit path**

`src/app/Application.h` - next to `applySketchConstraint` (line 338):

```cpp
    // Commit the Dimension tool's resolved pending dimension: one undoable
    // constraint add (or value+label update when the same pair is already
    // dimensioned), then open the ##DimEdit popup on it for value entry.
    void applyPendingDimension();
```

`src/app/Application.cpp` - after `applySketchConstraint`:

```cpp
void Application::applyPendingDimension() {
    if (!m_inSketchMode || !m_activeSketch || !m_sketchTool) return;
    const PendingDimension& pd = m_sketchTool->getPendingDimension();
    if (!pd.valid || !m_sketchTool->dimReadyToCommit()) return;

    // Label offset = placed position minus the auto anchor the renderer uses.
    // The renderer resolves anchor per type; store the raw placed position
    // relative to the dimension's geometric anchor (computed the same way the
    // label pass does - see dimensionAutoAnchor in Application_Viewport.cpp).
    glm::vec2 anchor = dimensionAutoAnchor(pd);           // Task 5 helper
    glm::vec2 off = m_sketchTool->getDimLabelPos() - anchor;

    int editId = -1;
    recordSketchMutation([&] {
        // Dedup: same type on the same (unordered) entity pair replaces the
        // value + label instead of stacking - matches applyDimension's policy.
        for (const auto& c : m_activeSketch->getConstraints()) {
            if (c.type != pd.type) continue;
            bool same = (c.entityA == pd.entityA && c.entityB == pd.entityB);
            bool swapped = (c.type == ConstraintType::Distance &&
                            c.entityA == pd.entityB && c.entityB == pd.entityA);
            if (same || swapped) {
                m_activeSketch->updateConstraintValue(c.id, pd.measured);
                m_activeSketch->setConstraintLabelOffset(c.id, off.x, off.y);
                editId = c.id;
                return;
            }
        }
        Constraint c{};
        c.type = pd.type;
        c.entityA = pd.entityA;
        c.entityB = pd.entityB;
        c.value = pd.measured;
        c.labelOffX = off.x;
        c.labelOffY = off.y;
        editId = m_activeSketch->addConstraint(c);
    });
    m_sketchTool->clearDimState();

    // Open the existing edit popup, prefilled with the measured value -
    // Enter drives the geometry, Esc keeps the measured value.
    if (editId >= 0) {
        m_dimEditingId = editId;
        if (pd.type == ConstraintType::Angle)
            std::snprintf(m_dimEditingBuf, sizeof(m_dimEditingBuf), "%.2f",
                          pd.measured * 180.0 / M_PI);
        else if (pd.type == ConstraintType::Radius)
            std::snprintf(m_dimEditingBuf, sizeof(m_dimEditingBuf), "%.2f",
                          pd.measured * 2.0); // edited as diameter
        else
            std::snprintf(m_dimEditingBuf, sizeof(m_dimEditingBuf), "%.2f", pd.measured);
        m_dimEditingFocus = true;
        m_dimOpenEditRequested = true; // viewport OpenPopup s ##DimEdit next frame
    }
}
```

Supporting pieces this step also adds:
- `Sketch::updateConstraintValue(int id, double v)` and `Sketch::setConstraintLabelOffset(int id, double x, double y)` - if no value-setter exists yet (check `grep -n "updateConstraintValue\|setConstraint" src/modeling/Sketch.h`), add both as trivial find-and-set members in `Sketch.cpp` (the `##DimEdit` popup already writes values somehow - reuse THAT mechanism if present instead of adding a duplicate; adapt this code to whichever setter exists).
- `bool m_dimOpenEditRequested = false;` on `Application` (`Application.h`, near `m_dimEditingId` - find with `grep -n "m_dimEditingId" src/app/Application.h`): the popup must be opened from the viewport window's ImGui scope; the existing label-click path calls `ImGui::OpenPopup("##DimEdit")` inline there. In the viewport's dimension-label section add:

```cpp
                if (m_dimOpenEditRequested) {
                    ImGui::OpenPopup("##DimEdit");
                    m_dimOpenEditRequested = false;
                }
```

- Angle popup note: the existing `##DimEdit` commit path already converts the typed degrees back for Angle and doubles/halves for Radius (read the commit handler around `Application_Viewport.cpp:2541` and confirm; it drives the existing label-click edits, so no change expected).

- [ ] **Step 3: Click routing**

`src/app/Application_Viewport.cpp`, in the sketch mouse-down chain (~line 5805) - add a branch BEFORE the `materializr::touchMode()` branch (dimension picking must not fall into the press-drag-release paths):

```cpp
                    } else if (m_sketchTool->getMode() == SketchToolMode::Dimension) {
                        // Picking mutates nothing - no undo record. The commit
                        // below records the constraint add as one SketchEditOp.
                        m_sketchTool->onMouseDown(sketchCoord, false);
                        if (m_sketchTool->dimReadyToCommit())
                            applyPendingDimension();
                    } else if (materializr::touchMode()) {
```

- [ ] **Step 4: Build + smoke test**

Run: `cmake --build build -j 8 2>&1 | tail -5`
Expected: clean. Manual smoke (needs a display; Little Snitch rule already persisted): launch the app, enter sketch mode, draw a line, press `d`, click the line, click empty space, type `25`, Enter → line resizes to 25 mm, label shows `25.00 mm`, Ctrl+Z removes the dimension. Defer the full manual matrix to Task 6 if headless.

- [ ] **Step 5: Commit**

```bash
git add src/app/Application.h src/app/Application.cpp src/app/Application_Viewport.cpp src/modeling/Sketch.h src/modeling/Sketch.cpp
git commit -m "sketch: d-key Dimension tool - routing and constraint commit path"
```

---

### Task 5: Viewport rendering - hover, ghost label, leaders, offsets

**Files:**
- Modify: `src/app/Application_Viewport.cpp` (dimension-label pass at ~line 2344; sketch overlay for hover/ghost)
- Modify: `src/app/Application.h` (declare `dimensionAutoAnchor`)

**Interfaces:**
- Consumes: `SketchTool::dimHitTest`, `getDimPhase`, `getPendingDimension`, `getCurrentPos`, `Constraint::labelOffX/Y`.
- Produces: `glm::vec2 Application::dimensionAutoAnchor(const PendingDimension&) const` - sketch-space auto anchor per dimension type; Task 4's commit already calls it (implement here, declare in the same commit as Task 4 if building incrementally - otherwise stub it returning the label pos so Task 4 compiles, then finish here).

- [ ] **Step 1: Auto-anchor helper**

`Application.h` (private, near other sketch helpers):

```cpp
    // Sketch-space auto anchor of a dimension's label: line/pair midpoint,
    // circle/arc center, or the midpoint of the point-to-line perpendicular
    // foot segment. Label offsets are stored relative to this.
    glm::vec2 dimensionAutoAnchor(const PendingDimension& pd) const;
```

`Application_Viewport.cpp` (near the label pass so the two stay in sync):

```cpp
glm::vec2 Application::dimensionAutoAnchor(const PendingDimension& pd) const {
    if (!m_activeSketch || !pd.valid) return glm::vec2(0.0f);
    const Sketch& sk = *m_activeSketch;
    auto lineEnds = [&sk](int id, glm::vec2& s, glm::vec2& e) {
        for (const auto& l : sk.getLines())
            if (l.id == id) {
                const SketchPoint* sp = sk.getPoint(l.startPointId);
                const SketchPoint* ep = sk.getPoint(l.endPointId);
                if (!sp || !ep) return false;
                s = sp->pos; e = ep->pos; return true;
            }
        return false;
    };
    switch (pd.type) {
        case ConstraintType::Distance: {
            const SketchPoint* a = sk.getPoint(pd.entityA);
            const SketchPoint* b = sk.getPoint(pd.entityB);
            return (a && b) ? 0.5f * (a->pos + b->pos) : glm::vec2(0.0f);
        }
        case ConstraintType::Radius: {
            for (const auto& c : sk.getCircles())
                if (c.id == pd.entityA) {
                    const SketchPoint* ctr = sk.getPoint(c.centerPointId);
                    if (ctr) return ctr->pos;
                }
            for (const auto& a : sk.getArcs())
                if (a.id == pd.entityA) {
                    const SketchPoint* ctr = sk.getPoint(a.centerPointId);
                    if (ctr) return ctr->pos;
                }
            return glm::vec2(0.0f);
        }
        case ConstraintType::DistancePointLine: {
            const SketchPoint* p = sk.getPoint(pd.entityA);
            glm::vec2 s, e;
            if (p && lineEnds(pd.entityB, s, e)) {
                glm::vec2 d = e - s;
                float len2 = glm::dot(d, d);
                if (len2 > 1e-12f) {
                    float t = glm::dot(p->pos - s, d) / len2; // foot on infinite line
                    glm::vec2 foot = s + d * t;
                    return 0.5f * (p->pos + foot);
                }
            }
            return p ? p->pos : glm::vec2(0.0f);
        }
        case ConstraintType::Angle: {
            glm::vec2 as, ae, bs, be;
            if (lineEnds(pd.entityA, as, ae) && lineEnds(pd.entityB, bs, be))
                return 0.25f * (as + ae + bs + be);
            return glm::vec2(0.0f);
        }
        default: return glm::vec2(0.0f);
    }
}
```

- [ ] **Step 2: Stored offsets + leader lines in the label pass**

In the label pass (~2395 onward), each type currently computes `mid + perp`-style auto positions. Change the pattern per type to:

```cpp
                        glm::vec2 anchor = /* existing auto anchor for this type */;
                        glm::vec2 autoOff = /* existing offset computation */;
                        bool placed = (c.labelOffX != 0.0 || c.labelOffY != 0.0);
                        glm::vec2 lpos = placed
                            ? anchor + glm::vec2(static_cast<float>(c.labelOffX),
                                                 static_cast<float>(c.labelOffY))
                            : anchor + autoOff;
                        if (placed) {
                            // Leader: thin line from the geometry anchor to the label.
                            ImVec2 sa, sb;
                            if (toImg(dim2world(anchor), sa) && toImg(dim2world(lpos), sb))
                                dl->AddLine(sa, sb, IM_COL32(255, 235, 120, 90), 1.0f);
                        }
                        drawLabel(lpos, lbl, c);
```

Apply to the existing `Distance`, `Radius`, `Angle` branches, and add a new branch for `DistancePointLine`:

```cpp
                    } else if (c.type == ConstraintType::DistancePointLine) {
                        PendingDimension pd;
                        pd.type = c.type; pd.entityA = c.entityA;
                        pd.entityB = c.entityB; pd.valid = true;
                        glm::vec2 anchor = dimensionAutoAnchor(pd);
                        std::snprintf(lbl, sizeof(lbl), "%.2f mm", c.value);
                        // (same placed/leader pattern as above; autoOff = (2,2))
```

- [ ] **Step 3: Hover highlight + ghost label**

In the sketch overlay section (same scope as the label pass, where `dl`, `dim2world`, `toImg` are available), when `m_sketchTool->getMode() == SketchToolMode::Dimension`:

```cpp
                // Dimension tool feedback: highlight the hovered pickable
                // entity; after picks resolve, ghost the pending label at the
                // cursor with a leader from its anchor.
                DimPick hov = m_sketchTool->dimHitTest(sketchCursor);
                // sketchCursor: current mouse in sketch coords - reuse the
                // same unprojection the click handler feeds onMouseDown.
                if (hov.kind == DimEntityKind::Point) {
                    const SketchPoint* p = m_activeSketch->getPoint(hov.id);
                    ImVec2 sp;
                    if (p && toImg(dim2world(p->pos), sp))
                        dl->AddCircle(sp, 7.0f, IM_COL32(255, 235, 120, 255), 0, 2.0f);
                } else if (hov.kind == DimEntityKind::Line) {
                    // overdraw the segment in accent color
                    for (const auto& l : m_activeSketch->getLines())
                        if (l.id == hov.id) {
                            const SketchPoint* a = m_activeSketch->getPoint(l.startPointId);
                            const SketchPoint* b = m_activeSketch->getPoint(l.endPointId);
                            ImVec2 sa, sb;
                            if (a && b && toImg(dim2world(a->pos), sa) &&
                                toImg(dim2world(b->pos), sb))
                                dl->AddLine(sa, sb, IM_COL32(255, 235, 120, 200), 3.0f);
                        }
                } // circles/arcs: AddCircle at center with radius in screen px
                  // (project center and center+radius*X to get screen radius).

                const PendingDimension& pend = m_sketchTool->getPendingDimension();
                if (pend.valid &&
                    (m_sketchTool->getDimPhase() == DimPhase::PlaceLabel ||
                     m_sketchTool->getDimPhase() == DimPhase::PickSecondOrPlace)) {
                    char gbuf[40];
                    if (pend.type == ConstraintType::Angle)
                        std::snprintf(gbuf, sizeof(gbuf), "%.1f°",
                                      std::abs(pend.measured) * 180.0 / M_PI);
                    else if (pend.type == ConstraintType::Radius)
                        std::snprintf(gbuf, sizeof(gbuf), "⌀%.2f", pend.measured * 2.0);
                    else
                        std::snprintf(gbuf, sizeof(gbuf), "%.2f mm", pend.measured);
                    glm::vec2 anchor = dimensionAutoAnchor(pend);
                    ImVec2 sa, sc;
                    if (toImg(dim2world(anchor), sa) &&
                        toImg(dim2world(sketchCursor), sc)) {
                        dl->AddLine(sa, sc, IM_COL32(255, 235, 120, 90), 1.0f);
                        dl->AddText(ImVec2(sc.x + 8, sc.y - 8),
                                    IM_COL32(255, 235, 120, 220), gbuf);
                    }
                }
```

Also route mouse-move: where the sketch cursor position is computed each frame for other tools, call `m_sketchTool->onMouseMove(sketchCursor)` in Dimension mode too (check it isn't already called unconditionally - `grep -n "onMouseMove" src/app/Application_Viewport.cpp`).

Overlay hint text - `SketchPlugin::renderOverlay` prints the mode banner; add a Dimension-mode banner (in `src/plugins/SketchPlugin.cpp`, `renderOverlay`):

```cpp
        // (inside renderOverlay, replacing the single Text call with a mode check)
        if (m_sketchTool->getMode() == SketchToolMode::Dimension) {
            const char* msg = "DIMENSION - Click an entity to dimension.";
            switch (m_sketchTool->getDimPhase()) {
                case DimPhase::PickSecondOrPlace:
                    msg = "DIMENSION - Click a second entity, or empty space to place."; break;
                case DimPhase::PlaceLabel:
                    msg = "DIMENSION - Click to place the label."; break;
                default: break;
            }
            ImGui::Text("%s", msg);
        } else {
            ImGui::Text(/* existing sketch-mode banner */);
        }
```

- [ ] **Step 4: Build + visual check**

Run: `cmake --build build -j 8 2>&1 | tail -5`
Expected: clean. Visual: hover highlights each entity kind; ghost label + leader track the cursor; placed labels sit where clicked and survive save/reload.

- [ ] **Step 5: Commit**

```bash
git add src/app/Application.h src/app/Application_Viewport.cpp src/plugins/SketchPlugin.cpp
git commit -m "sketch dimension tool: hover highlight, ghost label, leaders, stored label offsets"
```

---

### Task 6: Toolbar + help + full verification

**Files:**
- Modify: `src/ui/Toolbar.h` (ToolAction), `src/ui/Toolbar.cpp` (desktop `skBtn` row ~line 649; touch `add()` row ~line 160)
- Modify: `src/app/Application.cpp` (ToolAction dispatch ~line 1792)
- Modify: `src/ui/HelpPanel.cpp` (Sketching section ~line 46)

**Interfaces:**
- Consumes: `SketchToolMode::Dimension` (enum int 12: None0…Mirror11,Dimension12), `skBtn(label, modeId)` (`Toolbar.cpp:566`), touch `add(icon, label, action, active, tip)` (`Toolbar.cpp:160`).
- Produces: `ToolAction::SketchDimension`.

- [ ] **Step 1: ToolAction + buttons + dispatch**

`Toolbar.h` - in the sketch-tool group of the enum (line 17), append `SketchDimension` after `SketchSvg`.

`Toolbar.cpp` desktop row (after the Trim button, line ~649-650):

```cpp
    if (skBtn("Dimension", 12))    action = ToolAction::SketchDimension;
    tip("Dimension tool (D): click a line, circle, point pair, or two lines, "
        "place the label, then type the value.");
```

`Toolbar.cpp` touch row (~line 160, after the Trim `add(...)`): reuse an existing measure-ish icon if `TouchIcons.h` has one (`grep -n MZ_ICON src/ui/TouchIcons.h`); otherwise skip the touch row and note it in the commit message (toolbar button still reachable in desktop layouts; touch parity tracked as follow-up).

```cpp
        add(MZ_ICON_MEASURE, "Dimension", ToolAction::SketchDimension,
            m_activeSketchMode == 12,
            "Dimension: tap entities, tap to place the label, type the value.");
```

Update the mode-int comment at `Toolbar.cpp:126` to mention `12=Dimension`.

`Application.cpp` dispatch (after the `ToolAction::Trim` case, ~line 1792):

```cpp
        case ToolAction::SketchDimension:
            if (m_inSketchMode) m_sketchTool->setMode(SketchToolMode::Dimension);
            break;
```

- [ ] **Step 2: Help text**

`HelpPanel.cpp` Sketching section - extend the existing string:

```cpp
    section("Sketching",
        "Pick Sketch on XY/XZ/YZ from the Tools panel (or Sketch on Face after "
        "selecting one), then choose a tool: Line, Rectangle, Circle, Arc, "
        "Spline, Polygon, or Trim. Type a numeric dimension while placing to "
        "lock the size. Press D for the Dimension tool: click a line, circle, "
        "two points, or two lines (parallel = distance, angled = angle), "
        "click to place the label, then type the value. Switch to Select / "
        "Move to drag existing points and lines - double-click empties to "
        "select the whole sketch, then use Copy / Mirror / Rotate. Click "
        "Finish Sketch (or press Enter) to exit.");
```

- [ ] **Step 3: Full build + full test suite**

Run (UNSANDBOXED): `cmake --build build -j 8 && cd build && ctest --output-on-failure -j 4`
Expected: full suite green, including the 4 new-ish `test_sketch_dimension` groups and all pre-existing suites.

- [ ] **Step 4: Manual GUI matrix**

Launch the app; in a sketch:
1. `d` → banner shows DIMENSION; `Esc` → back to Select.
2. Line → empty click → type 25 → Enter: length 25, label where placed.
3. Circle → click → type 40 → Enter: diameter 40 (label ⌀).
4. Two points → distance; point + line → perpendicular distance.
5. Two parallel lines → distance; two angled lines → angle in degrees.
6. Construction line variant of (4): same behavior.
7. Re-dimension the same line: value replaced, no duplicate label.
8. Ctrl+Z after each: dimension gone. Ctrl+D still duplicates (no clash).
9. Save, reload: labels at placed positions; open an OLD project: loads clean.
10. Esc in value input: dimension stays at measured value.

- [ ] **Step 5: Commit**

```bash
git add src/ui/Toolbar.h src/ui/Toolbar.cpp src/app/Application.cpp src/ui/HelpPanel.cpp
git commit -m "sketch dimension tool: toolbar button, D shortcut help"
```

---

## Self-Review Notes (resolved during planning)

- **Spec deviation, line-line reference:** spec says "first line's start point ↔ second line"; the plan (and tests) use second line's start point ↔ first line, keeping the first pick as the reference entity in both parallel and angle branches. Same geometry, more consistent; noted in Task 3's commit message.
- **Value input reuse:** spec's "inline input at the label" is implemented by opening the existing `##DimEdit` popup (already positioned at the clicked label, already converts diameter/degrees). One input path, not two.
- **Orphan cleanup:** verified generic in `Sketch::pruneOrphanPoints()` - no per-type change needed (Task 1 Step 5 double-checks).
- **Angle spec "clamped to (0°,180°)":** existing Angle is signed [-π,π] and `##DimEdit` already handles it; the ghost label shows `abs()`. No new clamping introduced - behavior matches existing Angle constraint edits.
