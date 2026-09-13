# Second-Review Fix Report — AI Assistant (PR #103)

Fixes for the 5-reviewer external panel findings on top of the prior whole-branch review fix (d724966).

## Fix 1 — Mobile guards: `__ANDROID__` -> `MZ_MOBILE`, iOS exclusion added

- `src/app/Application_Dialogs.cpp`: added `#include "../platform_defs.h"`; wrapped the two
  `#include "../ai/AnthropicClient.h"` / `"../ai/OpenAiCompatibleClient.h"` lines in
  `#if !defined(MZ_MOBILE) ... #endif`; changed the Settings-tab body guard from
  `#if !defined(__ANDROID__)` to `#if !defined(MZ_MOBILE)`.
- `src/plugins/ForceLink.cpp`: added `#include "../platform_defs.h"`; changed both the
  `forceLink_AiAssistant()` declaration guard and its call-site guard to `#if !defined(MZ_MOBILE)`.
- `ios/CMakeLists.txt`: added, right after the existing `UpdateChecker.cpp` exclusion:
  ```
  list(FILTER MZ_SOURCES EXCLUDE REGEX "/src/ai/")                # AI Assistant is network-only; no INTERNET-equivalent entitlement requested for iOS
  list(FILTER MZ_SOURCES EXCLUDE REGEX "/AiAssistantPlugin\\.cpp$") # same reason as above
  ```
- Verified: `find src/ai -name '*.cpp'` lists exactly 5 files (AiSessionController.cpp,
  AiToolDispatcher.cpp, AiToolSchema.cpp, AnthropicClient.cpp, OpenAiCompatibleClient.cpp);
  the new iOS regexes are byte-identical to Android's existing ones
  (`android/app/jni/src/CMakeLists.txt` lines 67-68), so they match the same files.
  `MZ_MOBILE` is spelled identically everywhere (case-sensitive) to `src/platform_defs.h`'s
  definition.

## Fix 2 — Box height/depth swap

`src/ai/AiToolDispatcher.cpp`'s `addPrimitive` Box case now calls
`op->setBoxExtents(w, d, h)` (was `(w, h, d)`), matching `PrimitiveOp`'s documented
W(x)/D(y)/H(z) argument order. Added a comment explaining the order.

Test added: `AiToolDispatcher.AddBoxWithDifferentHeightAndDepthLandsOnTheCorrectWorldAxes`
— creates a box with height=20, depth=30 and asserts the world-space Y extent is 20
(height) and Z extent is 30 (depth), using `PrimitiveOp`'s `worldPnt` mapping
(user Z->world Y, user Y->world Z). Passed.

## Fix 3 — move_body/rotate_body coordinate convention

`src/ai/AiToolDispatcher.cpp`:
- `moveBody`: `op->setTranslation(dx, dz, dy)` (was `(dx, dy, dz)`), with a comment
  referencing `PrimitiveOp.cpp`'s `worldPnt()` and warning not to revert the order.
- `rotateBody`: `op->setRotation(ax, az, ay, angle)` (was `(ax, ay, az, angle)`), same
  rationale applied to the rotation axis.

Tests added in `tests/test_ai_tool_dispatcher.cpp`:
- `AiToolDispatcher.MoveBodyRoundTripsBackToTheOriginalPosition` — creates a sphere via
  `add_sphere` at (x=1,y=10,z=3), moves it by (dx=4,dy=-6,dz=2), asserts the intermediate
  world position matches `(ox+dx, oz+dz, oy+dy)` (proving the axis swap, not just that
  forward+inverse cancel), then undoes the move and asserts it lands back at the original
  world bbox origin. All passed.
- `AiToolDispatcher.MoveBodyRejectsAFractionalBodyId`, `...RejectsAnOutOfRangeBodyId`
  (shared with Fix 4, see below).

## Fix 4 — `requireBodyId` unchecked double->int cast

`src/ai/AiToolDispatcher.cpp`'s `requireBodyId` now checks, before casting:
`std::isfinite(raw)` and that `raw` is within `int`'s representable range (rejecting with
a clear error), and that `raw == std::floor(raw)` (rejecting fractional ids with
`"'body_id' must be a whole number, got 1.9"` style message). Added `#include <cmath>`
and `<limits>`.

Tests added: `AiToolDispatcher.MoveBodyRejectsAFractionalBodyId` (body_id=1.9),
`AiToolDispatcher.MoveBodyRejectsAnOutOfRangeBodyId` (body_id=1e20). Both passed.

## Fix 5 — `optNumber` silently defaults on wrong type

