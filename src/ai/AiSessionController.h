#pragma once
#include "AiTypes.h"
#include "LlmClient.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>

namespace materializr { class PluginContext; }

namespace materializr { namespace ai {

// KNOWN LIMITATION: AiSessionController is bound to whatever Document/History
// the PluginContext passed to poll() currently points at. If the active
// project changes while a turn is in flight, an in-flight tool call executes
// against the NEW active project, not the one the conversation started in.
// Not fixed in v1 - would need conversations bound to a stable project identity.
//
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

    // Ask the in-flight network request (if any) to stop. Cooperative, not
    // instant: the background thread notices via curl's progress callback
    // (see AnthropicClient/OpenAiCompatibleClient), which fires roughly once
    // a second, so isBusy() clears within about a second of calling this,
    // not immediately. A no-op if nothing is in flight.
    void cancel();
    // Seconds since the CURRENT network round-trip started (resets every
    // startTurn(), including each step of a multi-tool-call turn) - lets the
    // UI show "Thinking... 47s" instead of a static label, so a slow-but-
    // alive local model is visibly distinct from a genuinely frozen UI. 0
    // when not busy.
    double elapsedSeconds() const;

    // Resets the conversation to empty - history, scrollback, and step
    // count. A no-op while busy (isBusy()): clearing out from under an
    // in-flight turn would let that turn's result land in poll() right
    // after the clear and silently resurrect the old context. cancel()
    // first, wait for isBusy() to clear, then call this.
    void clear();

private:
    void startTurn();

    std::unique_ptr<LlmClient> m_client;
    std::vector<ChatMessage> m_messages;
    std::vector<ScrollbackLine> m_scrollback;
    std::future<LlmTurnResult> m_future;
    int m_stepCount = 0;
    static constexpr int kMaxStepsPerPrompt = 8;
    // Raw pointer captured by the async lambda in startTurn(), same lifetime
    // contract as LlmClient's own cancelFlag doc comment: this controller
    // never rebuilds/destroys itself while a turn is in flight, so the
    // address stays valid for the whole call.
    std::atomic<bool> m_cancelRequested{false};
    std::chrono::steady_clock::time_point m_turnStartedAt;
};

} } // namespace materializr::ai
