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

std::string fingerprint(const materializr::AppSettings::AiSettings& s) {
    return s.provider == materializr::AiProvider::Anthropic
               ? s.anthropicApiKey + "|" + s.anthropicModel
               : s.openAiApiKey + "|" + s.openAiBaseUrl + "|" + s.openAiModel;
}

AiSessionController& sessionFor(const materializr::AppSettings::AiSettings& ai) {
    const std::string fp = fingerprint(ai);
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

    if (!ImGui::Begin("AI Assistant")) { ImGui::End(); return; }

    const auto& ai = ctx.aiSettings();
    AiSessionController& session = sessionFor(ai);
    session.poll(ctx);

    ImGui::BeginChild("AiScrollback", ImVec2(0, -60), true);
    for (const auto& line : session.scrollback()) {
        using Kind = AiSessionController::ScrollbackLine::Kind;
        switch (line.kind) {
            case Kind::User:        ImGui::TextWrapped("You: %s", line.text.c_str()); break;
            case Kind::Assistant:   ImGui::TextWrapped("AI: %s", line.text.c_str()); break;
            case Kind::ToolSummary: ImGui::TextWrapped("%s", line.text.c_str()); break;
            case Kind::Error:
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::TextWrapped("%s", line.text.c_str());
                ImGui::PopStyleColor();
                break;
        }
    }
    if (session.isBusy()) ImGui::TextDisabled("Thinking...");
    ImGui::EndChild();

    if (!hasApiKeyConfigured(ai)) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                           "Set up your API key in Settings -> AI Assistant.");
    } else {
        const bool busy = session.isBusy();
        ImGui::BeginDisabled(busy);
        ImGui::InputText("##AiPrompt", inputBuf, sizeof(inputBuf));
        ImGui::SameLine();
        if (ImGui::Button("Send") && inputBuf[0] != '\0') {
            session.submitPrompt(inputBuf);
            inputBuf[0] = '\0';
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
}

} // namespace

REGISTER_PLUGIN(AiAssistant, [](materializr::PluginContext& ctx) {
    ctx.registerCommand({"AI Assistant", "", [](materializr::PluginContext&) {
        // The overlay renders unconditionally below; this command exists so
        // the feature is reachable from the menu/command surface even
        // though it needs no per-click action - matches every other
        // free-floating OverlayContribution's registerCommand-just-for-
        // discoverability pattern.
    }});
    ctx.registerOverlay({"AI Assistant", 100, renderOverlay});
});
