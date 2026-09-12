#pragma once
#include "LlmClient.h"
#include "AiToolSchema.h"

namespace materializr { namespace ai {

class AnthropicClient : public LlmClient {
public:
    AnthropicClient(std::string apiKey, std::string model)
        : m_apiKey(std::move(apiKey)), m_model(std::move(model)) {}

    LlmTurnResult sendTurn(const std::vector<ChatMessage>& messages,
                          const std::vector<ToolDef>& tools) override;

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
