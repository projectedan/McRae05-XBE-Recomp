// The DSP effects image on the host's global processor (GP): the title's DSOUND GP program and its effects image run
// on xemu's DSP56300 interpreter and DSP DMA unit (third_party/xemu-dsp), as the console runs them.
//
// The GP program is DSOUND's own (passed in from the XBE). It clears X and Y memory, then each frame waits for the
// frame interrupt, reads a command block from scratch 0x800, runs the effects chain at P:0x171 and DMAs the speaker
// bins out. The command block is the image's own header: DSOUND copies the decrypted image to the start of GP scratch,
// and on the first frame the program DMAs the effects' code to P:0x171 and their state to X:0x80. A mix bin b is X
// memory 0x1400 + b * 0x20 to read and 0xC00 + b * 0x20 to write, the same storage; the voice processor writes all 32
// fresh each frame and the speaker bins 0..5 are read back after it.
//
// The title changes effect state while it runs (SetEffectData into each effect's state segment in guest memory): each
// frame a word the title changed goes into X memory, then X memory goes back to the segment for the title to read.
#include "xdk/dsp_image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "kernel/kernel.h"

extern "C" {
#include "dsp_dma.h"
#include "interp/dsp_cpu.h"
}

using namespace kernel;

namespace dsp {
namespace {

constexpr uint32_t kFrame = 32;     // samples the GP runs at once
constexpr uint32_t kSpeakers = 6;
constexpr uint32_t kProgramWords = 0x173;  // the GP program, up to the effects chain's entry
constexpr uint32_t kChainEntry = 0x171;    // P address the program loads the effects to and calls each frame
constexpr uint32_t kStateBase = 0x80;      // X address the program loads the effects' state to
constexpr long kFrameLimit = 2000000;      // instructions a frame may take before it is given up

// image header (the GP program's command block): code words, state offset in the image, state words
constexpr size_t kStateOffset = 0x808;
// effect descriptor entry: {code, code bytes, state, state bytes, Y, Y bytes, scratch, scratch bytes}
constexpr size_t kEntryBytes = 32;
constexpr size_t kKeyBytes = 8;

// Image cipher (DSOUND's key schedule and block routine): a 64 bit LFSR with taps 0, 2, 3 and 63
struct Lfsr {
    uint32_t lo = 0;
    uint32_t hi = 0;
    void Step() {
        uint32_t taps = lo ^ (lo << 1) ^ (lo << 3);
        uint32_t top = (taps << 28) ^ hi;
        lo = (lo >> 1) | (hi << 31);
        hi = (top & 0x80000000u) | (hi >> 1);
    }
};

// the master key, the LFSR after a fixed seed and 81 steps
std::array<uint8_t, 8> MasterKey() {
    Lfsr g{0x49DE12BA, 0x7FA49BCA};
    for (int i = 0; i < 81; i++) g.Step();
    std::array<uint8_t, 8> k;
    std::memcpy(k.data(), &g.lo, 4);
    std::memcpy(k.data() + 4, &g.hi, 4);
    return k;
}

void Decrypt(const uint8_t* key, uint8_t* data, size_t n) {
    std::array<uint8_t, 8> m = MasterKey();
    Lfsr g;
    std::memcpy(&g.lo, m.data(), 4);
    std::memcpy(&g.hi, m.data() + 4, 4);
    g.Step();
    for (size_t i = 0; i < n; i++) {
        uint8_t mask = m[i & 7];
        uint8_t c = uint8_t(data[i] - key[i & 7] * mask);
        g.Step();
        data[i] = uint8_t(mask ^ (g.lo & 0xFF) ^ c);
    }
}

uint32_t U32(const std::vector<uint8_t>& img, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= img.size()) std::memcpy(&v, &img[at], 4);
    return v;
}

// 24 bit fractions
float Frac(uint32_t word) { return float(int32_t(word << 8)) / 2147483648.0f; }
uint32_t Word(float x) {
    x = std::clamp(x, -1.0f, 8388607.0f / 8388608.0f);
    return uint32_t(int32_t(std::lrint(x * 8388608.0f))) & 0xFFFFFF;
}

// an effect's state in X memory and in its guest segment
struct Effect {
    uint32_t x = 0;      // X address
    uint32_t guest = 0;  // the segment
    uint32_t words = 0;
    std::vector<uint32_t> shadow;  // the segment as last synced, to see what the title wrote since
};

struct Gp {
    dsp_core_t core;
    DSPDMAState dma;
    std::vector<uint8_t> scratch;
    uint32_t interrupts = 0;
    bool halt = false;
};

Gp* g_gp = nullptr;  // large: allocated once
// Run is called without DirectSound's lock, so the title never waits on the GP; Load and Run exclude each other here
std::mutex g_run_lock;
std::atomic<bool> g_loaded{false};  // read under DirectSound's lock, set under g_run_lock
std::vector<Effect> g_effects;

// a block of frames: the bins in, the speakers out of the last block (one block of latency)
float g_in[kFrame][kBins];
float g_out[kFrame][kSpeakers];
uint32_t g_at = 0;

// peripherals as the MCPX GP has them: frame and DMA interrupts, halt, the DMA unit
constexpr uint32_t kStartFrame = 1 << 1;
constexpr uint32_t kDmaEol = 1 << 7;
uint32_t ReadPeripheral(dsp_core_t*, uint32_t address) {
    switch (address) {
        case 0xFFFFB3: return 0;  // cycle counter, read for timing only
        case 0xFFFFC5: return g_gp->interrupts | (g_gp->dma.eol ? kDmaEol : 0);
        case 0xFFFFD4: return dsp_dma_read(&g_gp->dma, DMA_NEXT_BLOCK);
        case 0xFFFFD5: return dsp_dma_read(&g_gp->dma, DMA_START_BLOCK);
        case 0xFFFFD6: return dsp_dma_read(&g_gp->dma, DMA_CONTROL);
        case 0xFFFFD7: return dsp_dma_read(&g_gp->dma, DMA_CONFIGURATION);
    }
    return 0;
}
void WritePeripheral(dsp_core_t*, uint32_t address, uint32_t value) {
    switch (address) {
        case 0xFFFFC4:
            if (value & 1) g_gp->halt = true;  // the program idles until the next frame
            break;
        case 0xFFFFC5:
            g_gp->interrupts &= ~value;
            if (value & kDmaEol) g_gp->dma.eol = false;
            break;
        case 0xFFFFD4: dsp_dma_write(&g_gp->dma, DMA_NEXT_BLOCK, value); break;
        case 0xFFFFD5: dsp_dma_write(&g_gp->dma, DMA_START_BLOCK, value); break;
        case 0xFFFFD6: dsp_dma_write(&g_gp->dma, DMA_CONTROL, value); break;
        case 0xFFFFD7: dsp_dma_write(&g_gp->dma, DMA_CONFIGURATION, value); break;
    }
}
uint32_t ReadMemory(void*, int space, uint32_t address) { return dsp56k_read_memory(&g_gp->core, space, address); }
void WriteMemory(void*, int space, uint32_t address, uint32_t value) {
    dsp56k_write_memory(&g_gp->core, space, address, value);
}
void Scratch(void*, uint8_t* p, uint32_t address, size_t n, bool write) {
    std::vector<uint8_t>& s = g_gp->scratch;
    if (address > s.size() || n > s.size() - address) {
        LOG_FIRST(4, "dsound: GP scratch %X + %zX past its end", address, n);
        if (!write) std::memset(p, 0, n);
        return;
    }
    if (write) std::memcpy(&s[address], p, n);
    else std::memcpy(p, &s[address], n);
}
// the output FIFOs feed the encode processor, not run here: what goes out is read from the speaker bins
void Fifo(void*, uint8_t* p, unsigned index, size_t n, bool write) {
    LOG_FIRST(4, "dsound: GP %s FIFO %u, %zu bytes", write ? "writes" : "reads", index, n);
    if (!write) std::memset(p, 0, n);
}

// runs the program until it halts for the next frame
long RunFrame() {
    Gp& gp = *g_gp;
    gp.interrupts |= kStartFrame;
    gp.halt = false;
    long n = 0;
    while (!gp.halt && n < kFrameLimit) {
        dsp56k_execute_instruction(&gp.core);
        n++;
    }
    return n;
}

void SyncIn() {
    for (Effect& e : g_effects)
        for (uint32_t i = 0; i < e.words; i++) {
            uint32_t v = R32(e.guest + i * 4);
            if (v == e.shadow[i]) continue;
            e.shadow[i] = v;
            g_gp->core.xram[(e.x + i) % DSP_XRAM_SIZE] = v & 0xFFFFFF;
        }
}
// only words the GP changed go back: one the title wrote while the frame ran (it no longer waits for the GP) is left for
// the next SyncIn rather than overwritten with the old value
void SyncOut() {
    for (Effect& e : g_effects)
        for (uint32_t i = 0; i < e.words; i++) {
            uint32_t v = g_gp->core.xram[(e.x + i) % DSP_XRAM_SIZE];
            if (v == e.shadow[i]) continue;
            e.shadow[i] = v;
            W32(e.guest + i * 4, v);
        }
}

// DSPWATCH set: each change to an effect's first 20 state words, as the title and the effect change them
void Watch() {
    static const bool watch = std::getenv("DSPWATCH") != nullptr;
    if (!watch) return;
    static std::vector<std::array<uint32_t, 20>> seen;
    seen.resize(g_effects.size());
    for (uint32_t k = 0; k < g_effects.size(); k++) {
        std::array<uint32_t, 20> now;
        for (uint32_t i = 0; i < 20; i++) now[i] = g_gp->core.xram[(g_effects[k].x + i) % DSP_XRAM_SIZE];
        if (now == seen[k]) continue;
        seen[k] = now;
        char w[200] = "";
        for (uint32_t i = 0; i < 20; i++) snprintf(w + std::strlen(w), sizeof(w) - std::strlen(w), " %06X", now[i]);
        LOGI("dsound: effect %u state%s", k, w);
    }
}

void Block() {
    Gp& gp = *g_gp;
    for (uint32_t b = 0; b < kBins; b++)
        for (uint32_t i = 0; i < kFrame; i++) gp.core.mixbuffer[b * kFrame + i] = Word(g_in[i][b]);
    SyncIn();
    long n = RunFrame();
    if (n >= kFrameLimit) LOG_FIRST(2, "dsound: GP frame not finished in %ld instructions, pc %X", n, gp.core.pc);
    SyncOut();
    Watch();
    for (uint32_t b = 0; b < kSpeakers; b++)
        for (uint32_t i = 0; i < kFrame; i++) g_out[i][b] = Frac(gp.core.mixbuffer[b * kFrame + i]);
}

}  // namespace

