# UI and font tools

- `pcf.py`: reads the game's font tables (`assets/fonts/*.pcf`): each character's box on its sheet, the line height
  and the kerning counts. `python pcf.py assets/fonts/*.pcf` lists every font.
- `dds.py`: decodes a DXT1/3/5 `.dds` sheet to pixels (stdlib only), for measuring.
- `fontforge_template.py assets/fonts out/font_templates`: a FontForge template per font for drawing a font of one's
  own that fits the game's layout (boxes in the background layer, measured guides; no game art). Instructions go
  beside the templates in `README.txt`.
- `fontgen.cpp`: rebuilds every font sheet from a typeface installed on Windows (DirectWrite): each character the
  game's table lists, sized to the game's cap height, on its baseline, spanning the game's letter (stretched within
  20%), controller icons kept. Writes the sheets at a scale under the names `textures/replacement/` (beside the exe) expects, and
  `preview.png` with game text now and in each family: `fontgen assets/fonts out_dir 4 Arial Roboto` (the first
  family's sheets are written). `FONTGEN_DEBUG=<font>` prints each glyph's spans.
- `texkey.py`: the port's texture name for a `.dds` (gpu/texturedumper.cpp Key).
- `rebuild_font.cpp`, `sdf_demo.py`, `bmp2png.py`: the earlier previews of rebuilt and distance field fonts.
