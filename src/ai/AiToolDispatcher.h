#pragma once
#include "AiTypes.h"

namespace materializr { class PluginContext; }

namespace materializr { namespace ai {

struct ToolResult {
    bool ok = false;
    // Doubles as the human-readable scrollback line AND the text fed back
    // to the LLM as this tool's result - see AiSessionController (Task 8).
    std::string message;
};

// Validates args first (no document mutation on any validation failure),
// then builds the matching concrete Operation and pushes it through
// ctx.history().pushOperation() - the exact same imperative shape every
// existing interactive op-commit already uses.
ToolResult executeTool(materializr::PluginContext& ctx, const std::string& toolName,
                       const nlohmann::json& args);

} } // namespace materializr::ai
