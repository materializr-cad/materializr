#include "ai/AiSessionController.h"
#include "core/Document.h"
#include "core/History.h"
#include "plugin/PluginContext.h"

#include <gtest/gtest.h>
#include <thread>
#include <chrono>

using namespace materializr::ai;
using materializr::PluginContext;

namespace {
// A scripted LlmClient: each call to sendTurn returns the next entry in a
// pre-built queue, so the test controls exactly what the "model" does on
// each turn without any real network or thread.
class ScriptedClient : public LlmClient {
public:
    explicit ScriptedClient(std::vector<LlmTurnResult> turns)
        : m_turns(std::move(turns)) {}
    // Optionally fires onDelta with these fake pieces before returning the
    // scripted result, for tests that exercise the streaming wiring itself
    // rather than any particular provider's real SSE parsing.
    explicit ScriptedClient(std::vector<LlmTurnResult> turns,
                           std::vector<std::string> streamedDeltas)
        : m_turns(std::move(turns)), m_streamedDeltas(std::move(streamedDeltas)) {}
    LlmTurnResult sendTurn(const std::vector<ChatMessage>& messages,
                          const std::vector<ToolDef>&,
                          const std::atomic<bool>*,
                          const StreamDeltaCallback& onDelta) override {
        m_capturedCalls.push_back(messages);
        if (onDelta)
            for (const auto& piece : m_streamedDeltas) onDelta(piece);
        if (m_next >= m_turns.size()) {
            LlmTurnResult r;
            r.ok = false;
            r.error = "ScriptedClient ran out of scripted turns";
            return r;
        }
        return m_turns[m_next++];
    }
    const std::vector<std::vector<ChatMessage>>& capturedCalls() const {
        return m_capturedCalls;
    }
private:
    std::vector<LlmTurnResult> m_turns;
    size_t m_next = 0;
    std::vector<std::vector<ChatMessage>> m_capturedCalls;
    std::vector<std::string> m_streamedDeltas;
};

// Simulates a slow request the same way curl actually behaves: it doesn't
// return until either cancelFlag flips true (mimicking
// xferAbortIfCancelled aborting the transfer) or a generous safety timeout
// elapses (so a bug that never sets the flag fails the test instead of
// hanging it forever).
class SlowCancellableClient : public LlmClient {
public:
    LlmTurnResult sendTurn(const std::vector<ChatMessage>&, const std::vector<ToolDef>&,
                          const std::atomic<bool>* cancelFlag,
                          const StreamDeltaCallback&) override {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!(cancelFlag && cancelFlag->load())) {
            if (std::chrono::steady_clock::now() > deadline) {
                LlmTurnResult r;
                r.ok = false;
                r.error = "SlowCancellableClient: never got cancelled";
                return r;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        LlmTurnResult r;
        r.ok = false;
        r.error = "Cancelled";
        return r;
    }
};

PluginContext makeCtx(Document& doc, History& hist) {
    PluginContext ctx;
    ctx._bind(&doc, &hist, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    return ctx;
}

// Drive poll() until the controller stops being busy or a safety cap of
// iterations is hit (a real hang here is a bug the test must fail on, not
// spin forever).
void pumpUntilIdle(AiSessionController& sess, PluginContext& ctx) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sess.isBusy() && std::chrono::steady_clock::now() < deadline) {
        sess.poll(ctx);
        if (sess.isBusy()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

LlmTurnResult finalText(const std::string& text) {
    LlmTurnResult r;
    r.ok = true;
    r.finalText = text;
    return r;
}
LlmTurnResult toolCall(const std::string& id, const std::string& name,
                      nlohmann::json args) {
    LlmTurnResult r;
    r.ok = true;
    ToolCall c{id, name, std::move(args)};
    r.toolCalls.push_back(std::move(c));
    return r;
}
} // namespace

TEST(AiSessionController, AFinalTextTurnEndsTheSessionImmediately) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    AiSessionController sess(std::make_unique<ScriptedClient>(
        std::vector<LlmTurnResult>{finalText("All done.")}));

    sess.submitPrompt("say hi");
    pumpUntilIdle(sess, ctx);

    EXPECT_FALSE(sess.isBusy());
    ASSERT_FALSE(sess.scrollback().empty());
    EXPECT_EQ(sess.scrollback().back().text, "All done.");
}

TEST(AiSessionController, AToolCallTurnExecutesItAndContinuesTheLoop) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    AiSessionController sess(std::make_unique<ScriptedClient>(
        std::vector<LlmTurnResult>{
            toolCall("call_1", "add_box",
                     {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}),
            finalText("Made your box."),
        }));

    sess.submitPrompt("make a box");
    pumpUntilIdle(sess, ctx);

