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

    LlmTurnResult result;
    try {
        result = m_future.get(); // future becomes invalid; isBusy() -> false
                                  // again unless we start a new one below
    } catch (const std::exception& e) {
        result.ok = false;
        result.error = e.what();
    }

    if (!result.ok) {
        m_scrollback.push_back({ScrollbackLine::Kind::Error,
                                "AI request failed: " + result.error});
        return;
    }
    if (result.toolCalls.empty()) {
        m_scrollback.push_back({ScrollbackLine::Kind::Assistant, result.finalText});
        m_messages.push_back({ChatRole::Assistant, result.finalText, "", {}});
        return;
    }

    if (!result.finalText.empty()) {
        // Surface commentary the model sent alongside tool calls before the
        // tool-execution lines, so the user sees the model's stated intent.
        // Not added to m_messages: the replay format for an Assistant-with-
        // toolCalls turn only carries the tool_use/tool_calls blocks.
        m_scrollback.push_back({ScrollbackLine::Kind::Assistant, result.finalText});
    }

    m_messages.push_back({ChatRole::Assistant, "", "", result.toolCalls});
    for (size_t i = 0; i < result.toolCalls.size(); ++i) {
        const auto& call = result.toolCalls[i];
        if (m_stepCount >= kMaxStepsPerPrompt) {
            m_scrollback.push_back({ScrollbackLine::Kind::Error,
                                    "Stopped after " + std::to_string(kMaxStepsPerPrompt) +
                                    " steps."});
            // Every tool_use id in the assistant message above must have a
            // matching result or the next submitPrompt() sends an unbalanced
            // history and the provider rejects it with HTTP 400.
            for (size_t j = i; j < result.toolCalls.size(); ++j) {
                m_messages.push_back({ChatRole::ToolResult,
                                      "not executed: step limit reached",
                                      result.toolCalls[j].id, {}});
            }
            return; // do not start another turn, and don't run remaining calls
        }
        ToolResult toolResult;
        try {
            toolResult = executeTool(ctx, call.name, call.args);
        } catch (const std::exception& e) {
            toolResult = {false, std::string("internal error: ") + e.what()};
        }
        m_scrollback.push_back({toolResult.ok ? ScrollbackLine::Kind::ToolSummary
                                              : ScrollbackLine::Kind::Error,
                                "-> " + toolResult.message});
        m_messages.push_back({ChatRole::ToolResult, toolResult.message, call.id, {}});
        ++m_stepCount;
    }
    if (m_stepCount >= kMaxStepsPerPrompt) {
        m_scrollback.push_back({ScrollbackLine::Kind::Error,
                                "Stopped after " + std::to_string(kMaxStepsPerPrompt) +
                                " steps."});
        return;
    }
    startTurn();
}

} } // namespace materializr::ai
