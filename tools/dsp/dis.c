#include "dsp_cpu.c"
static dsp_core_t core;
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb");
    uint32_t w; int n = 0;
    while (fread(&w, 4, 1, f) == 1) core.pram[n++] = w & 0xFFFFFF;
    core.is_gp = true;
    { uint32_t save[4096]; memcpy(save, core.pram, sizeof save); dsp56k_reset_cpu(&core); memcpy(core.pram, save, sizeof save); }
    for (uint32_t pc = 0; pc < (uint32_t)n;) {
        core.pc = pc;
        core.disasm_prev_inst_pc = 0xFFFFFFFF;
        uint16_t len = disasm_instruction(&core, DSP_DISASM_MODE);
        printf("%s\n", disasm_get_instruction_text(&core));
        pc += len ? len : 1;
    }
}
