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
    // Captured by value/raw pointer: m_client and &m_cancelRequested outlive
    // every turn they're used in - AiSessionController never rebuilds/
    // destroys itself while a turn is in flight (poll() always completes one
    // before the next submitPrompt can start another - see the isBusy()
    // guard above; the chat overlay's sessionFor() has the matching
    // "never rebuild while busy" rule on its side).
    m_cancelRequested = false;
    m_turnStartedAt = std::chrono::steady_clock::now();
    LlmClient* client = m_client.get();
    const std::atomic<bool>* cancelFlag = &m_cancelRequested;
    std::vector<ChatMessage> messagesCopy = m_messages;
    m_future = std::async(std::launch::async, [client, messagesCopy, cancelFlag]() {
        return client->sendTurn(messagesCopy, allTools(), cancelFlag);
    });
}

void AiSessionController::cancel() {
    if (isBusy()) m_cancelRequested = true;
}

double AiSessionController::elapsedSeconds() const {
    if (!isBusy()) return 0.0;
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_turnStartedAt).count();
}

void AiSessionController::clear() {
    if (isBusy()) return;
    m_messages.clear();
    m_scrollback.clear();
    m_stepCount = 0;
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
        // A deliberate cancel() isn't a failure - don't red-flag it as one.
        m_scrollback.push_back(
            result.error == "Cancelled"
                ? ScrollbackLine{ScrollbackLine::Kind::ToolSummary, "Cancelled."}
                : ScrollbackLine{ScrollbackLine::Kind::Error,
                                "AI request failed: " + result.error});
        return;
    }
    if (result.toolCalls.empty()) {
        // Anthropic can legitimately return an empty end_turn reply
        // (particularly right after a tool result). Pushing an empty-content
        // Assistant message into history poisons the NEXT submitPrompt():
        // the provider rejects an empty message with HTTP 400, breaking
        // every subsequent turn in the conversation, not just this one.
        if (!result.finalText.empty()) {
            m_scrollback.push_back({ScrollbackLine::Kind::Assistant, result.finalText});
            m_messages.push_back({ChatRole::Assistant, result.finalText, "", {}});
        } else {
            // An empty reply with no tool calls used to go here silently -
            // looked identical to the UI just doing nothing. A reasoning
            // model spending its whole token budget on hidden "thinking"
            // before ever producing a tool call or reply text lands here
            // with result.truncated set - tell the user that specifically
            // rather than leaving them staring at a chat box that went
            // quiet with zero explanation.
            m_scrollback.push_back({ScrollbackLine::Kind::Error,
                result.truncated
                    ? "The model ran out of its response budget while "
                      "thinking, before it replied or used a tool. Try a "
                      "simpler request, or break it into smaller steps."
                    : "The model gave an empty reply and made no tool "
                      "calls for this turn."});
        }
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
        m_messages.push_back({ChatRole::ToolResult, toolResult.message, call.id, {},
                              toolResult.imagePng});
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
