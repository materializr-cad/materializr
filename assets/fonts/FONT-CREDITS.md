# Bundled fonts

Materializr ships these TrueType fonts for the sketch **Text** tool. All are
free/redistributable; full license texts are in `licenses/`. Copyright notices
are preserved below as required by their licenses.

| Family (picker name) | File | Copyright | License |
|---|---|---|---|
| Mono (JetBrains) | `JetBrainsMono-Regular.ttf` | © JetBrains s.r.o. | SIL OFL 1.1 |
| Sans (DejaVu) | `DejaVuSans.ttf` | Bitstream Vera / Arev fonts © Bitstream Inc., Tavmjong Bah | Bitstream Vera + Arev (permissive) |
| Serif (DejaVu) | `DejaVuSerif.ttf` | Bitstream Vera / Arev fonts © Bitstream Inc., Tavmjong Bah | Bitstream Vera + Arev (permissive) |
| Times (Liberation) | `LiberationSerif-Regular.ttf` | © Red Hat, Inc. | SIL OFL 1.1 |
| Arial (Liberation) | `LiberationSans-Regular.ttf` | © Red Hat, Inc. | SIL OFL 1.1 |
| Ubuntu | `Ubuntu-Regular.ttf` | © Canonical Ltd. | Ubuntu Font Licence 1.0 |
| Comic (Neue) | `ComicNeue-Regular.ttf` | © The Comic Neue Project Authors | SIL OFL 1.1 |
| Stencil (Black Ops) | `BlackOpsOne-Regular.ttf` | © The Black Ops One Project Authors (Matt McInerney) | SIL OFL 1.1 |
| Impact (Anton) | `Anton-Regular.ttf` | © The Anton Project Authors (Vernon Adams) | SIL OFL 1.1 |
| Script (Pacifico) | `Pacifico-Regular.ttf` | © The Pacifico Project Authors (Vernon Adams) | SIL OFL 1.1 |
| 中文 (Noto Sans SC) | `NotoSansSC.ttf` (subset: GB2312 level-1 hanzi + symbols + the UI strings) | © 2014-2021 Adobe (http://www.adobe.com/), Reserved Font Name 'Source'; Noto is a trademark of Google LLC | SIL OFL 1.1 |
| *(UI icons - not in the Text picker)* | `Iconoir.ttf` | Iconoir © Luca Burgio; font build © Silbad (iconoir-font) | MIT |

License texts in `licenses/`:
- `OFL-1.1.txt` - SIL Open Font License 1.1 (JetBrains Mono, Liberation, Comic Neue, Black Ops One, Anton, Pacifico).
- `NotoSansSC-OFL.txt` - SIL Open Font License 1.1 (Noto Sans SC / Source Han Sans; Adobe copyright and Reserved Font Name notice).
- `UFL-1.0.txt` - Ubuntu Font Licence 1.0 (Ubuntu).
- `DejaVu-copyright.txt` - Bitstream Vera + Arev (DejaVu).
- `Liberation-copyright.txt` - distribution copyright/license for the Liberation family.
- `Iconoir-LICENSE.txt` - MIT (Iconoir icon glyph font; used by the im-touch UI shell, merged into the ImGui atlas - deliberately NOT offered by the Text tool).

Liberation Serif and Liberation Sans are *metrically compatible* with Times New
Roman and Arial respectively (matching widths and shapes); the real Microsoft
fonts are proprietary and are NOT bundled.

Liberation 2.x is distributed under the SIL OFL 1.1; see `Liberation-copyright.txt`.

## Noto Sans SC: where the bundled file comes from

`NotoSansSC.ttf` (the same file under `android/app/src/main/assets/fonts/`) is a subset of
**Noto Sans CJK SC Regular, version 2.004**, the font Adobe and Google publish at
<https://github.com/notofonts/noto-cjk> (`Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf`,
sha256 `2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b`). It is an OpenType/CFF font
with a `.ttf` extension. Checked on 2026-11-22: every one of its 4,185 characters has the same
outline and advance width as upstream, and no glyph or name record was added. Rebuilding the subset from
upstream with the same characters and `ccmp`, `locl`, `vert`, `kern` layout features reproduces the
`BASE`, `GPOS`, `GSUB`, `OS/2`, `VORG`, `cmap`, `hhea`, `hmtx`, `maxp`, `name`, `post`, `vhea` and
`vmtx` tables byte for byte (`CFF` differs only in how the same glyphs are encoded, `head` in its
timestamp). `licenses/NotoSansSC-OFL.txt` is the upstream OFL 1.1 text with the copyright line from the
font's own name table. The font keeps its upstream name, "Noto Sans CJK SC".
