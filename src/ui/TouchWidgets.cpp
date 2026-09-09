#include "TouchWidgets.h"
#include "TouchTheme.h"
#include "TouchIcons.h"
#include "../ui_scale.h"
#include "../core/NumParse.h" // amountField commit parsing

#include <imgui_internal.h> // ImGuiItemFlags_Disabled for iconButton dimming

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include "../i18n.h"

namespace materializr {
namespace touchui {

namespace {

// Icon drawn centered in a rect at an arbitrary size (the atlas glyph is
// bitmap-scaled; fine at the small deltas we use - revisit if soft).
void drawIconCentered(ImDrawList* dl, const ImVec2& center, float size,
                      const char* icon, ImU32 col) {
    // MZ_ICON_PRIMITIVE sentinel (U+E001): a square overlapping a larger
    // circle (the CAD-sketch look - square top-left, circle through its
    // bottom-right corner). No Iconoir glyph reads as "basic solids".
    if (std::strcmp(icon, "\xee\x80\x81") == 0) {
        const float th = std::max(1.5f, size * 0.075f);
        const float hs = size * 0.28f;                       // square half-side
        const ImVec2 sc(center.x - size * 0.17f, center.y - size * 0.17f);
        const float r = size * 0.30f;                        // circle radius
        const ImVec2 cc(center.x + size * 0.15f, center.y + size * 0.15f);
        dl->AddRect(ImVec2(sc.x - hs, sc.y - hs), ImVec2(sc.x + hs, sc.y + hs),
                    col, 0.0f, 0, th);
        dl->AddCircle(cc, r, col, 0, th);
        return;
    }
    // MZ_ICON_CHAMFER sentinel (U+E000): Iconoir has no straight-corner-cut
    // glyph, so draw one - a square outline with its top-right corner
    // chamfered off. Matches Iconoir's 1.5px-at-24px stroke look.
    if (std::strcmp(icon, "\xee\x80\x80") == 0) {
        const float h = size * 0.40f;          // half side
        const float c = h * 0.95f;             // chamfer leg length
        const ImVec2 pts[5] = {
            ImVec2(center.x - h,     center.y - h),      // TL
            ImVec2(center.x + h - c, center.y - h),      // top edge, cut start
            ImVec2(center.x + h,     center.y - h + c),  // right edge, cut end
            ImVec2(center.x + h,     center.y + h),      // BR
            ImVec2(center.x - h,     center.y + h),      // BL
        };
        dl->AddPolyline(pts, 5, col, ImDrawFlags_Closed,
                        std::max(1.5f, size * 0.075f));
        return;
    }
    // MZ_ICON_UNFOLD sentinel (U+E002): a cube-unfold "Latin cross" of unit
    // squares - the flat-pattern look. (Iconoir's ruler-combine glyph didn't
    // read as "unfold".)
    if (std::strcmp(icon, "\xee\x80\x82") == 0) {
        const float cell = size * 0.22f;
        const float th   = std::max(1.2f, size * 0.05f);
        const ImVec2 o(center.x - cell * 2.0f, center.y - cell * 1.5f); // grid TL
        // (col,row) cells: a horizontal strip of 4 with two arms on the 3rd
        // column (the cross laid on its side, so it doesn't read as a crucifix).
        static const int cells[6][2] =
            {{2,0},{0,1},{1,1},{2,1},{3,1},{2,2}};
        for (const auto& c : cells)
            dl->AddRect(ImVec2(o.x + c[0] * cell,       o.y + c[1] * cell),
                        ImVec2(o.x + (c[0]+1) * cell,   o.y + (c[1]+1) * cell),
                        col, 0.0f, 0, th);
        return;
    }
    // MZ_ICON_PATTERN_LINEAR sentinel (U+E003): three squares in a row.
    if (std::strcmp(icon, "\xee\x80\x83") == 0) {
        const float cell = size * 0.26f;
        const float th   = std::max(1.5f, size * 0.06f);
        const ImVec2 o(center.x - cell * 1.5f, center.y - cell * 0.5f);
        for (int i = 0; i < 3; ++i)
            dl->AddRect(ImVec2(o.x + i * cell,     o.y),
                        ImVec2(o.x + (i+1) * cell, o.y + cell), col, 0.0f, 0, th);
        return;
    }
    // MZ_ICON_PATTERN_CIRCULAR sentinel (U+E004): three squares spaced around a
    // centre (120° apart, one at the bottom) - the radial-pattern look.
    if (std::strcmp(icon, "\xee\x80\x84") == 0) {
        const float hs = size * 0.12f;                 // square half-side
        const float r  = size * 0.30f;                 // ring radius
        const float th = std::max(1.5f, size * 0.06f);
        const ImVec2 ctr[3] = {
            ImVec2(center.x,              center.y + r),         // bottom
            ImVec2(center.x - 0.866f * r, center.y - 0.5f * r),  // top-left
            ImVec2(center.x + 0.866f * r, center.y - 0.5f * r),  // top-right
        };
        for (const auto& c : ctr)
            dl->AddRect(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs),
                        col, 0.0f, 0, th);
        return;
    }
    // MZ_ICON_THREAD sentinel (U+E005): a side-on flat-head screw with a
    // threaded shaft tapering to a point - reads as "cut threads", where the
    // old refresh-arrows glyph read as "reload".
    if (std::strcmp(icon, "\xee\x80\x85") == 0) {
        const float th   = std::max(1.5f, size * 0.075f);
        const float hw   = size * 0.32f;              // flat-head half-width
        const float sw   = size * 0.15f;              // shaft half-width
        const float topY = center.y - size * 0.44f;   // top of head
        const float neck = center.y - size * 0.28f;   // head/shaft join
        const float botY = center.y + size * 0.26f;   // shaft end / tip start
        const float tipY = center.y + size * 0.46f;   // point
        // Flat head.
        dl->AddRectFilled(ImVec2(center.x - hw, topY),
                          ImVec2(center.x + hw, neck), col, 1.0f);
        // Shaft sides + pointed tip.
        dl->AddLine(ImVec2(center.x - sw, neck), ImVec2(center.x - sw, botY), col, th);
        dl->AddLine(ImVec2(center.x + sw, neck), ImVec2(center.x + sw, botY), col, th);
        dl->AddLine(ImVec2(center.x - sw, botY), ImVec2(center.x, tipY), col, th);
        dl->AddLine(ImVec2(center.x + sw, botY), ImVec2(center.x, tipY), col, th);
        // Diagonal thread hatches across the shaft.
        const int nT = 4;
        const float dy = size * 0.045f;
        for (int i = 0; i < nT; ++i) {
            float y = neck + (botY - neck) * (i + 0.5f) / nT;
            dl->AddLine(ImVec2(center.x - sw, y + dy),
                        ImVec2(center.x + sw, y - dy), col, th * 0.9f);
        }
        return;
    }
    // MZ_ICON_OFFSET sentinel (U+E006): two nested rounded rectangles - a
    // shape and its parallel copy. Iconoir's expand/frame glyphs all read as
    // "resize", which is the one thing an offset is not.
    if (std::strcmp(icon, "\xee\x80\x86") == 0) {
        const float th = std::max(1.5f, size * 0.075f);
        const float ho = size * 0.42f;   // outer half-side
        const float hi = size * 0.22f;   // inner half-side
        dl->AddRect(ImVec2(center.x - ho, center.y - ho * 0.78f),
                    ImVec2(center.x + ho, center.y + ho * 0.78f),
                    col, size * 0.10f, 0, th);
        dl->AddRect(ImVec2(center.x - hi, center.y - hi * 0.78f),
                    ImVec2(center.x + hi, center.y + hi * 0.78f),
                    col, size * 0.06f, 0, th);
        return;
    }
    ImFont* font = ImGui::GetFont();
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, icon);
    dl->AddText(font, size, ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
                col, icon);
}

} // namespace

