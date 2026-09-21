#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../ai/AiSessionController.h"
#include "../ai/AnthropicClient.h"
#include "../ai/OpenAiCompatibleClient.h"
#include "../io/Settings.h"

#include <imgui.h>
#include <memory>

namespace {

using materializr::ai::AiSessionController;
using materializr::ai::AnthropicClient;
using materializr::ai::OpenAiCompatibleClient;
using materializr::ai::LlmClient;

// One conversation for the app's lifetime, matching the design's explicit
// choice that AI chat history is in-memory only and does not persist across
// a restart. Rebuilt whenever the provider/model/key changes so a mid-
// session Settings edit takes effect on the NEXT prompt rather than needing
// a relaunch.
std::unique_ptr<AiSessionController> g_session;
materializr::AiProvider g_sessionProvider;
std::string g_sessionKeyOrUrlFingerprint;
static bool g_overlayOpen = true;

std::string fingerprint(const materializr::AppSettings::AiSettings& s) {
    return s.provider == materializr::AiProvider::Anthropic
               ? s.anthropicApiKey + "|" + s.anthropicModel
               : s.openAiApiKey + "|" + s.openAiBaseUrl + "|" + s.openAiModel;
}

AiSessionController& sessionFor(const materializr::AppSettings::AiSettings& ai) {
    const std::string fp = fingerprint(ai);
    // Never rebuild while a turn is in flight: destroying g_session would
    // block on std::future's destructor (waiting for the async task, up to
    // the full curl timeout) on the render thread. Keep using the stale
    // session for the remainder of the in-flight turn; the rebuild happens
    // on the next call once the turn completes and settings still differ.
    if (g_session && g_session->isBusy()) return *g_session;
    if (!g_session || g_sessionProvider != ai.provider ||
        g_sessionKeyOrUrlFingerprint != fp) {
        std::unique_ptr<LlmClient> client;
        if (ai.provider == materializr::AiProvider::Anthropic)
            client = std::make_unique<AnthropicClient>(ai.anthropicApiKey, ai.anthropicModel);
        else
            client = std::make_unique<OpenAiCompatibleClient>(
                ai.openAiApiKey, ai.openAiBaseUrl, ai.openAiModel);
        g_session = std::make_unique<AiSessionController>(std::move(client));
        g_sessionProvider = ai.provider;
        g_sessionKeyOrUrlFingerprint = fp;
    }
    return *g_session;
}

bool hasApiKeyConfigured(const materializr::AppSettings::AiSettings& ai) {
    return ai.provider == materializr::AiProvider::Anthropic
               ? !ai.anthropicApiKey.empty()
               : true; // a local server (Ollama/LM Studio) often needs no key at all
}

void renderOverlay(materializr::PluginContext& ctx) {
    static char inputBuf[2048] = {};

    const auto& ai = ctx.aiSettings();
    AiSessionController& session = sessionFor(ai);
    // Poll unconditionally so an in-flight turn keeps advancing even while
    // the window is collapsed or closed - otherwise isBusy() never clears.
    session.poll(ctx);

    if (!g_overlayOpen) return;
    if (!ImGui::Begin("AI Assistant", &g_overlayOpen)) { ImGui::End(); return; }

    // Disabled while busy: clearing out from under an in-flight turn would
    // let that turn's result land right after the clear and silently
    // resurrect the old conversation - see AiSessionController::clear()'s
    // doc comment. Cancel first if a clear is wanted mid-turn.
    ImGui::BeginDisabled(session.isBusy());
    if (ImGui::SmallButton("Clear Chat")) session.clear();
    ImGui::EndDisabled();

    ImGui::BeginChild("AiScrollback", ImVec2(0, -60), true);
    for (const auto& line : session.scrollback()) {
        using Kind = AiSessionController::ScrollbackLine::Kind;
        switch (line.kind) {
            case Kind::User: {
                // A subtle tinted rect behind the sent line - the thing that
                // actually went to the model, worth making visually distinct
                // from its own replies/tool output at a glance, but low-alpha
                // so it stays a background cue, not another loud UI color.
                const std::string text = "You: " + line.text;
                const float wrapWidth = ImGui::GetContentRegionAvail().x;
                const ImVec2 textSize =
                    ImGui::CalcTextSize(text.c_str(), nullptr, false, wrapWidth);
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const ImVec2 padding(4.0f, 2.0f);
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(p0.x - padding.x, p0.y - padding.y),
                    ImVec2(p0.x + wrapWidth + padding.x, p0.y + textSize.y + padding.y),
                    ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f, 0.55f, 0.85f, 0.16f)),
                    3.0f);
                ImGui::TextWrapped("%s", text.c_str());
                break;
            }
            case Kind::Assistant:   ImGui::TextWrapped("AI: %s", line.text.c_str()); break;
            case Kind::ToolSummary: ImGui::TextWrapped("%s", line.text.c_str()); break;
            case Kind::Error:
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::TextWrapped("%s", line.text.c_str());
                ImGui::PopStyleColor();
                break;
        }
    }
    if (session.isBusy()) {
        // Elapsed time is the whole point: a static "Thinking..." label looks
        // identical whether the model is genuinely working (a slow local
        // model can legitimately take minutes) or the request silently
        // wedged - a ticking counter is visibly alive either way, and the
        // Stop button next to Send below means never having to wait out a
        // timeout to find out.
        ImGui::TextDisabled("Thinking... %.0fs", session.elapsedSeconds());
        // The actual point of streaming: show WHY it's taking a while (or
        // whether it's spiralling) instead of leaving the elapsed counter as
        // the only signal. Only OpenAiCompatibleClient streams today (see
        // LlmClient::sendTurn) - against Anthropic this is just always empty
        // and the section renders nothing, same as before streaming existed.
        std::string live = session.streamingText();
        if (!live.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            ImGui::TextWrapped("%s", live.c_str());
            ImGui::PopStyleColor();
            // Pin scroll to the bottom while it's still growing, the same way
            // a terminal follows fresh output - otherwise the user has to
            // keep manually re-scrolling down every time more text arrives.
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();

    if (!hasApiKeyConfigured(ai)) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                           "Set up your API key in Settings -> AI Assistant.");
    } else {
        const bool busy = session.isBusy();
        ImGui::BeginDisabled(busy);
        // EnterReturnsTrue changes InputText's return value from "the text
        // changed this frame" (true on every keystroke) to "Enter was
        // pressed" - exactly the submit signal a chat box wants, and it
        // keeps focus in the field afterward so the user can keep typing
        // without re-clicking.
        const bool enterPressed = ImGui::InputText("##AiPrompt", inputBuf, sizeof(inputBuf),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (busy) {
            // Stop takes over Send's own spot while a turn is in flight -
            // the standard chat-UI swap, so cancelling a stuck/slow turn
            // doesn't mean hunting for a button buried in the scrollback.
            if (ImGui::Button("Stop")) session.cancel();
        } else {
            const bool sendClicked = ImGui::Button("Send");
            if ((enterPressed || sendClicked) && inputBuf[0] != '\0') {
                session.submitPrompt(inputBuf);
                inputBuf[0] = '\0';
            }
        }
    }
    ImGui::End();
}

} // namespace

REGISTER_PLUGIN(AiAssistant, [](materializr::PluginContext& ctx) {
    // registerCommand/CommandContribution has no consumer anywhere in this
    // codebase (no menu or toolbar ever reads it), so closing the overlay left
    // no way to reopen it. registerToolbarButton has a real consumer
    // (src/ui/Toolbar.cpp / LayoutCommon.cpp).
    ctx.registerToolbarButton({"AI Assistant", "AI Assistant",
        materializr::SelectionContext::Always, 100,
        [](materializr::PluginContext&) { g_overlayOpen = !g_overlayOpen; },
        nullptr, "Open the AI Assistant chat."});
    ctx.registerOverlay({"AI Assistant", 100, renderOverlay});
});