Replaced `optNumber`'s single-return-value shape with
`bool optionalNumber(args, key, out, fallback, err)`, which distinguishes absent
(returns true, `out=fallback`) from present-but-non-numeric (returns false with an
error) from present-and-numeric (returns true, `out=value`). `addPrimitive`'s x/y/z
handling now uses it and returns a validation error on a wrong-typed value instead of
silently defaulting to 0.

Test added: `AiToolDispatcher.AddBoxRejectsANonNumericOptionalPosition` — `add_box` with
`x: "100"` (a string) is rejected and no body is created. Passed.

## Fix 6 — `OpenAiCompatibleClient::parseResponse` unchecked indexing

`src/ai/OpenAiCompatibleClient.cpp`: replaced the unchecked
`body["choices"][0]["message"]` with a guarded check (`contains`/`is_object`) that
returns `LlmTurnResult{ok=false, error="Response choice had no 'message'"}` on failure.
Inside the `tool_calls` loop, each entry is checked for `is_object()` and a `function`
object before being indexed; a malformed entry is `continue`d (skipped), not indexed
blindly.

Tests added: `OpenAiCompatibleClient.ParseResponseRejectsAChoiceWithNoMessage` (a choice
that's an empty object), `OpenAiCompatibleClient.ParseResponseSkipsAToolCallMissingFunction`
(a tool_calls entry with only an "id", no "function" — asserted the response still
parses `ok=true` with `toolCalls` empty, i.e. the one bad entry is skipped rather than
failing the whole response). Both passed.

## Fix 7 — `poll()`'s tool-execution loop not exception-guarded

`src/ai/AiSessionController.cpp`'s `poll()` now wraps each `executeTool(...)` call in
its own try/catch, converting any `std::exception` to
`ToolResult{false, "internal error: " + e.what()}` before it's used to build the
scrollback line and the `ChatMessage` fed back to the model.

Existing tests continue to pass (no scripted client throws, so behavior is unchanged
for them); the exception-safety itself was exercised by code review (the catch handler
mirrors the existing `m_future.get()` catch pattern one function up).

## Fix 8 — Test Connection future not exception-guarded

`src/app/Application_Dialogs.cpp`: wrapped `testFuture.get()` in try/catch, setting
`testResultIsError = true; testResultText = "Failed: " + e.what();` on catch, matching
the existing non-exceptional failure path's messaging.

Also added a comment above the Test Connection block documenting the accepted
plugin-ownership deviation (client construction lives in Application_Dialogs.cpp
rather than the AiAssistant plugin), per the "what not to do" list.

## Fix 9 — Importing Settings wipes AI configuration

`src/app/Application.cpp`'s `importSettings()`: now captures
`AppSettings::AiSettings savedAiSettings = m_aiSettings;` before `applyAppSettings(s)`
and restores `m_aiSettings = savedAiSettings;` immediately after, so an ordinary
settings import never touches the live AI configuration.

Also added a comment on `Application_Dialogs.cpp`'s `buffersLoaded` static explaining
it's an accepted limitation (no "reload on dialog open" pattern exists anywhere else in
this file to follow, and Import no longer touches `m_aiSettings` after this fix, so the
staleness window that mattered is closed).

## Fix 10 — 8-step cap checked once per turn, not per call

`src/ai/AiSessionController.cpp`'s `poll()`: the cap check now happens INSIDE the
`for (const auto& call : result.toolCalls)` loop, before each `executeTool` call —
if the cap is already hit, it pushes the "Stopped after N steps." scrollback line and
`return`s immediately without executing (or starting another turn for) the remaining
calls in that turn. `m_stepCount` is incremented per call, not per turn. The existing
end-of-loop cap check is kept (now checks after the whole turn completes, deciding
whether to `startTurn()` for the next turn).

Test added: `AiSessionController.ATurnWithMoreToolCallsThanTheCapStopsPartway` — scripts
one `LlmTurnResult` with 12 `add_box` tool calls in a single turn; asserts exactly 8
bodies are created, not 12. Passed. The pre-existing
`StopsAfterTheStepCapInsteadOfLoopingForever` test (20 separate one-call turns) also
still passes.

## Fix 11 — Accompanying assistant text dropped when tool calls present

`src/ai/AnthropicClient.cpp`'s `parseResponse`: `r.finalText = text;` is now
unconditional (was `if (r.toolCalls.empty()) r.finalText = text;`).

`src/ai/OpenAiCompatibleClient.cpp`'s `parseResponse`: text is now read from
`message["content"]` unconditionally into a local `text`, and `r.finalText = text;` is
set unconditionally at the end (was only set in an `else if` branch when no tool_calls
were present).

