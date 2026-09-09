// Regression test for the "Recover Sketch?" modal rendering as an empty,
// permanently-stuck 28x45 window that still blocks all input (issue: launch
// with a sketch draft + an autosave present).
//
// Root cause: Application::renderSketchRecoveryPrompt() / renderProjectRecoveryPrompt()
// call ImGui::OpenPopup() UNCONDITIONALLY every frame (src/app/Application.cpp,
// renderSketchRecoveryPrompt ~5344, renderProjectRecoveryPrompt ~5470) - a
// pattern the codebase's own comment there already recognized as dangerous:
// "opening a second popup at the same stack level ... makes the two close
// each other every frame ... neither ever draws" - and gates against it
// w.r.t. the Welcome screen and each other. Application_Dialogs.cpp's
// renderUpdatePopup() (~line 662) ALSO calls OpenPopup() unconditionally
// every frame while m_showUpdatePopup is true, but had NO gate against the
// recovery prompts. m_showUpdatePopup flips true asynchronously (the launch
// update check runs on a background thread and can resolve at any frame,
// including while a recovery prompt is up), so the two raw-every-frame
// OpenPopup calls contend for the same popup-stack level (0) and steal it
// from each other every single frame - via dear imgui's
// "OpenPopupEx / re-open" path (imgui.cpp), which treats every such steal as
// a fresh popup activation, resetting Size/ContentSize to 0 and hiding the
// window for exactly one frame (HiddenFramesCannotSkipItems=1) EVERY frame,
// forever. The window is never NOT hidden long enough to compute a real
// size, so it's permanently drawn nowhere at a degenerate floor size, while
// still occupying the modal popup stack (blocking all other input) - exactly
// the reported symptom.
//
// This test exercises the real dear imgui popup-stack mechanics headlessly
// (CreateContext / NewFrame / Begin / End, no window/GL backend) against two
// harness functions written to mirror the production call sites: `popupA()`
// (renderUpdatePopup) and `popupB()` (renderSketchRecoveryPrompt). It proves
// (1) the anti-pattern - no gate - deadlocks both popups permanently hidden
// at a degenerate size, and (2) the fix - popupA gates on popupB's pending
// state, exactly like Application_Dialogs.cpp now does - lets popupB render
// and stabilize at a real size, and popupA opens cleanly once popupB clears.

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>

#include <gtest/gtest.h>

namespace {

// A fresh ImGui context per test so the two TESTs below don't share popup
// stack / window state.
class ImGuiHeadlessTest : public ::testing::Test {
protected:
    void SetUp() override {
        IMGUI_CHECKVERSION();
        ctx_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx_);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1280, 800);
        io.IniFilename = nullptr; // no disk I/O from this test
        unsigned char* pixels; int w, h;
        io.Fonts->GetTexDataAsAlpha8(&pixels, &w, &h); // builds the default font
        io.Fonts->SetTexID((ImTextureID)(intptr_t)1);
    }
    void TearDown() override {
        ImGui::DestroyContext(ctx_);
        ctx_ = nullptr;
    }
    void NewFrame() {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
    }
    void EndFrame() { ImGui::Render(); }

    ImGuiContext* ctx_ = nullptr;
};

