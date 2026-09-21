#include "ai/OpenAiCompatibleClient.h"

#include <gtest/gtest.h>

using namespace materializr::ai;

TEST(OpenAiCompatibleClient, BuildRequestBodyIncludesModelMessagesAndTools) {
    std::vector<ChatMessage> messages = {{ChatRole::User, "make a box", "", {}}};
    nlohmann::json body = OpenAiCompatibleClient::buildRequestBody(
        messages, allTools(), "gpt-4o");

    EXPECT_EQ(body["model"], "gpt-4o");
    // messages[0] is the leading system prompt (see the dedicated test
    // below) - the actual conversation starts at index 1.
    ASSERT_EQ(body["messages"].size(), 2u);
    EXPECT_EQ(body["messages"][1]["role"], "user");
    ASSERT_TRUE(body["tools"].is_array());
    EXPECT_EQ(body["tools"].size(), allTools().size());
}

TEST(OpenAiCompatibleClient, BuildRequestBodyLeadsWithTheSystemPromptAndStatesTheAxisConvention) {
    // No separate top-level "system" field in this wire shape, unlike
    // Anthropic's - it has to be the first message. Steve reported the
    // model sometimes treats Y as up (a common convention elsewhere - Unity,
    // Maya, OpenGL) and misses edges/faces as a result - this is the fix.
    std::vector<ChatMessage> messages = {{ChatRole::User, "hi", "", {}}};
    nlohmann::json body = OpenAiCompatibleClient::buildRequestBody(messages, {}, "gpt-4o");
    ASSERT_FALSE(body["messages"].empty());
    EXPECT_EQ(body["messages"][0]["role"], "system");
    std::string prompt = body["messages"][0]["content"].get<std::string>();
    EXPECT_NE(prompt.find("Z = up"), std::string::npos) << prompt;
    EXPECT_NE(prompt.find("list_bodies"), std::string::npos) << prompt;
}

TEST(OpenAiCompatibleClient, BuildRequestBodySetsAMaxTokensCap) {
    // A local model with no stop condition can otherwise ramble toward its
    // full context window instead of failing fast - see the 2026-09-17
    // comment in buildRequestBody.
    std::vector<ChatMessage> messages = {{ChatRole::User, "make a box", "", {}}};
    nlohmann::json body = OpenAiCompatibleClient::buildRequestBody(messages, {}, "gpt-4o");
    ASSERT_TRUE(body.contains("max_tokens"));
    EXPECT_GT(body["max_tokens"].get<int>(), 0);
}

TEST(OpenAiCompatibleClient, BuildRequestBodyCapsHiddenReasoningTokensSeparatelyFromMaxTokens) {
    // A reasoning model can spend the WHOLE max_tokens budget on hidden
    // thinking and never reach a tool call (see the 2026-09-18 comment in
    // buildRequestBody) - OpenRouter's "reasoning.max_tokens" bounds that
    // specifically. Confirmed real Ollama servers ignore this field rather
    // than rejecting the request, so it is safe to send unconditionally.
    std::vector<ChatMessage> messages = {{ChatRole::User, "make a box", "", {}}};
    nlohmann::json body = OpenAiCompatibleClient::buildRequestBody(messages, {}, "gpt-4o");
    ASSERT_TRUE(body.contains("reasoning"));
    ASSERT_TRUE(body["reasoning"].contains("max_tokens"));
    EXPECT_GT(body["reasoning"]["max_tokens"].get<int>(), 0);
    EXPECT_LT(body["reasoning"]["max_tokens"].get<int>(), body["max_tokens"].get<int>())
        << "the reasoning cap must leave real room in max_tokens for the actual reply/tool call";
}

TEST(OpenAiCompatibleClient, BuildRequestBodyRequestsStreaming) {
    std::vector<ChatMessage> messages = {{ChatRole::User, "make a box", "", {}}};
    nlohmann::json body = OpenAiCompatibleClient::buildRequestBody(messages, {}, "gpt-4o");
    ASSERT_TRUE(body.contains("stream"));
    EXPECT_TRUE(body["stream"].get<bool>());
}

