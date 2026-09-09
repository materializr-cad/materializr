<!-- Thanks for contributing! Keep PRs focused - one logical change per PR. -->

## What this does

<!-- A sentence or two. Link any issue it closes: "Closes #123". -->

## How it was tested

<!-- e.g. "Built build-desktop, ran ctest (all pass), manually verified X." -->

## Checklist

- [ ] Builds on desktop (`cmake --build build-desktop`) and tests pass (`ctest`)
- [ ] If it touches Android, it builds there too - and desktop behavior is unchanged
- [ ] New user-facing **features live in a plugin** (`src/plugins/`), not bolted into core
- [ ] Touch-specific behavior is gated on `materializr::touchMode()`, not `#if __ANDROID__`
- [ ] Code matches the style of the surrounding files

<!-- By submitting, you agree your contribution is licensed under GPL-3.0-or-later. -->
