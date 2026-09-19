#pragma once
#include "AiTypes.h"

#include <atomic>
#include <functional>
#include <string>

namespace materializr { namespace ai {

// Fired (possibly many times) DURING sendTurn, from the SAME background
// thread sendTurn itself runs on, as the model streams its hidden reasoning
// and/or visible reply - lets the UI show live progress instead of a static
// "Thinking..." placeholder with no way to tell a genuinely slow model from
// a wedged one. May be an empty std::function - not every implementation
// streams (AnthropicClient currently ignores it entirely). The callback
// itself must be safe to call from a background thread; it must not touch
// Document/History or any ImGui state directly (see AiSessionController,
// which only ever appends the delta text to a mutex-guarded buffer here and
// reads that buffer back on the main thread in poll()).
using StreamDeltaCallback = std::function<void(const std::string& deltaText)>;

class LlmClient {
public:
    virtual ~LlmClient() = default;
    // Blocking. Called ONLY from a background worker thread (see
    // AiSessionController, Task 8) - never from the main/render thread.
    // `cancelFlag` is polled periodically DURING the request (via curl's
    // progress callback) from that same background thread; the CALLER sets
    // it to true from the main thread to abort an in-flight request. May be
    // null (no cancellation support) - real callers always pass one. Must
    // stay valid for the whole call, which AiSessionController guarantees
    // the same way it already guarantees `this` outlives the call: it never
    // rebuilds/destroys itself while a turn is in flight.
    virtual LlmTurnResult sendTurn(const std::vector<ChatMessage>& messages,
                                   const std::vector<ToolDef>& tools,
                                   const std::atomic<bool>* cancelFlag,
                                   const StreamDeltaCallback& onDelta = {}) = 0;
};

} } // namespace materializr::ai
