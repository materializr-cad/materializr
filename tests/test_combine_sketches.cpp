// Combine Sketches must re-id the absorbed sketch's geometry and keep every
// reference pointing at the copied elements. mergeInto() used to write
// `id[p.id] = p.id = next++`, which (right side first, C++17) keyed the
// old->new map by the NEW id: a copied line's endpoints then fell back to their
// old ids - landing on the TARGET sketch's points - and constraints were
// silently dropped or attached to unrelated elements.

#include "core/Document.h"
#include "modeling/CombineSketchesOp.h"
#include "modeling/Sketch.h"
#include "modeling/SketchConstraints.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <memory>

using materializr::Constraint;
using materializr::ConstraintType;
using materializr::Sketch;
using materializr::SketchLine;

namespace {
constexpr float kTol = 1e-5f;

glm::vec2 pointPos(const Sketch& sk, int id) {
    const auto* p = sk.getPoint(id);
    EXPECT_NE(p, nullptr) << "no point " << id;
    return p ? p->pos : glm::vec2(1e9f);
}

// The merged line whose endpoints sit at a and b (in either order), or null.
const SketchLine* findLine(const Sketch& sk, glm::vec2 a, glm::vec2 b) {
    for (const auto& l : sk.getLines()) {
        const auto* s = sk.getPoint(l.startPointId);
        const auto* e = sk.getPoint(l.endPointId);
        if (!s || !e) continue;
        bool fwd = glm::length(s->pos - a) < kTol && glm::length(e->pos - b) < kTol;
        bool rev = glm::length(s->pos - b) < kTol && glm::length(e->pos - a) < kTol;
        if (fwd || rev) return &l;
    }
    return nullptr;
}

// Combine `other` into `target` through the op, the way the app does it.
void combine(Document& doc, int targetId, const Sketch& targetBefore,
             int otherId, const Sketch& other) {
    CombineSketchesOp op;
    op.setTarget(targetId, targetBefore);
    op.addOther(otherId, other, "Other", true);
    ASSERT_TRUE(op.execute(doc));
}
} // namespace

// The target already owns some ids, so every absorbed id must shift. The copied
// line must end on the copied points, not on whatever target points happen to
// carry the line's old endpoint ids.
TEST(CombineSketches, CopiedLineEndsOnCopiedPoints) {
    Document doc;
    auto target = std::make_shared<Sketch>();
    int t0 = target->addPoint({100.0f, 100.0f});
    int t1 = target->addPoint({110.0f, 100.0f});
    target->addLine(t0, t1);
    int targetId = doc.addSketch(target, "Target");

    auto other = std::make_shared<Sketch>();
    int a = other->addPoint({0.0f, 0.0f});
    int b = other->addPoint({10.0f, 5.0f});
    other->addLine(a, b);
    int otherId = doc.addSketch(other, "Other");

    combine(doc, targetId, *target, otherId, *other);

    auto merged = doc.getSketch(targetId);
    ASSERT_TRUE(merged);
    EXPECT_EQ(merged->getLines().size(), 2u);
    EXPECT_NE(findLine(*merged, {0.0f, 0.0f}, {10.0f, 5.0f}), nullptr)
        << "the absorbed line no longer joins its own two points";
    EXPECT_NE(findLine(*merged, {100.0f, 100.0f}, {110.0f, 100.0f}), nullptr);
}

// A constraint on the absorbed geometry must survive the merge and reference
// the copied elements.
TEST(CombineSketches, ConstraintsFollowTheCopiedGeometry) {
    Document doc;
    auto target = std::make_shared<Sketch>();
    int t0 = target->addPoint({100.0f, 100.0f});
    int t1 = target->addPoint({110.0f, 100.0f});
    target->addLine(t0, t1);
    int targetId = doc.addSketch(target, "Target");

    auto other = std::make_shared<Sketch>();
    int a = other->addPoint({0.0f, 0.0f});
    int b = other->addPoint({10.0f, 0.0f});
    int line = other->addLine(a, b);
    Constraint dist;
    dist.type = ConstraintType::Distance;
    dist.entityA = a;
    dist.entityB = b;
    dist.value = 10.0;
    other->addConstraint(dist);
    Constraint horiz;
    horiz.type = ConstraintType::Horizontal;
    horiz.entityA = line;
    other->addConstraint(horiz);
    int otherId = doc.addSketch(other, "Other");

    combine(doc, targetId, *target, otherId, *other);

    auto merged = doc.getSketch(targetId);
    ASSERT_TRUE(merged);
    const SketchLine* copied = findLine(*merged, {0.0f, 0.0f}, {10.0f, 0.0f});
    ASSERT_NE(copied, nullptr);

    int nDist = 0, nHoriz = 0;
    for (const auto& k : merged->getConstraints()) {
        if (k.type == ConstraintType::Distance) {
            ++nDist;
            glm::vec2 pa = pointPos(*merged, k.entityA);
            glm::vec2 pb = pointPos(*merged, k.entityB);
            EXPECT_NEAR(glm::length(pa - pb), 10.0f, kTol)
                << "Distance points at the wrong pair of points";
        } else if (k.type == ConstraintType::Horizontal) {
            ++nHoriz;
            EXPECT_EQ(k.entityA, copied->id) << "Horizontal points at the wrong line";
        }
    }
    EXPECT_EQ(nDist, 1);
    EXPECT_EQ(nHoriz, 1);
}
