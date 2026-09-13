#include "ai/AnthropicClient.h"

#include <gtest/gtest.h>

using namespace materializr::ai;

TEST(AnthropicClient, BuildRequestBodyIncludesModelMessagesAndTools) {
    std::vector<ChatMessage> messages = {{ChatRole::User, "make a box", "", {}}};
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
        {ChatRole::User, "make a box", "", {}},
        {ChatRole::Assistant, "", "", {{"call_abc", "add_box", {}}}},
        {ChatRole::ToolResult, "Created body 1", "call_abc", {}},
    };
    nlohmann::json body = AnthropicClient::buildRequestBody(messages, {}, "claude-sonnet-4-5");
    const auto& last = body["messages"].back();
    EXPECT_EQ(last["role"], "user");
    ASSERT_EQ(last["content"].size(), 1u);
    EXPECT_EQ(last["content"][0]["type"], "tool_result");
    EXPECT_EQ(last["content"][0]["tool_use_id"], "call_abc");
    EXPECT_EQ(last["content"][0]["content"], "Created body 1");
}

TEST(AnthropicClient, BuildRequestBodyEmitsAssistantToolUseBlock) {
    std::vector<ChatMessage> messages = {
        {ChatRole::User, "make a box", "", {}},
        {ChatRole::Assistant, "", "",
         {{"call_1", "add_box", {{"width", 10}}}}},
    };
    nlohmann::json body = AnthropicClient::buildRequestBody(messages, {}, "claude-sonnet-4-5");
    ASSERT_EQ(body["messages"].size(), 2u);
    const auto& assistantMsg = body["messages"][1];
    EXPECT_EQ(assistantMsg["role"], "assistant");
    ASSERT_EQ(assistantMsg["content"].size(), 1u);
    EXPECT_EQ(assistantMsg["content"][0]["type"], "tool_use");
    EXPECT_EQ(assistantMsg["content"][0]["id"], "call_1");
    EXPECT_EQ(assistantMsg["content"][0]["name"], "add_box");
    EXPECT_EQ(assistantMsg["content"][0]["input"]["width"], 10);
}

TEST(AnthropicClient, BuildRequestBodyGroupsConsecutiveToolResultsIntoOneUserMessage) {
    std::vector<ChatMessage> messages = {
        {ChatRole::User, "make two boxes", "", {}},
        {ChatRole::Assistant, "", "",
         {{"call_1", "add_box", {}}, {"call_2", "add_box", {}}}},
        {ChatRole::ToolResult, "Created body 1", "call_1", {}},
        {ChatRole::ToolResult, "Created body 2", "call_2", {}},
    };
    nlohmann::json body = AnthropicClient::buildRequestBody(messages, {}, "claude-sonnet-4-5");
    ASSERT_EQ(body["messages"].size(), 3u);
    const auto& toolResultsMsg = body["messages"][2];
    EXPECT_EQ(toolResultsMsg["role"], "user");
    ASSERT_EQ(toolResultsMsg["content"].size(), 2u);
    EXPECT_EQ(toolResultsMsg["content"][0]["tool_use_id"], "call_1");
    EXPECT_EQ(toolResultsMsg["content"][1]["tool_use_id"], "call_2");
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

TEST(AnthropicClient, ParseResponseCapturesTextAlongsideToolCalls) {
    // A turn can legitimately carry both commentary text and a tool_use block -
    // finalText must not be dropped just because toolCalls is non-empty.
    nlohmann::json response = {
        {"content", {
            {{"type", "text"}, {"text", "Sure, I'll make that box now."}},
            {{"type", "tool_use"}, {"id", "call_1"}, {"name", "add_box"},
             {"input", {{"width", 10}, {"height", 10}, {"depth", 10}}}},
        }},
    };
    LlmTurnResult r = AnthropicClient::parseResponse(response, 200);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.toolCalls.size(), 1u);
    EXPECT_EQ(r.finalText, "Sure, I'll make that box now.");
}
