// Manual test harness for the AI Assistant feature - NOT a gtest, not
// registered with ctest. Drives the real AiSessionController /
// AiToolDispatcher / OpenAiCompatibleClient code path (same classes the app
// uses) against an in-memory Document, talking over real HTTP to whatever
// OpenAI-compatible server is listening at the URL below - normally a mock
// server standing in for a small/dumb model, so the tool schema and system
// prompt's clarity can be probed by hand without burning real API usage or
// downloading a real local model.
//
// Usage: ai_harness "first user prompt" ["second user prompt" ...]
// Each argv entry is submitted as a separate user turn, in order, on the
// SAME document/session - so a later prompt can reference geometry an
// earlier prompt created, exactly like a real chat.
//
// Env:
//   AI_HARNESS_URL   base URL, default http://127.0.0.1:8899/v1
//   AI_HARNESS_MODEL model name sent in the request body, default "dumb-test-model"

#include "ai/AiSessionController.h"
#include "ai/OpenAiCompatibleClient.h"
#include "core/Document.h"
#include "core/History.h"
#include "plugin/PluginContext.h"

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace materializr;
using namespace materializr::ai;

namespace {

const char* kindLabel(AiSessionController::ScrollbackLine::Kind k) {
    switch (k) {
        case AiSessionController::ScrollbackLine::Kind::User: return "USER";
        case AiSessionController::ScrollbackLine::Kind::Assistant: return "ASSISTANT";
        case AiSessionController::ScrollbackLine::Kind::ToolSummary: return "TOOL";
        case AiSessionController::ScrollbackLine::Kind::Error: return "ERROR";
    }
    return "?";
}

void dumpBodies(Document& doc) {
    std::vector<int> ids = doc.getAllBodyIds();
    if (ids.empty()) {
        std::printf("  (no bodies in the document)\n");
        return;
    }
    for (int id : ids) {
        const TopoDS_Shape& shape = doc.getBody(id);
        if (shape.IsNull()) continue;
        Bnd_Box box;
        try { BRepBndLib::Add(shape, box); } catch (...) { continue; }
        if (box.IsVoid()) { std::printf("  id %d \"%s\": (empty bbox)\n", id, doc.getBodyName(id).c_str()); continue; }
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        std::printf("  id %d \"%s\": world bbox [%.1f..%.1f, %.1f..%.1f, %.1f..%.1f]%s\n",
                    id, doc.getBodyName(id).c_str(), x0, x1, y0, y1, z0, z1,
                    doc.isBodyVisible(id) ? "" : " [hidden]");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s \"prompt one\" [\"prompt two\" ...]\n", argv[0]);
        return 2;
    }

    const char* urlEnv = std::getenv("AI_HARNESS_URL");
    const char* modelEnv = std::getenv("AI_HARNESS_MODEL");
    std::string baseUrl = urlEnv ? urlEnv : "http://127.0.0.1:8899/v1";
    std::string model = modelEnv ? modelEnv : "dumb-test-model";
    std::printf("=== ai_harness: baseUrl=%s model=%s ===\n\n", baseUrl.c_str(), model.c_str());

    Document doc;
    History hist;
    PluginContext ctx;
    ctx._bind(&doc, &hist, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {},
             [](std::vector<uint8_t>& out) {
                 // Stand-in for a real render - capture_view just needs SOME
                 // bytes to exercise the plumbing (this harness has no GL
                 // context to actually rasterize the viewport).
                 out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
                 return true;
             });

    auto client = std::make_unique<OpenAiCompatibleClient>("harness-dummy-key", baseUrl, model);
    AiSessionController session(std::move(client));

    size_t printedUpTo = 0;
    auto drainScrollback = [&]() {
        const auto& sb = session.scrollback();
        for (; printedUpTo < sb.size(); ++printedUpTo)
            std::printf("[%s] %s\n", kindLabel(sb[printedUpTo].kind), sb[printedUpTo].text.c_str());
    };

    for (int i = 1; i < argc; ++i) {
        std::printf("\n----- submitPrompt(%d/%d): %s -----\n", i, argc - 1, argv[i]);
        session.submitPrompt(argv[i]);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        while (session.isBusy()) {
            session.poll(ctx);
            drainScrollback();
            if (std::chrono::steady_clock::now() > deadline) {
                std::fprintf(stderr, "harness: turn %d timed out after 10 minutes\n", i);
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        drainScrollback();
        std::printf("\n-- document state after prompt %d --\n", i);
        dumpBodies(doc);
    }

    std::printf("\n=== harness done ===\n");
    return 0;
}
