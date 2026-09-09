// Tutorial / onboarding plugin.
//
// A short, skippable, step-by-step "Getting Started" overlay that teaches the
// basic Materializr workflow. It is cross-platform: each step explains both the
// mouse and the touch way of doing things, highlighting whichever input model
// the current run is using (materializr::touchMode()).
//
// The tour OPENS with a layout picker: three cards (Classic / Modern /
// im-touch), each with a plain-language description. Tapping a card switches
// the interface LIVE behind the modal - the running app is the preview, not a
// screenshot - and every later step then points at the right place for the
// layout the user picked (the highlighted "In this layout" line).
//
// Built entirely as a plugin (no Application changes beyond the generic plugin
// hooks + the ui_layout_bridge): it registers a Help > Getting Started menu
// item to launch it, and an OverlayContribution to draw the panel each frame.
// It auto-shows once on first run, then writes a marker so it stays out of the
// way until the user reopens it.

#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../app/ui_layout_bridge.h"
#include "../touch_mode.h"
#include "../i18n.h"
#include "../ui_scale.h"

#include <imgui.h>
#include <SDL.h>
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <fstream>
#include <string>
#include "../i18n.h"

namespace {

// One tour step. `mouse` / `touch` are optional input-specific lines shown
// under the body (null = omit). `where[3]` are optional per-layout location
// lines (Classic / Modern / im-touch): the one matching the CURRENT layout is
// shown highlighted, so the instructions always match what's on screen.
struct Step {
    const char* title;
    const char* body;
    const char* mouse;
    const char* touch;
    const char* where[3];   // Classic, Modern, im-touch (null = omit)
};

const Step kSteps[] = {
    {
        "Welcome to Materializr",
        "Materializr is a parametric 3D CAD app: sketch a 2D shape, turn it into "
        "a solid, then keep refining it. This quick tour covers the essentials - "
        "it takes a minute. You can reopen it anytime from Help > Getting Started.",
        nullptr, nullptr, {nullptr, nullptr, nullptr}
    },
    {
        "Moving the view",
        "Orbit, pan and zoom to look at your model from any angle. The cube in "
        "the top-right corner snaps to standard views, and Frame Selection (F) "
        "zooms onto whatever is selected.",
        "Orbit, pan and zoom with the mouse buttons and wheel (set them in "
        "Settings). The Interactions panel always lists the current bindings.",
        "Drag one finger to orbit; use two fingers to pan and pinch-zoom.",
        {
            "View actions live in the View menu on the top menu bar.",
            "View actions live under the \xE2\x80\xA6 menu (top-left) > View.",
            "View actions live behind the \xE2\x98\xB0 menu in the top bar."
        }
    },
    {
        "Selecting things",
        "Click a body, face or edge to select it - most tools act on the current "
        "selection. A small context menu gives quick actions on whatever you're "
        "pointing at.",
        "Click to select, Ctrl+click to add more. Right-click for the context menu.",
        "Tap to select; use the on-screen Multi toggle to add more. Press and hold "
        "(long-press) for the context menu.",
        {nullptr, nullptr, nullptr}
    },
    {
        "Sketching a shape",
        "Solids start as 2D sketches: pick a plane or a flat face to draw on, "
        "then lay down lines, rectangles and circles on the grid. Type a number "
        "while drawing to set an exact size, then finish the sketch.",
        "Click to place points; press Enter to finish a shape, Esc to cancel.",
        "Tap to place points, or press-drag-release to draw a segment. Use the "
        "Finish Sketch / Exit Sketch buttons.",
        {
            "Start from the Tools panel on the left: Sketch on XY / XZ / YZ, or "
            "select a flat face first.",
            "Start from the left rail: tap \"Sketch on...\", or select a flat "
            "face and choose Sketch.",
            "Tap the + button (bottom-right) and choose New Sketch."
        }
    },
    {
        "From sketch to solid",
        "Turn a closed sketch into a 3D body: select it and use Push/Pull to give "
        "it depth, or Revolve to spin it around an axis. On an existing solid, "
        "Push/Pull a face to move it, or add a Fillet/Chamfer to round or bevel "
        "edges. Drag the arrow - or type a distance - to set the amount.",
        nullptr, nullptr,
        {
            "The Tools panel lists what applies to your selection (Face "
            "Operations, Push / Pull, Shell...).",
            "Select something and the left rail fills with the tools that apply "
            "(Push, Extrude, Shell...).",
            "Select something and a floating tool dock appears along the left "
            "edge."
        }
    },
    {
        "History and undo",
        "Every operation is recorded in the history. Materializr is parametric, "
        "so you can go back and change an earlier step: click a history entry "
        "to edit its parameters and the whole model rebuilds.",
        "Undo / redo with Ctrl+Z / Ctrl+Y, or from the Edit menu.",
        "Undo / redo with the arrows in the top bar (or a keyboard, if one is "
        "attached).",
        {
            "The History panel sits on the right side.",
            "History is the second tab of the right panel.",
            "Tap the History button in the bottom-left corner."
        }
    },
    {
        "Organize, save and export",
        "Your bodies and sketches are listed so you can hide, rename and group "
        "them into folders. Save your work as a project, or export for 3D "
        "printing and other CAD tools - STL, 3MF, STEP, OBJ, glTF, IGES or "
        "BREP. Export writes every VISIBLE body to one file, so hiding a "
        "body leaves it out; to send just a few, select them, right-click "
        "and use Export.",
        "Right-click an item to move it into a folder. Export lives under "
        "File > Export.",
        "Long-press an item to move it into a folder. Export offers Share (send "
        "to another app) or save-to-file.",
        {
            "The Items panel is on the right; saving and exporting live in the "
            "File menu.",
            "Items is the first tab of the right panel; saving and exporting "
            "live under the \xE2\x80\xA6 menu > File.",
            "Tap the list icon in the top bar for your items; saving and "
            "exporting live behind the \xE2\x98\xB0 menu."
        }
    },
    {
        "Several projects at once",
        "Each project opens in its own tab, with its own history and camera - "
        "handy for copying a part from one design into another. The home "
        "screen lists your recent projects with a picture of each; it doesn't "
        "close what you're working on, so you can go and come back.",
        "Ctrl+Tab cycles the open tabs. File > Home Screen shows the grid.",
        "Tap the project name to switch between open projects; the home screen "
        "is behind the menu.",
        {
            "Tabs run along the top of the 3D view; the + opens a new one.",
            "Tabs are the pills in the top bar; the + beside them opens a new "
            "one.",
            "The project name (top-left) opens the list of open projects."
        }
    },
    {
        "Need more room?",
        "Short on screen space? Enlarge the 3D view by folding the side panels "
        "away, then bring them back when you need them.",
        nullptr, nullptr,
        {
            "Press F9 (or View > Hide Panels) to collapse the side panels.",
            "Tap the small chevron tabs on the left and right edges of the "
            "viewport to fold a column away - tap again to bring it back. The "
            "Focus button (top-right) cycles the same thing.",
            "im-touch is already minimal: panels appear only when something "
            "needs them and tuck away on their own."
        }
    },
    {
        "You're ready",
        "That's the basics. Explore the tools, and see Help > User Guide and "
        "Keyboard Shortcuts for more detail. Have fun building.",
        nullptr, nullptr, {nullptr, nullptr, nullptr}
    },
};
const int kStepCount = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));

