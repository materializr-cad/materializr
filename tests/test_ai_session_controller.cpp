#include "ai/AiSessionController.h"
#include "core/Document.h"
#include "core/History.h"
#include "plugin/PluginContext.h"

#include <gtest/gtest.h>

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
    LlmTurnResult sendTurn(const std::vector<ChatMessage>&,
                          const std::vector<ToolDef>&) override {
        if (m_next >= m_turns.size()) {
            LlmTurnResult r;
            r.ok = false;
            r.error = "ScriptedClient ran out of scripted turns";
            return r;
        }
        return m_turns[m_next++];
    }
private:
    std::vector<LlmTurnResult> m_turns;
    size_t m_next = 0;
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
    for (int i = 0; i < 100 && sess.isBusy(); ++i) sess.poll(ctx);
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
    EXPECT_LE(doc.getAllBodyIds().size(), 8u)
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
