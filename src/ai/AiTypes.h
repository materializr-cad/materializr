#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace materializr { namespace ai {

enum class ChatRole { User, Assistant, ToolResult };

struct ChatMessage {
    ChatRole role;
    std::string text;       // user/assistant text, or the tool result string
    std::string toolCallId; // only meaningful when role == ToolResult -
                             // must match the ToolCall::id it answers
};

struct ToolCall {
    std::string id;
    std::string name;
    nlohmann::json args;
};

struct LlmTurnResult {
    bool ok = false;
    std::string error;           // set iff !ok
    std::string finalText;       // set iff ok and toolCalls is empty
    std::vector<ToolCall> toolCalls;
};

enum class ToolParamType { Number, String };

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