TEST(OpenAiCompatibleClient, ParseSseStreamAccumulatesContentAcrossChunksAndFiresOnDelta) {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n"
        "\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n"
        "\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\", world!\"}}]}\n"
        "\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n"
        "\n"
        "data: [DONE]\n";
    std::vector<std::string> deltas;
    LlmTurnResult r = OpenAiCompatibleClient::parseSseStream(
        sse, 200, [&](const std::string& d) { deltas.push_back(d); });
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.finalText, "Hello, world!");
    EXPECT_TRUE(r.toolCalls.empty());
    EXPECT_FALSE(r.truncated);
    ASSERT_EQ(deltas.size(), 2u);
    EXPECT_EQ(deltas[0], "Hello");
    EXPECT_EQ(deltas[1], ", world!");
}

TEST(OpenAiCompatibleClient, ParseSseStreamReassemblesAToolCallSplitAcrossManyChunks) {
    // A real compat server fragments a single tool call's id/name in the
    // FIRST delta for its index, then streams "arguments" as successive
    // string pieces to concatenate across further deltas at the same index.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
        "\"function\":{\"name\":\"add_box\",\"arguments\":\"\"}}]}}]}\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"{\\\"width\\\":\"}}]}}]}\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"10}\"}}]}}]}\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n"
        "data: [DONE]\n";
    LlmTurnResult r = OpenAiCompatibleClient::parseSseStream(sse, 200);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.toolCalls.size(), 1u);
    EXPECT_EQ(r.toolCalls[0].id, "call_1");
    EXPECT_EQ(r.toolCalls[0].name, "add_box");
    EXPECT_EQ(r.toolCalls[0].args["width"], 10);
}

TEST(OpenAiCompatibleClient, ParseSseStreamFlagsTruncationFromTheFinalChunksFinishReason) {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n"
        "data: [DONE]\n";
    LlmTurnResult r = OpenAiCompatibleClient::parseSseStream(sse, 200);
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.truncated);
}

TEST(OpenAiCompatibleClient, ParseSseStreamFiresOnDeltaForReasoningWithoutKeepingItInFinalText) {
    // Hidden reasoning is only ever useful live (see StreamAccumulator's doc
    // comment) - it must reach onDelta for the UI but never end up in the
    // structured result, same as before streaming existed.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"reasoning\":\"let me think... \"}}]}\n"
        "data: {\"choices\":[{\"delta\":{\"reasoning\":\"ok.\"}}]}\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Done.\"}}]}\n"
        "data: [DONE]\n";
    std::vector<std::string> deltas;
    LlmTurnResult r = OpenAiCompatibleClient::parseSseStream(
        sse, 200, [&](const std::string& d) { deltas.push_back(d); });
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.finalText, "Done.");
    ASSERT_EQ(deltas.size(), 3u);
    EXPECT_EQ(deltas[0], "let me think... ");
    EXPECT_EQ(deltas[1], "ok.");
    EXPECT_EQ(deltas[2], "Done.");
}

TEST(OpenAiCompatibleClient, ParseSseStreamToleratesAMalformedChunkLineWithoutAborting) {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"before \"}}]}\n"
        "data: not valid json at all\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"after\"}}]}\n"
        "data: [DONE]\n";
    LlmTurnResult r = OpenAiCompatibleClient::parseSseStream(sse, 200);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.finalText, "before after");
}

TEST(OpenAiCompatibleClient, ParseSseStreamFallsBackToPlainJsonForAnErrorStatus) {
    // An error response is a plain JSON body, not an SSE stream, even though
    // "stream": true was in the request - must not try to parse it as one.
    std::string errorBody = R"({"error":{"message":"model not found"}})";
    LlmTurnResult r = OpenAiCompatibleClient::parseSseStream(errorBody, 404);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("model not found"), std::string::npos);
}

TEST(OpenAiCompatibleClient, BuildRequestBodyMapsToolResultToARealToolRole) {
    // Unlike Anthropic, OpenAI's shape HAS a dedicated "tool" role.
    std::vector<ChatMessage> messages = {
        {ChatRole::ToolResult, "Created body 1", "call_abc", {}},
    };
    nlohmann::json body = OpenAiCompatibleClient::buildRequestBody(messages, {}, "gpt-4o");
    const auto& last = body["messages"].back();
    EXPECT_EQ(last["role"], "tool");
    EXPECT_EQ(last["tool_call_id"], "call_abc");
    EXPECT_EQ(last["content"], "Created body 1");
}

