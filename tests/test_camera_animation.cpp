// Entering a sketch on a plane used to teleport the camera. Camera::animateTo
// glides there instead: perspective in flight, ending exactly on the requested
// (orthographic) view, with no jump at either hand-off, and any direct camera
// input first settles the move so it never fights the tween.

#include "viewport/Camera.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cmath>

using materializr::Camera;

namespace {

constexpr float kEps = 1e-3f;

float apparentHalfHeight(const Camera& c) {
    // What a perspective camera sees at its distance, in world units.
    return glm::length(c.getPosition() - c.getTarget()) *
           std::tan(glm::radians(c.getFov()) * 0.5f);
}

void runToEnd(Camera& c, float seconds = 0.35f, float dt = 1.0f / 60.0f) {
    for (float t = 0; t < seconds + 0.2f && c.isAnimating(); t += dt) c.tick(dt);
}

} // namespace

TEST(CameraAnimation, EndsExactlyOnTheRequestedOrthographicView) {
    Camera cam;                                    // default perspective view
    const glm::vec3 target(10, 0, 5), pos = target + glm::vec3(0, 40, 0), up(0, 0, 1);
    cam.animateTo(pos, target, up, true, 25.0f, 0.35f);
    EXPECT_TRUE(cam.isAnimating());
    EXPECT_FALSE(cam.isOrthographic()) << "in flight the view is perspective";

    runToEnd(cam);
    EXPECT_FALSE(cam.isAnimating());
    EXPECT_TRUE(cam.isOrthographic());
    EXPECT_NEAR(cam.getOrthoSize(), 25.0f, kEps);
    EXPECT_NEAR(glm::length(cam.getPosition() - pos), 0.0f, kEps);
    EXPECT_NEAR(glm::length(cam.getTarget() - target), 0.0f, kEps);
    EXPECT_NEAR(glm::length(cam.getUp() - up), 0.0f, kEps);
}

TEST(CameraAnimation, ItReallyMovesGraduallyNotInOneStep) {
    Camera cam;
    const glm::vec3 start = cam.getPosition();
    cam.animateTo({0, 50, 0}, {0, 0, 0}, {0, 0, 1}, true, 20.0f, 0.4f);
    cam.tick(0.1f);
    const glm::vec3 mid = cam.getPosition();
    EXPECT_TRUE(cam.isAnimating());
    EXPECT_GT(glm::length(mid - start), kEps) << "it started moving";
    EXPECT_GT(glm::length(mid - glm::vec3(0, 50, 0)), 1.0f) << "and has not arrived";
}

TEST(CameraAnimation, HandOffToOrthographicDoesNotChangeTheFraming) {
    // At the end of the glide the perspective view must frame the subject like
    // the orthographic box that replaces it, or the switch pops.
    Camera cam;
    cam.animateTo({0, 80, 0}, {0, 0, 0}, {0, 0, 1}, true, 30.0f, 0.3f);
    for (int i = 0; i < 100 && cam.isAnimating(); ++i) {
        cam.tick(0.01f);
        if (cam.isAnimating() && i > 25)   // just before the final step
            EXPECT_NEAR(apparentHalfHeight(cam), 30.0f, 3.0f);
    }
}

TEST(CameraAnimation, StartingFromOrthographicDoesNotPopEither) {
    Camera cam;
    cam.setOrthographic(true);
    cam.setOrthoSize(12.0f);
    cam.setPosition({0, 40, 0});
    cam.setTarget({0, 0, 0});
    cam.animateTo({30, 0, 0}, {0, 0, 0}, {0, 1, 0}, true, 40.0f, 0.3f);
    EXPECT_FALSE(cam.isOrthographic());
    EXPECT_NEAR(apparentHalfHeight(cam), 12.0f, 0.1f)
        << "the perspective stand-in frames the subject like the ortho box did";
}

TEST(CameraAnimation, DirectInputSettlesTheMoveFirst) {
    Camera cam;
    const glm::vec3 target(0, 0, 0), pos(0, 60, 0);
    cam.animateTo(pos, target, {0, 0, 1}, true, 20.0f, 0.5f);
    cam.tick(0.05f);
    ASSERT_TRUE(cam.isAnimating());
    cam.pan(1.0f, 0.0f);                           // the user grabs the camera
    EXPECT_FALSE(cam.isAnimating());
    EXPECT_TRUE(cam.isOrthographic()) << "the requested end state was applied";
    EXPECT_NEAR(cam.getOrthoSize(), 20.0f, kEps);
}

TEST(CameraAnimation, ZeroDurationIsImmediate) {
    Camera cam;
    cam.animateTo({0, 50, 0}, {1, 0, 0}, {0, 0, 1}, true, 15.0f, 0.0f);
    EXPECT_FALSE(cam.isAnimating());
    EXPECT_TRUE(cam.isOrthographic());
    EXPECT_NEAR(glm::length(cam.getTarget() - glm::vec3(1, 0, 0)), 0.0f, kEps);
}

