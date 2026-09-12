#include "ai/AnthropicClient.h"

#include <gtest/gtest.h>

using namespace materializr::ai;

TEST(AnthropicClient, BuildRequestBodyIncludesModelMessagesAndTools) {
    std::vector<ChatMessage> messages = {{ChatRole::User, "make a box", ""}};
    nlohmann::json body = AnthropicClient::buildRequestBody(
        messages, allTools(), "claude-sonnet-4-5");

    EXPECT_EQ(body["model"], "claude-sonnet-4-5");
    ASSERT_TRUE(body["messages"].is_array());
    ASSERT_EQ(body["messages"].size(), 1u);
    EXPECT_EQ(body["messages"][0]["role"], "user");
    ASSERT_TRUE(body["tools"].is_array());
    EXPECT_EQ(body["tools"].size(), allTools().size());
}

TEST(AnthropicClient, BuildRequestBodyMapsToolResultMessagesToUserToolResultBlocks) {
    // Anthropic has no separate "tool" role - a tool result rides inside a
    // user-role message as a tool_result content block.
    std::vector<ChatMessage> messages = {
        {ChatRole::User, "make a box", ""},
        {ChatRole::Assistant, "", ""}, // the tool_use turn itself isn't replayed here
        {ChatRole::ToolResult, "Created body 1", "call_abc"},
    };
    nlohmann::json body = AnthropicClient::buildRequestBody(messages, {}, "claude-sonnet-4-5");
    const auto& last = body["messages"].back();
    EXPECT_EQ(last["role"], "user");
    ASSERT_EQ(last["content"].size(), 1u);
    EXPECT_EQ(last["content"][0]["type"], "tool_result");
    EXPECT_EQ(last["content"][0]["tool_use_id"], "call_abc");
    EXPECT_EQ(last["content"][0]["content"], "Created body 1");
}

TEST(AnthropicClient, ParseResponseExtractsFinalTextWhenNoToolUse) {
    nlohmann::json response = {
        {"content", {{{"type", "text"}, {"text", "Done!"}}}},
    };
    LlmTurnResult r = AnthropicClient::parseResponse(response, 200);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.finalText, "Done!");
    EXPECT_TRUE(r.toolCalls.empty());
}

TEST(AnthropicClient, ParseResponseExtractsToolUseBlocks) {
    nlohmann::json response = {
        {"content", {
            {{"type", "text"}, {"text", "Sure, one moment."}},
            {{"type", "tool_use"}, {"id", "call_1"}, {"name", "add_box"},
             {"input", {{"width", 10}, {"height", 10}, {"depth", 10}}}},
        }},
    };
    LlmTurnResult r = AnthropicClient::parseResponse(response, 200);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.toolCalls.size(), 1u);
    EXPECT_EQ(r.toolCalls[0].id, "call_1");
    EXPECT_EQ(r.toolCalls[0].name, "add_box");
    EXPECT_EQ(r.toolCalls[0].args["width"], 10);
}

TEST(AnthropicClient, ParseResponseSurfacesAnHttpErrorStatus) {
    nlohmann::json response = {{"error", {{"message", "invalid x-api-key"}}}};
    LlmTurnResult r = AnthropicClient::parseResponse(response, 401);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("invalid x-api-key"), std::string::npos);
}

TEST(AnthropicClient, ParseResponseHandlesUnparseableJsonGracefully) {
    LlmTurnResult r = AnthropicClient::parseResponseFromRawBody("not json at all", 200);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}
