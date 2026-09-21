# Offer Lathe when a sketch profile (region) is selected - plan

Status: DRAFT. Codex plan review REQUIRED but Codex is usage-capped until
2026-09-22 17:03; independent review recorded below, Codex round queued before
anything goes upstream. Branch `feature/sketch-fillet-and-revolve-tools`.

## Problem

The Lathe / Revolve button is only offered when a WHOLE sketch or a body is
selected (`Toolbar.cpp` rail: sketch-selected branch ~line 306, body branch
~432; classic sidebar: the Mirror/Revolve row ~905). Clicking a closed profile
in the viewport selects a sketch REGION, and neither toolbar's region branch
offers Lathe (rail region branch 248-276: Push/Extrude/Subtract/Edit/Move/
Rotate; classic `renderSketchRegionTools` 1181+: Push-Pull/Extrude/Subtract/
Edit). Users therefore cannot find Lathe in the most natural flow.
`Application::beginRevolve` (Application_Dialogs.cpp:3565) ALREADY accepts a
region: it captures `e.sketchId` for `SelectionType::SketchRegion`, defaults to
Sweep (Lathe) mode, and its own comment describes "revolving a selected
profile". Only the button is missing. The command palette action "Revolve"
(RevolvePlugin) already works on a region; it is just undiscoverable.

## Change (UI only)

1. Rail region branch: add `add(MZ_ICON_LATHE, "Lathe", ToolAction::Revolve,
   false, "Spin the sketch's largest closed profile around an axis into a solid.")` after
   Subtract. Same label/tooltip strings as the sketch-selected branch, which
   are already in the i18n catalogue (`tools/i18n_catalogue.py` lines 598,
   1099), so no catalogue change.
2. Classic `renderSketchRegionTools`: add a "Lathe" button after Subtract with
   the same tip; not gated on `catalogOffers` (Revolve is not a catalogue
   tool - RevolvePlugin registers only a command).
3. No change to `beginRevolve`, `RevolveOp`, the touch layout (it consumes the
   rail's items), or any i18n.

## Verification

- Build; run ctest (no test touches the toolbar).
- Manual: draw a closed profile, Finish Sketch, click the profile: Lathe is on
  the rail / sidebar; click it: popup opens in Lathe mode with the profile
  captured. Also confirm multi-region selection (Ctrl+click) opens it with the
  first region's sketch (existing behaviour).
- Gates: em-dash, i18n drift, units audit, `far`/`near` grep.

## Risks

- R1. Multi-region selection: `beginRevolve` takes the FIRST sketch id and
  `applyRevolve` revolves the sketch's LARGEST region, not the clicked one.
  A user clicking a small inner region gets the big one revolved. For a
  region-click button that is a real UX mismatch, so the tooltip states it
  ("largest closed profile", new i18n entry in 5 languages) instead of saying
  "sketch profile". Independent review (advisor) required this wording. Not fixed here
  (the revolve plan's region-handling decision covers it).
- R2. Button count/ordering on the rail; low risk.

## Out of scope

Everything in `2026-09-20-revolve-sketch-ai-tool.md`, the fillet tool, and any
RevolveOp hardening.
