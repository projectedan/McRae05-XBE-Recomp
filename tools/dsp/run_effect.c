#include "dsp_cpu.c"
#include <math.h>
static dsp_core_t c;
static uint32_t rp(dsp_core_t* d, uint32_t a) { return 0; }
static void wp(dsp_core_t* d, uint32_t a, uint32_t v) {}
// usage: run3 code.bin state.bin stateaddr inaddr outaddr endpc freq
int main(int argc, char** argv) {
    dsp56k_reset_cpu(&c);
    c.read_peripheral = rp; c.write_peripheral = wp;
    FILE* f = fopen(argv[1], "rb"); uint32_t w; int n = 0;
    while (fread(&w, 4, 1, f) == 1) { c.pram[n++] = w & 0xFFFFFF; }
    fclose(f);
    uint32_t st = strtoul(argv[3], 0, 16), in = strtoul(argv[4], 0, 16), out = strtoul(argv[5], 0, 16), end = strtoul(argv[6], 0, 16);
    double freq = atof(argv[7]);
    f = fopen(argv[2], "rb"); for (int i = 0; i < 64; i++) { fread(&w, 4, 1, f); c.xram[st + i] = w & 0xFFFFFF; } fclose(f);
    double amp = atof(argv[8]), sumin = 0, sumout = 0;
    long t = 0;
    for (int frame = 0; frame < 400; frame++) {
        for (int i = 0; i < 32; i++, t++) {
            double x = amp * sin(2 * M_PI * freq * t / 48000.0);
            int32_t v = (int32_t)lrint(x * 8388608.0);
            dsp56k_write_memory(&c, DSP_SPACE_X, in + i, v & 0xFFFFFF);
        }
        for (int i = 0; i < 32; i++) dsp56k_write_memory(&c, DSP_SPACE_X, out + i, 0);
        c.pc = 0;
        for (long k = 0; k < 100000 && c.pc != end; k++) dsp56k_execute_instruction(&c);
        for (int i = 0; i < 32; i++) {
            uint32_t v = dsp56k_read_memory(&c, DSP_SPACE_X, out + i);
            int32_t s = (int32_t)(v << 8) >> 8;
            double y = s / 8388608.0;
            if (frame >= 100) { sumout += y * y; long tt = frame * 32 + i; double x = amp * sin(2 * M_PI * freq * tt / 48000.0); sumin += x * x; }
        }
    }
    printf("%g Hz %.2f dB\n", freq, 10 * log10(sumout / sumin));
}