bool railButton(const char* id, const char* icon, const char* label, bool active,
                float width, bool solid) {
    const float s = uiScale();
    const float w = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    // 52 (was 62): shorter so the whole tool set fits with less scrolling,
    // still comfortably above the 44pt touch floor. If this changes, update
    // the lite shell's bottom-bar pill alignment (hardcodes the same height).
    const float h = 52.0f * s;

    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##rail", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (active) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                          ImGui::GetColorU32(accentFill()), radius(12.0f * s));
    } else if (hovered || ImGui::IsItemActive()) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                          ImGui::GetColorU32(rowBg()), radius(12.0f * s));
    } else if (solid) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                          ImGui::GetColorU32(panelBg()), radius(12.0f * s));
    }

    const ImU32 fg = ImGui::GetColorU32(active ? onAccent() : textPrimary());
    const ImU32 fgDim = ImGui::GetColorU32(active ? onAccent() : textDim());
    drawIconCentered(dl, ImVec2(p.x + w * 0.5f, p.y + h * 0.38f), 22.0f * s, icon, fg);

    ImFont* font = ImGui::GetFont();
    const float ls = 11.0f * s;
    const ImVec2 lsz = font->CalcTextSizeA(ls, FLT_MAX, 0.0f, label);
    dl->AddText(font, ls,
                ImVec2(p.x + (w - lsz.x) * 0.5f, p.y + h * 0.62f), fgDim, label);
    ImGui::PopID();
    return pressed;
}

float pillButtonWidth(const char* icon, const char* label) {
    const float s = uiScale();
    const float h = std::max(ImGui::GetFrameHeight(), 44.0f * s);
    const float is = 17.0f * s;                       // icon size
    ImFont* font = ImGui::GetFont();
    float w = 20.0f * s; // horizontal padding total
    if (icon)  w += font->CalcTextSizeA(is, FLT_MAX, 0.0f, icon).x;
    if (label) w += ImGui::CalcTextSize(label).x + (icon ? 7.0f * s : 0.0f);
    return std::max(w, h); // never narrower than tall
}

bool pillButton(const char* id, const char* icon, const char* label, bool accent) {
    const float s = uiScale();
    const float h = std::max(ImGui::GetFrameHeight(), 44.0f * s);
    const float is = 17.0f * s;                       // icon size
    ImFont* font = ImGui::GetFont();
    const float w = pillButtonWidth(icon, label);

    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##pill", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec4 bg = accent ? accentFill() : rowBg();
    if (hovered && !accent) bg = hoverBg();
    if (ImGui::IsItemActive()) bg = accent ? accentDeep() : pressBg();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(bg), radius(h * 0.32f));

    const ImU32 fg = ImGui::GetColorU32(accent ? onAccent() : textPrimary());
    float x = p.x + 10.0f * s;
    if (icon) {
        const ImVec2 ts = font->CalcTextSizeA(is, FLT_MAX, 0.0f, icon);
        dl->AddText(font, is, ImVec2(x, p.y + (h - ts.y) * 0.5f), fg, icon);
        x += ts.x + (label ? 7.0f * s : 0.0f);
    }
    if (label) {
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(x, p.y + (h - ts.y) * 0.5f), fg, label);
    }
    ImGui::PopID();
    return pressed;
}

