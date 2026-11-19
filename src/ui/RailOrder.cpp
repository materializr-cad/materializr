#include "ui/RailOrder.h"

#include <algorithm>
#include <cmath>
#include <imgui_internal.h> // ImGuiWindow (slide offsets are window-relative)
#include "ui/TouchTheme.h"
#include "touch_mode.h"
#include "ui_scale.h"

namespace materializr {
namespace touchui {

namespace {

// Keys and contexts travel through a one-line settings value, so keep the
// separators out of them.
std::string clean(std::string s) {
    for (auto& c : s) if (c == '|' || c == ';' || c == '=' || c == '\n' || c == '\r') c = '_';
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(sep, a);
        if (b == std::string::npos) b = s.size();
        if (b > a) out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

} // namespace

std::string RailOrder::serialize() const {
    std::string out;
    // Sorted for a stable settings file.
    std::vector<std::string> ctxs;
    for (const auto& kv : m_saved) ctxs.push_back(kv.first);
    std::sort(ctxs.begin(), ctxs.end());
    for (const auto& c : ctxs) {
        const auto& keys = m_saved.at(c);
        if (keys.empty()) continue;
        if (!out.empty()) out += ';';
        out += clean(c) + '=';
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i) out += '|';
            out += clean(keys[i]);
        }
    }
    return out;
}

void RailOrder::parse(const std::string& text) {
    m_saved.clear();
    for (const auto& part : split(text, ';')) {
        const size_t eq = part.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        auto keys = split(part.substr(eq + 1), '|');
        if (!keys.empty()) m_saved[part.substr(0, eq)] = std::move(keys);
    }
}

std::vector<int> RailOrder::arrange(const std::string& ctx,
                                    const std::vector<std::string>& keys) {
    // A new frame: last frame's rects become the hit-test targets.
    const int frame = ImGui::GetFrameCount();
    if (frame != m_rectFrame) {
        m_prevRects.swap(m_rects);
        m_rects.clear();
        m_rectFrame = frame;
    }

    const int n = static_cast<int>(keys.size());
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;

    auto it = m_saved.find(ctx);
    if (it != m_saved.end()) {
        std::unordered_map<std::string, int> rank;
        for (size_t i = 0; i < it->second.size(); ++i) rank.emplace(it->second[i], static_cast<int>(i));
        // Known keys sort by their saved slot; an unknown one rides just after
        // the nearest known entry before it in the default order, so a tool
        // added later (or one that only shows in some states) keeps its
        // natural neighbourhood instead of sinking to the end.
        std::vector<double> r(n);
        double last = -1.0;
        int unknown = 0;
        for (int i = 0; i < n; ++i) {
            auto f = rank.find(keys[i]);
            if (f != rank.end()) { last = f->second; unknown = 0; r[i] = last; }
            else r[i] = last + 0.001 * ++unknown;
        }
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b) { return r[a] < r[b]; });
    }

    // Drag in flight (same context only). The working order lives in
    // m_dragOrder for the whole drag: rebuilding it from the saved order every
    // frame would snap the entry back as soon as the pointer stopped moving.
    if (!m_dragKey.empty() && ctx == m_ctx) {
        if (!m_dragOrder.empty()) {
            // Re-apply the working order to this frame's entries (entries that
            // appeared or vanished mid-drag are tolerated).
            std::vector<int> o2;
            std::vector<bool> used(n, false);
            for (const auto& k : m_dragOrder)
                for (int i : order)
                    if (!used[i] && keys[i] == k) { o2.push_back(i); used[i] = true; break; }
            for (int i : order) if (!used[i]) o2.push_back(i);
            order = o2;
        }
        int from = -1;
        for (int i = 0; i < n; ++i) if (keys[order[i]] == m_dragKey) { from = i; break; }
        if (from >= 0) {
            const ImVec2 mp = ImGui::GetMousePos();
            int to = -1;
            for (int i = 0; i < n; ++i) {
                auto f = m_prevRects.find(keys[order[i]]);
                if (f == m_prevRects.end()) continue;
                if (mp.x >= f->second.min.x && mp.x <= f->second.max.x &&
                    mp.y >= f->second.min.y && mp.y <= f->second.max.y) { to = i; break; }
            }
            if (to >= 0 && to != from) {
                const int moved = order[from];
                order.erase(order.begin() + from);
                order.insert(order.begin() + to, moved);
                m_dragMoved = true;
            }
        }
        m_dragOrder.clear();
        for (int i : order) m_dragOrder.push_back(keys[i]);
    }

    m_ctx = ctx;
    m_visible.clear();
    for (int i : order) m_visible.push_back(keys[i]);
    return order;
}

