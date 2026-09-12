#pragma once
#include "AiTypes.h"

namespace materializr { namespace ai {

// The fixed v1 tool list: primitives plus basic transforms and booleans.
// One definition, shared by both LLM clients - each formats it into its own
// provider's wire shape (toolsToAnthropicJson / toolsToOpenAiJson below).
const std::vector<ToolDef>& allTools();

nlohmann::json toolsToAnthropicJson(const std::vector<ToolDef>& tools);
nlohmann::json toolsToOpenAiJson(const std::vector<ToolDef>& tools);

} } // namespace materializr::ai
