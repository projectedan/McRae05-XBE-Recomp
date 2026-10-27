// offline: the game's GP program (kernel) + Rally5DSPImage on xemu's DSP56300 interpreter and DMA unit
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
extern "C" {
#include "interp/dsp_cpu.h"
#include "dsp_dma.h"
}
#define DSP_SPACE_X 0x00
#define DSP_SPACE_Y 0x01
#define DSP_SPACE_P 0x02
struct Lfsr { uint32_t lo = 0, hi = 0; void Step() { uint32_t taps = lo ^ (lo << 1) ^ (lo << 3); uint32_t top = (taps << 28) ^ hi; lo = (lo >> 1) | (hi << 31); hi = (top & 0x80000000u) | (hi >> 1); } };
std::array<uint8_t, 8> MasterKey() { Lfsr g{0x49DE12BA, 0x7FA49BCA}; for (int i = 0; i < 81; i++) g.Step(); std::array<uint8_t, 8> k; memcpy(k.data(), &g.lo, 4); memcpy(k.data() + 4, &g.hi, 4); return k; }
void Decrypt(const uint8_t* key, uint8_t* data, size_t n) { auto m = MasterKey(); Lfsr g; memcpy(&g.lo, m.data(), 4); memcpy(&g.hi, m.data() + 4, 4); g.Step(); for (size_t i = 0; i < n; i++) { uint8_t mask = m[i & 7]; uint8_t c = uint8_t(data[i] - key[i & 7] * mask); g.Step(); data[i] = uint8_t(mask ^ (g.lo & 0xFF) ^ c); } }
std::vector<uint8_t> Load(const char* p) { FILE* f = fopen(p, "rb"); std::vector<uint8_t> v(1 << 20); v.resize(fread(v.data(), 1, v.size(), f)); fclose(f); return v; }

static dsp_core_t core;
static DSPDMAState dma;
static std::vector<uint8_t> scratch(0x80000);
static uint32_t interrupts = 0;
static bool halt = false;
static long fifo_bytes[4];
static uint32_t rp(dsp_core_t*, uint32_t a) {
    switch (a) {
        case 0xFFFFB3: return 0;
        case 0xFFFFC5: return interrupts | (dma.eol ? 0x80 : 0);
        case 0xFFFFD4: return dsp_dma_read(&dma, DMA_NEXT_BLOCK);
        case 0xFFFFD5: return dsp_dma_read(&dma, DMA_START_BLOCK);
        case 0xFFFFD6: return dsp_dma_read(&dma, DMA_CONTROL);
        case 0xFFFFD7: return dsp_dma_read(&dma, DMA_CONFIGURATION);
    }
    return 0xABABA;
}
static void wp(dsp_core_t*, uint32_t a, uint32_t v) {
    switch (a) {
        case 0xFFFFC4: if (v & 1) halt = true; break;
        case 0xFFFFC5: interrupts &= ~v; if (v & 0x80) dma.eol = false; break;
        case 0xFFFFD4: dsp_dma_write(&dma, DMA_NEXT_BLOCK, v); break;
        case 0xFFFFD5: dsp_dma_write(&dma, DMA_START_BLOCK, v); break;
        case 0xFFFFD6: dsp_dma_write(&dma, DMA_CONTROL, v); break;
        case 0xFFFFD7: dsp_dma_write(&dma, DMA_CONFIGURATION, v); break;
    }
}
static uint32_t mr(void*, int s, uint32_t a) { return dsp56k_read_memory(&core, s, a); }
static void mw(void*, int s, uint32_t a, uint32_t v) { dsp56k_write_memory(&core, s, a, v); }
static void srw(void*, uint8_t* p, uint32_t a, size_t n, bool dir) {
    if (a + n > scratch.size()) { printf("scratch out of range %X %zu\n", a, n); return; }
    if (dir) memcpy(&scratch[a], p, n); else memcpy(p, &scratch[a], n);
}
static void frw(void*, uint8_t* p, unsigned i, size_t n, bool dir) { fifo_bytes[i & 3] += long(n); if (!dir) memset(p, 0, n); }