TEST(OpenAiCompatibleClient, BuildRequestBodyFollowsAnImageResultWithASyntheticUserImageMessage) {
    // OpenAI's "tool" role only accepts string content - no vision provider
    // in this family accepts an image_url part there - so a screenshot rides
    // in a synthetic "user" message appended right after the tool result.
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a};
    std::vector<ChatMessage> messages = {
        {ChatRole::ToolResult, "Captured the current view", "call_1", {}, png},
    };
    nlohmann::json body = OpenAiCompatibleClient::buildRequestBody(messages, {}, "gpt-4o");
    // +1 for the leading system prompt (see BuildRequestBodyLeadsWith...).
    ASSERT_EQ(body["messages"].size(), 3u);

    const auto& toolMsg = body["messages"][1];
    EXPECT_EQ(toolMsg["role"], "tool");
    EXPECT_TRUE(toolMsg["content"].is_string())
        << "the tool-role message itself must stay plain text";
    EXPECT_EQ(toolMsg["content"], "Captured the current view");

    const auto& imageMsg = body["messages"][2];
    EXPECT_EQ(imageMsg["role"], "user");
    ASSERT_TRUE(imageMsg["content"].is_array());
    bool sawImage = false;
    for (const auto& part : imageMsg["content"]) {
        if (part["type"] == "image_url") {
            sawImage = true;
            std::string url = part["image_url"]["url"].get<std::string>();
            EXPECT_EQ(url.rfind("data:image/png;base64,", 0), 0u);
        }
    }
    EXPECT_TRUE(sawImage);
}

TEST(OpenAiCompatibleClient, BuildRequestBodyAddsNoExtraMessageWhenAToolResultHasNoImage) {
    std::vector<ChatMessage> messages = {
        {ChatRole::ToolResult, "Created body 1", "call_abc", {}},
    };
    nlohmann::json body = OpenAiCompatibleClient::buildRequestBody(messages, {}, "gpt-4o");
    // +1 for the leading system prompt (see BuildRequestBodyLeadsWith...).
    EXPECT_EQ(body["messages"].size(), 2u);
}

TEST(OpenAiCompatibleClient, BuildRequestBodyEmitsAssistantToolCallsArray) {
    std::vector<ChatMessage> messages = {
        {ChatRole::Assistant, "", "",
         {{"call_1", "add_box", {{"width", 10}}}}},
    };
    nlohmann::json body = OpenAiCompatibleClient::buildRequestBody(messages, {}, "gpt-4o");
    const auto& last = body["messages"].back();
    EXPECT_EQ(last["role"], "assistant");
    EXPECT_TRUE(last["content"].is_null());
    ASSERT_EQ(last["tool_calls"].size(), 1u);
    EXPECT_EQ(last["tool_calls"][0]["id"], "call_1");
    EXPECT_EQ(last["tool_calls"][0]["type"], "function");
    EXPECT_EQ(last["tool_calls"][0]["function"]["name"], "add_box");
    nlohmann::json parsedArgs =
        nlohmann::json::parse(last["tool_calls"][0]["function"]["arguments"].get<std::string>());
    EXPECT_EQ(parsedArgs["width"], 10);
}

TEST(OpenAiCompatibleClient, ParseResponseExtractsFinalTextWhenNoToolCalls) {
    nlohmann::json response = {
        {"choices", {{{"message", {{"role", "assistant"}, {"content", "Done!"}}}}}},
    };
    LlmTurnResult r = OpenAiCompatibleClient::parseResponse(response, 200);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.finalText, "Done!");
    EXPECT_TRUE(r.toolCalls.empty());
}

TEST(OpenAiCompatibleClient, ParseResponseExtractsFunctionToolCalls) {
    nlohmann::json response = {{"choices", {{{"message", {
        {"role", "assistant"},
        {"content", nullptr},
        {"tool_calls", {{
            {"id", "call_1"},
            {"type", "function"},
            {"function", {{"name", "add_box"},
                         {"arguments", "{\"width\":10,\"height\":10,\"depth\":10}"}}},
        }}},
    }}}}}};
    LlmTurnResult r = OpenAiCompatibleClient::parseResponse(response, 200);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.toolCalls.size(), 1u);
    EXPECT_EQ(r.toolCalls[0].id, "call_1");
    EXPECT_EQ(r.toolCalls[0].name, "add_box");
    EXPECT_EQ(r.toolCalls[0].args["width"], 10);
}

