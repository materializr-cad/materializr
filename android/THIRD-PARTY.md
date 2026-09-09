# Third-party components (Android build)

The Android port links the same core dependencies as desktop Materializr, plus
SDL2 in place of GLFW. None are vendored in this repo; `scripts/setup-deps.sh`
fetches and (where needed) cross-compiles them.

| Component | Version | License | Role |
|---|---|---|---|
| [OpenCASCADE Technology](https://github.com/Open-Cascade-SAS/OCCT) | 7.8.1 | LGPL-2.1 (with exception) | CAD geometry kernel |
| [SDL2](https://github.com/libsdl-org/SDL) | 2.30.9 | Zlib | Windowing, input, GL context (desktop + Android) |
| [Dear ImGui](https://github.com/ocornut/imgui) | docking | MIT | UI |
| [GLM](https://github.com/g-truc/glm) | 1.0.1 | MIT | Vector/matrix math |
| [FreeType](https://freetype.org/) | 2.13.3 | FTL / GPL-2.0 | TrueType outlines (linked into OCCT TKService) |

OpenCASCADE is LGPL: it is linked as shared libraries (`libTK*.so`), so the
LGPL's dynamic-linking allowance applies. The cross-build disables the Draw
(Tcl/Tk) harness and other optional modules; see `scripts/setup-deps.sh` for the
exact configuration.

## Bundled fonts

The sketch Text tool ships 10 TrueType fonts (in `assets/fonts/`, also mirrored
under `android/app/src/main/assets/`). All are free/redistributable - SIL Open
Font License 1.1, the Ubuntu Font Licence 1.0, or the Bitstream Vera/Arev
permissive license. Per-font copyright notices, the picker mapping, and the full
license texts are in `assets/fonts/FONT-CREDITS.md` and `assets/fonts/licenses/`.

Note: "Times (Liberation)" and "Arial (Liberation)" are the OFL Liberation fonts,
which are *metric-compatible* stand-ins for the proprietary Microsoft fonts - the
real Times New Roman / Arial are NOT bundled.

Materializr's own source is under the license in the repository root `LICENSE`.
