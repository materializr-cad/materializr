#pragma once
// Shared surface of the interface layouts (Settings → Appearance → Interface).
//
// Materializr ships three layouts - classic, modern and im-touch (see
// UiLayout in io/Settings.h) - and may grow more. They are presentation
// SKINS over one set of fundamentals, and must never drift apart in what the
// user can DO:
//
//   - Menus: all layouts render the same File/Edit/View/Help item lists
//     (renderFileMenuItems & co., LayoutCommon.cpp). Classic hosts them in
//     the menu bar, modern/im-touch in the ⋯ overflow popup. Add a menu item
//     to the shared list ONCE and every layout gets it; plugin menu
//     contributions arrive through renderPluginMenuItems the same way.
//   - Tools: the selection-context tool catalogue is Toolbar::railTools()
//     (classic renders it as the docked Tools palette; modern as the left
//     rail; im-touch as the floating tool bar). A new tool or plugin
//     ToolAction added to the catalogue appears in all layouts.
//   - Panels (Items/History/Properties): one content renderer per panel
//     (renderContent()), hosted by classic's docks, modern's side panel, and
//     im-touch's overlays.
//
// If a feature can only be reached in one layout, that's a bug - either move
// it into one of the shared lists above, or add it to each layout's chrome
// (src/app/layout/<name>/) in the same change.

#include <imgui.h>

namespace materializr::layoutui {

// Window flags for the fixed chrome windows (bars, rails, panels) of the
// modern and im-touch layouts. All shell windows are ##-named +
// NoSavedSettings so they never touch imgui.ini - switching back to the
// classic layout restores its saved dock arrangement untouched.
constexpr ImGuiWindowFlags kShellWindowFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;

// The Materializr logo (embedded RGBA, ui/LogoTexture.h) as a lazily-uploaded
// GL texture, shared by the modern top-bar chip and the im-touch project
// chip. Uploaded once; lives with the GL context.
ImTextureID logoTexture();

} // namespace materializr::layoutui