float twoRowButtonWidth(const char* caption, const char* value) {
    const float s = uiScale();
    ImFont* font = ImGui::GetFont();
    const float capSz = std::max(11.0f * s, ImGui::GetFontSize() * 0.72f);
    const float w1 = font->CalcTextSizeA(capSz, FLT_MAX, 0.0f, caption).x;
    const float w2 = ImGui::CalcTextSize(value).x;
    return std::max(w1, w2) + 20.0f * s;   // horizontal padding total
}

bool twoRowButton(const char* id, const char* caption, const char* value,
                  bool accent) {
    const float s = uiScale();
    const float h = std::max(ImGui::GetFrameHeight(), 44.0f * s);
    const float w = twoRowButtonWidth(caption, value);
    ImFont* font = ImGui::GetFont();
    const float capSz = std::max(11.0f * s, ImGui::GetFontSize() * 0.72f);

    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##two", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec4 bg = accent ? accentFill() : rowBg();
    if (hovered && !accent) bg = hoverBg();
    if (ImGui::IsItemActive()) bg = accent ? accentDeep() : pressBg();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(bg),
                      radius(h * 0.32f));

    // Two stacked, horizontally-centred rows: dim caption over bold value.
    const ImU32 fgCap = ImGui::GetColorU32(accent ? onAccent() : textDim());
    const ImU32 fgVal = ImGui::GetColorU32(accent ? onAccent() : textPrimary());
    const ImVec2 capTs = font->CalcTextSizeA(capSz, FLT_MAX, 0.0f, caption);
    const ImVec2 valTs = ImGui::CalcTextSize(value);
    const float gap = 2.0f * s;
    const float blockH = capTs.y + gap + valTs.y;
    float y = p.y + (h - blockH) * 0.5f;
    dl->AddText(font, capSz, ImVec2(p.x + (w - capTs.x) * 0.5f, y), fgCap, caption);
    y += capTs.y + gap;
    dl->AddText(ImVec2(p.x + (w - valTs.x) * 0.5f, y), fgVal, value);

    ImGui::PopID();
    return pressed;
}

bool iconButton(const char* id, const char* icon, float side) {
    const float s = uiScale();
    if (side <= 0.0f) side = std::max(ImGui::GetFrameHeight(), 44.0f * s);

    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##ib", ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const bool enabled =
        !(ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled);
    ImVec4 bg = rowBg();
    if (hovered) bg = hoverBg();
    if (ImGui::IsItemActive()) bg = pressBg();
    dl->AddRectFilled(p, ImVec2(p.x + side, p.y + side),
                      ImGui::GetColorU32(bg), radius(10.0f * s));
    drawIconCentered(dl, ImVec2(p.x + side * 0.5f, p.y + side * 0.5f), 17.0f * s,
                     icon,
                     ImGui::GetColorU32(enabled ? textPrimary() : textDim()));
    ImGui::PopID();
    return pressed;
}

bool fab(const char* id, const char* icon, float diameter) {
    const float s = uiScale();
    if (diameter <= 0.0f) diameter = 56.0f * s;

    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##fab", ImVec2(diameter, diameter));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec4 bg = accentFill();
    if (hovered) bg = ImVec4(0.62f, 0.75f, 0.96f, 1.0f);
    if (ImGui::IsItemActive()) bg = accentDeep();
    const ImVec2 c(p.x + diameter * 0.5f, p.y + diameter * 0.5f);
    dl->AddCircleFilled(c, diameter * 0.5f, ImGui::GetColorU32(bg));
    drawIconCentered(dl, c, 24.0f * s, icon, ImGui::GetColorU32(onAccent()));
    ImGui::PopID();
    return pressed;
}

