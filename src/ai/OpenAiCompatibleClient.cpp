#include "OpenAiCompatibleClient.h"

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

nlohmann::json OpenAiCompatibleClient::buildRequestBody(
        const std::vector<ChatMessage>& messages, const std::vector<ToolDef>& tools,
        const std::string& model) {
    nlohmann::json out;
    out["model"] = model;
    nlohmann::json msgs = nlohmann::json::array();
    for (const auto& m : messages) {
        if (m.role == ChatRole::ToolResult) {
            msgs.push_back({{"role", "tool"}, {"tool_call_id", m.toolCallId},
                            {"content", m.text}});
        } else if (m.role == ChatRole::Assistant && !m.toolCalls.empty()) {
            nlohmann::json toolCalls = nlohmann::json::array();
            for (const auto& c : m.toolCalls)
                toolCalls.push_back({{"id", c.id}, {"type", "function"},
                                     {"function", {{"name", c.name},
                                                   {"arguments", c.args.dump()}}}});
            msgs.push_back({{"role", "assistant"}, {"content", nullptr},
                            {"tool_calls", toolCalls}});
        } else {
            msgs.push_back({{"role", m.role == ChatRole::User ? "user" : "assistant"},
                            {"content", m.text}});
        }
    }
    out["messages"] = msgs;
    if (!tools.empty()) out["tools"] = toolsToOpenAiJson(tools);
    return out;
}

LlmTurnResult OpenAiCompatibleClient::parseResponse(const nlohmann::json& body,
                                                    long httpStatus) {
    LlmTurnResult r;
    if (httpStatus < 200 || httpStatus >= 300) {
        r.ok = false;
        r.error = body.contains("error") && body["error"].contains("message")
                      ? body["error"]["message"].get<std::string>()
                      : ("Request returned HTTP " + std::to_string(httpStatus));
        return r;
    }
    if (!body.contains("choices") || body["choices"].empty()) {
        r.ok = false;
        r.error = "Response had no 'choices'";
        return r;
    }
    const auto& message = body["choices"][0]["message"];
    r.ok = true;
    if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
        for (const auto& tc : message["tool_calls"]) {
            ToolCall call;
            call.id = tc.value("id", "");
            const auto& fn = tc["function"];
            call.name = fn.value("name", "");
            // "arguments" is normally a JSON-encoded string, but a
            // misbehaving local server can send it as a JSON object/other
            // type directly - guard the type before touching it as a string,
            // in addition to catching malformed JSON *inside* the string.
            std::string argsStr = "{}";
            if (fn.contains("arguments") && fn["arguments"].is_string())
                argsStr = fn["arguments"].get<std::string>();
            try {
                call.args = nlohmann::json::parse(argsStr);
            } catch (const nlohmann::json::parse_error&) {
                // A local model emitted non-JSON arguments - fall back to an
                // empty object so AiToolDispatcher's own arg validation
                // reports a clean "missing required argument" instead of
                // this layer crashing on it.
                call.args = nlohmann::json::object();
            }
            r.toolCalls.push_back(std::move(call));
        }
    } else if (message.contains("content") && message["content"].is_string()) {
        r.finalText = message["content"].get<std::string>();
    }
    return r;
}

LlmTurnResult OpenAiCompatibleClient::parseResponseFromRawBody(const std::string& rawBody,
                                                               long httpStatus) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(rawBody);
    } catch (const nlohmann::json::parse_error&) {
        LlmTurnResult r;
        r.ok = false;
        r.error = "The server returned a response that wasn't valid JSON "
                  "(HTTP " + std::to_string(httpStatus) + ")";
        return r;
    }
    return parseResponse(parsed, httpStatus);
}

LlmTurnResult OpenAiCompatibleClient::sendTurn(const std::vector<ChatMessage>& messages,
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
    // A dummy/empty key is fine for a local server (Ollama/LM Studio) that
    // doesn't check it - the header is just always sent for consistency.
    headers = curl_slist_append(headers, ("Authorization: Bearer " + m_apiKey).c_str());

    std::string url = m_baseUrl + "/chat/completions";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestStr.c_str());
    // Allows BOTH http and https (unlike AnthropicClient's https-only): a
    // local Ollama/LM Studio endpoint is plain HTTP by default
    // (http://localhost:11434). Still restricted to those two so a
    // malformed/malicious base URL (file://, scp://, ...) can't be honored.
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
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