void Load(const std::vector<uint8_t>& image, size_t table, const std::vector<uint32_t>& states, int32_t reverb,
          const std::vector<uint8_t>& program) {
    std::lock_guard<std::mutex> run(g_run_lock);
    g_loaded = false;
    g_effects.clear();
    if (program.size() < kProgramWords * 4) {
        LOGE("dsound: no GP program, the effects image is not run");
        return;
    }
    uint32_t count = uint32_t(states.size());
    size_t keys_at = table + 8 + count * kEntryBytes;
    if (keys_at + count * kKeyBytes > image.size()) return;
    // the image as DSOUND hands it to the GP: every effect's code decrypted in place
    std::vector<uint8_t> img = image;
    std::vector<uint8_t> keys(img.begin() + long(keys_at), img.begin() + long(keys_at + count * kKeyBytes));
    std::array<uint8_t, 8> master = MasterKey();
    Decrypt(master.data(), keys.data(), keys.size());
    size_t scratch = img.size();
    for (uint32_t i = 0; i < count; i++) {
        size_t entry = table + 8 + i * kEntryBytes;
        uint32_t code_at = U32(img, entry), code_bytes = U32(img, entry + 4);
        uint32_t state_at = U32(img, entry + 8), state_bytes = U32(img, entry + 12);
        scratch = std::max<size_t>(scratch, size_t(U32(img, entry + 24)) + U32(img, entry + 28));
        if (code_at <= img.size() && code_bytes <= img.size() - code_at)
            Decrypt(&keys[i * kKeyBytes], &img[code_at], code_bytes);
        Effect e;
        e.x = kStateBase + (state_at - U32(img, kStateOffset)) / 4;
        e.guest = states[i];
        e.words = state_bytes / 4;
        e.shadow.resize(e.words);
        for (uint32_t k = 0; k < e.words; k++) e.shadow[k] = R32(e.guest + k * 4);
        g_effects.push_back(std::move(e));
        // DSPDUMP set: each effect's decrypted code and first 64 state words beside the exe, for study offline
        if (std::getenv("DSPDUMP") && code_at <= img.size() && code_bytes <= img.size() - code_at) {
            char name[64];
            snprintf(name, sizeof name, "dsp_effect_%02u.bin", i);
            if (FILE* f = std::fopen(name, "wb")) {
                std::fwrite(&img[code_at], 1, code_bytes, f);
                std::fclose(f);
            }
            snprintf(name, sizeof name, "dsp_state_%02u.bin", i);
            if (FILE* f = std::fopen(name, "wb")) {
                for (uint32_t k = 0; k < 64; k++) {
                    uint32_t w = R32(states[i] + k * 4);
                    std::fwrite(&w, 4, 1, f);
                }
                std::fclose(f);
            }
        }
    }
    if (!g_gp) g_gp = new Gp();
    Gp& gp = *g_gp;
    // the program's own buffers past the effects': the speaker rings at 0x8000, the FX bins at 0xB000
    gp.scratch.assign(std::max<size_t>(scratch, 0x10000) + 0x1000, 0);
    std::memcpy(gp.scratch.data(), img.data(), img.size());
    dsp56k_reset_cpu(&gp.core);
    gp.core.is_gp = true;
    gp.core.read_peripheral = ReadPeripheral;
    gp.core.write_peripheral = WritePeripheral;
    gp.dma = DSPDMAState{};
    gp.dma.mem_read = ReadMemory;
    gp.dma.mem_write = WriteMemory;
    gp.dma.scratch_rw = Scratch;
    gp.dma.fifo_rw = Fifo;
    gp.interrupts = 0;
    for (uint32_t i = 0; i < kProgramWords; i++) {
        uint32_t w;
        std::memcpy(&w, &program[i * 4], 4);
        gp.core.pram[i] = w & 0xFFFFFF;
    }
    gp.core.pc = 0;
    // the program starts: clears memory, sets its DMA lists, waits for the first frame
    long n = RunFrame();
    if (!gp.halt) {
        LOGE("dsound: the GP program did not start (pc %X after %ld instructions)", gp.core.pc, n);
        return;
    }
    // a silent first frame downloads the effects before the title can change their state
    std::memset(gp.core.mixbuffer, 0, sizeof gp.core.mixbuffer);
    RunFrame();
    if (!gp.halt || gp.core.xram[4]) {
        LOGE("dsound: the GP program did not load the effects image (pc %X)", gp.core.pc);
        return;
    }
    SyncOut();
    std::memset(g_in, 0, sizeof g_in);
    std::memset(g_out, 0, sizeof g_out);
    g_at = 0;
    g_loaded = true;
    LOGR("dsound: GP program and effects image running, %u effects, reverb %d, scratch %zu KB", count, reverb,
         gp.scratch.size() / 1024);
}

bool Loaded() { return g_loaded; }

void Run(float* bins, uint32_t frames) {
    std::lock_guard<std::mutex> run(g_run_lock);
    if (!g_loaded) return;
    for (uint32_t i = 0; i < frames; i++) {
        float* frame = bins + size_t(i) * kBins;
        for (uint32_t b = 0; b < kBins; b++) g_in[g_at][b] = frame[b];
        for (uint32_t b = 0; b < kSpeakers; b++) frame[b] = g_out[g_at][b];
        if (++g_at == kFrame) {
            Block();
            g_at = 0;
        }
    }
}

}  // namespace dsp