int segmented(const char* id, const char* const items[], int count, int active) {
    const float s = uiScale();
    const float h = 44.0f * s;
    const float w = ImGui::GetContentRegionAvail().x;

    // Segments are sized proportionally to their labels (a short "Items" cedes
    // room to "History & Properties") instead of equal halves; text is clipped
    // to its segment so a too-narrow panel can't bleed the label off-panel.
    float need[16];
    float total = 0.0f;
    const int n = count > 16 ? 16 : count;
    for (int i = 0; i < n; ++i) {
        need[i] = ImGui::CalcTextSize(items[i]).x + 24.0f * s;
        total += need[i];
    }

    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int result = active;
    float x = p.x;
    for (int i = 0; i < n; ++i) {
        const float seg = w * (need[i] / total);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(ImVec2(x, p.y));
        if (ImGui::InvisibleButton("##seg", ImVec2(seg, h))) result = i;
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 a(x, p.y), b(x + seg, p.y + h);
        if (i == active) {
            // Active segment: outlined pill (mockup style).
            dl->AddRectFilled(a, b, ImGui::GetColorU32(rowBg()), radius(10.0f * s));
            dl->AddRect(a, b, ImGui::GetColorU32(accentDeep()), radius(10.0f * s), 0,
                        2.0f * s);
        } else if (hovered) {
            dl->AddRectFilled(a, b, ImGui::GetColorU32(rowBg()), radius(10.0f * s));
        }
        const ImVec2 ts = ImGui::CalcTextSize(items[i]);
        dl->PushClipRect(a, b, true);
        dl->AddText(ImVec2(a.x + std::max(6.0f * s, (seg - ts.x) * 0.5f),
                           a.y + (h - ts.y) * 0.5f),
                    ImGui::GetColorU32(i == active ? textPrimary() : textDim()),
                    items[i]);
        dl->PopClipRect();
        ImGui::PopID();
        x += seg;
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Spacing();
    ImGui::PopID();
    return result;
}

void sectionHeader(const char* text) {
    const float s = uiScale();
    ImGui::Dummy(ImVec2(0.0f, 6.0f * s));
    // Small caps: uppercase at a smaller size, tracked out by the font.
    char buf[64];
    int n = 0;
    for (const char* c = text; *c && n < 63; ++c)
        buf[n++] = (*c >= 'a' && *c <= 'z') ? static_cast<char>(*c - 32) : *c;
    buf[n] = 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddText(ImGui::GetFont(), 12.0f * s, p, ImGui::GetColorU32(textDim()), buf);
    ImGui::Dummy(ImVec2(0.0f, 16.0f * s));
}

bool timelineBox(const char* id, const char* icon, bool current, bool editing,
                 bool dim, ImU32 iconCol, float side, const char* label) {
    const float s = uiScale();
    if (side <= 0.0f) side = 48.0f * s;

    const bool hasLabel = label && label[0];
    // History steps stack the label UNDER a slightly smaller icon, in a slightly
    // smaller font, with tight side padding - so a long run of named steps stays
    // compact. These metrics are private to this widget (the timeline is its
    // only caller), so nothing else is affected.
    const float iconSz   = 18.0f * s;                     // ~2px smaller
    const float fscale   = 0.82f;                         // label a little smaller
    const float fontSize = ImGui::GetFontSize() * fscale;
    const float padX     = 8.0f * s;                      // side dead-space
    const float padY     = 6.0f * s;
    const float vgap     = 3.0f * s;                      // icon → label gap
    ImVec2 ts(0.0f, 0.0f);
    if (hasLabel) {
        ts = ImGui::CalcTextSize(label);
        ts.x *= fscale;   // width scales with the font (glyphs scale uniformly)
        ts.y = fontSize;
    }
    const float boxW = hasLabel ? (std::max(iconSz, ts.x) + 2.0f * padX) : side;
    const float boxH = hasLabel ? (padY + iconSz + vgap + ts.y + padY) : side;

    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##tl", ImVec2(boxW, boxH));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec4 bg = current ? accentFill() : rowBg();
    if (hovered && !current) bg = hoverBg();
    if (ImGui::IsItemActive())
        bg = current ? accentDeep() : pressBg();
    dl->AddRectFilled(p, ImVec2(p.x + boxW, p.y + boxH),
                      ImGui::GetColorU32(bg), radius(10.0f * s));
    if (editing)
        dl->AddRect(p, ImVec2(p.x + boxW, p.y + boxH),
                    ImGui::GetColorU32(accentDeep()), radius(10.0f * s), 0, 2.0f * s);

    ImU32 fg = iconCol;
    if (fg == 0)
        fg = ImGui::GetColorU32(current ? onAccent()
                                        : (dim ? textDim() : textPrimary()));
    const float iconCy = hasLabel ? (p.y + padY + iconSz * 0.5f)
                                   : (p.y + boxH * 0.5f);
    drawIconCentered(dl, ImVec2(p.x + boxW * 0.5f, iconCy), iconSz, icon, fg);
    if (hasLabel)
        dl->AddText(ImGui::GetFont(), fontSize,
                    ImVec2(p.x + (boxW - ts.x) * 0.5f, p.y + padY + iconSz + vgap),
                    fg, label);
    ImGui::PopID();
    return pressed;
}

float numberPadWidth(float keyW) {
    const float s = uiScale();
    if (keyW <= 0.0f) keyW = 68.0f * s;
    return 3.0f * keyW + 2.0f * 6.0f * s;
}

void valueReadout(const char* id, const char* text, bool dim, float width) {
    const float s = uiScale();
    if (width <= 0.0f) width = numberPadWidth();
    const float h = 52.0f * s;
    const float ts = 26.0f * s;   // ~2x body text - the value is the point

    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h),
                      ImGui::GetColorU32(ImGuiCol_FrameBg), radius(8.0f * s));
    ImFont* font = ImGui::GetFont();
    ImVec2 sz = font->CalcTextSizeA(ts, FLT_MAX, 0.0f, text);
    // Right-aligned like a calculator; clip long values on the left by
    // nudging them right of the well's left padding at worst.
    float tx = p.x + width - 12.0f * s - sz.x;
    if (tx < p.x + 8.0f * s) tx = p.x + 8.0f * s;
    dl->AddText(font, ts, ImVec2(tx, p.y + (h - sz.y) * 0.5f),
                ImGui::GetColorU32(dim ? textDim() : accentFill()), text);
    ImGui::Dummy(ImVec2(width, h));
    ImGui::PopID();
}

bool numberPad(const char* id, char* buf, size_t bufSize, float keyW,
               float keyH, bool allowSign) {
    const float s = uiScale();
    if (keyW <= 0.0f) keyW = 68.0f * s;
    if (keyH <= 0.0f) keyH = 42.0f * s;
    bool changed = false;
    ImGui::PushID(id);
    // Nudge the glyphs UP a couple of px. ImGui centres a button label on the
    // full frame height, which reads as sitting low once the keys are short
    // and wide - the digit ends up optically below the middle of the key.
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.40f));
    static const char kRows[4][4] = {"789", "456", "123", ".0<"};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (c) ImGui::SameLine(0.0f, 6.0f * s);
            const char key = kRows[r][c];
            char lbl[8];
            if (key == '<') std::snprintf(lbl, sizeof(lbl), "%s", ICON_IC_ERASE);
            else { lbl[0] = key; lbl[1] = '\0'; }
            ImGui::PushID(r * 3 + c);
            if (ImGui::Button(lbl, ImVec2(keyW, keyH))) {
                size_t len = std::strlen(buf);
                if (key == '<') {
                    if (len > 0) { buf[len - 1] = '\0'; changed = true; }
                } else if (key == '.') {
                    if (!std::strchr(buf, '.') && len + 1 < bufSize) {
                        buf[len] = '.'; buf[len + 1] = '\0'; changed = true;
                    }
                } else if (len + 1 < bufSize) {
                    buf[len] = key; buf[len + 1] = '\0'; changed = true;
                }
            }
            ImGui::PopID();
        }
    }
    if (allowSign) {
        // Full-width ± toggling a leading minus (push/pull: negative = cut).
        if (ImGui::Button("+ / -", ImVec2(numberPadWidth(keyW), keyH))) {
            const size_t len = std::strlen(buf);
            if (buf[0] == '-') {
                std::memmove(buf, buf + 1, len);   // includes the NUL
            } else if (len + 1 < bufSize) {
                std::memmove(buf + 1, buf, len + 1);
                buf[0] = '-';
            }
            changed = true;
        }
    }
    ImGui::PopStyleVar();   // ButtonTextAlign
    ImGui::PopID();
    return changed;
}

