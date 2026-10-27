# UI and font tools

- `pcf.py`: reads the game's font tables (`assets/fonts/*.pcf`): each character's box on its sheet, the line height
  and the kerning counts. `python pcf.py assets/fonts/*.pcf` lists every font.
- `dds.py`: decodes a DXT1/3/5 `.dds` sheet to pixels (stdlib only), for measuring.
- `fontforge_template.py assets/fonts out/font_templates`: a FontForge template per font for drawing a font of one's
  own that fits the game's layout (boxes in the background layer, measured guides; no game art). Instructions go
  beside the templates in `README.txt`.
- `rebuild_font.cpp`, `sdf_demo.py`, `bmp2png.py`: the earlier previews of rebuilt and distance field fonts.
