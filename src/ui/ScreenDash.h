#pragma once
// Dashed screen-space lines that cost the same however far the zoom goes.
//
// The sketch overlay draws guides as one ImDrawList line per dash. Zoomed far
// in, a guide's endpoints project to millions of pixels apart, so the loop
// emitted millions of dashes (millions of vertices) per frame. Desktop crawls;
// on OpenGL ES 3 the ImGui backend has 16-bit indices and no large-mesh
// support, so past 64k vertices the indices wrap and the screen fills with
// garbled glyphs from the font atlas (issue #172, shot 5).
//
// Only the part of the line inside the clip rectangle is dashed. The pattern
// keeps its phase from p0, so the dashes do not crawl while the line is panned.

#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace materializr {

// Calls seg(a, b) for each dash of the line p0 -> p1 that touches the clip
// rectangle (grown by `margin` px). `on`/`off` are dash and gap lengths in px.
template <class F>
void forEachVisibleDash(ImVec2 p0, ImVec2 p1, float on, float off,
                        ImVec2 clipMin, ImVec2 clipMax, float margin, F&& seg) {
    const float dx = p1.x - p0.x, dy = p1.y - p0.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    const float period = on + off;
    if (!(len > 1.0f) || !(period > 0.0f)) return;
    const float ux = dx / len, uy = dy / len;

    // Parameter range [t0, t1] (in px along the line) inside the grown clip
    // rect - slab method on each axis.
    float t0 = 0.0f, t1 = len;
    const float lo[2] = { clipMin.x - margin, clipMin.y - margin };
    const float hi[2] = { clipMax.x + margin, clipMax.y + margin };
    const float o[2]  = { p0.x, p0.y };
    const float u[2]  = { ux, uy };
    for (int a = 0; a < 2; ++a) {
        if (std::fabs(u[a]) < 1e-9f) {
            if (o[a] < lo[a] || o[a] > hi[a]) return;      // parallel and outside
            continue;
        }
        float ta = (lo[a] - o[a]) / u[a];
        float tb = (hi[a] - o[a]) / u[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 >= t1) return;                              // never enters the view
    }
    // Start on a dash boundary so the phase matches an unclipped line.
    float t = std::floor(t0 / period) * period;
    for (; t < t1; t += period) {
        const float s = std::max(t, 0.0f);
        const float e = std::min(t + on, len);
        if (e <= s) continue;
        seg(ImVec2(p0.x + ux * s, p0.y + uy * s), ImVec2(p0.x + ux * e, p0.y + uy * e));
    }
}

} // namespace materializr