bool amountField(const char* id, const char* label, double* v,
                 const char* suffix, int decimals, bool allowSign,
                 double minV, double maxV, const ImVec2* padPos) {
    const float s = uiScale();
    bool changed = false;

    ImGui::PushID(id);
    if (label && *label) {
        if (suffix && *suffix) ImGui::Text("%s (%s)", label, suffix);
        else                   ImGui::TextUnformatted(label);
    }

    // Native keyboard field (tap to focus) - replaces the in-app number pad on
    // the face-op panels (push/pull, extrude, fillet, chamfer). FIXED item
    // width so the field can't grow off-screen as digits are typed; the text
    // scrolls inside it instead.
    char fmt[8];
    std::snprintf(fmt, sizeof(fmt), "%%.%df", decimals < 0 ? 0 : decimals);
    ImGui::SetNextItemWidth(
        std::max(ImGui::GetContentRegionAvail().x, numberPadWidth(40.0f * s)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * s, 10.0f * s));
    double nv = *v;
    if (ImGui::InputDouble("##amt", &nv, 0.0, 0.0, fmt,
                           ImGuiInputTextFlags_CharsDecimal |
                           ImGuiInputTextFlags_AutoSelectAll)) {
        if (!allowSign && nv < 0.0) nv = 0.0;
        if (minV < maxV) nv = nv < minV ? minV : (nv > maxV ? maxV : nv);
        *v = nv;
        changed = true;
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    (void)padPos;   // pad-anchor no longer used (native keyboard)
    return changed;
}

bool numberField(const char* id, const char* label, double* v, const char* fmt,
                 bool* opened, const char* hint) {
    const float s = uiScale();
    bool committed = false;

    // Per-field entry buffer, keyed by ImGui id so a field scrolled away and
    // back doesn't inherit another field's half-typed digits. s_open is the
    // ONE unfolded field: opening a second collapses the first, which keeps
    // the panel from growing by a pad per field. s_justOpened defers the
    // scroll-into-view by a frame, so it runs once the pad has a real height.
    struct Entry { char buf[32]; };
    static std::map<ImGuiID, Entry> s_entries;
    static ImGuiID s_open = 0;
    static ImGuiID s_justOpened = 0;

    ImGui::PushID(id);
    const ImGuiID key = ImGui::GetID("##numfield");
    const bool openHere = (s_open == key);

    // Honour ImGui's label conventions, which InputDouble got for free and a
    // plain TextUnformatted does not: "##id" means NO visible label (printing
    // it verbatim leaked raw ids like "##sketchDimT" into the UI), and
    // "Name##id" shows only "Name".
    if (label && *label) {
        const char* lblEnd = ImGui::FindRenderedTextEnd(label);
        if (lblEnd > label) ImGui::TextUnformatted(label, lblEnd);
    }

    // The well doubles as the readout: while unfolded it shows the LIVE typed
    // buffer, not the stored value. A separate calculator-style readout above
    // the keys cost ~50*s of height and pushed the digits being typed out of
    // view whenever the panel had to scroll to reach the keys - the field
    // itself is the obvious place to show them, and it's already on screen.
    const bool hintState = hint && *v <= 0.0;   // empty-is-meaningful fields
    char shown[64];
    if (openHere) {
        const Entry& e = s_entries[key];
        std::snprintf(shown, sizeof(shown), "%s",
                      e.buf[0] ? e.buf : (hintState ? hint : "0"));
    } else if (hintState) {
        std::snprintf(shown, sizeof(shown), "%s", hint);
    } else {
        std::snprintf(shown, sizeof(shown), fmt ? fmt : "%g", *v);
    }

    // Fit the pad to the panel rather than the other way round. A fixed key
    // width wider than the host panel doesn't just overflow - the well is
    // sized to match it, and ImGui centres a button's label, so the label
    // lands outside the clip rect and the field renders BLANK.
    //
    // Sizes are deliberately modest: the pad lives inside a scrolling
    // properties panel UNDER as many as six other fields, so every row it
    // spends is a row the caller has to scroll past.
    const float gap   = 4.0f * s;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float keyW  = std::min(std::max((avail - 2.0f * gap) / 3.0f, 38.0f * s),
                                 78.0f * s);
    // 24, not the 42 this started at. The pad lives in a scrolling properties
    // column under as many as six other fields, so every unit of key HEIGHT is
    // a unit the caller has to scroll past - and a number pad is the one
    // keyboard where nobody needs a big target per key, because the keys are
    // huge in the width axis and there are only twelve of them. Still ~48px on
    // a tablet (uiScale ~2), comfortably above the 44pt touch-target floor.
    const float keyH  = 24.0f * s;
    const float padW  = numberPadWidth(keyW);

    // A plain button, so the pad unfolds on an explicit tap and NOTHING else -
    // the old keyboard's habit of appearing whenever a dialog opened is the
    // specific behaviour this must not repeat.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * s, 6.0f * s));
    if (openHere)
        ImGui::PushStyleColor(ImGuiCol_Text, accentFill());
    else if (hintState)
        ImGui::PushStyleColor(ImGuiCol_Text, textDim());
    if (ImGui::Button(shown, ImVec2(avail, 0.0f))) {
        if (openHere) {
            s_open = 0;               // tapping the open field folds it again
        } else {
            Entry e{};
            if (!hintState)   // hint fields start EMPTY: nothing typed = keep
                std::snprintf(e.buf, sizeof(e.buf), fmt ? fmt : "%g", *v);
            s_entries[key] = e;
            s_open = key;
            s_justOpened = key;
            if (opened) *opened = true;
        }
    }
    if (openHere || (!openHere && hintState)) ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // Park the well at the TOP of the view on the frame after it unfolds, so
    // the whole pad has the panel's height below it. Scrolling the pad's
    // BOTTOM into view instead (the obvious reading of "keep it visible")
    // pushes the well - and so the digits being typed - off the top.
    if (openHere && s_justOpened == key) {
        ImGui::SetScrollHereY(0.0f);
        s_justOpened = 0;
    }

    if (openHere) {
        Entry& e = s_entries[key];

        // allowSign=false: the pad's own sign key is a full-width row, and a
        // separate Enter row under it made the block taller than the panel had
        // room for. The three share one row instead.
        numberPad("##pad", e.buf, sizeof(e.buf), keyW, keyH, /*allowSign=*/false);

        const float thirdW = (padW - 2.0f * gap) / 3.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.40f));
        if (ImGui::Button("+ / -", ImVec2(thirdW, keyH))) {
            const size_t len = std::strlen(e.buf);
            if (e.buf[0] == '-') {
                std::memmove(e.buf, e.buf + 1, len);       // includes the NUL
            } else if (len + 1 < sizeof(e.buf)) {
                std::memmove(e.buf + 1, e.buf, len + 1);
                e.buf[0] = '-';
            }
        }
        ImGui::SameLine(0.0f, gap);
        if (ImGui::Button(MZ_ICON_CLOSE, ImVec2(thirdW, keyH))) {
            s_open = 0;               // fold, *v untouched
        }
        ImGui::SameLine(0.0f, gap);
        // Enter commits. Parsing here rather than per keystroke is what makes
        // the caller see one change instead of one per digit - a history-step
        // editor rebuilds once on commit, not on every tap.
        if (ImGui::Button(materializr::tr("Enter"), ImVec2(thirdW, keyH))) {
            char* end = nullptr;
            const double parsed = std::strtod(e.buf, &end);
            if (end != e.buf) { *v = parsed; committed = true; }
            s_open = 0;
        }
        ImGui::PopStyleVar();   // ButtonTextAlign
    }

    ImGui::PopID();
    return committed;
}