static long RunFrame(long cap) {
    interrupts |= 2;  // start frame
    halt = false;
    long n = 0;
    while (!halt && n < cap) { dsp56k_execute_instruction(&core); n++; }
    return n;
}
float Frac(uint32_t w) { return float(int32_t(w << 8)) / 2147483648.0f; }
uint32_t Word(float x) { x = std::fmax(-1.f, std::fmin(x, 0.99999f)); return uint32_t(int32_t(lrintf(x * 8388608.0f))) & 0xFFFFFF; }

int main(int argc, char** argv) {
    auto img = Load(argv[1]);
    auto kern = Load(argv[2]);
    auto u32 = [&](size_t o) { uint32_t v = 0; memcpy(&v, &img[o], 4); return v; };
    size_t t = 0x818 + size_t(u32(0x804) + u32(0x80C)) * 4;
    uint32_t count = u32(t);
    std::vector<uint8_t> keys(img.begin() + t + 8 + count * 32, img.begin() + t + 8 + count * 32 + count * 8);
    auto mk = MasterKey(); Decrypt(mk.data(), keys.data(), keys.size());
    for (uint32_t i = 0; i < count; i++) { size_t e = t + 8 + i * 32; Decrypt(&keys[i * 8], &img[u32(e)], u32(e + 4)); }
    memcpy(scratch.data(), img.data(), img.size());
    dsp56k_reset_cpu(&core);
    core.is_gp = true;
    core.read_peripheral = rp; core.write_peripheral = wp;
    dma.mem_read = mr; dma.mem_write = mw; dma.scratch_rw = srw; dma.fifo_rw = frw;
    for (size_t i = 0; i < kern.size() / 4; i++) { uint32_t w; memcpy(&w, &kern[i * 4], 4); core.pram[i] = w & 0xFFFFFF; }
    core.pc = 0;
    // to the first wait for a frame
    long boot = RunFrame(1000000);
    printf("boot: %ld instructions, halt %d, pc %X\n", boot, halt, core.pc);
    // replay: the recorded sends into their bins
    std::vector<int16_t> rec;
    { FILE* rf = fopen(getenv("REC"), "rb"); fseek(rf, 0, SEEK_END); long sz = ftell(rf); fseek(rf, 44, SEEK_SET); rec.resize((sz - 44) / 2); fread(rec.data(), 2, rec.size(), rf); fclose(rf); }
    const int rframes = int(rec.size() / 16 / 32);
    const int map[][2] = {{3, 3}, {13, 6}, {23, 7}, {2, 8}, {6, 9}, {7, 10}, {8, 11}, {9, 12}, {10, 13}};
    std::vector<uint32_t> conv; { auto b = Load("dsp_reverb_generic.bin"); conv.resize(b.size() / 4); memcpy(conv.data(), b.data(), b.size()); }
    double acc[3] = {}; int cnt = 0;
    for (int f = 0; f < rframes; f++) {
        if (f == 2) { const uint32_t xr = 0x80 + (0x4218 - 0x37DC) / 4; for (uint32_t w = 0xAC / 4; w < 0x220 / 4; w++) core.xram[xr + w] = conv[w] & 0xFFFFFF; }
        for (int b = 0; b < 32; b++) for (int i = 0; i < 32; i++) core.mixbuffer[b * 32 + i] = 0;
        for (auto& m : map) for (int i = 0; i < 32; i++) core.mixbuffer[m[0] * 32 + i] = Word(rec[(size_t(f) * 32 + i) * 16 + m[1]] / 32768.0f);
        RunFrame(2000000);
        for (int i = 0; i < 32; i++) { float l = Frac(core.mixbuffer[0 * 32 + i]); float r6 = Frac(core.mixbuffer[6 * 32 + i]); float in10 = rec[(size_t(f) * 32 + i) * 16 + 13] / 32768.0f; acc[0] += l * l; acc[1] += r6 * r6; acc[2] += in10 * in10; }
        if (++cnt == 750) { printf("%6.1f s  FL %7.1f  bin6 %7.1f  reverb in %7.1f\n", (f + 1) * 32 / 48000.0, 10 * log10(acc[0] / 24000 + 1e-14), 10 * log10(acc[1] / 24000 + 1e-14), 10 * log10(acc[2] / 24000 + 1e-14)); acc[0] = acc[1] = acc[2] = 0; cnt = 0; }
    }
    return 0;
}
