#pragma once
// Eases items into a new order instead of letting them jump.
//
// When a list is reordered (a dragged tab pill passing its neighbour), every entry
// is laid out at its new slot straight away, so hit-testing and scrolling stay
// right. Only what the entry DRAWS is shifted: it starts where it was drawn last
// frame and decays to its slot in ~0.2 s. The same motion as the tool rail
// (touchui::RailOrder), for anything that reorders.
//
// Use, per entry and per frame:
//   anim.begin();            // just before the entry's widget
//   ImGui::Button(...);      // the entry
//   anim.end(key);           // straight after it; key = stable identity, not an index
// and keep the app rendering while `animating()`.
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace materializr {

class SlideAnim {
public:
    void begin() { m_vtxStart = ImGui::GetWindowDrawList()->VtxBuffer.Size; }

    void end(const void* key) {
        const int start = m_vtxStart;
        m_vtxStart = -1;
        // `animating()` reports the frame that just finished: collect over this
        // frame's entries, publish when the next frame's first entry arrives.
        const int frame = ImGui::GetFrameCount();
        if (frame != m_animFrame) {
            m_animating = m_animSeen;
            m_animSeen = false;
            m_animFrame = frame;
        }
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 wp = ImGui::GetWindowPos();
        // Relative to the window's content, so scrolling doesn't read as motion.
        const ImVec2 rel(mn.x - wp.x + ImGui::GetScrollX(), mn.y - wp.y + ImGui::GetScrollY());
        Slide& st = m_slides[key];
        if (st.lastFrame < frame - 1) {
            st.off = ImVec2(0, 0);             // (re)appearing: no slide in from nowhere
        } else {
            st.off.x += st.prevRel.x - rel.x;  // the slot moved: start from where it was
            st.off.y += st.prevRel.y - rel.y;
        }
        st.prevRel = rel;
        st.lastFrame = frame;
        const float dt = std::min(ImGui::GetIO().DeltaTime, 0.05f);
        const float k = std::exp(-dt * 18.0f);
        st.off.x *= k;
        st.off.y *= k;
        if (std::fabs(st.off.x) < 0.4f && std::fabs(st.off.y) < 0.4f) st.off = ImVec2(0, 0);
        else m_animSeen = true;
        if (start < 0 || (st.off.x == 0.0f && st.off.y == 0.0f)) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = start; i < dl->VtxBuffer.Size; ++i) {
            dl->VtxBuffer.Data[i].pos.x += st.off.x;
            dl->VtxBuffer.Data[i].pos.y += st.off.y;
        }
    }

    // Still sliding: keep rendering until it settles. (Stale once the list stops
    // being drawn, hence the frame check.)
    bool animating() const { return m_animating && m_animFrame >= ImGui::GetFrameCount() - 2; }

private:
    struct Slide { ImVec2 prevRel{0, 0}; ImVec2 off{0, 0}; int lastFrame = -10; };
    std::unordered_map<const void*, Slide> m_slides;
    int  m_vtxStart = -1;
    bool m_animating = false;
    bool m_animSeen = false;
    int  m_animFrame = -1;
};

} // namespace materializr
