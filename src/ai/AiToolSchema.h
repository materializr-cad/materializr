#pragma once
#include "AiTypes.h"

namespace materializr { namespace ai {

// The fixed v1 tool list: primitives plus basic transforms and booleans.
// One definition, shared by both LLM clients - each formats it into its own
// provider's wire shape (toolsToAnthropicJson / toolsToOpenAiJson below).
const std::vector<ToolDef>& allTools();

// Prepended to every request (AnthropicClient as the top-level "system"
// field, OpenAiCompatibleClient as a leading system-role message - see each
// buildRequestBody). Exists mainly to make the X/Y/Z convention impossible
// to miss: it's mentioned in individual tool param descriptions too, but
// that's easy to skim past, and Y-up is a common enough convention
// elsewhere (Unity, Maya, OpenGL, ...) that a model can default back to it
// out of habit unless told plainly, up front, every turn.
const std::string& systemPrompt();

nlohmann::json toolsToAnthropicJson(const std::vector<ToolDef>& tools);
nlohmann::json toolsToOpenAiJson(const std::vector<ToolDef>& tools);

} } // namespace materializr::ai
