#pragma once
#include "AiTypes.h"

namespace materializr { namespace ai {

class LlmClient {
public:
    virtual ~LlmClient() = default;
    // Blocking. Called ONLY from a background worker thread (see
    // AiSessionController, Task 8) - never from the main/render thread.
    virtual LlmTurnResult sendTurn(const std::vector<ChatMessage>& messages,
                                   const std::vector<ToolDef>& tools) = 0;
};

} } // namespace materializr::ai
