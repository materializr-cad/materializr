#include "OpenAiCompatibleClient.h"
#include "../core/Base64.h"

#include <curl/curl.h>
#include <optional>

namespace materializr { namespace ai {

nlohmann::json OpenAiCompatibleClient::buildRequestBody(
        const std::vector<ChatMessage>& messages, const std::vector<ToolDef>& tools,
        const std::string& model) {
    nlohmann::json out;
    out["model"] = model;
    // Unlike AnthropicClient (which has always hardcoded 4096), this path
    // had NO cap - discovered 2026-09-17 when a local model rambled with no
    // stop condition and ran toward its full context window (tens of
    // minutes at normal token-generation speed) instead of failing fast.
    // Real OpenAI and most compatible servers accept this; Ollama's compat
    // layer otherwise defaults to unbounded.
    //
    // 4096 turned out too tight once reasoning models entered the picture -
    // confirmed 2026-09-18 that a moderately open-ended prompt ("make a
    // model of an a10 warthog") spends its ENTIRE 4096-token budget on
    // hidden reasoning before ever emitting a tool call, coming back with
    // finish_reason "length" and a totally empty response (see
    // AiSessionController::poll()'s `truncated` handling). 16000 is a
    // generous backstop for the actual reply/tool-call content, which stays
    // small even for the most verbose tool - the real fix for reasoning
    // itself is the "reasoning" field below.
    out["max_tokens"] = 16000;
    // OpenRouter's unified reasoning-control extension: caps HIDDEN thinking
    // tokens specifically, separate from max_tokens above, so a reasoning
    // model can't spend its entire budget deliberating and never reach a
    // tool call - raising max_tokens alone doesn't fix this, a sufficiently
    // open-ended prompt just reasons for longer (measured 2026-09-18: the
    // same warthog prompt with no cap here took 255s/~8000 reasoning tokens
    // and counting on a bigger budget; with this cap, 32s/~700). Real OpenAI
    // uses a different field ("reasoning_effort") for its own reasoning
    // models and ignores this one; Ollama's compat layer also silently
    // ignores unrecognised fields (confirmed, does not error) - so this is
    // safe to send unconditionally to every OpenAI-compatible endpoint, not
    // just OpenRouter.
    out["reasoning"] = {{"max_tokens", 1500}};
    // Server-Sent-Events streaming - lets sendTurn forward reasoning/content
    // deltas to the UI as they arrive instead of only after the whole
    // response lands. See applySseChunk/accumulatorToResponseJson: the FINAL
    // LlmTurnResult is reconstructed from the accumulated deltas and goes
    // through the exact same parseResponse as a non-streamed reply, so
    // nothing downstream needs to know streaming happened at all.
    out["stream"] = true;
    nlohmann::json msgs = nlohmann::json::array();
    // No separate top-level "system" field in this API shape (unlike
    // Anthropic's Messages API) - a system-role message has to be the first
    // entry in the array instead.
    msgs.push_back({{"role", "system"}, {"content", systemPrompt()}});
    for (const auto& m : messages) {
        if (m.role == ChatRole::ToolResult) {
            msgs.push_back({{"role", "tool"}, {"tool_call_id", m.toolCallId},
                            {"content", m.text}});
            if (!m.imagePng.empty()) {
                // OpenAI's "tool" role only accepts string content - a
                // vision-capable image_url part has to ride in a "user"
                // message instead. Emitted right after the tool result so it
                // reads, in order, as "here's what that tool produced."
                // Synthesized fresh from ChatMessage::imagePng every call
                // (nothing is stored back into m_messages), so this stays in
                // sync automatically if history is replayed or truncated.
                msgs.push_back({{"role", "user"},
                                {"content", {{{"type", "text"},
                                              {"text", "[Screenshot from the tool call above]"}},
                                             {{"type", "image_url"},
                                              {"image_url",
                                               {{"url", "data:image/png;base64," +
                                                        base64Encode(m.imagePng.data(),
                                                                     m.imagePng.size())}}}}}}});
            }
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
    if (!body["choices"][0].is_object() || !body["choices"][0].contains("message")) {
        r.ok = false;
        r.error = "Response choice had no 'message'";
        return r;
    }
    const auto& message = body["choices"][0]["message"];
    r.ok = true;
    r.truncated = body["choices"][0].value("finish_reason", "") == "length";
    std::string text;
    if (message.contains("content") && message["content"].is_string())
        text = message["content"].get<std::string>();
    if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
        for (const auto& tc : message["tool_calls"]) {
            if (!tc.is_object() || !tc.contains("function") ||
                !tc["function"].is_object())
                continue; // malformed entry - skip it, don't index blindly
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
    }
    // Capture accompanying text unconditionally - a response can legitimately
    // carry both commentary and tool_calls in the same message.
    r.finalText = text;
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

void OpenAiCompatibleClient::applySseChunk(const nlohmann::json& chunk, StreamAccumulator& acc,
                                           const StreamDeltaCallback& onDelta) {
    if (!chunk.contains("choices") || chunk["choices"].empty() ||
        !chunk["choices"][0].is_object())
        return;
    const auto& choice = chunk["choices"][0];
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string())
        acc.finishReason = choice["finish_reason"].get<std::string>();
    if (!choice.contains("delta") || !choice["delta"].is_object()) return;
    const auto& delta = choice["delta"];
    // Some servers stream reasoning under "reasoning", others "reasoning_content" -
    // forward either straight to the UI; neither is part of the final
    // LlmTurnResult (see StreamAccumulator's doc comment).
    for (const char* key : {"reasoning", "reasoning_content"}) {
        if (delta.contains(key) && delta[key].is_string()) {
            const std::string piece = delta[key].get<std::string>();
            if (!piece.empty() && onDelta) onDelta(piece);
        }
    }
    if (delta.contains("content") && delta["content"].is_string()) {
        const std::string piece = delta["content"].get<std::string>();
        acc.content += piece;
        if (!piece.empty() && onDelta) onDelta(piece);
    }
    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        for (const auto& tc : delta["tool_calls"]) {
            if (!tc.is_object() || !tc.contains("index") || !tc["index"].is_number())
                continue;
            auto& call = acc.toolCallsByIndex[tc["index"].get<int>()];
            if (tc.contains("id") && tc["id"].is_string())
                call.id = tc["id"].get<std::string>();
            if (tc.contains("function") && tc["function"].is_object()) {
                const auto& fn = tc["function"];
                if (fn.contains("name") && fn["name"].is_string())
                    call.name += fn["name"].get<std::string>();
                // "arguments" arrives as successive string FRAGMENTS to
                // concatenate, not a replacement each time - a compat server
                // streams e.g. `{"wid`, `th":10,`, `"height":10}` across
                // several deltas for one tool call.
                if (fn.contains("arguments") && fn["arguments"].is_string())
                    call.arguments += fn["arguments"].get<std::string>();
            }
        }
    }
}

nlohmann::json OpenAiCompatibleClient::accumulatorToResponseJson(const StreamAccumulator& acc) {
    nlohmann::json message;
    message["role"] = "assistant";
    message["content"] = acc.content.empty() ? nlohmann::json(nullptr) : nlohmann::json(acc.content);
    if (!acc.toolCallsByIndex.empty()) {
        nlohmann::json toolCalls = nlohmann::json::array();
        for (const auto& [index, call] : acc.toolCallsByIndex)
            toolCalls.push_back({{"id", call.id}, {"type", "function"},
                                 {"function", {{"name", call.name},
                                               {"arguments", call.arguments}}}});
        message["tool_calls"] = toolCalls;
    }
    return {{"choices", {{{"message", message}, {"finish_reason", acc.finishReason}}}}};
}

namespace {
// One line of an SSE stream, already stripped of its trailing "\r\n" - "" for
// a blank keep-alive line, the JSON payload for a "data: ..." line (with
// "[DONE]" reported as std::nullopt, the sentinel that ends the stream), and
// std::nullopt with no payload processing needed for anything else (SSE
// comment lines, "event:" lines this API never sends, etc).
std::optional<std::string> sseDataPayload(const std::string& line) {
    if (line.rfind("data:", 0) != 0) return std::nullopt;
    size_t start = line.find_first_not_of(' ', 5);
    std::string payload = (start == std::string::npos) ? "" : line.substr(start);
    if (payload == "[DONE]") return std::nullopt;
    return payload;
}
} // namespace

LlmTurnResult OpenAiCompatibleClient::parseSseStream(const std::string& sseBody, long httpStatus,
                                                     const StreamDeltaCallback& onDelta) {
    if (httpStatus < 200 || httpStatus >= 300)
        return parseResponseFromRawBody(sseBody, httpStatus);
    StreamAccumulator acc;
    size_t pos = 0;
    while (pos <= sseBody.size()) {
        size_t nl = sseBody.find('\n', pos);
        std::string line = sseBody.substr(pos, (nl == std::string::npos ? sseBody.size() : nl) - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (auto payload = sseDataPayload(line)) {
            try {
                applySseChunk(nlohmann::json::parse(*payload), acc, onDelta);
            } catch (const nlohmann::json::parse_error&) {
                // A malformed/truncated chunk line - skip it, matches the
                // tolerance parseResponse already has for malformed entries.
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return parseResponse(accumulatorToResponseJson(acc), httpStatus);
}

namespace {
// CURLOPT_WRITEDATA payload for the real streaming request: everything
// applySseChunk needs, plus the raw byte buffer curl keeps appending to (used
// unparsed for a non-2xx error body - see the httpStatus check below, same
// fallback parseResponseFromRawBody already handled before streaming existed)
// and how far into it complete lines have already been consumed.
struct StreamWriteContext {
    CURL* curl = nullptr;
    std::string rawBody;
    size_t consumedUpTo = 0;
    StreamAccumulator acc;
    const StreamDeltaCallback* onDelta = nullptr;
};

size_t streamWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* ctx = static_cast<StreamWriteContext*>(userp);
    const size_t kMaxResponse = 4u * 1024 * 1024;
    if (ctx->rawBody.size() + total > kMaxResponse) return 0;
    ctx->rawBody.append(static_cast<char*>(contents), total);

    // An error response (bad model, bad key, ...) is a plain JSON body, not
    // an SSE stream, even though "stream": true was requested - leave it in
    // rawBody untouched for sendTurn to hand to parseResponseFromRawBody
    // once the transfer finishes, exactly as it did before streaming existed.
    long httpStatus = 0;
    curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &httpStatus);
    if (httpStatus < 200 || httpStatus >= 300) return total;

    for (;;) {
        size_t nl = ctx->rawBody.find('\n', ctx->consumedUpTo);
        if (nl == std::string::npos) break;
        std::string line = ctx->rawBody.substr(ctx->consumedUpTo, nl - ctx->consumedUpTo);
        ctx->consumedUpTo = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (auto payload = sseDataPayload(line)) {
            try {
                OpenAiCompatibleClient::applySseChunk(
                    nlohmann::json::parse(*payload), ctx->acc,
                    ctx->onDelta ? *ctx->onDelta : StreamDeltaCallback{});
            } catch (const nlohmann::json::parse_error&) {
                // Same tolerance as parseSseStream - a stray malformed chunk
                // must not abort an otherwise-live stream.
            }
        }
    }
    return total;
}

// See AnthropicClient's identical callback - curl polls this roughly once a
// second for the whole request (including a local server's cold model
// load), and a non-zero return aborts it right away with
// CURLE_ABORTED_BY_CALLBACK.
int xferAbortIfCancelled(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* cancelFlag = static_cast<const std::atomic<bool>*>(clientp);
    return (cancelFlag && cancelFlag->load()) ? 1 : 0;
}
} // namespace

LlmTurnResult OpenAiCompatibleClient::sendTurn(const std::vector<ChatMessage>& messages,
                                              const std::vector<ToolDef>& tools,
                                              const std::atomic<bool>* cancelFlag,
                                              const StreamDeltaCallback& onDelta) {
    nlohmann::json requestBody = buildRequestBody(messages, tools, m_model);
    std::string requestStr = requestBody.dump();

    CURL* curl = curl_easy_init();
    if (!curl) {
        LlmTurnResult r;
        r.ok = false;
        r.error = "Failed to initialise libcurl.";
        return r;
    }

    StreamWriteContext ctx;
    ctx.curl = curl;
    ctx.onDelta = &onDelta;
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
    // A generous outer safety net, not the primary control anymore - now
    // that cancelFlag/xferAbortIfCancelled exists, the user's Cancel button
    // is how a slow-but-alive request (a local server cold-loading a
    // multi-GB model, or just a big/slow generation) actually gets stopped.
    // This only guards against curl itself wedging. Connect timeout stays
    // short - that fails fast on a wrong host/port, a different failure
    // than "slow to answer".
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferAbortIfCancelled);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelFlag);

    CURLcode code = curl_easy_perform(curl);
    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        LlmTurnResult r;
        r.ok = false;
        r.error = (code == CURLE_ABORTED_BY_CALLBACK) ? "Cancelled"
                                                       : curl_easy_strerror(code);
        return r;
    }
    try {
        if (httpStatus >= 200 && httpStatus < 300)
            return parseResponse(accumulatorToResponseJson(ctx.acc), httpStatus);
        return parseResponseFromRawBody(ctx.rawBody, httpStatus);
    } catch (const std::exception& e) {
        LlmTurnResult r;
        r.ok = false;
        r.error = e.what();
        return r;
    }
}

} } // namespace materializr::ai