TEST(CameraAnimation, OppositeViewDirectionsStillSwingSmoothly) {
    // Looking down then looking up: no unique shortest arc, must not blow up.
    Camera cam;
    cam.setPosition({0, 50, 0});
    cam.setTarget({0, 0, 0});
    cam.setUp({0, 0, 1});
    cam.animateTo({0, -50, 0}, {0, 0, 0}, {0, 0, -1}, false, 10.0f, 0.4f);
    for (int i = 0; i < 60 && cam.isAnimating(); ++i) {
        cam.tick(0.01f);
        const glm::vec3 p = cam.getPosition();
        EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
        EXPECT_NEAR(glm::length(glm::vec3(0, 0, 0) - p), 50.0f, 0.5f);
    }
}

TEST(CameraAnimation, TickIsANoOpWhenIdle) {
    Camera cam;
    const glm::vec3 p = cam.getPosition();
    EXPECT_FALSE(cam.tick(0.1f));
    EXPECT_EQ(cam.getPosition(), p);
}

namespace {

// A sketch-style orthographic view looking along `dir` with screen-up `up`.
Camera orthoView(const glm::vec3& dir, const glm::vec3& up) {
    Camera cam;
    cam.setOrthographic(true);
    cam.setOrthoSize(12.0f);
    cam.setPosition(dir * 40.0f);
    cam.setTarget({0, 0, 0});
    cam.setUp(up);
    return cam;
}

} // namespace

TEST(CameraOrbitExit, SidewaysSketchViewIsNotRolledUpright) {
    // UI X vertical on screen (world X up): orbiting out must pivot about that
    // screen-vertical axis, not roll the picture to world Y on its own.
    Camera cam = orthoView({0, 0, 1}, {1, 0, 0});
    for (int i = 0; i < 30; ++i) {
        cam.orbit(i < 3 ? 4.0f : 0.0f, 0.0f);
        cam.tick(1.0f / 60.0f);
        EXPECT_NEAR(glm::length(cam.getUp() - glm::vec3(1, 0, 0)), 0.0f, kEps) << "frame " << i;
    }
    EXPECT_FALSE(cam.isOrthographic());
    const glm::vec3 dir = glm::normalize(cam.getPosition() - cam.getTarget());
    EXPECT_GT(std::abs(dir.y), 0.01f) << "the drag really orbited (about world X)";
    EXPECT_NEAR(dir.x, 0.0f, kEps) << "turning about the screen-vertical axis keeps X";
}

TEST(CameraOrbitExit, UprightAndUpsideDownViewsKeepTheirHorizon) {
    for (float sg : {1.0f, -1.0f}) {
        Camera cam = orthoView({0, 0, 1}, {0, sg, 0});
        cam.orbit(10.0f, 3.0f);
        EXPECT_NEAR(glm::length(cam.getUp() - glm::vec3(0, sg, 0)), 0.0f, kEps);
    }
}

TEST(CameraOrbitExit, TopViewTipsOffTheHorizonIntoWorldUp) {
    Camera cam = orthoView({0, 1, 0}, {0, 0, -1});
    cam.orbit(0.0f, 5.0f);
    EXPECT_NEAR(glm::length(cam.getUp() - glm::vec3(0, 1, 0)), 0.0f, kEps);
}

TEST(CameraOrbitExit, SlightlyOffPoleTopViewDoesNotSpinOnTheFirstFrame) {
    // The pivot re-anchor can leave an ortho Top/Bottom view a few degrees off
    // exact. The first orbit then used the tilt's azimuth for yaw and snapped the
    // picture by up to 180 degrees.
    auto screenUp = [](const Camera& c) {
        const glm::vec3 d = glm::normalize(c.getTarget() - c.getPosition());
        return glm::normalize(c.getUp() - d * glm::dot(c.getUp(), d));
    };
    for (float sy : {1.0f, -1.0f})
    for (const glm::vec3 up : {glm::vec3(1, 0, 0), glm::vec3(0, 0, 1), glm::vec3(-1, 0, 0)})
    for (const glm::vec3 off : {glm::vec3(1.5f, 0, 0), glm::vec3(0, 0, 3.0f), glm::vec3(-2.0f, 0, 2.0f)}) {
        Camera cam = orthoView({0, sy, 0}, up);
        cam.setTarget(off);                       // pivot moved sideways by ~2-4 degrees
        glm::vec3 prev = screenUp(cam);
        for (int i = 0; i < 20; ++i) {
            cam.orbit(i < 5 ? 2.0f : 0.0f, 0.0f);
            cam.tick(1.0f / 60.0f);
            const glm::vec3 u = screenUp(cam);
            const float step = std::acos(glm::clamp(glm::dot(u, prev), -1.0f, 1.0f));
            EXPECT_LT(step, glm::radians(15.0f)) << "sy " << sy << " frame " << i;
            prev = u;
        }
    }
}
