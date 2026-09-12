#pragma once
#include "AiTypes.h"
#include "LlmClient.h"

#include <future>
#include <memory>

namespace materializr { class PluginContext; }

namespace materializr { namespace ai {

// Drives one AI conversation: submitPrompt() starts a turn, poll() (called
// once per frame from the chat overlay's OverlayContribution::render, the
// same way UpdateChecker's result is polled from Application's main loop)
// advances it. Owns the std::future for the in-flight network call - all
// Document/History mutation happens inside poll() on the caller's thread
// (the main thread, in the real app), never inside the async lambda itself.
class AiSessionController {
public:
    struct ScrollbackLine {
        enum class Kind { User, Assistant, ToolSummary, Error };
        Kind kind;
        std::string text;
    };

    explicit AiSessionController(std::unique_ptr<LlmClient> client);

    void submitPrompt(const std::string& userText);
    // Call once per frame. No-op if nothing is in flight.
    void poll(materializr::PluginContext& ctx);
    bool isBusy() const { return m_future.valid(); }
    const std::vector<ScrollbackLine>& scrollback() const { return m_scrollback; }

private:
    void startTurn();

    std::unique_ptr<LlmClient> m_client;
    std::vector<ChatMessage> m_messages;
    std::vector<ScrollbackLine> m_scrollback;
    std::future<LlmTurnResult> m_future;
    int m_stepCount = 0;
    static constexpr int kMaxStepsPerPrompt = 8;
};

} } // namespace materializr::ai