// The layout picker cards (page shown before step 1). Descriptions are for a
// NEW user: what it looks like and who it suits, no jargon.
struct LayoutCard {
    const char* name;
    const char* blurb;
};
const LayoutCard kLayouts[3] = {
    { "Classic",
      "Desktop-style. A menu bar on top and every panel visible at once. "
      "Best with a mouse and keyboard." },
    { "Modern",
      "Clean and focused. A tool rail on the left, tabbed panels on the "
      "right, everything else out of the way. Great on any screen." },
    { "im-touch",
      "Built for fingers. A full-screen model with floating buttons and "
      "big touch targets. Made for tablets." },
};

// --- State (process-lifetime; the plugin is a singleton overlay) -------------
bool g_open = false;
bool g_onPicker = false;   // the layout-picker page (before step 1)
bool g_onLanguage = false; // the language page (before the layout picker)
int  g_step = 0;
bool g_firstRunChecked = false;

// A writable, persistent, per-user marker so the tour auto-shows only on first
// run. SDL_GetPrefPath resolves to the right place on every platform (and
// creates the directory), including Android's app storage.
std::string markerPath() {
    char* base = SDL_GetPrefPath("Materializr", "Materializr");
    std::string p = base ? std::string(base) + "tutorial_seen" : std::string();
    if (base) SDL_free(base);
    return p;
}
bool tutorialSeen() {
    std::string p = markerPath();
    if (p.empty()) return false;
    std::ifstream f(p);
    return f.good();
}
void markSeen() {
    std::string p = markerPath();
    if (p.empty()) return;
    std::ofstream f(p);
    if (f) f << "1\n";
}

