#include "ViewCube.h"
#include "Camera.h"
#include "../touch_mode.h"
#include "../ui_scale.h"   // uiScale - the widget tracks the interface size

#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {

ViewCube::ViewCube() {}

// 3DSMax-style navigation overlay: a projected 3D cube (face clicks snap to
// orthographic views) wrapped in a horizon ring (drag to rotate the camera
// around its target). Drawn with ImDrawList so it shares ImGui's window state.

ViewCubeAction ViewCube::render(Camera& camera, bool invertDrag, bool lightMode,
                                bool releaseIsGesture)
{
    ViewCubeAction action = ViewCubeAction::None;

    // "Ink" = the labels / borders / arrows / home fill: light on the dark theme,
    // dark on the light theme so they don't read as white over a light viewport.
    // "Paper" is its opposite, used for the home glyph that sits on the ink fill.
    // The cube face fills (blue/grey) and the yellow hover stay the same in both.
    auto ink   = [lightMode](int a){ return lightMode ? IM_COL32(40, 42, 52, a)
                                                      : IM_COL32(225, 226, 236, a); };
    auto paper = [lightMode](int a){ return lightMode ? IM_COL32(236, 237, 244, a)
                                                      : IM_COL32(60, 60, 70, a); };

    // --- Layout: top-right of the current window, leaving room for the title bar.
    //     cubeR is the cube's half-extent; widgetR is the radius used for the
    //     surrounding accessory positions (arrows, home button, triad) so the
    //     overall widget keeps a roomy layout even when the cube is small.
    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 ws = ImGui::GetWindowSize();
    const float pad     = 10.0f * (materializr::touchMode() ? 1.5f
                                                             : materializr::uiScale());
    // Touch: enlarge the whole widget by a modest factor so the small tap
    // targets (rotation arrows, roll arcs, corner dots, Home) become comfortably
    // finger-sized. Everything below is sized off `ts`, so the cube, its
    // accessories and their hit-tests grow together. 1.0 on desktop = unchanged.
    // The widget is a UI element, so it tracks the INTERFACE size - otherwise a
    // HiDPI desktop doubles every font and panel around it while the cube, its
    // arrows, the Home button and the axis triad stay at their 1x pixel sizes
    // and read as shrunken (Steve, on a 2x Framework panel).
    //
    // Touch deliberately keeps its own 1.5 rather than following uiScale: the
    // factor there exists to make small tap targets finger-sized, the Android
    // uiScale runs 1.4-2.5, and the tablet's widget is if anything already too
    // big. Changing that needs the tablet in hand to measure, not a guess.
    const float ts      = materializr::touchMode() ? 1.5f : materializr::uiScale();
    const float cubeR   = 19.0f * ts;   // half-extent of cube projection (px)
    const float widgetR = 38.0f * ts;   // accessory placement radius
    float topOffset = 42.0f * ts;  // push below the window title bar
    if (materializr::touchMode()) {
        // The docked "Viewport" tab bar is ~2x tall in touch mode, so the Home
        // button (60 px above the cube centre) hid behind it. Drop the whole
        // widget so the top accessories clear it.
        topOffset = 74.0f * ts;
    }
    topOffset += m_extraTop; // e.g. below the im-touch-lite floating buttons
    // Extra left inset in touch mode: the enlarged widget (and its Home button)
    // overhang further right, so nudge the whole thing ~10 px off the edge.
    const float rightInset = pad + widgetR + 26.0f * ts +
                             (materializr::touchMode() ? 10.0f * ts : 0.0f) + m_extraLeft;
    ImVec2 center(wp.x + ws.x - rightInset,
                  wp.y + pad + widgetR + topOffset);

    // Cache the widget's screen anchor for the snap widget to tuck beneath. The
    // lowest accessory is the axis triad at center.y + widgetR + 22*ts; pad past
    // its glyph so the snap square clears the whole widget at any scale/offset.
    m_widgetCenterX = center.x;
    m_widgetBottomY = center.y + widgetR + 42.0f * ts;

    // --- Camera view-rotation matrix (no translation), so the cube spins with
    //     the camera's orientation.
    glm::mat4 V = camera.getViewMatrix();
    V[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);

    // Project local cube corners (±1 on each axis) to screen space using only
    // the rotation: x → screen.x, y → -screen.y (flip), z → depth.
    auto projectXY = [&](const glm::vec3& v) -> ImVec2 {
        glm::vec4 e = V * glm::vec4(v, 1.0f);
        return ImVec2(center.x + e.x * cubeR, center.y - e.y * cubeR);
    };
    auto eyeZ = [&](const glm::vec3& v) -> float {
        return (V * glm::vec4(v, 1.0f)).z;
    };

    static const glm::vec3 kCorners[8] = {
        {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
        {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
    };
    ImVec2 sc[8];
    for (int i = 0; i < 8; ++i) sc[i] = projectXY(kCorners[i]);

    // Face definitions. Corner order is CCW from OUTSIDE the cube, so a face's
    // 2D screen winding flips sign when the face turns away from the camera.
    struct Face { int c[4]; glm::vec3 n; const char* label; ViewCubeAction act; };
    // Side faces use single-letter labels (L/F/R/B) since the cube is now
    // half its old size and full words don't fit cleanly. Top/Bottom keep
    // their full label - they're the most recognisable and have more room
    // on the square top/bottom faces of the projection.
    static const Face kFaces[6] = {
        {{4,5,6,7}, { 0, 0, 1}, "F",      ViewCubeAction::Front},
        {{1,0,3,2}, { 0, 0,-1}, "B",      ViewCubeAction::Back},
        {{0,4,7,3}, {-1, 0, 0}, "L",      ViewCubeAction::Left},
        {{5,1,2,6}, { 1, 0, 0}, "R",      ViewCubeAction::Right},
        {{3,7,6,2}, { 0, 1, 0}, "Top",    ViewCubeAction::Top},
        {{0,1,5,4}, { 0,-1, 0}, "Bottom", ViewCubeAction::Bottom},
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mp = ImGui::GetMousePos();

    // Only act on clicks the viewport actually owns. The cube reads global mouse
    // state, so without this a click on a panel that OVERLAPS the cube's corner
    // (e.g. the Move Face Cancel button, which sits over the cube) registers as a
    // cube-face press too - snapping the camera to that face.
    //
    // NOTE: do NOT use io.WantCaptureMouse here. The Viewport is a normal docked
    // window (not a passthrough central node), so WantCaptureMouse is true across
    // the ENTIRE viewport - including the bare canvas and the cube itself - which
    // killed all cube clicks. The cube draws inside the Viewport window's scope,
    // so IsWindowHovered() is the right signal: true when the viewport (and not an
    // overlay sitting on top of it) is the hovered window at the cursor. An
    // overlapping panel/button is a separate window, so it flips this false.
    const bool uiBlocked = !ImGui::IsWindowHovered();
    auto cubeClicked = [&]() {
        return !uiBlocked && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    };

    // --- Cube faces. Compute visibility per face from eye-space normal.z.
    auto pointInQuad = [&](const ImVec2* q, ImVec2 p) -> bool {
        float sign = 0.0f;
        for (int i = 0; i < 4; ++i) {
            ImVec2 a = q[i], b = q[(i+1) % 4];
            float c = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
            if (i == 0) sign = c;
            else if ((c > 0) != (sign > 0) && std::abs(c) > 1e-3f && std::abs(sign) > 1e-3f)
                return false;
        }
        return true;
    };

    bool cubeHover = false;
    // Two passes so back faces (depth-sorted) draw before front faces. Cheap:
    // sort by face-centroid eye Z descending (further first).
    struct VisFace { int idx; float depth; };
    std::vector<VisFace> drawList;
    drawList.reserve(6);
    bool faceFront[6] = { false, false, false, false, false, false };
    for (int i = 0; i < 6; ++i) {
        glm::vec4 ne = V * glm::vec4(kFaces[i].n, 0.0f);
        if (ne.z > 0.0f) {
            faceFront[i] = true;
            glm::vec3 ctr3 = kFaces[i].n; // face center is the normal scaled to 1
            drawList.push_back({i, eyeZ(ctr3)});
        }
    }
    std::sort(drawList.begin(), drawList.end(),
              [](const VisFace& a, const VisFace& b){ return a.depth < b.depth; });

    // A cube vertex / edge is part of the visible silhouette - and so clickable -
    // whenever it borders a front-facing face. The earlier code culled on the
    // corner's OWN eye-space Z (eyeZ(corner) < 0), which crosses zero at ~45° for
    // the side corners of a face that is itself still visible (front-facing up to
    // 90°). That made edges and corner dots vanish as soon as a face tilted past
    // 45°. Keying off face-adjacency instead matches the real silhouette.
    auto cornerVisible = [&](int ci) {
        for (int f = 0; f < 6; ++f) {
            if (!faceFront[f]) continue;
            for (int k = 0; k < 4; ++k) if (kFaces[f].c[k] == ci) return true;
        }
        return false;
    };
    auto edgeVisible = [&](int a, int b) {
        for (int f = 0; f < 6; ++f) {
            if (!faceFront[f]) continue;
            bool ha = false, hb = false;
            for (int k = 0; k < 4; ++k) {
                if (kFaces[f].c[k] == a) ha = true;
                if (kFaces[f].c[k] == b) hb = true;
            }
            if (ha && hb) return true;
        }
        return false;
    };

    for (auto& vf : drawList) {
        const Face& f = kFaces[vf.idx];
        ImVec2 q[4] = { sc[f.c[0]], sc[f.c[1]], sc[f.c[2]], sc[f.c[3]] };
        bool hover = pointInQuad(q, mp);
        if (hover) cubeHover = true;
        ImU32 fill = hover ? IM_COL32(80, 140, 220, 230) : IM_COL32(70, 75, 90, 215);
        dl->AddConvexPolyFilled(q, 4, fill);
        dl->AddPolyline(q, 4, ink(255), ImDrawFlags_Closed, 1.4f);

        // Label centred on the face.
        ImVec2 ctr((q[0].x + q[1].x + q[2].x + q[3].x) * 0.25f,
                   (q[0].y + q[1].y + q[2].y + q[3].y) * 0.25f);
        ImVec2 ts = ImGui::CalcTextSize(f.label);
        dl->AddText(ImVec2(ctr.x - ts.x * 0.5f, ctr.y - ts.y * 0.5f),
                    ink(255), f.label);

        // Press on a face arms a pending snap; the actual snap fires on RELEASE
        // (and only if the user didn't drag in between, so dragging the cube
        // produces a free orbit instead).
        if (hover && cubeClicked()) {
            m_pendingClick = f.act;
            m_cubeDragging = false;
        }
    }

    // --- Edge click-spots: clicking the seam between two visible faces snaps to
    //     a two-face view (looking down that edge). Hover-revealed with no
    //     persistent marker so the small cube stays uncluttered - hovering near a
    //     cube edge highlights the whole segment. Tested after faces (an edge
    //     wins over the face it lies on) but before corners (a corner still wins
    //     at the very ends, since the hit zone is restricted to the mid-segment).
    struct Edge { int a, b; ViewCubeAction act; };
    static const Edge kEdges[12] = {
        {6,7, ViewCubeAction::TopFront},    {2,3, ViewCubeAction::TopBack},
        {3,7, ViewCubeAction::TopLeft},     {2,6, ViewCubeAction::TopRight},
        {4,5, ViewCubeAction::BottomFront}, {0,1, ViewCubeAction::BottomBack},
        {0,4, ViewCubeAction::BottomLeft},  {1,5, ViewCubeAction::BottomRight},
        {4,7, ViewCubeAction::FrontLeft},   {5,6, ViewCubeAction::FrontRight},
        {0,3, ViewCubeAction::BackLeft},    {1,2, ViewCubeAction::BackRight},
    };
    for (const auto& e : kEdges) {
        // Visible while either face sharing this edge faces the camera.
        if (!edgeVisible(e.a, e.b)) continue;
        ImVec2 A = sc[e.a], B = sc[e.b];
        ImVec2 ab(B.x - A.x, B.y - A.y);
        float lenSq = ab.x * ab.x + ab.y * ab.y;
        float t = ((mp.x - A.x) * ab.x + (mp.y - A.y) * ab.y) / std::max(lenSq, 1e-6f);
        bool hover = false;
        if (t > 0.2f && t < 0.8f) { // mid-segment only; ends belong to corners
            ImVec2 p(A.x + ab.x * t, A.y + ab.y * t);
            float d = std::sqrt((mp.x - p.x) * (mp.x - p.x) +
                                (mp.y - p.y) * (mp.y - p.y));
            hover = d < 6.0f * ts;
        }
        if (hover) {
            // Highlight the whole seam so it's clear which two faces will show.
            dl->AddLine(A, B, IM_COL32(255, 220, 80, 255), 3.0f * ts);
            cubeHover = true;
            if (cubeClicked()) {
                m_pendingClick = e.act;
                m_cubeDragging = false;
            }
        }
    }

    // --- Corner click-spots: a small dot at each visible vertex snaps to the
    //     matching isometric view. We test corners after faces so a face hover
    //     wins when they overlap (corner spots sit inside the face polygons).
    static const ViewCubeAction kCornerActions[8] = {
        ViewCubeAction::BackBottomLeft,    // 0: -X -Y -Z
        ViewCubeAction::BackBottomRight,   // 1: +X -Y -Z
        ViewCubeAction::BackTopRight,      // 2: +X +Y -Z
        ViewCubeAction::BackTopLeft,       // 3: -X +Y -Z
        ViewCubeAction::FrontBottomLeft,   // 4: -X -Y +Z
        ViewCubeAction::FrontBottomRight,  // 5: +X -Y +Z
        ViewCubeAction::FrontTopRight,     // 6: +X +Y +Z
        ViewCubeAction::FrontTopLeft       // 7: -X +Y +Z
    };
    for (int i = 0; i < 8; ++i) {
        if (!cornerVisible(i)) continue; // vertex with no front-facing face
        ImVec2 cp = sc[i];
        float dist = std::sqrt((mp.x - cp.x) * (mp.x - cp.x) +
                               (mp.y - cp.y) * (mp.y - cp.y));
        bool hover = dist < 7.0f * ts;
        ImU32 col = hover ? IM_COL32(255, 220, 80, 240) : ink(200);
        dl->AddCircleFilled(cp, (hover ? 6.0f : 4.0f) * ts, col);
        if (hover && cubeClicked()) {
            m_pendingClick = kCornerActions[i];
            m_cubeDragging = false;
            cubeHover = true; // counts as cube interaction for hover-suppression
        }
        if (hover) cubeHover = true;
    }

    // --- Four 90° rotation arrows positioned just outside the cube on each
    //     cardinal side. Clicking emits RotateLeft/Right/Up/Down.
    {
        const float r = widgetR + 18.0f * ts;   // arrow centre distance
        const float s = 6.0f * ts;              // arrow half-size
        struct Arrow { ImVec2 dir; ViewCubeAction act; };
        Arrow arrows[4] = {
            {{-1, 0}, ViewCubeAction::RotateLeft},
            {{ 1, 0}, ViewCubeAction::RotateRight},
            {{ 0,-1}, ViewCubeAction::RotateUp},
            {{ 0, 1}, ViewCubeAction::RotateDown}
        };
        for (auto& ar : arrows) {
            ImVec2 ac(center.x + ar.dir.x * r, center.y + ar.dir.y * r);
            // Triangle pointing in ar.dir.
            ImVec2 tip(ac.x + ar.dir.x * s, ac.y + ar.dir.y * s);
            // Two base corners perpendicular to dir.
            ImVec2 perp(-ar.dir.y, ar.dir.x);
            ImVec2 b1(ac.x - ar.dir.x * s + perp.x * s, ac.y - ar.dir.y * s + perp.y * s);
            ImVec2 b2(ac.x - ar.dir.x * s - perp.x * s, ac.y - ar.dir.y * s - perp.y * s);
            float dx = mp.x - ac.x, dy = mp.y - ac.y;
            bool hover = std::sqrt(dx * dx + dy * dy) < s + 3.0f * ts;
            ImU32 col = hover ? IM_COL32(255, 220, 80, 255) : ink(220);
            dl->AddTriangleFilled(tip, b1, b2, col);
            if (hover) {
                cubeHover = true;
                if (cubeClicked()) {
                    m_pendingClick = ar.act;
                    m_cubeDragging = false;
                }
            }
        }
    }

    // --- Two FreeCAD-style large sweeping roll arrows at the top corners of
    //     the widget. Each is a quarter-circle arc with a clear arrowhead at
    //     the inward end. Clicking rolls the camera 90° around the view
    //     direction so a snapped ortho view re-orients without un-snapping.
    {
        const float ringR = widgetR + 14.0f * ts; // arc radius (large sweep)
        struct Roll { float a0; float a1; ViewCubeAction act; };
        // a0 → a1 sweep direction; angles in radians, 0 = +X (right), going CCW
        // in math convention. We use Y-flipped screen so visually CCW math =
        // CCW screen at the top of the cube.
        Roll rolls[2] = {
            // Top-left arrow: arc on upper-left going from "left" (π) to "top"
            // (π/2). Arrowhead at the top, indicating CCW roll.
            { float(M_PI),         float(M_PI * 0.55f), ViewCubeAction::RollLeft },
            // Top-right arrow: arc on upper-right going from "top" to "right".
            { float(M_PI * 0.45f), 0.0f,                ViewCubeAction::RollRight },
        };
        for (auto& rb : rolls) {
            // Generate ~12 segments along the arc to test hover & draw.
            const int seg = 14;
            ImVec2 pts[seg + 1];
            for (int i = 0; i <= seg; ++i) {
                float t = rb.a0 + (rb.a1 - rb.a0) * (i / float(seg));
                // Screen Y inverted: subtract sin instead of add so the arc
                // sits ABOVE the cube (top of screen has lower y in ImGui).
                pts[i] = ImVec2(center.x + std::cos(t) * ringR,
                                center.y - std::sin(t) * ringR);
            }
            // Hover test: distance from cursor to the arc's polyline.
            float bestD = 1e9f;
            for (int i = 0; i < seg; ++i) {
                ImVec2 a = pts[i], b = pts[i + 1];
                ImVec2 ab(b.x - a.x, b.y - a.y);
                float lenSq = ab.x * ab.x + ab.y * ab.y;
                float t = ((mp.x - a.x) * ab.x + (mp.y - a.y) * ab.y) / std::max(lenSq, 1e-6f);
                t = std::clamp(t, 0.0f, 1.0f);
                ImVec2 p(a.x + ab.x * t, a.y + ab.y * t);
                float d = std::sqrt((mp.x - p.x) * (mp.x - p.x) +
                                    (mp.y - p.y) * (mp.y - p.y));
                if (d < bestD) bestD = d;
            }
            bool hover = bestD < 8.0f * ts;
            ImU32 col = hover ? IM_COL32(255, 220, 80, 255) : ink(235);
            // Polyline body.
            for (int i = 0; i < seg; ++i) {
                dl->AddLine(pts[i], pts[i + 1], col, 3.0f * ts);
            }
            // Arrowhead at the end of the arc, oriented along the tangent.
            ImVec2 tip = pts[seg];
            ImVec2 prev = pts[seg - 1];
            ImVec2 tan(tip.x - prev.x, tip.y - prev.y);
            float tl = std::sqrt(tan.x * tan.x + tan.y * tan.y);
            if (tl > 1e-4f) { tan.x /= tl; tan.y /= tl; }
            ImVec2 base(tip.x - tan.x * 8.0f * ts, tip.y - tan.y * 8.0f * ts);
            ImVec2 perp(-tan.y * 5.0f * ts, tan.x * 5.0f * ts);
            dl->AddTriangleFilled(tip,
                                   ImVec2(base.x + perp.x, base.y + perp.y),
                                   ImVec2(base.x - perp.x, base.y - perp.y),
                                   col);
            if (hover) {
                cubeHover = true;
                if (cubeClicked()) {
                    m_pendingClick = rb.act;
                    m_cubeDragging = false;
                }
            }
        }
    }

    // --- Small "Home" reset-view button: a circle at the top-right of the
    //     widget. Clicking snaps to the default FrontTopRight (3/4 iso) view.
    {
        ImVec2 hc(center.x + widgetR + 22.0f * ts, center.y - widgetR - 22.0f * ts);
        float dx = mp.x - hc.x, dy = mp.y - hc.y;
        bool hover = std::sqrt(dx * dx + dy * dy) < 10.0f * ts;
        ImU32 fill = hover ? IM_COL32(255, 220, 80, 255) : ink(230);
        dl->AddCircleFilled(hc, 8.0f * ts, fill);
        dl->AddCircle      (hc, 8.0f * ts, paper(255), 0, 1.2f * ts);
        // Simple house glyph: triangle roof + small square body inside the circle.
        ImVec2 roofL(hc.x - 4.0f * ts, hc.y - 0.5f * ts), roofR(hc.x + 4.0f * ts, hc.y - 0.5f * ts),
               roofT(hc.x,             hc.y - 4.5f * ts);
        dl->AddTriangleFilled(roofL, roofT, roofR, paper(255));
        dl->AddRectFilled(ImVec2(hc.x - 3.0f * ts, hc.y - 0.5f * ts),
                          ImVec2(hc.x + 3.0f * ts, hc.y + 3.5f * ts),
                          paper(255));
        if (hover) {
            cubeHover = true;
            if (cubeClicked()) {
                m_pendingClick = ViewCubeAction::Home;
                m_cubeDragging = false;
            }
        }
    }

    // --- Coloured axis triad at the bottom-left of the widget. The three
    //     short arms rotate with the camera so the user can read off the
    //     current orientation. Labels use the user's Z-up convention:
    //     user X → world X (red), user Y → world Z (green), user Z → world Y
    //     (blue) - matches the rest of the UI.
    {
        ImVec2 tc(center.x - widgetR - 22.0f * ts, center.y + widgetR + 22.0f * ts);
        const float armLen = 18.0f * ts;
        // Triad colours match the world grid: X red, Y (world up) green, Z blue.
        // Labels follow the user's Z-up convention (their Z is world Y).
        struct Arm { glm::vec3 world; ImU32 col; const char* lbl; };
        Arm arms[3] = {
            { {1, 0, 0}, IM_COL32(230,  60,  60, 255), "X" }, // world X
            { {0, 0, 1}, IM_COL32( 80, 130, 240, 255), "Y" }, // world Z, blue (user Y = forward/back)
            { {0, 1, 0}, IM_COL32( 70, 200,  90, 255), "Z" }, // world Y, green (user Z = up)
        };
        for (auto& ar : arms) {
            glm::vec4 e = V * glm::vec4(ar.world, 0.0f);
            ImVec2 tip(tc.x + e.x * armLen, tc.y - e.y * armLen);
            dl->AddLine(tc, tip, ar.col, 2.0f);
            // Small label near the tip.
            ImVec2 lp(tip.x + (e.x >= 0 ? 2.0f : -8.0f),
                      tip.y - (e.y >= 0 ? 8.0f : -2.0f));
            dl->AddText(lp, ar.col, ar.lbl);
        }
        // Origin dot.
        dl->AddCircleFilled(tc, 2.5f, IM_COL32(220, 220, 230, 255));
    }

    // --- Drag the cube body itself to free-orbit (yaw + pitch). Click without
    //     drag snaps to the pressed face / accessory (arrows, roll, Home commit
    //     on release too, so a gesture can cancel them like any face).
    if (m_pendingClick != ViewCubeAction::None) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
            m_cubeDragging = true;
            ImVec2 d = ImGui::GetIO().MouseDelta;
            // Drag scaling: ~0.5° per pixel - gentle enough for precise aiming
            // but covers full rotation in a short stroke. `invertDrag` flips
            // the orbit sign for users who prefer the opposite mapping.
            const float k = invertDrag ? 0.01f : -0.01f; // radians per pixel
            camera.rotateAroundTarget(d.x * k, d.y * k);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            // A release synthesized by a two-finger takeover is not a click -
            // the finger is mid-pinch, not lifting. Cancel instead of commit
            // (the "pinch with one finger on the cube snaps the view" bug, #38).
            if (!m_cubeDragging && !releaseIsGesture)
                action = m_pendingClick; // commit the snap
            m_pendingClick = ViewCubeAction::None;
            m_cubeDragging = false;
        }
    }

    // Remember overall hover (including in-progress drags) so the viewport
    // suppresses its own click logic for everything the cube is handling.
    m_lastHovered = cubeHover || m_cubeDragging ||
                    (m_pendingClick != ViewCubeAction::None);
    return action;
}

} // namespace materializr