TEST(OpenAiCompatibleClient, ParseResponseSurfacesAnHttpErrorStatus) {
    nlohmann::json response = {{"error", {{"message", "model not found"}}}};
    LlmTurnResult r = OpenAiCompatibleClient::parseResponse(response, 404);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("model not found"), std::string::npos);
}

TEST(OpenAiCompatibleClient, ParseResponseHandlesMalformedFunctionArgumentsGracefully) {
    // A local model (Ollama/LM Studio) can emit non-JSON "arguments" - must
    // not crash, must surface a clear tool-level error instead.
    nlohmann::json response = {{"choices", {{{"message", {
        {"role", "assistant"},
        {"tool_calls", {{
            {"id", "call_1"},
            {"function", {{"name", "add_box"}, {"arguments", "not json"}}},
        }}},
    }}}}}};
    LlmTurnResult r = OpenAiCompatibleClient::parseResponse(response, 200);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.toolCalls.size(), 1u);
    EXPECT_TRUE(r.toolCalls[0].args.is_object());
    EXPECT_TRUE(r.toolCalls[0].args.empty())
        << "unparseable arguments must fall back to an empty object, not crash";
}

TEST(OpenAiCompatibleClient, ParseResponseCapturesTextAlongsideToolCalls) {
    // A response can legitimately carry both commentary content and tool_calls -
    // finalText must not be dropped just because tool_calls is non-empty.
    nlohmann::json response = {{"choices", {{{"message", {
        {"role", "assistant"},
        {"content", "Sure, I'll make that box now."},
        {"tool_calls", {{
            {"id", "call_1"},
            {"type", "function"},
            {"function", {{"name", "add_box"},
                         {"arguments", "{\"width\":10,\"height\":10,\"depth\":10}"}}},
        }}},
    }}}}}};
    LlmTurnResult r = OpenAiCompatibleClient::parseResponse(response, 200);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.toolCalls.size(), 1u);
    EXPECT_EQ(r.finalText, "Sure, I'll make that box now.");
}

TEST(OpenAiCompatibleClient, ParseResponseRejectsAChoiceWithNoMessage) {
    nlohmann::json response = {{"choices", {nlohmann::json::object()}}};
    LlmTurnResult r = OpenAiCompatibleClient::parseResponse(response, 200);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

TEST(OpenAiCompatibleClient, ParseResponseFlagsTruncationOnFinishReasonLength) {
    // A reasoning model can burn its whole max_tokens budget on hidden
    // "thinking" before ever emitting content or a tool call - confirmed
    // 2026-09-18 against a real OpenRouter model. That must be distinguishable
    // from a legitimate empty turn so AiSessionController can say so.
    nlohmann::json response = {{"choices", {{
        {"finish_reason", "length"},
        {"message", {{"role", "assistant"}, {"content", nullptr}}},
    }}}};
    LlmTurnResult r = OpenAiCompatibleClient::parseResponse(response, 200);
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.truncated);
    EXPECT_TRUE(r.toolCalls.empty());
    EXPECT_TRUE(r.finalText.empty());
}

TEST(OpenAiCompatibleClient, ParseResponseDoesNotFlagTruncationOnNormalFinish) {
    nlohmann::json response = {{"choices", {{
        {"finish_reason", "tool_calls"},
        {"message", {{"role", "assistant"}, {"content", nullptr}}},
    }}}};
    LlmTurnResult r = OpenAiCompatibleClient::parseResponse(response, 200);
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(r.truncated);
}

TEST(OpenAiCompatibleClient, ParseResponseSkipsAToolCallMissingFunction) {
    // A malformed tool_calls entry lacking "function" must be skipped, not
    // indexed blindly (operator[] on a missing key is UB on a const json).
    nlohmann::json response = {{"choices", {{{"message", {
        {"role", "assistant"},
        {"tool_calls", {
            nlohmann::json::object({{"id", "call_1"}}), // missing "function"
        }},
    }}}}}};
    LlmTurnResult r = OpenAiCompatibleClient::parseResponse(response, 200);
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.toolCalls.empty());
}
