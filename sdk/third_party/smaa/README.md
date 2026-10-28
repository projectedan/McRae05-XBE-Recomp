# SMAA

Enhanced Subpixel Morphological Antialiasing by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and
Diego Gutierrez, from https://github.com/iryoku/smaa (master, fetched 2026-10-28), MIT licence (`LICENSE.txt`).

- `SMAA.hlsl`: the shader, unchanged. The build wraps it into a string header (`smaa_hlsl.inc`); the renderer
  defines its samplers and texture macros through `SMAA_CUSTOM_SL` and runs its three passes over the finished frame
  (`D3D12Backend::Smaa`).
- `AreaTex.h`, `SearchTex.h`: the precomputed lookup tables, unchanged (160x560 R8G8, 64x16 R8).
