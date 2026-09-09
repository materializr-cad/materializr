# Releasing

Materializr ships through two GitHub-release channels. Both are driven entirely
by **publishing a GitHub Release** - the `linux`, `windows`, and `macos`
workflows trigger on `release: [published]`, build their platform, and attach
the assets (AppImage + `.zsync`, `.exe`, `.dmg`) to whichever release fired.

## Channels

| Channel | Tag form | GitHub flag | Who sees it |
|---|---|---|---|
| **Stable** | `vMAJOR.MINOR.PATCH` (e.g. `v1.2.8`) | normal release | everyone |
| **Beta** | `vMAJOR.MINOR.PATCH-beta.N` (e.g. `v1.3.0-beta.1`) | **pre-release** | only users who tick *Settings → Include pre-release (beta) builds* |

The in-app update check keeps the channels apart for free:

- **Stable** queries GitHub's `/releases/latest`, which **excludes pre-releases**
  by definition - so a beta never nags a stable user.
- **Beta** (opt-in) queries `/releases` (the full list, newest first) and offers
  the most recent build, pre-release or not.

Version ordering is semver-aware: `1.3.0-beta.1 < 1.3.0-beta.2 < 1.3.0`, and
`beta.2 < beta.10` (numeric, not lexical). So a beta tester is offered each new
beta and then the final release that supersedes it.

## Cut a beta (pre-release)

1. Make sure `main` is green and the work you want to expose is pushed.
2. Pick the next version's tag with a `-beta.N` suffix:

   ```bash
   gh release create v1.3.0-beta.1 \
     --prerelease \
     --title "1.3.0 Beta 1" \
     --notes "Early access to <feature>. Feedback welcome; expect rough edges."
   ```

   `--prerelease` is what marks it as a pre-release (the channel divider). The
   three platform workflows build and attach assets automatically - no manual
   uploads.
3. Increment `N` for each subsequent beta of the same target version
   (`-beta.2`, `-beta.3`, …).

> The native `versionName`/`versionCode` in `android/app/build.gradle` and the
> desktop `MATERIALIZR_VERSION` are independent of the tag - bump them when the
> beta represents a real version change you want shown in-app.

## Promote a beta to stable

When the target version is ready, publish a normal (non-pre-release) release:

```bash
gh release create v1.3.0 \
  --title "1.3.0" \
  --notes "<release notes>"
```

No `--prerelease` flag → it becomes the `/releases/latest`, so every stable user
is offered it, and beta testers (who were on `1.3.0-beta.N`) are offered the
final `1.3.0` since `beta.N < final`.

## Notes

- Pre-releases still appear on the public Releases page (collapsed under
  "Pre-release"); that's expected - discoverability for testers who want to grab
  one manually.
- To stop offering betas, just don't publish any; opted-in users simply see the
  latest stable until a new beta appears.