bool amountField(const char* id, const char* label, float* v,
                 const char* suffix, int decimals, bool allowSign,
                 float minV, float maxV, const ImVec2* padPos) {
    double d = static_cast<double>(*v);
    const bool changed =
        amountField(id, label, &d, suffix, decimals, allowSign,
                    static_cast<double>(minV), static_cast<double>(maxV),
                    padPos);
    if (changed) *v = static_cast<float>(d);
    return changed;
}

bool treeGroup(const char* id, const char* label, int count, bool open,
               bool* rightClicked, const char* trailingLabel,
               bool* trailingClicked) {
    const float s = uiScale();
    const float h = 40.0f * s;
    const float w = ImGui::GetContentRegionAvail().x;

    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Trailing action pill (e.g. "+ Folder") - its own exclusive hit area on
    // the right, submitted BEFORE the group toggle so the toggle (which then
    // covers only the remaining width) can't swallow its taps. Same lesson as
    // treeLeaf's eye button.
    float trailW = 0.0f;
    if (trailingLabel && *trailingLabel) {
        const ImVec2 tts = ImGui::CalcTextSize(trailingLabel);
        trailW = tts.x + 20.0f * s;
        const float bx = p.x + w - trailW;
        ImGui::SetCursorScreenPos(ImVec2(bx, p.y));
        if (ImGui::InvisibleButton("##trail", ImVec2(trailW, h)) &&
            trailingClicked)
            *trailingClicked = true;
        const bool thov = ImGui::IsItemHovered();
        ImVec4 fill = accentFill();
        fill.w = thov ? 0.60f : 0.35f;
        dl->AddRectFilled(ImVec2(bx, p.y + 4.0f * s),
                          ImVec2(bx + trailW, p.y + h - 4.0f * s),
                          ImGui::GetColorU32(fill), radius(6.0f * s));
        dl->AddText(ImVec2(bx + 10.0f * s, p.y + (h - tts.y) * 0.5f),
                    ImGui::GetColorU32(textPrimary()), trailingLabel);
        ImGui::SetCursorScreenPos(p);   // rewind so the toggle starts at p
    }

    // Toggle covers everything left of the trailing pill (or the full width
    // when there's no pill), so the two never overlap.
    const float grpW =
        std::max(1.0f, w - (trailW > 0.0f ? trailW + 6.0f * s : 0.0f));
    const bool pressed = ImGui::InvisibleButton("##grp", ImVec2(grpW, h));
    if (rightClicked && ImGui::IsItemClicked(ImGuiMouseButton_Right))
        *rightClicked = true;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered)
        dl->AddRectFilled(p, ImVec2(p.x + grpW, p.y + h),
                          ImGui::GetColorU32(rowHoverBg()), radius(6.0f * s));

    // Disclosure triangle, drawn (font glyphs for ▶/▼ look mismatched at
    // this size). Open points down, closed points right.
    const float tri = 5.0f * s;
    const ImVec2 c(p.x + 14.0f * s, p.y + h * 0.5f);
    const ImU32 fg = ImGui::GetColorU32(textPrimary());
    if (open)
        dl->AddTriangleFilled(ImVec2(c.x - tri, c.y - tri * 0.6f),
                              ImVec2(c.x + tri, c.y - tri * 0.6f),
                              ImVec2(c.x, c.y + tri), fg);
    else
        dl->AddTriangleFilled(ImVec2(c.x - tri * 0.6f, c.y - tri),
                              ImVec2(c.x + tri, c.y),
                              ImVec2(c.x - tri * 0.6f, c.y + tri), fg);

    char buf[80];
    std::snprintf(buf, sizeof(buf), "%s (%d)", label, count);
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(p.x + 28.0f * s, p.y + (h - ts.y) * 0.5f), fg, buf);
    ImGui::PopID();
    return pressed;
}

