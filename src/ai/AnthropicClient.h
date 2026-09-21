#pragma once
#include "LlmClient.h"
#include "AiToolSchema.h"

namespace materializr { namespace ai {

class AnthropicClient : public LlmClient {
public:
    AnthropicClient(std::string apiKey, std::string model)
        : m_apiKey(std::move(apiKey)), m_model(std::move(model)) {}

    // Does not stream yet - onDelta is accepted (to satisfy LlmClient) but
    // never called. See OpenAiCompatibleClient for the streaming path.
    LlmTurnResult sendTurn(const std::vector<ChatMessage>& messages,
                          const std::vector<ToolDef>& tools,
                          const std::atomic<bool>* cancelFlag,
                          const StreamDeltaCallback& onDelta = {}) override;

    // Pure, network-free - directly unit-testable.
    static nlohmann::json buildRequestBody(const std::vector<ChatMessage>& messages,
                                           const std::vector<ToolDef>& tools,
                                           const std::string& model);
    static LlmTurnResult parseResponse(const nlohmann::json& body, long httpStatus);
    // Convenience for a raw response string that might not even be valid
    // JSON (a proxy error page, a truncated connection, ...).
    static LlmTurnResult parseResponseFromRawBody(const std::string& rawBody,
                                                  long httpStatus);

private:
    std::string m_apiKey;
    std::string m_model;
};

} } // namespace materializr::ai
