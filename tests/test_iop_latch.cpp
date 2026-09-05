// A controller that stops being active must not keep a viewport handle latched.
//
// Application_Viewport reads anyIopDraggingHandle() into gizmoOwnsDrag, which
// becomes suppressCamDrag: a latch left set kills camera ORBIT and PAN for the
// rest of the session. The ViewCube keeps working because it never goes through
// the drag path, so the symptom reads as "the mouse broke" rather than "an op
// never finished" — which is exactly what made it hard to place when it was
// reported.
//
// cleanup() has always cleared the latch. setActive(false) did not, and Move
// Face is a custom-lifecycle controller that commits and cancels through
// setActive(false) instead — so tilting a face and then opening another project
// (which cancels every active controller) wedged navigation.
#include <gtest/gtest.h>

#include "app/InteractiveOpController.h"
#include "core/Operation.h"

#include <memory>

using namespace materializr;

namespace {

// The smallest thing that satisfies the interface. None of the four hooks is
// reached by this test: the latch is base-class state and the point is that no
// subclass has to remember to clear it.
class StubController : public InteractiveOpController {
public:
    // The lifecycle calls under test are protected; a custom-lifecycle
    // controller reaches them from its own methods, and so does this.
    using InteractiveOpController::setActive;
    using InteractiveOpController::setDraggingHandle;
    using InteractiveOpController::teardown;

protected:
    const char* title() const override { return "Stub"; }
    int onBegin(const IopContext&) override { return -1; }
    std::unique_ptr<Operation> buildOp(const IopContext&) override { return nullptr; }
    void panelBody(const IopContext&, bool&) override {}
};

} // namespace

TEST(IopHandleLatch, DeactivatingDropsIt) {
    StubController c;
    c.setActive(true);
    c.setDraggingHandle(true);
    ASSERT_TRUE(c.draggingHandle());

    c.setActive(false);
    EXPECT_FALSE(c.draggingHandle())
        << "a latch surviving deactivation suppresses camera orbit and pan for "
           "the rest of the session";
    EXPECT_FALSE(c.active());
}

TEST(IopHandleLatch, TeardownDropsItToo) {
    StubController c;
    c.setActive(true);
    c.setDraggingHandle(true);
    c.teardown();
    EXPECT_FALSE(c.draggingHandle());
    EXPECT_FALSE(c.active());
}

TEST(IopHandleLatch, ActivatingDoesNotLatchByItself) {
    // Only an actual handle grab may set it — otherwise merely opening a tool
    // would stand the camera off.
    StubController c;
    c.setActive(true);
    EXPECT_FALSE(c.draggingHandle());
}
