#pragma once
#include "LlmClient.h"
#include "AiToolSchema.h"

namespace materializr { namespace ai {

class OpenAiCompatibleClient : public LlmClient {
public:
    OpenAiCompatibleClient(std::string apiKey, std::string baseUrl, std::string model)
        : m_apiKey(std::move(apiKey)), m_baseUrl(std::move(baseUrl)),
          m_model(std::move(model)) {}

    LlmTurnResult sendTurn(const std::vector<ChatMessage>& messages,
                          const std::vector<ToolDef>& tools) override;

    static nlohmann::json buildRequestBody(const std::vector<ChatMessage>& messages,
                                           const std::vector<ToolDef>& tools,
                                           const std::string& model);
    static LlmTurnResult parseResponse(const nlohmann::json& body, long httpStatus);
    static LlmTurnResult parseResponseFromRawBody(const std::string& rawBody,
                                                  long httpStatus);

private:
    std::string m_apiKey;
    std::string m_baseUrl;
    std::string m_model;
};

} } // namespace materializr::ai