    EXPECT_FALSE(sess.isBusy());
    EXPECT_EQ(doc.getAllBodyIds().size(), 1u)
        << "the tool call must actually have executed against the document";
    EXPECT_EQ(sess.scrollback().back().text, "Made your box.");
}

TEST(AiSessionController, StopsAfterTheStepCapInsteadOfLoopingForever) {
    // Script far more tool-call turns than the cap - the controller must
    // stop on its own rather than exhausting the script or spinning.
    std::vector<LlmTurnResult> turns;
    for (int i = 0; i < 20; ++i)
        turns.push_back(toolCall("call_" + std::to_string(i), "add_box",
                                 {{"width", 1.0}, {"height", 1.0}, {"depth", 1.0}}));
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    AiSessionController sess(std::make_unique<ScriptedClient>(turns));

    sess.submitPrompt("keep making boxes");
    pumpUntilIdle(sess, ctx);

    EXPECT_FALSE(sess.isBusy());
    EXPECT_EQ(doc.getAllBodyIds().size(), 8u)
        << "the 8-step cap must actually bound how many tool calls run";
}

TEST(AiSessionController, AnInvalidToolCallFeedsTheErrorBackRatherThanStopping) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    AiSessionController sess(std::make_unique<ScriptedClient>(
        std::vector<LlmTurnResult>{
            toolCall("call_1", "add_box", {{"width", -5.0}}), // invalid
            finalText("Fixed it."), // the model gets the error and recovers
        }));

    sess.submitPrompt("make a box");
    pumpUntilIdle(sess, ctx);

    EXPECT_FALSE(sess.isBusy());
    EXPECT_TRUE(doc.getAllBodyIds().empty())
        << "the invalid call must not have created a body";
    EXPECT_EQ(sess.scrollback().back().text, "Fixed it.");
}

TEST(AiSessionController, TheSecondTurnReplaysTheAssistantsToolUseAndItsResult) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    auto scripted = std::make_unique<ScriptedClient>(
        std::vector<LlmTurnResult>{
            toolCall("call_1", "add_box",
                     {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}}),
            finalText("Made your box."),
        });
    ScriptedClient* rawClient = scripted.get();
    AiSessionController sess(std::move(scripted));

    sess.submitPrompt("make a box");
    pumpUntilIdle(sess, ctx);

    ASSERT_EQ(rawClient->capturedCalls().size(), 2u);
    const std::vector<ChatMessage>& secondCall = rawClient->capturedCalls()[1];
    ASSERT_EQ(secondCall.size(), 3u);
    EXPECT_EQ(secondCall[0].role, ChatRole::User);
    EXPECT_EQ(secondCall[1].role, ChatRole::Assistant);
    ASSERT_EQ(secondCall[1].toolCalls.size(), 1u);
    EXPECT_EQ(secondCall[1].toolCalls[0].id, "call_1");
    EXPECT_EQ(secondCall[2].role, ChatRole::ToolResult);
    EXPECT_EQ(secondCall[2].toolCallId, "call_1");
}

TEST(AiSessionController, StreamedDeltasAccumulateIntoStreamingText) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    AiSessionController sess(std::make_unique<ScriptedClient>(
        std::vector<LlmTurnResult>{finalText("Done.")},
        std::vector<std::string>{"Thinking about ", "the request...", " ok, done."}));

    sess.submitPrompt("do something");
    pumpUntilIdle(sess, ctx);

    EXPECT_EQ(sess.streamingText(), "Thinking about the request... ok, done.");
}

TEST(AiSessionController, StreamingTextResetsAtTheStartOfEachTurnRatherThanAccumulatingForever) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    auto scripted = std::make_unique<ScriptedClient>(
        std::vector<LlmTurnResult>{finalText("First."), finalText("Second.")},
        std::vector<std::string>{"same fake delta each turn"});
    AiSessionController sess(std::move(scripted));

    sess.submitPrompt("first");
    pumpUntilIdle(sess, ctx);
    sess.submitPrompt("second");
    pumpUntilIdle(sess, ctx);

    EXPECT_EQ(sess.streamingText(), "same fake delta each turn")
        << "startTurn() must clear the buffer, not append across turns";
}

TEST(AiSessionController, StreamingTextIsEmptyBeforeAnythingIsSubmitted) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    AiSessionController sess(std::make_unique<ScriptedClient>(
        std::vector<LlmTurnResult>{finalText("Hi.")}));
    (void)ctx;
    EXPECT_TRUE(sess.streamingText().empty());
}

