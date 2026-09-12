# AI Assistant Design

**Status: Implemented** (2026-09-12, Task 11 integration pass complete)

## Goal

A user types what they want in plain language ("make a 20mm cube with a 5mm
hole through it") and the app builds it. Setup must be quick: paste an API
key (or point at a local server) and go.

## Platform Scope: Desktop Only

This feature is **not built for Android**. Every backend (Anthropic,
OpenAI, and even a local Ollama/LM Studio server on `localhost`) is a
network call, and Android gates *all* socket access - including loopback -
behind the `INTERNET` manifest permission. The app currently declares no
such permission (`android/app/src/main/AndroidManifest.xml`), and adding
it just for this feature is explicitly out of scope. The only mobile-native
alternative, an on-device model bundled into the app, is a fundamentally
different and much larger feature than "paste a key and go" - not this spec.

Excluded from the Android build the same way `UpdateChecker.cpp` (this
codebase's only other network dependency) already is:
`android/app/jni/src/CMakeLists.txt`'s source glob gets one more
`list(FILTER MZ_SOURCES EXCLUDE REGEX "/plugins/ai_assistant/")` line, and
`src/plugins/ForceLink.cpp`'s call to `forceLink_AiAssistant()` (declaration
included) is wrapped in `#if !defined(__ANDROID__)` - mirroring how
`Application.cpp` already guards its update-check call sites. This is a
genuinely platform-specific exclusion (the Android permission model itself),
not a UI/touch-mode preference, so it's the correct use of `__ANDROID__`
per this repo's own convention.

## Non-goals (v1)

- Android/mobile support of any kind (see Platform Scope above).
- Sketching, extrude-from-sketch, mates, patterns. The AI only creates
  primitives and combines/transforms them (booleans, move/rotate/scale).
- Multi-provider selection at runtime beyond the two shapes below. No
  provider plugin system.
- A preview/confirm step before an AI action lands. Every action is a normal
  undoable `Operation`; the user relies on Ctrl+Z, same as everywhere else
  in the app.
- Streaming token-by-token UI. A turn either finishes or it doesn't; the
  overlay shows a spinner while waiting.

## Architecture

One new plugin owns the whole feature end to end, per this project's rule
that features live in plugins and core stays generic infrastructure:

```
src/ai/                     - testable logic, linked into materializr_core
    AiTypes.h                    - shared ChatMessage/ToolCall/LlmTurnResult types
    AiToolSchema.h/.cpp          - the fixed tool definitions, provider-agnostic
    AiToolDispatcher.h/.cpp      - tool call -> concrete Operation -> pushOperation
    LlmClient.h                  - abstract sendTurn() interface
    AnthropicClient.h/.cpp       - Messages API implementation
    OpenAiCompatibleClient.h/.cpp - Chat Completions shape (OpenAI/Ollama/LM Studio)
    AiSessionController.h/.cpp   - conversation state machine (see Data Flow)
src/plugins/
    AiAssistantPlugin.cpp    - REGISTER_PLUGIN, toolbar button, OverlayContribution
                               (untested UI glue only)
```

This split - testable logic under `src/ai/`, untested UI glue as a single
`src/plugins/AiAssistantPlugin.cpp` - was a deliberate, superior adaptation
made during planning so the logic could be linked into the test-only
`materializr_core` library, matching the existing `Mate.cpp`/`MateSolver.cpp`
(testable) vs `MatePlugin.cpp` (untested UI) precedent.

One small core addition: `PluginContext` gets a way to read the AI-relevant
slice of `AppSettings` (provider, keys, base URL, model):

```cpp
// PluginContext.h
const AiSettings& aiSettings() const; // AiSettings is the small slice of
                                       // AppSettings defined below, not the
                                       // whole settings struct
```

This mirrors the existing precedent of `PluginContext::markDocumentDirty()`
being added specifically for MatePlugin's need - a narrow, justified
extension of the plugin/host contract, not a new general settings-bus.

The conversation held by `AiSessionController` is in-memory only, scoped to
one plugin instance for the app's lifetime - it is NOT persisted to disk
and does NOT survive an app restart. Nothing in this design requires it to;
a fresh conversation each launch is expected v1 behavior, not a gap.

## Components

### AppSettings additions (`src/io/Settings.h`)

```cpp
enum class AiProvider { Anthropic, OpenAiCompatible };
struct AiSettings {
    AiProvider provider = AiProvider::Anthropic;
    std::string anthropicApiKey;
    std::string anthropicModel = "claude-sonnet-4-5";
    std::string openAiApiKey;
    std::string openAiBaseUrl = "https://api.openai.com/v1";
    std::string openAiModel = "gpt-4o";
};
AiSettings ai; // one new member on AppSettings
```

All fields under `ai` are excluded from `SettingsIO::exportJson`, the same
way `lastProjectPath` already is - an exported settings backup must not leak
a key. They persist via the same plain-text `key = value` mechanism
everything else in `AppSettings` uses; no migration needed since unknown
keys are already ignored and missing keys already fall back to defaults.

### Settings dialog: new "AI Assistant" tab

`Application_Dialogs.cpp`'s `renderSettings()`, one more
`ImGui::BeginTabItem` block, same shape as every existing tab:

- Provider combo (Anthropic / OpenAI-compatible)
- Anthropic: API key field (`ImGuiInputTextFlags_Password`), model name field
- OpenAI-compatible: API key field, base URL field (pre-filled with the
  OpenAI default; the user overwrites it with `http://localhost:11434/v1`
  for Ollama or LM Studio's local endpoint), model name field
- "Test Connection" button: fires one minimal request (no tools, prompt
  "reply with OK") and shows a green check or the raw error inline. This is
  the whole point of "quick and simple" - the user gets a yes/no on their
  setup before ever touching the chat panel.

### Tool schema (`AiToolSchema.h`)

A small, fixed, provider-agnostic list, each entry a name + parameter list
(name, type, required/optional, description) as plain data:

- `add_box(width, height, depth, x, y, z)`
- `add_cylinder(radius, height, x, y, z)`
- `add_sphere(radius, x, y, z)`
- `add_cone(bottom_radius, top_radius, height, x, y, z)`
- `add_torus(major_radius, minor_radius, x, y, z)`
- `move_body(body_id, dx, dy, dz)`
- `rotate_body(body_id, axis_x, axis_y, axis_z, angle_degrees)`
- `scale_body(body_id, factor)` (uniform only in v1 - `TransformOp` supports
  per-axis too, but a single factor is the common case and keeps the schema
  small; per-axis is a trivial follow-up if wanted)
- `boolean_op(target_body_id, tool_body_id, mode)` where `mode` is one of
  `union` / `subtract` / `intersect`

Each `LlmClient` implementation formats this same list into its own
provider's wire shape (Anthropic's `tools` array with JSON-schema `input_schema`,
OpenAI's `tools` array with `function.parameters`). The schema itself has
exactly one definition, shared by both.

`x, y, z` on the "add" tools are optional, default 0,0,0 (world origin) -
this covers the common case ("make a cube") without forcing the model to
always guess a position, while still letting a multi-step prompt place
things apart from each other.

### AiToolDispatcher

Pure logic, no network, no ImGui:

```cpp
struct ToolResult { bool ok; std::string message; }; // message doubles as
    // the human-readable summary AND the text fed back to the LLM as the
    // tool result

ToolResult execute(materializr::PluginContext& ctx,
                    const std::string& toolName,
                    const JsonValue& args);
```

Validates args first (positive dimensions, a referenced `body_id` actually
exists in the document, `mode` is one of the three allowed strings) and
returns `{false, "<reason>"}` without touching the document on any
validation failure. On success, builds the matching concrete `Operation`
(`PrimitiveOp` for the five `add_*` tools via its `Kind` enum,
`TransformOp` for move/rotate/scale via its `TransformType` enum,
`BooleanOp` for `boolean_op`), calls
`ctx.history().pushOperation(std::move(op), ctx.document())`, and returns
`{true, "<short summary, e.g. Created Box (body 7)>"}`.

This is the exact same imperative shape every existing interactive
op-commit already uses (see `Application::commitPrimitivePopup()`) - no new
document-mutation pattern, just a non-ImGui caller of the existing one.

### LlmClient

```cpp
struct ToolCall { std::string id, name; JsonValue args; };
struct LlmTurnResult {
    bool ok;
    std::string error;           // set iff !ok
    std::string finalText;       // set iff ok and toolCalls is empty
    std::vector<ToolCall> toolCalls;
};

class LlmClient {
public:
    virtual ~LlmClient() = default;
    // Blocking. Called ONLY from the background worker thread (see Data
    // Flow) - never from the main/render thread.
    virtual LlmTurnResult sendTurn(const std::vector<ChatMessage>& messages,
                                   const std::vector<ToolDef>& tools) = 0;
};
```

`AnthropicClient` and `OpenAiCompatibleClient` each: build the request JSON
via `nlohmann::json`, POST with libcurl (mirroring `UpdateChecker.cpp`'s
existing raw-libcurl pattern - HTTPS-pinned, capped response size, 5s
connect / 20s total timeout, a little more generous than the update
checker's since an LLM completion is slower than a GitHub API hit), parse
the response into `LlmTurnResult`. Auth header differs (`x-api-key` vs
`Authorization: Bearer`); tool-call shape differs (`content` blocks with
`type: tool_use` vs `choices[0].message.tool_calls`) - both isolated
entirely inside each client, `AiSessionController` only ever sees the
common `LlmTurnResult`.

### New dependency: nlohmann/json

Header-only, pulled in via CMake `FetchContent` - the same mechanism
`tests/CMakeLists.txt` already uses for googletest, so no new build-setup
step for you. Replaces nothing existing (`UpdateChecker`'s hand-rolled
`findJsonString` stays as-is; it's sufficient for the one field it reads
and isn't worth touching for this feature).

## Data Flow

```
User types prompt, hits Send
  -> AiSessionController appends a "user" ChatMessage
  -> disables input, shows spinner
  -> kicks off std::async(sendTurn(messages, tools)) on the CONFIGURED client
     (same std::future + per-frame wait_for(0) poll pattern as
     UpdateChecker - network-only work off-thread, zero Document access
     off-thread)

Overlay's OverlayContribution::render() (already called every frame) polls:
  future not ready -> nothing to do this frame
  future ready, !ok -> scrollback error line, re-enable input, STOP
  future ready, ok, finalText set -> scrollback AI message, re-enable input, STOP
  future ready, ok, toolCalls non-empty ->
      for each ToolCall (in order, all on the MAIN thread, inside render()):
          result = AiToolDispatcher::execute(ctx, call.name, call.args)
          scrollback one line ("-> Created Box (body 7)" or "-> Error: ...")
          messages.append(ToolResultMessage{call.id, result.message})
      stepCount += 1
      if stepCount >= kMaxStepsPerPrompt (8):
          scrollback "Stopped after 8 steps." re-enable input, STOP
      else:
          kick off the NEXT std::async turn with the updated messages
```

The loop is entirely driven by the overlay's own per-frame render call -
no new polling infrastructure on `Application`, no cross-plugin wiring.

**The assistant's own tool-use turn MUST be replayed into conversation
history on the next request.** After executing tool calls, `poll()` must
push an `Assistant`-role `ChatMessage` carrying those `ToolCall`s (in
addition to the `ToolResult` messages) before starting the next turn -
otherwise both Anthropic and OpenAI reject the follow-up request, since a
`tool_result`/`role:"tool"` message must follow an assistant message that
actually made the corresponding tool call. This was a real bug found in the
final whole-branch review (missed by every per-task review because the
scripted test fake discarded the `messages` argument); recorded here so it
isn't reintroduced.

## Error Handling

- **No API key configured for the selected provider**: checked before the
  first `std::async` is even created; scrollback shows "Set up your API key
  in Settings -> AI Assistant" and a button that opens Settings directly.
- **Network/HTTP failure** (timeout, DNS, non-2xx, connection refused -
  the last one being the common case for "Ollama isn't running"):
  scrollback error line with the concrete reason, no auto-retry.
- **Unparseable response body**: scrollback "The AI's response couldn't be
  read" plus a truncated raw snippet for debugging, no auto-retry.
- **Invalid tool arguments**: `AiToolDispatcher` rejects before touching the
  document; the rejection reason is fed back to the model as the tool
  result (not shown as a hard error to the user), so the agentic loop lets
  the model retry with corrected arguments on its own within the step
  budget - this is the main payoff of choosing the agentic-loop model.
- **Step cap hit**: a plain scrollback note, not an error; the user can
  just continue with another prompt.

## Testing

- `AiToolDispatcher`: unit tests per tool, same style as every other
  `Operation`-construction test in this codebase - e.g.
  `AiToolDispatcher.AddBoxCreatesABodyWithTheGivenDimensions`,
  `AiToolDispatcher.RejectsANegativeBoxDimension`,
  `AiToolDispatcher.BooleanOpRejectsAnUnknownBodyId`. No network involved.
- `AnthropicClient` / `OpenAiCompatibleClient`: the request-building and
  response-parsing are pure functions of JSON in, JSON/struct out - tested
  against hand-built request/response fixtures, not a real API call. E.g.
  `AnthropicClient.ParsesToolUseBlocksIntoToolCalls`,
  `OpenAiCompatibleClient.ParsesFunctionCallToolCalls`.
- `AiToolSchema`: one test per provider confirming every tool definition
  formats into valid, complete JSON for that provider's shape (a
  round-trip/shape test, not a network test).
- No GUI/overlay test - matches this project's established convention of
  not unit-testing plugin ImGui glue (confirmed with `MatePlugin` this
  session: zero existing tests touch `PluginContext`/`REGISTER_PLUGIN`/ImGui
  rendering anywhere in the codebase).
- Manual smoke test before calling this done: real (or local Ollama) key,
  "make a 20mm cube", confirm a body appears and Ctrl+Z removes it; then a
  multi-step prompt ("a 30mm cube with a 10mm hole through the center") to
  exercise the agentic loop and a boolean.

## Open Risk, Named Rather Than Solved

The settings file (`settings.cfg`) is plain, unencrypted text. Storing an
API key there is consistent with how this app already handles every other
setting, and was the explicitly chosen trade-off (see design discussion),
but it is worth the user knowing: anyone with read access to their config
directory can read the key. No action taken beyond the export-exclusion
already specified above.