TreeLeafAction treeLeaf(const char* id, const char* icon, const char* label,
                        bool* visible, bool selected, const float* swatchRGB) {
    TreeLeafAction act;
    const float s = uiScale();
    const float h = 40.0f * s;
    const float w = ImGui::GetContentRegionAvail().x;
    const float indent = 26.0f * s;   // children sit under the group text
    const float eyeW = 34.0f * s;
    const float swW = swatchRGB ? 40.0f * s : 0.0f;   // trailing colour swatch

    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Claim the whole row rect up front. The internals below are placed with
    // SetCursorScreenPos, and ImGui flags any such jump past the window's
    // current content max ("uses SetCursorPos to extend window boundaries -
    // submit an item e.g. Dummy()") - which, unfixed, fired every frame for
    // the im-touch Bodies tree (##LiteTree). Same lesson as listRow: claim the
    // rect with a Dummy, and derive the row bottom the way ItemSize() does
    // (post-Dummy cursor minus spacing) rather than the raw p.y + h - at a
    // fractional uiScale ImGui truncates the item advance to whole pixels, so
    // p.y + h overshoots the claimed max by the fraction and trips the warning
    // on every row. The max() guards the first auto-resize frame where the
    // content region reports ~0 wide.
    ImGui::Dummy(ImVec2(std::max(w, indent + eyeW + swW + 1.0f), h));
    const float rowBottom =
        ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;

    // Eye first - its own exclusive hit area (a row button submitted before
    // it would swallow the taps; same lesson as listRow's checkbox).
    ImGui::SetCursorScreenPos(ImVec2(p.x + indent, p.y));
    if (ImGui::InvisibleButton("##eye", ImVec2(eyeW, h))) {
        if (visible) { *visible = !*visible; act.eyeToggled = true; }
    }
    const bool eyeHov = ImGui::IsItemHovered();

    // Colour swatch (right edge) - also its own exclusive hit area, submitted
    // before the row so a swatch tap doesn't select the row.
    if (swatchRGB) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - swW, p.y));
        if (ImGui::InvisibleButton("##swatch", ImVec2(swW, h)))
            act.swatchClicked = true;
    }

    // Row body (select) - the middle, between the eye and the swatch.
    ImGui::SetCursorScreenPos(ImVec2(p.x + indent + eyeW, p.y));
    act.clicked = ImGui::InvisibleButton(
        "##row", ImVec2(std::max(1.0f, w - indent - eyeW - swW), h));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) act.rightClicked = true;
    const bool rowHov = ImGui::IsItemHovered();

    const float fillR = p.x + w - swW;   // fills stop short of the swatch
    if (selected) {
        ImVec4 selBg = accentFill();
        selBg.w = 0.30f;   // soft fill - the tree stays see-through
        dl->AddRectFilled(ImVec2(p.x + indent, p.y), ImVec2(fillR, p.y + h),
                          ImGui::GetColorU32(selBg), radius(6.0f * s));
    } else if (rowHov) {
        dl->AddRectFilled(ImVec2(p.x + indent, p.y), ImVec2(fillR, p.y + h),
                          ImGui::GetColorU32(rowHoverBg()), radius(6.0f * s));
    }

    const bool shown = !visible || *visible;
    const ImU32 dimCol  = ImGui::GetColorU32(textDim());
    const ImU32 mainCol = ImGui::GetColorU32(shown ? textPrimary() : textDim());
    if (visible)
        drawIconCentered(dl, ImVec2(p.x + indent + eyeW * 0.5f, p.y + h * 0.5f),
                         15.0f * s, shown ? MZ_ICON_VISIBLE : MZ_ICON_HIDDEN,
                         eyeHov ? ImGui::GetColorU32(textPrimary()) : dimCol);
    // Type icon + name (clipped so a long name can't run under the swatch).
    const float ix = p.x + indent + eyeW + 4.0f * s;
    drawIconCentered(dl, ImVec2(ix + 9.0f * s, p.y + h * 0.5f), 15.0f * s,
                     icon, mainCol);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->PushClipRect(ImVec2(ix + 24.0f * s, p.y),
                     ImVec2(fillR - 4.0f * s, p.y + h), true);
    dl->AddText(ImVec2(ix + 24.0f * s, p.y + (h - ts.y) * 0.5f), mainCol,
                label);
    dl->PopClipRect();

    // The swatch itself, drawn on top of the fills.
    if (swatchRGB) {
        const float sq = 22.0f * s;
        const ImVec2 a(p.x + w - swW + (swW - sq) * 0.5f, p.y + (h - sq) * 0.5f);
        const ImVec2 b(a.x + sq, a.y + sq);
        const ImU32 sc = ImGui::ColorConvertFloat4ToU32(
            ImVec4(swatchRGB[0], swatchRGB[1], swatchRGB[2], 1.0f));
        dl->AddRectFilled(a, b, sc, radius(4.0f * s));
        dl->AddRect(a, b, ImGui::GetColorU32(textDim()), radius(4.0f * s));
    }

    // rowBottom is bit-exact with the Dummy's claimed max, so stacking the
    // next row here never re-extends the boundary.
    ImGui::SetCursorScreenPos(ImVec2(p.x, rowBottom));
    ImGui::PopID();
    return act;
}

