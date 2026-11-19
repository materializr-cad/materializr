#pragma once
// User-arranged order of the tool rail (Modern and im-touch layouts).
//
// The tool catalogue (Toolbar::railTools()) fixes WHICH tools a selection
// context offers and a default order. This class lets the user rearrange them:
// press and hold a rail button, drag it to a new slot, release. The other
// buttons shift to fill the gap live (the rail simply re-renders in the new
// order every frame while the drag is in flight). The order is saved per
// layout + selection context and survives restarts (AppSettings::toolOrder).
//
// Protocol for a rail renderer, once per frame per context:
//   1. build one string key per entry it will draw (tools AND group buttons),
//      in its default order;
//   2. `order = arrange(ctx, keys)` -> indices into `keys`, in display order;
//   3. draw the entries in that order; right after each entry's railButton
//      call `clicked = item(key, clicked)`. A press that turned into a drag
//      returns false so the drop doesn't also fire the tool.
// Keys must be stable across frames (never a loop index) because the ImGui id
// of the button must follow the entry as it moves.
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

namespace materializr {
namespace touchui {

class RailOrder {
public:
    // Seconds a press must be held, without moving, before the button can be
    // dragged. Long enough that a slow ordinary click never arms it.
    static constexpr float kHoldSeconds = 0.7f;

    // Settings round trip. Format: "ctx=key|key|key;ctx2=key|key".
    std::string serialize() const;
    void        parse(const std::string& text);
    void        reset() { m_saved.clear(); m_changed = true; }

    // True once after the saved order changed (a drag was dropped, or reset):
    // the caller persists the settings.
    bool takeChanged() { const bool c = m_changed; m_changed = false; return c; }

    // Display order for `keys` (default order in, indices out) in context
    // `ctx`: saved order first, entries the saved list doesn't know keep their
    // default place relative to their neighbours, and a drag in flight moves
    // the dragged entry to the slot under the pointer.
    std::vector<int> arrange(const std::string& ctx,
                             const std::vector<std::string>& keys);

    // Call just BEFORE the entry's button is submitted: marks where its
    // drawing starts so item() can slide it (see below). Optional - an entry
    // without it simply doesn't animate.
    void beginEntry();

    // Call straight after the entry's button was submitted. Returns whether
    // the button counts as clicked. Also eases the button from where it last
    // sat to its slot: the layout itself is untouched (ImGui lays the entry
    // out at its real slot, so hit-testing and scrolling stay correct); only
    // the vertices the button drew are shifted by a decaying offset.
    bool item(const std::string& key, bool clicked);

    // True while any button is still sliding, so the app keeps rendering
    // frames until the motion settles instead of idling mid-slide.
    // (Stale after a layout switch away from the rail, hence the frame check.)
    bool animating() const { return m_animating && m_animFrame >= ImGui::GetFrameCount() - 2; }

    // True while a button is being dragged (the layout can dim other UI).
    bool dragging() const { return !m_dragKey.empty(); }

private:
    struct Rect { ImVec2 min, max; };

    std::unordered_map<std::string, std::vector<std::string>> m_saved;
    std::string m_ctx;                       // context arrange() last served
    std::vector<std::string> m_visible;      // arranged keys of that context
    std::string m_dragKey;                   // armed/dragged entry
    bool        m_dragMoved = false;         // it actually changed slot
    std::vector<std::string> m_dragOrder;    // working order while dragging
    std::unordered_map<std::string, Rect> m_rects;     // this frame
    std::unordered_map<std::string, Rect> m_prevRects; // last frame
    int         m_rectFrame = -1;

    // Slide animation, per entry key. Positions are relative to the window
    // content so scrolling the rail doesn't read as motion.
    struct Slide { ImVec2 prevRel{0, 0}; ImVec2 off{0, 0}; int lastFrame = -10; };
    std::unordered_map<std::string, Slide> m_slides;
    int         m_vtxStart = -1;
    bool        m_animating = false;
    int         m_animFrame = -1;
    bool        m_animSeen = false;
    bool        itemImpl(const std::string& key, bool clicked);
    void        slide(const std::string& key);
    bool        m_changed = false;

    void commit();
};

} // namespace touchui
} // namespace materializr
