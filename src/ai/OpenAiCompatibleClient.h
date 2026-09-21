#pragma once
#include "LlmClient.h"
#include "AiToolSchema.h"

#include <map>

namespace materializr { namespace ai {

// Accumulates one streamed tool_calls[i] across however many SSE chunks its
// id/name/arguments arrive fragmented over - a compat server splits a single
// tool call's "arguments" string across many deltas, keyed by its position
// ("index") in the tool_calls array, not by id (id may only appear once, in
// the first fragment).
struct StreamToolCallAccumulator {
    std::string id;
    std::string name;
    std::string arguments;
};

// Everything a streamed response has built up so far. Deliberately doesn't
// keep "reasoning" text around once emitted - unlike content/tool_calls, it
// was never part of the final structured LlmTurnResult even before
// streaming existed, only ever useful live (via onDelta) for the UI to show.
struct StreamAccumulator {
    std::string content;
    std::string finishReason;
    std::map<int, StreamToolCallAccumulator> toolCallsByIndex;
};

class OpenAiCompatibleClient : public LlmClient {
public:
    OpenAiCompatibleClient(std::string apiKey, std::string baseUrl, std::string model)
        : m_apiKey(std::move(apiKey)), m_baseUrl(std::move(baseUrl)),
          m_model(std::move(model)) {}

    LlmTurnResult sendTurn(const std::vector<ChatMessage>& messages,
                          const std::vector<ToolDef>& tools,
                          const std::atomic<bool>* cancelFlag,
                          const StreamDeltaCallback& onDelta = {}) override;

    static nlohmann::json buildRequestBody(const std::vector<ChatMessage>& messages,
                                           const std::vector<ToolDef>& tools,
                                           const std::string& model);
    static LlmTurnResult parseResponse(const nlohmann::json& body, long httpStatus);
    static LlmTurnResult parseResponseFromRawBody(const std::string& rawBody,
                                                  long httpStatus);

    // Feeds one already-unwrapped SSE data payload (the JSON after "data: ",
    // with "[DONE]" already filtered out by the caller) into `acc`, firing
    // onDelta for any new reasoning/content text. Shared by the real
    // incremental curl callback (one line at a time, as bytes arrive) and by
    // parseSseStream below (a whole response at once) - same logic either way.
    static void applySseChunk(const nlohmann::json& chunk, StreamAccumulator& acc,
                              const StreamDeltaCallback& onDelta);
    // Rebuilds the plain (non-streamed) response shape parseResponse already
    // understands, from whatever the accumulator has collected so far.
    static nlohmann::json accumulatorToResponseJson(const StreamAccumulator& acc);
    // Testable, non-network entry point: parses a COMPLETE SSE response body
    // (every "data: ..." line already received) into the same LlmTurnResult
    // a real streamed request produces. The real sendTurn doesn't call this -
    // it parses incrementally as bytes arrive, for genuinely live deltas -
    // but both paths go through applySseChunk/accumulatorToResponseJson, so
    // this exercises the real parsing logic.
    static LlmTurnResult parseSseStream(const std::string& sseBody, long httpStatus,
                                        const StreamDeltaCallback& onDelta = {});

private:
    std::string m_apiKey;
    std::string m_baseUrl;
    std::string m_model;
};

} } // namespace materializr::ai
