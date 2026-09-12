#include "AiSessionController.h"
#include "AiToolDispatcher.h"
#include "AiToolSchema.h"
#include "../plugin/PluginContext.h"

namespace materializr { namespace ai {

AiSessionController::AiSessionController(std::unique_ptr<LlmClient> client)
    : m_client(std::move(client)) {}

void AiSessionController::submitPrompt(const std::string& userText) {
    if (isBusy()) return; // one turn in flight at a time
    m_messages.push_back({ChatRole::User, userText, ""});
    m_scrollback.push_back({ScrollbackLine::Kind::User, userText});
    m_stepCount = 0;
    startTurn();
}

void AiSessionController::startTurn() {
    // Captured by value: m_client is a pointer the lambda doesn't own the
    // lifetime of, but AiSessionController outlives every turn it starts
    // (poll() always completes a turn before the next submitPrompt can
    // start another - see the isBusy() guard above).
    LlmClient* client = m_client.get();
    std::vector<ChatMessage> messagesCopy = m_messages;
    m_future = std::async(std::launch::async, [client, messagesCopy]() {
        return client->sendTurn(messagesCopy, allTools());
    });
}

void AiSessionController::poll(materializr::PluginContext& ctx) {
    if (!m_future.valid()) return;
    if (m_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;

    LlmTurnResult result = m_future.get(); // future becomes invalid; isBusy() -> false
                                            // again unless we start a new one below

    if (!result.ok) {
        m_scrollback.push_back({ScrollbackLine::Kind::Error,
                                "AI request failed: " + result.error});
        return;
    }
    if (result.toolCalls.empty()) {
        m_scrollback.push_back({ScrollbackLine::Kind::Assistant, result.finalText});
        return;
    }

    for (const auto& call : result.toolCalls) {
        ToolResult toolResult = executeTool(ctx, call.name, call.args);
        m_scrollback.push_back({toolResult.ok ? ScrollbackLine::Kind::ToolSummary
                                              : ScrollbackLine::Kind::Error,
                                "-> " + toolResult.message});
        m_messages.push_back({ChatRole::ToolResult, toolResult.message, call.id});
    }
    ++m_stepCount;
    if (m_stepCount >= kMaxStepsPerPrompt) {
        m_scrollback.push_back({ScrollbackLine::Kind::Error,
                                "Stopped after " + std::to_string(kMaxStepsPerPrompt) +
                                " steps."});
        return;
    }
    startTurn();
}

} } // namespace materializr::ai