// The LANGUAGE page, shown before everything else on a first run where no
// language has been chosen. It has to be first and it has to be readable
// without already knowing the UI language, so:
//   * every language is listed in ITS OWN name ("Espanol", not "Spanish");
//   * tapping one switches the whole UI live, so the confirmation that you
//     picked right is the page itself changing under your finger;
//   * the only prose is one line, and it is translated into all of them.
// Reachable afterwards from Settings > Appearance, so a mis-tap is not a trap.
void renderLanguagePage(bool& close) {
    using namespace materializr;
    ImGui::TextDisabled("%s", materializr::tr("Language"));
    ImGui::SeparatorText(materializr::tr("Choose your language"));
    ImGui::Spacing();

    const int current = currentLanguageIndex();
    for (int i = 0; i < languageCount(); ++i) {
        ImGui::PushID(i);
        const bool active = (current == i);
        // Native name first, English name after, so the list is navigable
        // whichever of the two you happen to read.
        char label[96];
        std::snprintf(label, sizeof(label), "%s", languageNativeName(i));
        if (ImGui::RadioButton(label, active)) requestLanguage(i);
        if (i != 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", languageEnglishName(i));
        }
        ImGui::PopID();
    }

    ImGui::Spacing();
    // Honest about the state of the work: the tool and menu names are
    // translated, the long help text mostly is not. Better to say so than to
    // let someone conclude the translation is broken.
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", materializr::tr("Menus and tool names are translated. "
                                 "Some longer help text is still English."));
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button(materializr::tr("Continue"), materializr::uiSz(110, 0)))
        g_onLanguage = false;
    const float skipW = uiW(80.0f);
    const float rightX = ImGui::GetWindowContentRegionMax().x - skipW;
    if (rightX > ImGui::GetCursorPosX()) ImGui::SameLine(rightX);
    else                                 ImGui::SameLine();
    if (ImGui::Button(materializr::tr("Skip"), materializr::uiSz(80, 0))) close = true;
}

