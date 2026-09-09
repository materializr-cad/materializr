#pragma once
#include "InteractiveOpController.h"

#include <glm/glm.hpp>
#include <gp_Dir.hxx>

namespace materializr {

// Split Body - one tool, an axis, an offset, and a ghost plane showing exactly
// where the cut lands before you commit it.
//
// This replaces three separate "Split X / Split Y / Split Z" buttons that each
// cut through the body's bounding-box centre and gave no way to see, or move,
// the cut. SplitBodyOp always took an arbitrary gp_Pln - only the UI ever
// assumed the middle.
//
// Deliberately NOT a live preview. Previewing a split means executing it, which
// mints a second body and a second Items entry on every keystroke; the ghost
// plane says everything the user needs and the op runs once, at commit. That is
// what wantsLivePreview() == false buys, and why the commit is forced back
// inline (the base would otherwise defer it as "slow", which this is not).
class SplitController : public InteractiveOpController {
public:
    // User-axis (0=X 1=Y 2=Z, printer convention) to a world direction.
    // Materializr's world is Y-up, so user-Y is world Z and user-Z is world Y.
    static gp_Dir worldNormal(int userAxis);

protected:
    const char* title() const override { return "Split Body"; }
    int onBegin(const IopContext& ctx) override;
    std::unique_ptr<Operation> buildOp(const IopContext& ctx) override;
    void panelBody(const IopContext& ctx, bool& changed) override;
    void drawOverlay(const IopOverlay& ov) const override;
    void onCleanup() override;

    bool wantsLivePreview(const IopContext&) const override { return false; }
    bool wantsDeferredCommit(const IopContext&) const override { return false; }

private:
    // Half-extent of the body along the current axis - how far the plane can
    // travel and still cut anything.
    float axisHalf() const;
    // Plane centre in world space at the current axis + offset.
    glm::vec3 planeCentre() const;
    // The ghost quad's four world corners, in order.
    bool planeCorners(glm::vec3 out[4]) const;

    int   m_axis = 0;         // user axis: 0=X, 1=Y, 2=Z
    float m_offset = 0.0f;    // mm from the body's bbox centre, along the axis

    glm::vec3 m_centre{0.0f}; // body bbox centre (world)
    glm::vec3 m_half{1.0f};   // body bbox half-extents (world)
    bool  m_haveBox = false;
};

} // namespace materializr
