#!/bin/bash
# Builds dis (disassembler) and run_effect (runs one effect of the DSP image on a sine, prints its gain) from xemu's
# DSP56300 interpreter (GPL, fetched here, not kept in this repository). Linux or WSL with clang.
#   tools/dsp/build.sh [out folder]
# Inputs come from the game run with DSPDUMP=1 (dsp_effect_NN.bin code, dsp_state_NN.bin state, beside the exe):
#   dis dsp_effect_03.bin
#   run_effect dsp_effect_03.bin dsp_state_03.bin <state X address, code word 1> <in> <out> <end pc> <Hz> <amplitude>
#   e.g. run_effect dsp_effect_03.bin dsp_state_03.bin 103 5B6 5D6 54 1000 0.005
set -e
H=$(cd "$(dirname "$0")" && pwd); OUT=${1:-$H/out}; mkdir -p "$OUT/inc/qemu"
X=$OUT/xemu
[ -d "$X" ] || { git clone -q --depth 1 --filter=blob:none --sparse https://github.com/xemu-project/xemu "$X"; git -C "$X" sparse-checkout set hw/xbox/mcpx; }
D=$X/hw/xbox/mcpx/apu/dsp
cp "$H/osdep.h" "$H/bswap.h" "$OUT/inc/qemu/"; cp "$H/trace.h" "$OUT/inc/"
clang -O1 -w -I"$OUT/inc" -I"$D" -I"$D/interp" "$H/dis.c" -o "$OUT/dis"
clang -O1 -w -I"$OUT/inc" -I"$D" -I"$D/interp" "$H/run_effect.c" -o "$OUT/run_effect" -lm
echo "built $OUT/dis $OUT/run_effect"