// The picker page: three selectable cards. Tapping one switches the layout
// LIVE (the app behind the modal is the preview). Continue starts the tour.
void renderLayoutPicker(bool& close) {
    using namespace materializr;
    ImGui::TextDisabled("%s", materializr::tr("First, make it yours"));
    ImGui::SeparatorText(materializr::tr("Choose your workspace"));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(
        materializr::tr("Materializr has three interface styles. Tap one to try it - the app behind this window switches instantly, so you can see the real thing. You can change your mind anytime in Settings > Appearance."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const int current = currentUiLayoutIndex();
    for (int i = 0; i < 3; ++i) {
        const bool active = (current == i);
        // A card = a full-width selectable region with title + wrapped blurb.
        ImGui::PushID(i);
        ImVec2 pad = ImGui::GetStyle().FramePadding;
        const float wrapW = ImGui::GetContentRegionAvail().x - 2.0f * pad.x
                            - uiW(28.0f);
        ImVec2 blurbSz = ImGui::CalcTextSize(kLayouts[i].blurb, nullptr,
                                             false, wrapW);
        const float cardH = ImGui::GetTextLineHeightWithSpacing() +
                            blurbSz.y + 3.0f * pad.y;
        ImVec2 tl = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##card", active, 0, ImVec2(0, cardH)))
            requestUiLayout(i);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 br(tl.x + ImGui::GetContentRegionAvail().x, tl.y + cardH);
        if (active)
            dl->AddRect(tl, br, ImGui::GetColorU32(ImGuiCol_CheckMark),
                        4.0f, 0, 2.0f);
        // Radio dot + title + blurb drawn over the selectable.
        ImVec2 c(tl.x + pad.x + uiW(8.0f),
                 tl.y + pad.y + ImGui::GetTextLineHeight() * 0.5f);
        dl->AddCircle(c, uiW(6.0f), ImGui::GetColorU32(ImGuiCol_Text), 0, 1.5f);
        if (active)
            dl->AddCircleFilled(c, uiW(3.2f),
                                ImGui::GetColorU32(ImGuiCol_CheckMark));
        ImVec2 txt(tl.x + pad.x + uiW(22.0f), tl.y + pad.y);
        dl->AddText(txt, ImGui::GetColorU32(ImGuiCol_Text), kLayouts[i].name);
        ImVec2 blurbPos(txt.x + uiW(6.0f),
                        txt.y + ImGui::GetTextLineHeightWithSpacing());
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), blurbPos,
                    ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    kLayouts[i].blurb, nullptr, wrapW);
        ImGui::PopID();
        ImGui::Spacing();
    }


    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button(materializr::tr("Continue"), materializr::uiSz(110, 0))) {
        g_onPicker = false;
        g_step = 0;
    }
    const float skipW = uiW(80.0f);
    const float rightX = ImGui::GetWindowContentRegionMax().x - skipW;
    if (rightX > ImGui::GetCursorPosX()) ImGui::SameLine(rightX);
    else                                 ImGui::SameLine();
    if (ImGui::Button(materializr::tr("Skip"), materializr::uiSz(80, 0))) close = true;
}