ListRowAction listRow(const char* id, bool* checked, const char* label,
                      bool selected, bool withOverflow) {
    ListRowAction act;
    const float s = uiScale();
    const float h = 44.0f * s;
    const float w = ImGui::GetContentRegionAvail().x;
    const float box = 22.0f * s;
    const float pad = 10.0f * s;
    const float ovW = withOverflow ? h : 0.0f;

    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lead = checked ? pad + box + pad : pad;

    // Claim the WHOLE row rect first. The internals below are placed with
    // SetCursorScreenPos, and ImGui flags any such jump past the window's
    // current content max at the next item ("uses SetCursorPos to extend
    // window boundaries - submit an item e.g. Dummy()"). With the row rect
    // claimed up front, every internal placement stays within bounds. The
    // max() guards the FIRST frame of an auto-resize host, where the content
    // region reports ~0 wide - claim at least the internals' extent or the
    // checkbox placement still lands out of bounds (and the warning banner
    // it trips sticks for the whole session).
    ImGui::Dummy(ImVec2(std::max(w, lead + ovW + 1.0f), h));
    // ImGui TRUNCATES item advances to whole pixels, so the claimed row
    // bottom is trunc-based - recompute it exactly the way ItemSize() does
    // (post-Dummy cursor minus spacing) instead of the raw p.y + h, which at
    // a fractional uiScale overshoots the claim by the fraction and trips
    // the boundary warning on EVERY row (found via instrumented logcat:
    // cur.y 673.80 vs max.y 673.00 at s = 1.7).
    const float rowBottom =
        ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;

    // Checkbox (visibility) FIRST, with its own exclusive hit area - a row
    // button submitted before it would claim its clicks (ImGui gives the
    // press to the first hovered item), leaving the checkbox untappable.
    bool chkHov = false;
    if (checked) {
        const ImVec2 cb(p.x + pad, p.y + (h - box) * 0.5f);
        ImGui::SetCursorScreenPos(cb);
        if (ImGui::InvisibleButton("##chk", ImVec2(box, box))) {
            *checked = !*checked;
            act.toggled = true;
        }
        chkHov = ImGui::IsItemHovered();
    }

    // Row body (select) - from after the checkbox to before the trailing ⋯,
    // so the three hit areas never overlap.
    ImGui::SetCursorScreenPos(ImVec2(p.x + lead, p.y));
    act.clicked = ImGui::InvisibleButton(
        "##row", ImVec2(std::max(1.0f, w - lead - ovW), h));
    const bool rowHov = ImGui::IsItemHovered();

    if (selected)
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                          ImGui::GetColorU32(rowBg()), radius(10.0f * s));
    else if (rowHov)
        dl->AddRectFilled(p, ImVec2(p.x + w - ovW, p.y + h),
                          ImGui::GetColorU32(rowHoverBg()),
                          radius(10.0f * s));

    if (checked) {
        const ImVec2 cb(p.x + pad, p.y + (h - box) * 0.5f);
        if (*checked) {
            dl->AddRectFilled(cb, ImVec2(cb.x + box, cb.y + box),
                              ImGui::GetColorU32(accentFill()), radius(6.0f * s));
            ImFont* font = ImGui::GetFont();
            const float cs = 14.0f * s;
            const ImVec2 ts = font->CalcTextSizeA(cs, FLT_MAX, 0.0f, MZ_ICON_CHECK);
            dl->AddText(font, cs,
                        ImVec2(cb.x + (box - ts.x) * 0.5f, cb.y + (box - ts.y) * 0.5f),
                        ImGui::GetColorU32(onAccent()), MZ_ICON_CHECK);
        } else {
            dl->AddRect(cb, ImVec2(cb.x + box, cb.y + box),
                        ImGui::GetColorU32(chkHov ? textDim() : hairline()),
                        radius(6.0f * s), 0, 2.0f * s);
        }
    }

    // Label.
    const float lx = p.x + lead;
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(lx, p.y + (h - ts.y) * 0.5f),
                ImGui::GetColorU32(textPrimary()), label);

    // Trailing ⋯.
    if (withOverflow) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - ovW, p.y));
        if (ImGui::InvisibleButton("##ovf", ImVec2(ovW, h))) act.overflow = true;
        const bool ovHov = ImGui::IsItemHovered();
        drawIconCentered(dl, ImVec2(p.x + w - ovW * 0.5f, p.y + h * 0.5f),
                         16.0f * s, MZ_ICON_MORE,
                         ImGui::GetColorU32(ovHov ? textPrimary() : textDim()));
    }

    // Next row starts right below (tight stacking). rowBottom is bit-exact
    // with the Dummy's claimed max, so this never extends boundaries.
    ImGui::SetCursorScreenPos(ImVec2(p.x, rowBottom));
    ImGui::PopID();
    return act;
}

} // namespace touchui
} // namespace materializr