`src/ai/AiSessionController.cpp`'s `poll()`: when `result.toolCalls` is non-empty and
`result.finalText` is non-empty, a `{Kind::Assistant, result.finalText}` scrollback line
is pushed BEFORE the tool-execution loop. This text is NOT added to `m_messages` — the
Assistant-with-toolCalls `ChatMessage` still has `text=""`, preserving the existing,
tested history-replay shape.

Tests added: `AnthropicClient.ParseResponseCapturesTextAlongsideToolCalls`,
`OpenAiCompatibleClient.ParseResponseCapturesTextAlongsideToolCalls`. Both passed.

## Fix 12 — No way to reopen the AI Assistant overlay after closing it

`src/plugins/AiAssistantPlugin.cpp`: switched from `ctx.registerCommand(...)` (no
consumer anywhere in the codebase) to `ctx.registerToolbarButton(...)` (real consumer:
`src/ui/Toolbar.cpp` / `LayoutCommon.cpp`), following the `BoundaryFillPlugin.cpp`
pattern — name/section "AI Assistant", `SelectionContext::Always`, priority 100, an
action lambda toggling `g_overlayOpen`, no `toolFactory`, and a tooltip. Verified by
reading the code (no live-GUI test expected per this project's convention): the
lambda captures nothing but flips the same `g_overlayOpen` the overlay checks, so
clicking the toolbar button after closing the overlay reopens it. Compiles clean as
part of the full `materializr` rebuild below.

## Known-limitation comment (cross-project mutation race)

Added to `src/ai/AiSessionController.h` above the class declaration, documenting that
`AiSessionController` is bound to whatever Document/History the current
`PluginContext` points at, and an in-flight tool call executes against the NEW active
project if the user switches projects mid-turn. Not fixed in v1 per the brief.

## Minor items also fixed

- **Torus major/minor ordering**: `AiToolDispatcher.cpp`'s Torus case now rejects
  `major_radius <= minor_radius` before `pushOperation`, instead of letting `PrimitiveOp`
  reject it after wasting a history revision.
- **Empty-tool_calls-discards-text edge case**: resolved as a side effect of Fix 11 —
  `finalText` capture is now fully unconditional in both clients.

## Verification

### All 6 AI test suites (built via `cmake --build . --target <name>`, then run directly)

```
test_ai_settings:            3 tests, 3 passed
test_ai_tool_schema:         5 tests, 5 passed
test_ai_tool_dispatcher:    12 tests, 12 passed  (was 7; added 5)
test_ai_anthropic_client:    9 tests, 9 passed   (was 8; added 1)
test_ai_openai_client:      10 tests, 10 passed  (was 7; added 3)
test_ai_session_controller:  7 tests, 7 passed   (was 6; added 1)
```
All new tests named in the brief are present and passing:
`AddBoxWithDifferentHeightAndDepthLandsOnTheCorrectWorldAxes` (Fix 2 test, exact name
per brief wasn't prescribed - matches its described assertion),
`MoveBodyRoundTripsBackToTheOriginalPosition`, `MoveBodyRejectsAFractionalBodyId`,
`MoveBodyRejectsAnOutOfRangeBodyId`, `AddBoxRejectsANonNumericOptionalPosition`,
`ParseResponseRejectsAChoiceWithNoMessage`, `ParseResponseSkipsAToolCallMissingFunction`,
`ATurnWithMoreToolCallsThanTheCapStopsPartway`,
`ParseResponseCapturesTextAlongsideToolCalls` (both clients).

### `materializr` app target — clean rebuild

Reconfigured (`cmake .`) and rebuilt after removing `CMakeFiles/materializr.dir/`
entirely: full recompile of all `materializr` target sources, linked successfully,
`[100%] Built target materializr`. One pre-existing, unrelated warning
(`ConstructionPlaneOp.cpp` switch-case coverage) - not touched by this pass.

### Full ctest

`ctest -j 4`: **119/119 passed** once run unsandboxed (3 tests -
`test_parallel_mesh`, `test_stl_import`, `test_project_thumbnail` - fail under the
Claude Code sandbox due to denied `/tmp` writes, a known, documented, pre-existing
environment artifact unrelated to this change; confirmed by re-running just those 3
unsandboxed, where all 3 pass). Sandboxed run: 116/119 passed, same 3 known failures.

### Style gates

```
python3 tools/no_em_dashes.py     -> "no em-dashes", exit 0
python3 tools/no_double_build.py  -> "no double-built booleans", exit 0
python3 tools/units_audit.py      -> exit 0
grep -rnE '\bfar\b|\bnear\b' src/ai/ src/plugins/AiAssistantPlugin.cpp -> no matches
```

All checks pass.
