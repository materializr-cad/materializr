#pragma once
#include <glm/glm.hpp>
#include <cmath>

namespace materializr {

// The two in-plane directions a gizmo offers for a plane with normal `n`.
//
// The obvious construction - pick any reference, project it, then B = N × A -
// is HANDED: flipping the normal flips B. That matters because a surface
// normal's sign is often incidental (OCCT's buildVoid names a hole's two
// mouths by its own walk order, so the "entry" normal of an identical hole can
// point either way). The result was a green arrow pointing +Y on one hole and
// -Y on the next, which reads as reversed controls.
//
// So: drop the world axis the plane is most normal to, and keep the other two
// in X→Y→Z order, projected into the plane and left POSITIVELY oriented. The
// arrows then always point along +X/+Y/+Z - stable under a normal flip, and
// matching the X=red / Y=green / Z=blue colouring the gizmo applies. The pair
// is orthonormal but not necessarily right-handed about n; only use it where
// handedness doesn't matter (translation, not a rotation sweep).
inline void inPlaneAxes(const glm::vec3& n, glm::vec3& axisA, glm::vec3& axisB) {
    const glm::vec3 world[3] = {{1.0f, 0.0f, 0.0f},
                                {0.0f, 1.0f, 0.0f},
                                {0.0f, 0.0f, 1.0f}};
    int drop = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(glm::dot(n, world[i])) > std::abs(glm::dot(n, world[drop])))
            drop = i;

    glm::vec3 inPlane[2];
    int k = 0;
    for (int i = 0; i < 3; ++i)
        if (i != drop) inPlane[k++] = world[i];

    auto project = [&](glm::vec3 w) {
        glm::vec3 v = w - glm::dot(w, n) * n;
        return (glm::length(v) > 1e-5f) ? glm::normalize(v) : w;
    };
    axisA = project(inPlane[0]);

    // Projecting the second world axis independently leaves the pair SKEWED on
    // an oblique plane (two world axes aren't perpendicular once flattened into
    // it). Grid snapping decomposes the drag onto these and rebuilds it as
    // a*A + b*B, which is only the same vector if they're orthonormal - so
    // orthogonalise against A. A no-op on axis-aligned planes, where the two
    // are already square, which is what keeps the arrows on +X/+Y/+Z there.
    glm::vec3 b = project(inPlane[1]);
    b -= glm::dot(b, axisA) * axisA;
    if (glm::length(b) > 1e-5f) {
        b = glm::normalize(b);
        // Keep the positive sense of the world axis it came from.
        if (glm::dot(b, inPlane[1]) < 0.0f) b = -b;
        axisB = b;
    } else {
        axisB = glm::normalize(glm::cross(n, axisA));  // degenerate: any in-plane
    }
}

} // namespace materializr
