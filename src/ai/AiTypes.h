#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace materializr { namespace ai {

enum class ChatRole { User, Assistant, ToolResult };

struct ToolCall {
    std::string id;
    std::string name;
    nlohmann::json args;
};

struct ChatMessage {
    ChatRole role;
    std::string text;                // user/assistant text, or the tool result string
    std::string toolCallId;          // only meaningful when role == ToolResult -
                                      // must match the ToolCall::id it answers
    std::vector<ToolCall> toolCalls; // only meaningful when role == Assistant; empty
                                      // for a plain text reply, non-empty for a
                                      // tool-calling turn
    // PNG bytes attached to a ToolResult message (e.g. a future screenshot
    // tool). Empty for every other role and for text-only tool results.
    // AnthropicClient/OpenAiCompatibleClient each decide how to shape this
    // into their own wire format - see buildRequestBody in both.
    std::vector<uint8_t> imagePng;
};

struct LlmTurnResult {
    bool ok = false;
    std::string error;           // set iff !ok
    std::string finalText;       // set iff ok and toolCalls is empty
    std::vector<ToolCall> toolCalls;
    // True iff the provider stopped the turn for hitting its output-token cap
    // (OpenAI-compatible finish_reason "length" / Anthropic stop_reason
    // "max_tokens") rather than finishing normally. A reasoning model can
    // spend its ENTIRE budget on hidden "thinking" before ever emitting a
    // tool call or reply text, landing here with an empty finalText and no
    // toolCalls - lets the caller tell that apart from a legitimate empty
    // turn and say so instead of going silent.
    bool truncated = false;
};

enum class ToolParamType { Number, String, Boolean, IntegerArray };

struct ToolParam {
    std::string name;
    ToolParamType type;
    bool required = true;
    std::string description;
};

struct ToolDef {
    std::string name;
    std::string description;
    std::vector<ToolParam> params;
};

} } // namespace materializr::ai