TEST(AiSessionController, ATruncatedEmptyTurnSurfacesAsAnErrorInsteadOfGoingSilent) {
    // Used to be a silent no-op (see the removed comment in poll()) - a
    // reasoning model that spends its whole token budget "thinking" before
    // ever replying or calling a tool must not look identical to nothing
    // having happened at all.
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    LlmTurnResult truncated;
    truncated.ok = true;
    truncated.truncated = true;
    AiSessionController sess(std::make_unique<ScriptedClient>(
        std::vector<LlmTurnResult>{truncated}));

    sess.submitPrompt("make a model of an a10 warthog");
    pumpUntilIdle(sess, ctx);

    EXPECT_FALSE(sess.isBusy());
    ASSERT_FALSE(sess.scrollback().empty());
    const auto& last = sess.scrollback().back();
    EXPECT_EQ(last.kind, AiSessionController::ScrollbackLine::Kind::Error);
    EXPECT_NE(last.text.find("budget"), std::string::npos) << last.text;
}

TEST(AiSessionController, AnEmptyNonTruncatedTurnStillSurfacesAsAnError) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    LlmTurnResult empty;
    empty.ok = true; // no finalText, no toolCalls, not truncated either
    AiSessionController sess(std::make_unique<ScriptedClient>(
        std::vector<LlmTurnResult>{empty}));

    sess.submitPrompt("hello");
    pumpUntilIdle(sess, ctx);

    EXPECT_FALSE(sess.isBusy());
    ASSERT_FALSE(sess.scrollback().empty());
    EXPECT_EQ(sess.scrollback().back().kind,
             AiSessionController::ScrollbackLine::Kind::Error);
}

TEST(AiSessionController, ANetworkFailureEndsTheSessionWithAnErrorLine) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    LlmTurnResult failure;
    failure.ok = false;
    failure.error = "Connection refused";
    AiSessionController sess(std::make_unique<ScriptedClient>(
        std::vector<LlmTurnResult>{failure}));

    sess.submitPrompt("make a box");
    pumpUntilIdle(sess, ctx);

    EXPECT_FALSE(sess.isBusy());
    bool sawError = false;
    for (const auto& line : sess.scrollback())
        if (line.kind == AiSessionController::ScrollbackLine::Kind::Error &&
            line.text.find("Connection refused") != std::string::npos)
            sawError = true;
    EXPECT_TRUE(sawError);
}

TEST(AiSessionController, ATurnWithMoreToolCallsThanTheCapStopsPartway) {
    // A single turn bundling more tool calls than the 8-step cap must stop
    // partway through THAT turn's loop, not run all of them before checking.
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    LlmTurnResult bigTurn;
    bigTurn.ok = true;
    for (int i = 0; i < 12; ++i) {
        ToolCall c{"call_" + std::to_string(i), "add_box",
                  {{"width", 1.0}, {"height", 1.0}, {"depth", 1.0}}};
        bigTurn.toolCalls.push_back(std::move(c));
    }
    AiSessionController sess(std::make_unique<ScriptedClient>(
        std::vector<LlmTurnResult>{bigTurn}));

    sess.submitPrompt("make twelve boxes");
    pumpUntilIdle(sess, ctx);

    EXPECT_FALSE(sess.isBusy());
    EXPECT_EQ(doc.getAllBodyIds().size(), 8u)
        << "the cap must stop execution partway through a single oversized turn";
}

TEST(AiSessionController, ACappedTurnKeepsHistoryBalancedForTheNextPrompt) {
    // The capped turn bundles 12 tool calls but only 8 run. Every one of the
    // 12 tool_use ids in the assistant message must still get a matching
    // ToolResult, or the next submitPrompt() sends unbalanced history and the
    // provider rejects it with HTTP 400.
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);

    LlmTurnResult bigTurn;
    bigTurn.ok = true;
    for (int i = 0; i < 12; ++i) {
        ToolCall c{"call_" + std::to_string(i), "add_box",
                  {{"width", 1.0}, {"height", 1.0}, {"depth", 1.0}}};
        bigTurn.toolCalls.push_back(std::move(c));
    }
    auto scripted = std::make_unique<ScriptedClient>(
        std::vector<LlmTurnResult>{bigTurn, finalText("ok")});
    ScriptedClient* rawClient = scripted.get();
    AiSessionController sess(std::move(scripted));

    sess.submitPrompt("make twelve boxes");
    pumpUntilIdle(sess, ctx);
    ASSERT_FALSE(sess.isBusy())
        << "the cap must have stopped the first prompt without starting a new turn";

    sess.submitPrompt("go on");
    pumpUntilIdle(sess, ctx);

    ASSERT_EQ(rawClient->capturedCalls().size(), 2u);
    const std::vector<ChatMessage>& secondPromptMessages = rawClient->capturedCalls()[1];
    size_t assistantToolCallCount = 0;
    size_t toolResultCount = 0;
    for (const auto& msg : secondPromptMessages) {
        if (msg.role == ChatRole::Assistant && !msg.toolCalls.empty())
            assistantToolCallCount += msg.toolCalls.size();
        if (msg.role == ChatRole::ToolResult)
            ++toolResultCount;
    }
    ASSERT_EQ(assistantToolCallCount, 12u);
    EXPECT_EQ(toolResultCount, assistantToolCallCount)
        << "every tool_use id from the capped turn must have a matching ToolResult";
}