// Mirrors Application_Dialogs.cpp::renderUpdatePopup(): a modal that opens
// unconditionally, every frame, while `active` is true.
void popupA_updatePopup(bool active, bool gateOnB, bool bPending) {
    if (!active) return;
    if (gateOnB && bPending) return; // the fix
    ImGui::OpenPopup("Check for Updates");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 220), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Check for Updates", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::Text("Current version: 0.0.0");
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// Mirrors Application.cpp::renderSketchRecoveryPrompt(): a modal that opens
// unconditionally, every frame, while `*pending` is true.
bool popupB_sketchRecovery(bool* pending) {
    if (!*pending) return false;
    ImGui::OpenPopup("Recover Sketch?");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    bool contentRan = false;
    if (ImGui::BeginPopupModal("Recover Sketch?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoSavedSettings)) {
        contentRan = true;
        ImGui::TextUnformatted("An unfinished sketch from your last session was found.");
        ImGui::TextDisabled("It wasn't committed before the app closed.");
        if (ImGui::Button("Restore it")) { *pending = false; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) { *pending = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    return contentRan;
}

} // namespace

// ---------------------------------------------------------------------------
// (1) The anti-pattern: popupA has NO gate against popupB. Once popupA
// activates mid-flight (simulating the async update check resolving while
// the sketch-recovery prompt is up), the two raw-every-frame OpenPopup calls
// steal the popup-stack level from each other every frame. Assert this
// really does deadlock - both windows permanently Hidden at a degenerate
// size - so the mechanism behind the reported bug is pinned, not assumed.
// ---------------------------------------------------------------------------
TEST_F(ImGuiHeadlessTest, UngatedCompetingPopupsDeadlockHiddenForever) {
    bool sketchPending = true;
    bool updateActive = false;

    // Frames 1-2: only popupB is active - it opens and stabilizes normally.
    for (int frame = 1; frame <= 2; ++frame) {
        NewFrame();
        popupA_updatePopup(updateActive, /*gateOnB=*/false, sketchPending);
        popupB_sketchRecovery(&sketchPending);
        EndFrame();
    }
    {
        ImGuiWindow* b = ImGui::FindWindowByName("Recover Sketch?");
        ASSERT_NE(b, nullptr);
        EXPECT_FALSE(b->Hidden);
        EXPECT_GT(b->Size.x, 100.0f) << "sketch prompt should have a real, laid-out width";
    }

    // Frame 3: the async update check resolves - popupA activates. From here
    // on both call OpenPopup() every frame with no turn-taking.
    updateActive = true;
    for (int frame = 3; frame <= 15; ++frame) {
        NewFrame();
        popupA_updatePopup(updateActive, /*gateOnB=*/false, sketchPending);
        popupB_sketchRecovery(&sketchPending);
        EndFrame();
    }

    ImGuiWindow* a = ImGui::FindWindowByName("Check for Updates");
    ImGuiWindow* b = ImGui::FindWindowByName("Recover Sketch?");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    // This is the reported bug, reproduced: both windows are permanently
    // Hidden (never actually drawn/rendered) and pinned at the degenerate
    // "just reset, nothing measured yet" floor size - not the real ~478x92
    // content size - even though m_pendingSketchRecovery is STILL true (the
    // user can never see or click the buttons that would clear it).
    EXPECT_TRUE(sketchPending) << "prompt can never be dismissed - its buttons never render";
    EXPECT_TRUE(a->Hidden);
    EXPECT_TRUE(b->Hidden);
    EXPECT_LT(b->Size.x, 50.0f) << "stuck at the degenerate floor size, never the real content size";
}

// ---------------------------------------------------------------------------
// (2) The fix: popupA gates on popupB's pending flag, mirroring the turn-
// taking gate added to Application_Dialogs.cpp::renderUpdatePopup(). Assert
// popupB renders and stays stable regardless of when popupA's trigger fires,
// and popupA itself opens cleanly and stabilizes once popupB clears.
// ---------------------------------------------------------------------------
TEST_F(ImGuiHeadlessTest, GatedUpdatePopupLetsSketchRecoveryRenderAndStabilize) {
    bool sketchPending = true;
    bool updateActive = false;

    for (int frame = 1; frame <= 2; ++frame) {
        NewFrame();
        popupA_updatePopup(updateActive, /*gateOnB=*/true, sketchPending);
        popupB_sketchRecovery(&sketchPending);
        EndFrame();
    }

    // The async update check "resolves" mid-flight, same as the deadlock test.
    updateActive = true;
    float stableWidth = -1.0f;
    for (int frame = 3; frame <= 8; ++frame) {
        NewFrame();
        popupA_updatePopup(updateActive, /*gateOnB=*/true, sketchPending);
        popupB_sketchRecovery(&sketchPending);
        EndFrame();

        ImGuiWindow* b = ImGui::FindWindowByName("Recover Sketch?");
        ASSERT_NE(b, nullptr);
        EXPECT_FALSE(b->Hidden) << "frame " << frame;
        if (frame == 4) stableWidth = b->Size.x; // one settle frame after the trigger
    }
    ASSERT_GT(stableWidth, 100.0f);
    // "Check for Updates" must not exist yet - it's still gated off.
    EXPECT_EQ(ImGui::FindWindowByName("Check for Updates"), nullptr);

    // User dismisses the sketch prompt (mirrors clicking "Discard", which
    // this harness can't do via a synthetic mouse click - set the flag the
    // button's handler would have set instead).
    sketchPending = false;

    // Now popupA should open cleanly (one hidden settle frame) and stabilize.
    for (int frame = 1; frame <= 3; ++frame) {
        NewFrame();
        popupA_updatePopup(updateActive, /*gateOnB=*/true, sketchPending);
        popupB_sketchRecovery(&sketchPending);
        EndFrame();
    }
    ImGuiWindow* a = ImGui::FindWindowByName("Check for Updates");
    ASSERT_NE(a, nullptr);
    EXPECT_FALSE(a->Hidden);
    EXPECT_FLOAT_EQ(a->Size.x, 400.0f);
    EXPECT_FLOAT_EQ(a->Size.y, 220.0f);
}
