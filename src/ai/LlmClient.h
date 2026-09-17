#pragma once
#include "AiTypes.h"

#include <atomic>

namespace materializr { namespace ai {

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
                                   const std::atomic<bool>* cancelFlag) = 0;
};

} } // namespace materializr::ai