void RailOrder::commit() {
    if (m_ctx.empty() || m_visible.empty()) return;
    auto& saved = m_saved[m_ctx];
    // Keep the slots of entries that aren't on the rail right now (a tool that
    // only appears in some states): each sits after the same visible neighbour
    // it followed before.
    std::vector<std::string> out = m_visible;
    std::unordered_map<std::string, bool> vis;
    for (const auto& k : m_visible) vis[k] = true;
    std::string anchor;       // last visible key seen in the old list
    size_t pos = 0;           // insertion point following the anchor in `out`
    for (const auto& k : saved) {
        if (vis.count(k)) {
            anchor = k;
            auto f = std::find(out.begin(), out.end(), k);
            pos = static_cast<size_t>(f - out.begin()) + 1;
            continue;
        }
        out.insert(out.begin() + std::min(pos, out.size()), k);
        ++pos;
    }
    if (out != saved) { saved = std::move(out); m_changed = true; }
}

bool RailOrder::item(const std::string& key, bool clicked) {
    const bool r = itemImpl(key, clicked);
    slide(key);
    return r;
}

bool RailOrder::itemImpl(const std::string& key, bool clicked) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    m_rects[key] = {mn, mx};
    const float s = materializr::uiScale();

    // Arm. Two ways in:
    //  - hold still on the pressed button for kHoldSeconds (the touch gesture,
    //    and what the hold ring below counts down), or
    //  - with a mouse, simply press and drag a little: a click on a rail
    //    button never needs to move, so a real drag is unambiguous. (Touch
    //    keeps the long press so a finger that slides doesn't pick tools up.)
    if (m_dragKey.empty() && ImGui::IsItemActive() && io.MouseDown[0]) {
        const ImVec2 d(io.MousePos.x - io.MouseClickedPos[0].x,
                       io.MousePos.y - io.MouseClickedPos[0].y);
        const float dist2 = d.x * d.x + d.y * d.y;
        const float held = io.MouseDownDuration[0];
        const bool still = dist2 <= (12.0f * s) * (12.0f * s);
        if ((still && held >= kHoldSeconds) ||
            (!touchMode() && dist2 > (8.0f * s) * (8.0f * s))) {
            m_dragKey = key;
            m_dragMoved = false;
            m_dragOrder.clear();
        } else if (still && held > 0.2f) {
            // Hold ring: the outline fades in so the press visibly "charges".
            const float t = std::min(1.0f, (held - 0.2f) / (kHoldSeconds - 0.2f));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 acc = ImGui::GetColorU32(accentFill());
            const ImU32 a = (acc & 0x00FFFFFFu) | (static_cast<ImU32>(255.0f * t) << 24);
            dl->AddRect(mn, mx, a, 12.0f * s, 0, 2.0f * s);
        }
    }

    if (m_dragKey == key) {
        // Lifted. A hold that never moved anywhere stays an ordinary click so
        // a slow press can't swallow a tool.
        if (!io.MouseDown[0]) {
            const bool moved = m_dragMoved;
            m_dragKey.clear();
            m_dragOrder.clear();
            if (moved) { commit(); return false; }
            return clicked;
        }
        // Picked-up look: accent outline + faint fill so it reads as held.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 acc = ImGui::GetColorU32(accentFill());
        dl->AddRectFilled(mn, mx, (acc & 0x00FFFFFFu) | 0x38000000u, 12.0f * s);
        dl->AddRect(mn, mx, acc, 12.0f * s, 0, 2.0f * s);
        return false; // never a click while held
    }
    return clicked;
}

void RailOrder::beginEntry() {
    m_vtxStart = ImGui::GetWindowDrawList()->VtxBuffer.Size;
}

void RailOrder::slide(const std::string& key) {
    const int start = m_vtxStart;
    m_vtxStart = -1;

    // `animating()` reflects the frame that just finished: collect across this
    // frame's entries, publish when the next frame's first entry arrives.
    const int frame = ImGui::GetFrameCount();
    if (frame != m_animFrame) {
        m_animating = m_animSeen;
        m_animSeen = false;
        m_animFrame = frame;
    }

    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const ImVec2 mn = ImGui::GetItemRectMin();
    // Window-content-relative slot position.
    const ImVec2 rel(mn.x - win->Pos.x + win->Scroll.x, mn.y - win->Pos.y + win->Scroll.y);

    Slide& st = m_slides[key];
    if (st.lastFrame < frame - 1) {
        st.off = ImVec2(0, 0);            // (re)appearing: no slide-in from nowhere
    } else {
        // The slot moved: start from where the button WAS drawn, then ease.
        st.off.x += st.prevRel.x - rel.x;
        st.off.y += st.prevRel.y - rel.y;
    }
    st.prevRel = rel;
    st.lastFrame = frame;

    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.05f);
    const float k = std::exp(-dt * 18.0f);   // ~0.2 s to settle
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

} // namespace touchui
} // namespace materializr