TEST(AiSessionController, ACaptureViewResultCarriesItsImageIntoTheNextTurnsHistory) {
    // capture_view is the one tool whose ToolResult carries image bytes (see
    // AiToolDispatcher::captureView) - this is the one path that actually
    // exercises ToolResult::imagePng -> ChatMessage::imagePng end to end.
    Document doc;
    History hist;
    PluginContext ctx;
    ctx._bind(&doc, &hist, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {},
             [](std::vector<uint8_t>& out) {
                 out = {0x89, 'P', 'N', 'G'};
                 return true;
             });

    auto scripted = std::make_unique<ScriptedClient>(
        std::vector<LlmTurnResult>{
            toolCall("call_1", "capture_view", {}),
            finalText("Looks good so far."),
        });
    ScriptedClient* rawClient = scripted.get();
    AiSessionController sess(std::move(scripted));

    sess.submitPrompt("check your progress");
    pumpUntilIdle(sess, ctx);

    ASSERT_EQ(rawClient->capturedCalls().size(), 2u);
    const std::vector<ChatMessage>& secondCall = rawClient->capturedCalls()[1];
    ASSERT_EQ(secondCall.size(), 3u);
    EXPECT_EQ(secondCall[2].role, ChatRole::ToolResult);
    EXPECT_EQ(secondCall[2].imagePng, (std::vector<uint8_t>{0x89, 'P', 'N', 'G'}));
}

TEST(AiSessionController, CancelStopsAnInFlightTurnAndSurfacesItAsNotAnError) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    AiSessionController sess(std::make_unique<SlowCancellableClient>());

    sess.submitPrompt("do something slow");
    ASSERT_TRUE(sess.isBusy());
    // Give the background thread a moment to actually start (and enter its
    // wait loop) before cancelling, so this exercises stopping something
    // genuinely in flight rather than racing submitPrompt itself.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sess.cancel();
    pumpUntilIdle(sess, ctx);

    EXPECT_FALSE(sess.isBusy());
    ASSERT_FALSE(sess.scrollback().empty());
    const auto& last = sess.scrollback().back();
    EXPECT_EQ(last.kind, AiSessionController::ScrollbackLine::Kind::ToolSummary)
        << "a deliberate cancel must not read as a failure";
    EXPECT_EQ(last.text, "Cancelled.");
}

TEST(AiSessionController, CancelWithNothingInFlightIsANoOp) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    AiSessionController sess(std::make_unique<ScriptedClient>(
        std::vector<LlmTurnResult>{finalText("Hi.")}));

    sess.cancel(); // nothing submitted yet - must not crash or misbehave
    EXPECT_FALSE(sess.isBusy());

    sess.submitPrompt("hello");
    pumpUntilIdle(sess, ctx);
    EXPECT_EQ(sess.scrollback().back().text, "Hi.");
}

TEST(AiSessionController, ElapsedSecondsIsZeroWhenIdleAndPositiveWhileBusy) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    AiSessionController sess(std::make_unique<SlowCancellableClient>());

    EXPECT_EQ(sess.elapsedSeconds(), 0.0);

    sess.submitPrompt("do something slow");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_GT(sess.elapsedSeconds(), 0.0);

    sess.cancel();
    pumpUntilIdle(sess, ctx);
    EXPECT_EQ(sess.elapsedSeconds(), 0.0)
        << "resets back to 0 once the turn is no longer in flight";
}

TEST(AiSessionController, ClearEmptiesTheScrollbackAndHistory) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    AiSessionController sess(std::make_unique<ScriptedClient>(
        std::vector<LlmTurnResult>{finalText("Made it.")}));

    sess.submitPrompt("hello");
    pumpUntilIdle(sess, ctx);
    ASSERT_FALSE(sess.scrollback().empty());

    sess.clear();
    EXPECT_TRUE(sess.scrollback().empty());
}

TEST(AiSessionController, ClearIsANoOpWhileATurnIsInFlight) {
    Document doc;
    History hist;
    PluginContext ctx = makeCtx(doc, hist);
    AiSessionController sess(std::make_unique<SlowCancellableClient>());

    sess.submitPrompt("do something slow");
    ASSERT_TRUE(sess.isBusy());
    sess.clear(); // must not disturb the in-flight turn
    EXPECT_FALSE(sess.scrollback().empty())
        << "the User line from submitPrompt must survive a clear() while busy";

    sess.cancel();
    pumpUntilIdle(sess, ctx);
}
