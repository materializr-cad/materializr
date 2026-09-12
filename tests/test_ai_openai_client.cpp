#include "ai/OpenAiCompatibleClient.h"

#include <gtest/gtest.h>

using namespace materializr::ai;

TEST(OpenAiCompatibleClient, BuildRequestBodyIncludesModelMessagesAndTools) {
    std::vector<ChatMessage> messages = {{ChatRole::User, "make a box", "", {}}};
    nlohmann::json body = OpenAiCompatibleClient::buildRequestBody(
        messages, allTools(), "gpt-4o");

    EXPECT_EQ(body["model"], "gpt-4o");
    ASSERT_EQ(body["messages"].size(), 1u);
    EXPECT_EQ(body["messages"][0]["role"], "user");
    ASSERT_TRUE(body["tools"].is_array());
    EXPECT_EQ(body["tools"].size(), allTools().size());
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
