#include "AnthropicClient.h"

#include <curl/curl.h>

namespace materializr { namespace ai {

namespace {
size_t writeToString(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    std::string* out = static_cast<std::string*>(userp);
    const size_t kMaxResponse = 4u * 1024 * 1024;
    if (out->size() + total > kMaxResponse) return 0;
    out->append(static_cast<char*>(contents), total);
    return total;
}
} // namespace

nlohmann::json AnthropicClient::buildRequestBody(const std::vector<ChatMessage>& messages,
                                                 const std::vector<ToolDef>& tools,
                                                 const std::string& model) {
    nlohmann::json out;
    out["model"] = model;
    out["max_tokens"] = 4096;
    nlohmann::json msgs = nlohmann::json::array();
    nlohmann::json pendingToolResults = nlohmann::json::array();
    auto flushToolResults = [&]() {
        if (pendingToolResults.empty()) return;
        msgs.push_back({{"role", "user"}, {"content", pendingToolResults}});
        pendingToolResults = nlohmann::json::array();
    };
    for (const auto& m : messages) {
        if (m.role == ChatRole::ToolResult) {
            pendingToolResults.push_back({{"type", "tool_result"},
                                          {"tool_use_id", m.toolCallId},
                                          {"content", m.text}});
            continue;
        }
        flushToolResults();
        if (m.role == ChatRole::Assistant && !m.toolCalls.empty()) {
            nlohmann::json blocks = nlohmann::json::array();
            for (const auto& c : m.toolCalls)
                blocks.push_back({{"type", "tool_use"}, {"id", c.id},
                                  {"name", c.name}, {"input", c.args}});
            msgs.push_back({{"role", "assistant"}, {"content", blocks}});
            continue;
        }
        msgs.push_back({{"role", m.role == ChatRole::User ? "user" : "assistant"},
                        {"content", m.text}});
    }
    flushToolResults();
    out["messages"] = msgs;
    if (!tools.empty()) out["tools"] = toolsToAnthropicJson(tools);
    return out;
}

LlmTurnResult AnthropicClient::parseResponse(const nlohmann::json& body, long httpStatus) {
    LlmTurnResult r;
    if (httpStatus < 200 || httpStatus >= 300) {
        r.ok = false;
        r.error = body.contains("error") && body["error"].contains("message")
                      ? body["error"]["message"].get<std::string>()
                      : ("Anthropic API returned HTTP " + std::to_string(httpStatus));
        return r;
    }
    if (!body.contains("content") || !body["content"].is_array()) {
        r.ok = false;
        r.error = "Anthropic response had no 'content' array";
        return r;
    }
    std::string text;
    for (const auto& block : body["content"]) {
        if (!block.contains("type")) continue;
        if (block["type"] == "text" && block.contains("text"))
            text += block["text"].get<std::string>();
        else if (block["type"] == "tool_use") {
            ToolCall call;
            call.id = block.value("id", "");
            call.name = block.value("name", "");
            call.args = block.value("input", nlohmann::json::object());
            r.toolCalls.push_back(std::move(call));
        }
    }
    r.ok = true;
    // Capture accompanying text unconditionally - a turn can legitimately carry
    // both commentary text and tool_use blocks in the same response.
    r.finalText = text;
    return r;
}

LlmTurnResult AnthropicClient::parseResponseFromRawBody(const std::string& rawBody,
                                                        long httpStatus) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(rawBody);
    } catch (const nlohmann::json::parse_error&) {
        LlmTurnResult r;
        r.ok = false;
        r.error = "Anthropic returned a response that wasn't valid JSON "
                  "(HTTP " + std::to_string(httpStatus) + ")";
        return r;
    }
    return parseResponse(parsed, httpStatus);
}

LlmTurnResult AnthropicClient::sendTurn(const std::vector<ChatMessage>& messages,
                                       const std::vector<ToolDef>& tools) {
    nlohmann::json requestBody = buildRequestBody(messages, tools, m_model);
    std::string requestStr = requestBody.dump();

    CURL* curl = curl_easy_init();
    if (!curl) {
        LlmTurnResult r;
        r.ok = false;
        r.error = "Failed to initialise libcurl.";
        return r;
    }

    std::string responseBody;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("x-api-key: " + m_apiKey).c_str());
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestStr.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);

    CURLcode code = curl_easy_perform(curl);
    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        LlmTurnResult r;
        r.ok = false;
        r.error = curl_easy_strerror(code);
        return r;
    }
    try {
        return parseResponseFromRawBody(responseBody, httpStatus);
    } catch (const std::exception& e) {
        LlmTurnResult r;
        r.ok = false;
        r.error = e.what();
        return r;
    }
}

} } // namespace materializr::ai