void renderTutorial(materializr::PluginContext&) {
    using namespace materializr;

    // First overlay frame: auto-open on a fresh install, otherwise stay hidden.
    if (!g_firstRunChecked) {
        g_firstRunChecked = true;
        if (!tutorialSeen()) {
            g_open = true;
            g_onPicker = true;
            g_step = 0;
            // Only ASK when the user has never chosen. Someone who already set
            // a language (or reopens the tour later) goes straight to layout.
            g_onLanguage = (materializr::currentLanguageIndex() < 0);
        }
    }
    if (!g_open) return;

    g_step = std::clamp(g_step, 0, kStepCount - 1);
    const Step& s = kSteps[g_step];

    auto close = []() { g_open = false; markSeen(); };

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // Fixed (slightly wider) width, but height auto-fits the CURRENT step every
    // frame (the 0 axis) so a longer step grows the window instead of forcing a
    // scrollbar - the body and the Next/Skip row stay visible without scrolling.
    // Re-centered each frame so it grows symmetrically and never runs off the
    // bottom; both width and height are clamped to the available screen.
    const float w = std::min(uiW(520.0f), vp->WorkSize.x * 0.90f);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                        ImVec2(FLT_MAX, vp->WorkSize.y * 0.92f));

    bool keepOpen = true;
    if (ImGui::Begin("Getting Started###Tutorial", &keepOpen,
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoMove)) {
        if (g_onLanguage) {
            bool wantClose = false;
            renderLanguagePage(wantClose);
            if (wantClose) { ImGui::End(); close(); return; }
            ImGui::End();
            if (!keepOpen) close();
            return;
        }
        if (g_onPicker) {
            bool wantClose = false;
            renderLayoutPicker(wantClose);
            if (wantClose) { ImGui::End(); close(); return; }
            ImGui::End();
            if (!keepOpen) close();
            return;
        }

        ImGui::TextDisabled(materializr::tr("Step %d of %d"), g_step + 1, kStepCount);
        ImGui::SeparatorText(s.title);

        ImGui::PushTextWrapPos(0.0f); // wrap at the window's right edge
        ImGui::TextUnformatted(s.body);

        // Layout-specific location line: where to find this in the layout the
        // user picked on the first page. Highlighted - it's the line that makes
        // the step actionable on THIS screen.
        {
            const int li = std::clamp(currentUiLayoutIndex(), 0, 2);
            if (s.where[li]) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.55f, 0.90f, 0.70f, 1.0f));
                ImGui::TextWrapped(materializr::tr("In this layout:  %s"), s.where[li]);
                ImGui::PopStyleColor();
            }
        }

        // Input-specific guidance: highlight the model this run is using, but
        // show both so the tour reads the same on every platform.
        if (s.mouse || s.touch) {
            const bool touch = touchMode();
            auto guide = [](const char* tag, const char* text, bool active) {
                if (!text) return;
                const ImVec4 col = active ? ImVec4(0.55f, 0.80f, 1.00f, 1.0f)
                                          : ImVec4(0.60f, 0.60f, 0.64f, 1.0f);
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextWrapped("%s  %s", tag, text);
                ImGui::PopStyleColor();
            };
            // Active input first so the reader sees the relevant one immediately.
            if (touch) { guide("Touch:", s.touch, true);  guide("Mouse:", s.mouse, false); }
            else       { guide("Mouse:", s.mouse, true);  guide("Touch:", s.touch, false); }
        }
        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool last = (g_step == kStepCount - 1);
        if (ImGui::Button(materializr::tr("Back"), uiSz(80, 0))) {
            if (g_step > 0) g_step--;
            else            g_onPicker = true;   // back to the layout picker
        }
        ImGui::SameLine();
        if (ImGui::Button(last ? "Finish" : "Next", uiSz(80, 0))) {
            if (last) close();
            else      g_step++;
        }
        // Skip / Close pinned to the right edge.
        const float skipW = uiW(80.0f);
        const float rightX = ImGui::GetWindowContentRegionMax().x - skipW;
        if (rightX > ImGui::GetCursorPosX()) ImGui::SameLine(rightX);
        else                                 ImGui::SameLine();
        if (ImGui::Button(last ? "Close" : "Skip", uiSz(80, 0))) close();
    }
    ImGui::End();

    if (!keepOpen) close(); // window's [x]
}

} // namespace

REGISTER_PLUGIN(Tutorial, [](materializr::PluginContext& ctx) {
    // Launcher: Help > Getting Started (rendered via Application's generic
    // plugin-menu wiring). Reopening also starts at the layout picker - it
    // shows the current choice and is the friendliest place to change it.
    materializr::MenuContribution menu;
    menu.path = "Help > Getting Started";
    menu.priority = 10;
    menu.action = [](materializr::PluginContext&) {
        g_open = true;
        g_onPicker = true;
        g_step = 0;
    };
    ctx.registerMenuItem(std::move(menu));

    // The per-frame overlay that draws the panel.
    materializr::OverlayContribution overlay;
    overlay.name = "Tutorial";
    overlay.render = [](materializr::PluginContext& c) { renderTutorial(c); };
    ctx.registerOverlay(std::move(overlay));
})
