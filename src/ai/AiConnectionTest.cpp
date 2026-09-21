#include "AiConnectionTest.h"
#include "AnthropicClient.h"
#include "OpenAiCompatibleClient.h"

#include <memory>

namespace materializr { namespace ai {

std::future<LlmTurnResult> testConnection(const AppSettings::AiSettings& ai) {
    std::unique_ptr<LlmClient> client;
    if (ai.provider == AiProvider::Anthropic)
        client = std::make_unique<AnthropicClient>(ai.anthropicApiKey, ai.anthropicModel);
    else
        client = std::make_unique<OpenAiCompatibleClient>(
            ai.openAiApiKey, ai.openAiBaseUrl, ai.openAiModel);
    return std::async(std::launch::async,
        [c = std::shared_ptr<LlmClient>(std::move(client))]() {
            std::vector<ChatMessage> msgs = {{ChatRole::User, "Reply with OK.", ""}};
            // No cancel UI for the one-shot connection test - it's a single
            // small request the safety-net CURLOPT_TIMEOUT already bounds.
            return c->sendTurn(msgs, {}, nullptr);
        });
}

} } // namespace materializr::ai
