# xemu DSP56300 interpreter and DSP DMA unit

The MCPX APU's DSP core and its DMA unit, from [xemu](https://github.com/xemu-project/xemu)
(`hw/xbox/mcpx/apu/dsp`, commit 25e1ee2fd59a451f2ab57e825b4f8a5414220c8b, 2026-10-22). The runtime runs the title's
global processor program and DSP effects image on them (`runtime/xdk/dsp_image.cpp`).

| Files | From | Licence |
|-------|------|---------|
| `interp/` | `dsp/interp/`: the DSP56300 interpreter and disassembler (from Hatari) | GPL-2.0-or-later (`COPYING`) |
| `dsp_dma.c`, `dsp_dma.h`, `dsp_dma_regs.h`, `debug.h` | `dsp/` | LGPL-2.1-or-later (`COPYING.LIB`) |
| `shim/` | written for this SDK: the QEMU headers they include, cut down to what they use | as the SDK |

The xemu files are unchanged. Their copyright notices are in each file.
