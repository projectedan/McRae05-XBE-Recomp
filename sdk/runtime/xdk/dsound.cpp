// DirectSound (XDK 4831 and 5849) on a host software mixer. Buffers and streams are read from guest memory as they play.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "audio/audio.h"
#include "xbe/xbe.h"
#include "xdk/hle.h"
#include "xdk/dsp_image.h"
#include "xdk/layout.h"

using namespace kernel;

namespace kernel {
extern const xbe::Image* g_image;
}
namespace dispatch {
X86Func* Find(uint32_t guest);
}

// configs written before the SDK knew the name
#ifndef SYM_IDirectSound_SynchPlayback
#define SYM_IDirectSound_SynchPlayback FFFFFF00
#define HAS_IDirectSound_SynchPlayback 0
#endif
#ifndef SYM_IDirectSound_DownloadEffectsImage
#define SYM_IDirectSound_DownloadEffectsImage FFFFFF01
#define HAS_IDirectSound_DownloadEffectsImage 0
#endif
#ifndef SYM_IDirectSound_GetEffectData
#define SYM_IDirectSound_GetEffectData FFFFFF02
#define HAS_IDirectSound_GetEffectData 0
#endif
#ifndef SYM_IDirectSound_CommitEffectData
#define SYM_IDirectSound_CommitEffectData FFFFFF03
#define HAS_IDirectSound_CommitEffectData 0
#endif
#ifndef SYM_IDirectSound_GetOutputLevels
#define SYM_IDirectSound_GetOutputLevels FFFFFF04
#define HAS_IDirectSound_GetOutputLevels 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetPlayRegion
#define SYM_IDirectSoundBuffer_SetPlayRegion FFFFFF05
#define HAS_IDirectSoundBuffer_SetPlayRegion 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetAllParameters
#define SYM_IDirectSoundBuffer_SetAllParameters FFFFFF06
#define HAS_IDirectSoundBuffer_SetAllParameters 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetFormat
#define SYM_IDirectSoundBuffer_SetFormat FFFFFF07
#define HAS_IDirectSoundBuffer_SetFormat 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetOutputBuffer
#define SYM_IDirectSoundBuffer_SetOutputBuffer FFFFFF08
#define HAS_IDirectSoundBuffer_SetOutputBuffer 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetRolloffCurve
#define SYM_IDirectSoundBuffer_SetRolloffCurve FFFFFF09
#define HAS_IDirectSoundBuffer_SetRolloffCurve 0
#endif
#ifndef SYM_IDirectSoundBuffer_StopEx
#define SYM_IDirectSoundBuffer_StopEx FFFFFF0A
#define HAS_IDirectSoundBuffer_StopEx 0
#endif
#ifndef SYM_DirectSoundUseLightHRTF
#define SYM_DirectSoundUseLightHRTF FFFFFF0B
#define HAS_DirectSoundUseLightHRTF 0
#endif
#ifndef SYM_IDirectSoundStream_SetEG
#define SYM_IDirectSoundStream_SetEG FFFFFF0C
#define HAS_IDirectSoundStream_SetEG 0
#endif
#ifndef SYM_IDirectSoundBuffer_Pause
#define SYM_IDirectSoundBuffer_Pause FFFFFF0D
#define HAS_IDirectSoundBuffer_Pause 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetEG
#define SYM_IDirectSoundBuffer_SetEG FFFFFF0E
#define HAS_IDirectSoundBuffer_SetEG 0
#endif

namespace {

constexpr uint32_t kStreamVtable = SYM(DSound_StreamVtable);  // CDirectSoundStream XMediaObject vtable
constexpr uint32_t kStreamVtable2 = SYM(DSound_StreamVtable2);
constexpr uint32_t kPacketPending = 0x8000000A, kFlushed = 0x80004004;  // E_PENDING, E_ABORT

struct Packet {
    uint32_t data, size, completed_ptr, status_ptr, context;
    uint32_t used = 0;
};

// DSMIXBIN numbers the host mixes by name
enum MixBin : uint32_t {
    kFrontLeft,
    kFrontRight,
    kCentre,
    kSub,
    kBackLeft,
    kBackRight,
    kCrosstalkFrontLeft,
    kCrosstalkFrontRight,
    kCrosstalkBackLeft,
    kCrosstalkBackRight,
    kFirstSend,  // effect sends from here up
};
constexpr uint32_t kBins = dsp::kBins;
constexpr uint32_t kInvalidCall = 0x88780032u;  // DSERR_INVALIDCALL
constexpr uint32_t kMaxChannels = 8;
// rows of a voice's mix: its channels, then a mono 3D voice's two ears
constexpr uint32_t kLeftEar = kMaxChannels;
constexpr uint32_t kRightEar = kMaxChannels + 1;
constexpr uint32_t kRows = kMaxChannels + 2;
constexpr uint32_t kEarLength = 64;  // past the longest interaural delay, 31 samples

// gain of each row on each mix bin
struct Gains {
    float bin[kRows][kBins];
};

struct Voice {
    uint32_t obj = 0;
    bool stream = false;
    int refs = 1;
    // format
    uint16_t tag = 1, channels = 1, bits = 16, align = 2;
    uint32_t rate = 48000;
    // buffer
    // play region in bytes from data, loop region in bytes from the play region
    uint32_t data = 0, bytes = 0, loop_start = 0, loop_len = 0, play_start = 0, play_len = 0;
    uint32_t owned = 0;  // memory the library made for the buffer
    bool playing = false, looping = false, paused = false;
    bool sync = false;  // held until IDirectSound_SynchPlayback
    bool active = false, ending = false;  // stream: took a packet since it went quiet, quiet again once they run out
    double pos = 0;  // frames
    uint32_t freq = 0;
    // stream
    uint32_t max_packets = 1, callback = 0, context = 0;
    std::deque<Packet> packets;
    // levels
    float volume = 1, headroom = 1;
    uint8_t nbins = 0, bin[8] = {};
    int32_t bin_mb[32] = {};  // by mix bin
    std::vector<float> curve;  // rolloff
    bool is3d = false;
    bool dialogue = false;  // speech: front left, right and centre alike, nothing else (SetBins)
    uint32_t mode = 0;  // DS3DMODE: 1 head relative, 2 disabled
    float p[3] = {}, min_dist = 1, max_dist = 1e9f;
    float rolloff = 1;  // IDirectSoundBuffer_SetRolloffFactor and the stream's, times the listener's
    std::vector<std::pair<uint32_t, uint32_t>> notify;  // SetNotificationPositions: byte offset, event handle
    Gains gain = {};  // the last block's gains, the next block ramps from them
    bool has_gain = false;
    // a mono 3D voice's interaural delay in samples and its recent samples
    float itd = 0;
    float ear[kEarLength] = {};
    uint32_t ear_pos = 0;
    // ADPCM block cache: the block read last and the one read before it, a frame between two blocks reads both
    int64_t block[2] = {-1, -1};
    uint8_t last = 0;  // decoded[last] holds block[last]
    int16_t decoded[2][65 * 8];
};

struct Done {
    uint32_t voice, callback, context, packet_context, status;
};

std::recursive_mutex g_lock;
std::map<uint32_t, Voice> g_voices;
float g_listener[3] = {}, g_front[3] = {0, 0, 1}, g_top[3] = {0, 1, 0};
float g_rolloff = 1;  // IDirectSound_SetRolloffFactor
// Completed packets' callbacks, run as the XDK runs them: in DirectSoundDoWork on the title's thread, so they never
// overlap the title's own stream code. A stream flushed or released runs its own at once, before it is gone.
std::deque<Done> g_done;
std::condition_variable_any g_done_cv;
thread_local std::vector<Done> t_flushed;  // run by the STREAM call that flushed them, once g_lock is let go
std::atomic<int64_t> g_pumped{0};          // the last DirectSoundDoWork, steady ms
bool g_started = false;
uint32_t g_ds = 0;
// IDirectSound_SetMixBinHeadroom: the voice processor halves a bin per step, 1 for every bin but the last by default
uint8_t g_bin_headroom[kBins] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0};
struct Segment {
    uint32_t at;
    uint32_t bytes;
};
std::vector<Segment> g_effect_states;  // each effect's state segment of the downloaded image
int32_t g_reverb_effect = -1;          // the image's I3DL2 reverb (DSEFFECTIMAGELOC)

void ForgetBlocks(Voice& v) {
    v.block[0] = -1;
    v.block[1] = -1;
}

float Db(int32_t hundredths) { return hundredths <= -10000 ? 0.0f : std::pow(10.0f, hundredths / 2000.0f); }

uint32_t FramesOf(const Voice& v, uint32_t bytes) {
    if (v.tag == 0x69) return bytes / (36u * v.channels) * 64;  // WAVE_FORMAT_XBOX_ADPCM
    return bytes / std::max<uint32_t>(v.align, 1);
}
uint32_t BytesOf(const Voice& v, uint32_t frames) {
    if (v.tag == 0x69) return frames / 64 * 36 * v.channels;
    return frames * v.align;
}

const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kStep[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
                       31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
                       130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
                       544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
                       2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
                       9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// one 36 byte per channel IMA block, the voice plays 64 of its 65 frames
void DecodeBlock(const uint8_t* b, int ch, int16_t* out) {
    for (int c = 0; c < ch; c++) {
        int pred = int16_t(b[c * 4] | b[c * 4 + 1] << 8), idx = std::min<int>(b[c * 4 + 2], 88);
        out[c] = int16_t(pred);
        const uint8_t* d = b + ch * 4;
        for (int k = 0; k < 8; k++)
            for (int j = 0; j < 8; j++) {
                int n = (d[(k * ch + c) * 4 + j / 2] >> ((j & 1) * 4)) & 15;
                int step = kStep[idx], diff = step >> 3;
                if (n & 1) diff += step >> 2;
                if (n & 2) diff += step >> 1;
                if (n & 4) diff += step;
                pred = std::clamp(n & 8 ? pred - diff : pred + diff, -32768, 32767);
                idx = std::clamp(idx + kIndex[n], 0, 88);
                out[(1 + k * 8 + j) * ch + c] = int16_t(pred);
            }
    }
}

// the decoded ADPCM block at guest address at, decoded once while it stays in the cache
const int16_t* DecodedBlock(Voice& v, int64_t at) {
    for (uint8_t k = 0; k < 2; k++) {
        if (v.block[k] != at) continue;
        v.last = k;
        return v.decoded[k];
    }
    uint8_t k = v.last ^ 1;  // the slot not read last
    DecodeBlock(static_cast<const uint8_t*>(HostPtr(uint32_t(at))), std::min<int>(v.channels, 8), v.decoded[k]);
    v.block[k] = at;
    v.last = k;
    return v.decoded[k];
}

// sample of channel c at frame f of memory starting at data
float Sample(Voice& v, uint32_t data, uint32_t f, int c) {
    c = std::min<int>(c, v.channels - 1);
    const uint8_t* p = static_cast<const uint8_t*>(HostPtr(data));
    if (v.tag == 0x69) {
        int64_t at = int64_t(data) + int64_t(f / 64) * 36 * v.channels;
        return DecodedBlock(v, at)[(f % 64) * v.channels + c] / 32768.0f;
    }
    if (v.bits == 8) return (p[f * v.align + c] - 128) / 128.0f;
    int16_t s;
    std::memcpy(&s, p + f * v.align + c * 2, 2);
    return s / 32768.0f;
}

void Complete(Voice& v, Packet& pk, uint32_t status) {
    if (pk.completed_ptr) W32(pk.completed_ptr, status == 0 ? pk.size : pk.used);
    if (pk.status_ptr) W32(pk.status_ptr, status);
    if (v.callback) {
        g_done.push_back({v.obj, v.callback, v.context, pk.context, status});
        g_done_cv.notify_one();
    } else if (pk.context) {
        if (ObjectRef e = FromHandle(pk.context)) SetEvent(*e);
    }
}

// The stereo downmix of the speaker bins, the console's: centre and back pair at -3 dB. The low frequency channel joins
// low passed, as a stereo set's bass does: CMR sends its engine there near full scale and full range, and folded in
// whole its mid range sat on the co-driver's (measured against a recording of the game, his calls stood 4 to 6 dB
// less clear of the rest). Dialogue (see Voice::dialogue) is lifted by the remaining gap. All of it can be set in the
// settings file (the game's mix_ options); the defaults are these.
struct Mix {
    float dialogue = 1.78f;  // +5 dB
    float front = 1.0f;
    float centre = 0.707f;
    float back = 0.707f;
    float sub = 0.5f;
    uint32_t sub_hz = 180;  // 0: the whole range
};
Mix g_mix;
// audio_effects in the settings file: the effects image on the emulated GP (1) or the hand-written stand-ins (0)
bool g_gp = true;
bool ImageLoaded() { return g_gp ? dsp::Loaded() : dsp::legacy::Loaded(); }

// a second order low pass (RBJ), one per output side
struct LowPass {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void Set(float hz) {
        *this = LowPass();
        if (hz <= 0) return;
        float w = 6.2831853f * hz / float(audio::kRate), c = std::cos(w), al = std::sin(w) / (2 * 0.7071f);
        float a0 = 1 + al;
        b0 = (1 - c) / 2 / a0;
        b1 = (1 - c) / a0;
        b2 = b0;
        a1 = -2 * c / a0;
        a2 = (1 - al) / a0;
    }
    float Run(float x) {
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2 + 1e-20f;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

// A voice with no speaker bin and no effects image plays its loudest bin on the front pair; with speaker bins the
// crosstalk bins join their speakers and effect sends are dropped. Without the title's image nothing reads them.
void RouteWithoutImage(const Voice& v, Gains& g) {
    bool speakers = false;
    for (uint32_t s = 0; s < v.nbins; s++) speakers |= v.bin[s] < kFirstSend && v.bin[s] != kSub;
    if (!v.nbins) speakers = true;
    for (auto& row : g.bin) {
        if (!speakers) {
            float loudest = *std::max_element(row, row + kBins);
            std::fill(row, row + kBins, 0.0f);
            row[kFrontLeft] = loudest;
            row[kFrontRight] = loudest;
            continue;
        }
        row[kFrontLeft] += row[kCrosstalkFrontLeft];
        row[kFrontRight] += row[kCrosstalkFrontRight];
        row[kBackLeft] += row[kCrosstalkBackLeft];
        row[kBackRight] += row[kCrosstalkBackRight];
        std::fill(row + kCrosstalkFrontLeft, row + kBins, 0.0f);
    }
}

// The library gives every hardware voice (two channels) a run of n / voices slots. A stereo voice plays its left
// channel on even slots and its right on odd ones.
uint32_t ChannelOfSlot(const Voice& v, uint32_t voice, uint32_t slot) {
    uint32_t voice_channels = std::min<uint32_t>(v.channels - voice * 2, 2);
    return voice * 2 + slot % voice_channels;
}

// each channel's level on each bin; the voice processor divides a bin by 2^headroom, which the image may undo
Gains SlotGains(const Voice& v) {
    static const uint8_t kSpeakerBins[6] = {0, 1, 2, 3, 4, 5};  // no mix bins given: the speakers in order
    uint32_t channels = std::min<uint32_t>(v.channels, kMaxChannels);
    uint32_t voices = (channels - 1) / 2 + 1;
    const uint8_t* bin = v.nbins ? v.bin : kSpeakerBins;
    uint32_t n = v.nbins ? v.nbins : std::min<uint32_t>(voices * 2, 6);
    uint32_t per = std::max<uint32_t>(n / voices, 1);
    bool image = ImageLoaded();
    Gains g = {};
    for (uint32_t k = 0; k < voices; k++) {
        for (uint32_t s = k * per; s < (k + 1) * per && s < n; s++) {
            uint32_t b = bin[s];
            float level = Db(v.bin_mb[b]) * v.volume * v.headroom;
            if (image) level = std::ldexp(level, -g_bin_headroom[b]);
            g.bin[ChannelOfSlot(v, k, s)][b] += level;
        }
    }
    return g;
}

// A 3D voice as the library's 3D pass sees it. Its levels have no left/right term: a mono voice's two ears differ
// by the interaural delay, and on a stereo console the back crosstalk pair is silent.
struct Place {
    bool on = false;
    float att = 1;      // distance, every bin
    float front = 1;    // crosstalk front pair
    float centre = 1;   // centre bin
    float azimuth = 0;  // degrees, plus to the right
    float elevation = 0;
};

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// the library's octant estimate of the angle of along from across: 45 degrees where they are equal
float OctantAngle(float along, float across) {
    float a = std::fabs(along);
    float s = std::fabs(across);
    if (a > s) return 45 * s / a;
    if (s > 0) return 90 - 45 * a / s;
    return 0;
}

float Azimuth(const float d[3], const float front[3], const float right[3]) {
    float ahead = Dot(d, front);
    float side = Dot(d, right);
    float deg = OctantAngle(ahead, side);
    if (ahead < 0) deg = 180 - deg;
    return side < 0 ? -deg : deg;
}

float Elevation(const float d[3], const float front[3], const float right[3], const float top[3]) {
    float flat = std::hypot(Dot(d, front), Dot(d, right));
    float up = Dot(d, top);
    float deg = OctantAngle(flat, up);
    return up < 0 ? -deg : deg;
}

// interaural delay in samples by azimuth in steps of 3 degrees at elevation 0, the far ear's HRTF delay
float Itd(float azimuth) {
    static const uint8_t kDelay[61] = {0,  2,  3,  4,  4,  5,  6,  9,  10, 11, 12, 12, 13, 14, 17, 18,
                                       19, 20, 21, 21, 22, 23, 24, 25, 26, 28, 29, 29, 31, 30, 30, 31,
                                       30, 28, 27, 28, 27, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15,
                                       14, 13, 13, 12, 11, 8,  7,  6,  6,  3,  2,  2,  0};
    constexpr int kStep = 3;
    if (!(std::fabs(azimuth) <= 180)) return 0;
    int deg = int(std::fabs(azimuth) + kStep / 2.0f) / kStep * kStep;
    float delay = kDelay[deg / kStep];
    return azimuth < 0 ? -delay : delay;
}

// A voice whose fifth bin is the centre shares the front with it below 45 degrees: 0.7071 cos^2(2a) and
// 0.8409 cos(2a) by whole degrees of azimuth and elevation
void ShareCentre(Place& at) {
    constexpr float kRadiansPerDegree = 0.017453293f;
    constexpr int kCentreLimit = 45;
    int az = int(std::fabs(at.azimuth));
    int el = int(std::fabs(at.elevation));
    if (az >= kCentreLimit || el >= kCentreLimit) {
        at.centre = 0;
        return;
    }
    float both = std::cos(2 * az * kRadiansPerDegree) * std::cos(2 * el * kRadiansPerDegree);
    at.centre *= 0.7071f * both;
    at.front *= 1 - 0.5f * both * both;
}

// distance attenuation, 1.0 inside the min distance: min / distance, or the voice's rolloff curve
float Attenuation(const Voice& v, float dist) {
    if (dist <= v.min_dist) return 1;
    // DirectSound's rolloff: min / (min + factor (distance - min)), none at factor 0
    if (v.curve.empty()) {
        float r = v.rolloff * g_rolloff;
        if (r <= 0) return 1;
        return v.min_dist / (v.min_dist + r * (std::min(dist, v.max_dist) - v.min_dist));
    }
    if (v.max_dist <= v.min_dist) return 1;
    // points spread evenly from min to max distance, 1.0 at min
    float at = (std::min(dist, v.max_dist) - v.min_dist) / (v.max_dist - v.min_dist) * v.curve.size();
    size_t i = std::min(size_t(at), v.curve.size() - 1);
    float from = i ? v.curve[i - 1] : 1.0f;
    return from + (v.curve[i] - from) * (at - i);
}

Place Locate(const Voice& v) {
    static const float kAhead[3] = {0, 0, 1};
    static const float kRight[3] = {1, 0, 0};
    static const float kUp[3] = {0, 1, 0};
    Place at;
    if (!v.is3d || v.mode == 2) return at;
    at.on = true;
    bool relative = v.mode == 1;  // the position is already in listener space
    float d[3] = {v.p[0], v.p[1], v.p[2]};
    for (int i = 0; i < 3 && !relative; i++) d[i] -= g_listener[i];
    at.att = Attenuation(v, std::sqrt(Dot(d, d)));
    float right[3] = {g_top[1] * g_front[2] - g_top[2] * g_front[1], g_top[2] * g_front[0] - g_top[0] * g_front[2],
                      g_top[0] * g_front[1] - g_top[1] * g_front[0]};
    const float* front = relative ? kAhead : g_front;
    const float* side = relative ? kRight : right;
    const float* top = relative ? kUp : g_top;
    at.azimuth = Azimuth(d, front, side);
    at.elevation = Elevation(d, front, side, top);
    at.centre = at.front;
    if (v.nbins >= 5 && v.bin[4] == kCentre) ShareCentre(at);
    return at;
}

float BinScale(const Place& at, uint32_t b) {
    if (!at.on) return 1;
    if (b == kCentre) return at.att * at.centre;
    if (b == kCrosstalkFrontLeft || b == kCrosstalkFrontRight) return at.att * at.front;
    if (b == kCrosstalkBackLeft || b == kCrosstalkBackRight) return 0;
    return at.att;
}

// a mono 3D voice's crosstalk bins play its ears: the left bins the left ear, the right bins the right one
void SplitEars(Gains& g) {
    static const uint32_t kLeft[2] = {kCrosstalkFrontLeft, kCrosstalkBackLeft};
    static const uint32_t kRight[2] = {kCrosstalkFrontRight, kCrosstalkBackRight};
    for (int k = 0; k < 2; k++) {
        std::swap(g.bin[0][kLeft[k]], g.bin[kLeftEar][kLeft[k]]);
        std::swap(g.bin[0][kRight[k]], g.bin[kRightEar][kRight[k]]);
    }
}

Gains Levels(const Voice& v, const Place& at) {
    Gains g = SlotGains(v);
    for (auto& row : g.bin) {
        for (uint32_t b = 0; b < kBins; b++) row[b] *= BinScale(at, b);
    }
    if (at.on && v.channels == 1) SplitEars(g);
    if (!ImageLoaded()) RouteWithoutImage(v, g);
    // a sound placed at NaN goes silent alone; its NaN gain would silence the whole mix
    for (auto& row : g.bin) {
        for (float& gain : row) {
            if (!std::isfinite(gain)) gain = 0;
        }
    }
    return g;
}

// a row's level on one bin, ramped across the block: levels follow the game once a frame, stepped a fast turn clicks
struct Lane {
    uint32_t row;
    uint32_t bin;
    float gain;
    float step;
};

// the levels the block can hear; one zero at both ends of its ramp adds nothing to the mix
std::vector<Lane> AudibleLanes(const Gains& from, const Gains& to, uint32_t frames) {
    std::vector<Lane> lanes;
    for (uint32_t r = 0; r < kRows; r++) {
        for (uint32_t b = 0; b < kBins; b++) {
            float a = from.bin[r][b];
            float z = to.bin[r][b];
            if (a != 0 || z != 0) lanes.push_back({r, b, a, (z - a) / float(frames)});
        }
    }
    return lanes;
}

// the voice's recent samples, the ear further from the sound reads them later
void PushEar(Voice& v, float x) {
    v.ear[v.ear_pos % kEarLength] = x;
    v.ear_pos++;
}
float EarAt(const Voice& v, float delay) {
    float back = std::max(delay, 0.0f);
    uint32_t whole = uint32_t(back);
    float part = back - float(whole);
    float now = v.ear[(v.ear_pos - 1 - whole) % kEarLength];
    float before = v.ear[(v.ear_pos - 2 - whole) % kEarLength];
    return now + (before - now) * part;
}

// a stream goes quiet: the synch hold ends, a started one loses its pause as well
void Deactivate(Voice& v) {
    v.sync = false;
    if (v.active) v.active = v.paused = v.ending = false;
}

// where a frame and the one after it are read from, for interpolation
struct Cursor {
    uint32_t data;
    uint32_t f;
    uint32_t next_data;
    uint32_t next_f;
};

// the stream's frame at its position, none once its packets ran out
std::optional<Cursor> StreamFrame(Voice& v) {
    uint32_t f = uint32_t(v.pos);
    while (!v.packets.empty() && f >= FramesOf(v, v.packets.front().size)) {
        Packet pk = v.packets.front();
        v.packets.pop_front();
        pk.used = pk.size;
        Complete(v, pk, 0);
        v.pos -= FramesOf(v, pk.size);
        f = uint32_t(v.pos);
    }
    if (v.packets.empty()) {
        v.pos = 0;
        if (v.ending) Deactivate(v);
        return std::nullopt;
    }
    Packet& pk = v.packets.front();
    Cursor at = {pk.data, f, pk.data, f};
    if (f + 1 < FramesOf(v, pk.size)) {
        at.next_f = f + 1;
    } else if (v.packets.size() > 1) {
        at.next_data = v.packets[1].data;
        at.next_f = 0;
    }
    return at;
}

// the buffer's frame at its position, looping or stopping at the end of its region; none once stopped
std::optional<Cursor> BufferFrame(Voice& v) {
    uint32_t f = uint32_t(v.pos);
    uint32_t start = v.looping ? FramesOf(v, v.loop_start) : 0;
    uint32_t end = FramesOf(v, v.looping ? v.loop_start + v.loop_len : v.play_len);
    if (f >= end) {
        if (!v.looping || end <= start) {
            v.playing = false;
            v.pos = 0;
            return std::nullopt;
        }
        v.pos = start + std::fmod(v.pos - start, double(end - start));
        f = uint32_t(v.pos);
    }
    if (!v.data) return std::nullopt;
    uint32_t base = v.data + v.play_start;
    Cursor at = {base, f, base, f};
    if (f + 1 < end) {
        at.next_f = f + 1;
    } else if (v.looping) {
        at.next_f = start;
    }
    return at;
}

int64_t NowMs();

constexpr uint32_t kOffsetStop = 0xFFFFFFFF;  // DSBPN_OFFSETSTOP
void SignalHandle(uint32_t handle) {
    if (ObjectRef o = FromHandle(handle)) SetEvent(*o);
}
// The events of the positions the play cursor reached after one byte up to another (wrapping when the buffer looped),
// and the stop events when it stopped. The position a play starts from is not reached: CMR asks for offset 0 on its
// samples to see a loop come round, and took one signalled as the play began for a finished sound and stopped it.
void Notify(const Voice& v, uint32_t from, uint32_t to, bool stopped) {
    for (const auto& [at, event] : v.notify) {
        bool passed;
        if (at == kOffsetStop) {
            passed = stopped;
        } else if (stopped) {
            passed = at > from;
        } else if (to >= from) {
            passed = at > from && at <= to;
        } else {
            passed = at > from || at <= to;
        }
        if (passed) SignalHandle(event);
    }
}

// adds the voice to the bins, frames of kBins interleaved, its levels ramped across the block
void MixVoice(Voice& v, float* bins, uint32_t frames) {
    Place at = Locate(v);
    Gains target = Levels(v, at);
    if (v.dialogue && g_mix.dialogue != 1.0f) {
        for (auto& row : target.bin)
            for (float& g : row) g *= g_mix.dialogue;
    }
    float itd = Itd(at.azimuth);
    if (!v.has_gain) {
        v.gain = target;
        v.itd = itd;
    }
    v.has_gain = true;
    std::vector<Lane> lanes = AudibleLanes(v.gain, target, frames);
    bool ears = at.on && v.channels == 1;
    float itd_step = (itd - v.itd) / float(frames);
    uint32_t channels = std::min<uint32_t>(v.channels, kMaxChannels);
    uint32_t read = ears ? 1u : 0u;  // the channels a lane plays, a bit each
    for (const Lane& lane : lanes) {
        if (lane.row < kMaxChannels) read |= 1u << lane.row;
    }
    double rate = double(v.freq ? v.freq : v.rate) / audio::kRate;
    uint32_t from = BytesOf(v, uint32_t(v.pos));  // for the notification positions the block passes
    v.block[v.last ^ 1] = -1;  // the title may have rewritten that block's memory since the last mix
    for (uint32_t i = 0; i < frames; i++) {
        std::optional<Cursor> cursor = v.stream ? StreamFrame(v) : BufferFrame(v);
        if (!cursor) break;
        float t = float(v.pos - cursor->f);
        float row[kRows] = {};
        for (uint32_t c = 0; c < channels; c++) {
            if (!(read >> c & 1)) continue;
            float x = Sample(v, cursor->data, cursor->f, int(c));
            row[c] = x + (Sample(v, cursor->next_data, cursor->next_f, int(c)) - x) * t;
        }
        if (ears) {
            v.itd += itd_step;
            PushEar(v, row[0]);
            row[kLeftEar] = EarAt(v, v.itd);
            row[kRightEar] = EarAt(v, -v.itd);
        }
        float* frame = bins + size_t(i) * kBins;
        for (Lane& lane : lanes) {
            lane.gain += lane.step;
            frame[lane.bin] += row[lane.row] * lane.gain;
        }
        v.pos += rate;
    }
    v.gain = target;
    v.itd = itd;
    if (!v.stream && !v.notify.empty()) Notify(v, from, BytesOf(v, uint32_t(v.pos)), !v.playing);
}

// XBE_AUDIO_REC=seconds: the first that many seconds of output saved beside the exe as audio_rec.wav, 16 bit at
// 48 kHz, 16 channels: the six speaker bins after the effects image (front left, right, centre, low frequency, back
// left, right); as voices send them, bins 13, 23, the centre, the 3D bins 6 to 9 and the reverb's 10; then the stereo
// output. For measuring a mix offline.
class Recorder {
public:
    static constexpr uint32_t kChannels = 16;
    bool On() { return Open(); }
    double Seconds() const { return double(written_) / audio::kRate; }  // into the recording
    void Add(const float* bins, const float* sends, const float* out, uint32_t frames) {
        std::vector<int16_t> pcm;
        pcm.reserve(size_t(frames) * kChannels);
        auto put = [&](float x) { pcm.push_back(int16_t(std::lrint(std::clamp(x, -1.0f, 1.0f) * 32767.0f))); };
        for (uint32_t i = 0; i < frames && written_ < limit_; i++, written_++) {
            const float* b = bins + size_t(i) * kBins;
            const float* in = sends + size_t(i) * kBins;
            for (uint32_t c = 0; c < 6; c++) put(b[c]);
            put(in[13]);
            put(in[23]);
            for (uint32_t c = 2; c <= 10; c += c == 2 ? 4 : 1) put(in[c]);  // centre, 3D bins 6 to 9, reverb 10
            put(out[i * 2]);
            put(out[i * 2 + 1]);
        }
        std::fwrite(pcm.data(), 2, pcm.size(), f_);
        // the header follows the data each second, so a file cut short still plays
        if (written_ >= limit_ || written_ - header_at_ >= audio::kRate) {
            Header();
            header_at_ = written_;
        }
        if (written_ >= limit_) {
            std::fclose(f_);
            f_ = nullptr;
            done_ = true;
            LOGI("dsound: recorded %u seconds to audio_rec.wav", unsigned(limit_ / audio::kRate));
        }
    }

private:
    bool Open() {
        if (f_) return true;
        if (done_ || tried_) return false;
        tried_ = true;
        const char* e = std::getenv("XBE_AUDIO_REC");
        if (!e) return false;
        limit_ = uint64_t(std::max(1, std::atoi(e))) * audio::kRate;
        f_ = std::fopen("audio_rec.wav", "wb");
        if (!f_) return false;
        Header();
        LOGI("dsound: recording %s seconds to audio_rec.wav", e);
        return true;
    }
    void Header() {
        auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f_); };
        auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f_); };
        uint32_t data = uint32_t(written_ * kChannels * 2);
        long at = std::ftell(f_);
        std::fseek(f_, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, f_);
        w32(36 + data);
        std::fwrite("WAVEfmt ", 1, 8, f_);
        w32(16);
        w16(1);
        w16(kChannels);
        w32(audio::kRate);
        w32(audio::kRate * kChannels * 2);
        w16(kChannels * 2);
        w16(16);
        std::fwrite("data", 1, 4, f_);
        w32(data);
        if (at > 44) std::fseek(f_, at, SEEK_SET);
    }
    FILE* f_ = nullptr;
    bool tried_ = false, done_ = false;
    uint64_t limit_ = 0, written_ = 0, header_at_ = 0;
};
Recorder g_recorder;

void Render(float* out, uint32_t frames) {
    static std::vector<float> bins;  // the output thread alone renders
    bins.assign(size_t(frames) * kBins, 0.0f);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    // while recording, each voice's level, and once a second the loudest: what is playing when, by name
    bool rec = g_recorder.On();
    static std::vector<float> before;
    static std::map<uint32_t, double> energy;
    for (auto& [obj, v] : g_voices) {
        if (v.paused || v.sync || (!v.stream && !v.playing)) continue;
        if (rec) before = bins;
        MixVoice(v, bins.data(), frames);
        if (rec) {
            double e = 0;
            for (size_t k = 0; k < bins.size(); k++) e += double(bins[k] - before[k]) * (bins[k] - before[k]);
            energy[obj] += e;
        }
    }
    static double next_report = 1;
    if (rec && g_recorder.Seconds() >= next_report) {
        std::vector<std::pair<double, uint32_t>> loud;
        for (auto& [obj, e] : energy) loud.push_back({e, obj});
        std::sort(loud.rbegin(), loud.rend());
        std::string t;
        for (size_t k = 0; k < loud.size() && k < 5; k++) {
            auto it = g_voices.find(loud[k].second);
            if (it == g_voices.end() || loud[k].first <= 0) continue;
            const Voice& v = it->second;
            char b[160];
            int n = std::snprintf(b, sizeof b, " | %08X %.0f dB %s%s%s sample %08X bins", v.obj,
                                  10 * std::log10(loud[k].first / audio::kRate + 1e-12), v.stream ? "stream" : "buffer",
                                  v.looping ? " looped" : "", v.is3d ? " 3D" : "", v.stream ? 0 : v.data);
            for (uint32_t i = 0; i < v.nbins && n < int(sizeof b) - 12; i++)
                n += std::snprintf(b + n, sizeof b - size_t(n), " %u:%d", v.bin[i], v.bin_mb[v.bin[i]]);
            t += b;
        }
        LOGI("dsound: loudest at %.0f s%s", next_report, t.empty() ? " | none" : t.c_str());
        energy.clear();
        next_report += 1;
    }
    static std::vector<float> sends;  // what the effects image reads, for the recording
    if (g_recorder.On()) sends = bins;
    if (ImageLoaded()) (g_gp ? dsp::Run : dsp::legacy::Run)(bins.data(), frames);
    static LowPass sub_lp;
    static uint32_t sub_hz = ~0u;
    if (sub_hz != g_mix.sub_hz) sub_lp.Set(float(sub_hz = g_mix.sub_hz));
    // a peak limiter instead of clipping: instant down, back up over about 100 ms
    static float limit = 1.0f;
    constexpr float kCeiling = 0.98f, kRelease = 0.9998f;
    for (uint32_t i = 0; i < frames; i++) {
        const float* b = &bins[size_t(i) * kBins];
        float sub = g_mix.sub * sub_lp.Run(b[kSub]), centre = g_mix.centre * b[kCentre];
        float l = g_mix.front * b[kFrontLeft] + centre + g_mix.back * b[kBackLeft] + sub;
        float r = g_mix.front * b[kFrontRight] + centre + g_mix.back * b[kBackRight] + sub;
        float peak = std::max(std::fabs(l), std::fabs(r)) * limit;
        if (peak > kCeiling) limit *= kCeiling / peak;
        out[i * 2] = std::clamp(l * limit, -1.0f, 1.0f);
        out[i * 2 + 1] = std::clamp(r * limit, -1.0f, 1.0f);
        limit = std::min(1.0f, limit / kRelease);
    }
    if (g_recorder.On()) g_recorder.Add(bins.data(), sends.data(), out, frames);
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Run(X86Context& ctx, const std::vector<Done>& done) {
    for (const Done& d : done) CallGuest(ctx, d.callback, {d.context, d.packet_context, d.status});
}

void Start() {
    if (g_started) return;
    g_started = true;
    audio::StartOutput(Render);
    // for a title that never calls DirectSoundDoWork: its callbacks come from here instead
    StartHostThread("dsound", [](X86Context& ctx) {
        constexpr int64_t kPumpMs = 250;
        for (;;) {
            std::vector<Done> done;
            {
                std::unique_lock<std::recursive_mutex> lock(g_lock);
                g_done_cv.wait_for(lock, std::chrono::milliseconds(50));
                if (NowMs() - g_pumped < kPumpMs) continue;
                done.assign(g_done.begin(), g_done.end());
                g_done.clear();
            }
            Run(ctx, done);
        }
    });
}

// WAVEFORMATEX
void ReadFormat(Voice& v, uint32_t wfx) {
    if (!wfx) return;
    v.tag = R16(wfx);
    v.channels = std::max<uint16_t>(R16(wfx + 2), 1);
    v.rate = R32(wfx + 4);
    v.align = std::max<uint16_t>(R16(wfx + 12), 1);
    v.bits = R16(wfx + 14);
    if (v.tag != 1 && v.tag != 0x69) LOG_FIRST(8, "dsound: format tag %X not supported", v.tag);
}

// DSMIXBINS: the voice's bins and their volumes
void SetBins(Voice& v, uint32_t mixbins) {
    v.nbins = 0;  // none listed: front left and right
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 8), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) {
        v.bin[i] = R32(pairs + i * 8) & 31;
        v.bin_mb[v.bin[i]] = int32_t(R32(pairs + i * 8 + 4));
    }
    v.nbins = uint8_t(n);
    // dialogue, as the XDK's samples route it: the same level on front left, right and centre, every other bin silent
    // (CMR's co-driver; his calls and the bed under them meet on those three speakers, the mix settings tell them apart)
    {
        bool fl = false, fr = false, c = false, other = false;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t b = v.bin[i];
            int32_t mb = v.bin_mb[b];
            if (b == kFrontLeft) fl = true;
            else if (b == kFrontRight) fr = true;
            else if (b == kCentre) c = true;
            else if (mb > -10000) other = true;
        }
        v.dialogue = fl && fr && c && !other && v.bin_mb[kFrontLeft] == v.bin_mb[kCentre] &&
                     v.bin_mb[kFrontRight] == v.bin_mb[kCentre];
    }
    // each distinct bin list with its volumes (the first 48), to follow a title's routing
    {
        char t[200] = "";
        for (uint32_t i = 0; i < n; i++)
            snprintf(t + std::strlen(t), sizeof(t) - std::strlen(t), " %u:%d", v.bin[i], v.bin_mb[v.bin[i]]);
        static std::set<std::string> seen;
        if (seen.size() < 48 && seen.insert(t).second) LOGI("dsound: voice %08X bins%s", v.obj, t);
    }
}
// volumes only, the bins stay
void SetBinVolumes(Voice& v, uint32_t mixbins) {
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 32), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) v.bin_mb[R32(pairs + i * 8) & 31] = int32_t(R32(pairs + i * 8 + 4));
}
// 600 mB unless 3D or a mix in or effect in buffer
float DefaultHeadroom(uint32_t flags) { return flags & 0x382010 ? 1.0f : Db(-600); }

Voice* Find(uint32_t obj) {
    auto it = g_voices.find(obj);
    return it == g_voices.end() ? nullptr : &it->second;
}

uint32_t NewBuffer(uint32_t desc, uint32_t out) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t obj = mem::AllocSystem(0x40);
    std::memset(HostPtr(obj), 0, 0x40);
    Voice& v = g_voices[obj];
    v.obj = obj;
    uint32_t flags = R32(desc + 4), bytes = R32(desc + 8);  // DSBUFFERDESC: size, flags, bytes, format, mix bins
    ReadFormat(v, R32(desc + 12));
    SetBins(v, R32(desc + 16));
    v.is3d = flags & 0x10;  // DSBCAPS_CTRL3D
    v.headroom = DefaultHeadroom(flags);
    if (bytes) {
        v.data = v.owned = mem::AllocContiguous(bytes, 0, 0xFFFFFFFF, 4);
        v.bytes = v.play_len = v.loop_len = bytes;
    }
    W32(out, obj);
    return 0;
}

// the effect's state segment when offset and size lie inside it, else 0
uint32_t EffectState(uint32_t effect, uint32_t offset, uint32_t size) {
    if (effect >= g_effect_states.size()) return 0;
    const Segment& state = g_effect_states[effect];
    if (offset > state.bytes || size > state.bytes - offset) return 0;
    return state.at;
}

float F32(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

}  // namespace

// device

// DSOUND objects are the host's; .XTLID names linked exports that would still run console code on them
static void AuditExports() {
    static const uint32_t handled[] = {
        SYM(DirectSoundCreate), SYM(IDirectSound_Release), SYM(DirectSoundUseFullHRTF), SYM(DirectSoundUseLightHRTF),
        SYM(IDirectSound_GetSpeakerConfig),
        SYM(IDirectSound_SetEffectData), SYM(IDirectSound_EnableHeadphones), SYM(IDirectSound_SetMixBinHeadroom),
        SYM(IDirectSound_CommitDeferredSettings), SYM(IDirectSound_SetDopplerFactor), SYM(IDirectSound_SetRolloffFactor),
        SYM(IDirectSound_SetDistanceFactor), SYM(IDirectSound_SetVelocity), SYM(IDirectSound_SetI3DL2Listener),
        SYM(IDirectSound_SetPosition), SYM(IDirectSound_SetOrientation), SYM(IDirectSound_SynchPlayback), SYM(DirectSoundDoWork),
        SYM(IDirectSound_DownloadEffectsImage), SYM(IDirectSound_GetEffectData), SYM(IDirectSound_CommitEffectData),
        SYM(IDirectSound_GetOutputLevels), SYM(IDirectSoundBuffer_SetPlayRegion), SYM(IDirectSoundBuffer_SetAllParameters),
        SYM(IDirectSoundBuffer_SetFormat), SYM(IDirectSoundBuffer_SetOutputBuffer), SYM(IDirectSoundBuffer_SetRolloffCurve),
        SYM(IDirectSoundBuffer_StopEx), SYM(IDirectSoundBuffer_Pause), SYM(IDirectSoundBuffer_SetEG),
        SYM(XAudioDownloadEffectsImage), SYM(DirectSoundCreateBuffer), SYM(IDirectSound_CreateSoundBuffer),
        SYM(IDirectSoundBuffer_Release), SYM(IDirectSoundBuffer_Unlock), SYM(IDirectSoundBuffer_SetBufferData),
        SYM(IDirectSoundBuffer_Play), SYM(IDirectSoundBuffer_Stop), SYM(IDirectSoundBuffer_SetLoopRegion),
        SYM(IDirectSoundBuffer_GetStatus), SYM(IDirectSoundBuffer_GetCurrentPosition), SYM(IDirectSoundBuffer_SetCurrentPosition),
        SYM(IDirectSoundBuffer_Lock), SYM(IDirectSoundBuffer_SetVolume), SYM(IDirectSoundBuffer_SetFrequency),
        SYM(IDirectSoundBuffer_SetFilter), SYM(IDirectSoundBuffer_SetHeadroom), SYM(IDirectSoundBuffer_SetMixBinVolumes),
        SYM(IDirectSoundBuffer_SetMixBins), SYM(IDirectSoundBuffer_SetMaxDistance), SYM(IDirectSoundBuffer_SetMinDistance),
        SYM(IDirectSoundBuffer_SetPosition), SYM(IDirectSoundBuffer_SetVelocity), SYM(IDirectSoundBuffer_SetI3DL2Source),
        SYM(DirectSoundCreateStream), SYM(CDirectSoundStream_AddRef), SYM(CDirectSoundStream_SetFormat),
        SYM(CDirectSoundStream_Release), SYM(CDirectSoundStream_GetInfo), SYM(CDirectSoundStream_GetStatus),
        SYM(CDirectSoundStream_Process), SYM(CDirectSoundStream_Discontinuity), SYM(CDirectSoundStream_Flush),
        SYM(CDirectSoundStream_FlushEx), SYM(CDirectSoundStream_Pause), SYM(CDirectSoundStream_SetVolume),
        SYM(CDirectSoundStream_SetFrequency), SYM(CDirectSoundStream_SetFilter), SYM(CDirectSoundStream_SetHeadroom),
        SYM(CDirectSoundStream_SetMixBinVolumes), SYM(CDirectSoundStream_SetMixBins), SYM(CDirectSoundStream_SetMaxDistance),
        SYM(CDirectSoundStream_SetMinDistance), SYM(CDirectSoundStream_SetPosition), SYM(CDirectSoundStream_SetVelocity),
        SYM(CDirectSoundStream_SetI3DL2Source), SYM(IDirectSoundStream_SetEG)};
    std::string left;
    // the library's own section: the WMA decoder shares the export ids and touches no DSOUND object
    uint32_t lo = 0, hi = 0;
    for (auto& s : g_image->sections)
        if (SYM(DirectSoundCreate) - s.va < s.vsize) lo = s.va, hi = s.va + s.vsize;
    for (auto& s : g_image->sections)
        for (uint32_t o = 0; s.name == ".XTLID" && o + 8 <= std::min(s.vsize, s.rsize); o += 8) {
            uint32_t id = g_image->U32(s.raw + o), va = g_image->U32(s.raw + o + 4);
            auto done = [&](uint32_t f) { return std::find(std::begin(handled), std::end(handled), f) != std::end(handled); };
            // a jmp to a host version is covered by it
            if (id >> 16 != 0xB || va < lo || va >= hi || !dispatch::Find(va) || done(va) ||  // 0xB: DSOUND's library id
                (G<uint8_t>(va) == 0xE9 && done(va + 5 + R32(va + 1))))
                continue;
            char b[24];
            std::snprintf(b, sizeof b, " %05X at %08X", id, va);
            left += b;
        }
    if (!left.empty()) LOGW("dsound: exports running their original code:%s", left.c_str());
}

HLE_STDCALL(DirectSoundCreate, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    if (!g_ds) {
        AuditExports();
        g_ds = mem::AllocSystem(0x40);
        std::memset(HostPtr(g_ds), 0, 0x40);
    }
    W32(a[1], g_ds);
    return 0;
}
HLE_STDCALL(IDirectSound_Release, 1) { return 0; }
HLE_STDCALL(DirectSoundUseFullHRTF, 0) { return 0; }
HLE_STDCALL(DirectSoundUseLightHRTF, 0) { return 0; }
HLE_STDCALL(IDirectSound_GetSpeakerConfig, 2) {
    W32(a[1], 0);  // DSSPEAKER_STEREO
    return 0;
}
// (this, effect, offset, data, size, flags): writes the effect's state
HLE_STDCALL(IDirectSound_SetEffectData, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t state = EffectState(a[1], a[2], a[4]);
    if (!state || !a[3]) return kInvalidCall;
    std::memcpy(HostPtr(state + a[2]), HostPtr(a[3]), a[4]);
    return 0;
}
HLE_STDCALL(IDirectSound_EnableHeadphones, 2) { return 0; }
// (this, mix bin, headroom)
HLE_STDCALL(IDirectSound_SetMixBinHeadroom, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (a[1] < kBins) g_bin_headroom[a[1]] = a[2] & 7;
    return 0;
}
HLE_STDCALL(IDirectSound_CommitDeferredSettings, 1) { return 0; }
HLE_STDCALL(IDirectSound_SetDopplerFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetRolloffFactor, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_rolloff = F32(a[1]);
    LOG_FIRST(2, "dsound: listener rolloff factor %g", g_rolloff);
    return 0;
}
HLE_STDCALL(IDirectSound_SetDistanceFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetVelocity, 5) { return 0; }
// DSOUND's conversion of an I3DL2 listener into the reverb effect's state, found in its section by its first bytes:
// a thiscall on an object whose first dword is the state, with (listener, state). The listener it reads is a dword
// then DSI3DL2LISTENER; it rewrites the state's delays, gains and filters in place.
static uint32_t ListenerToReverb() {
    static const uint8_t kStart[] = {0x55, 0x8B, 0xEC, 0x51, 0x51, 0x8B, 0x45, 0x0C, 0x56, 0x57, 0x8B,
                                     0x7D, 0x08, 0x8B, 0xF1, 0x89, 0x06, 0xD9, 0x47, 0x28, 0xD8, 0x0D};
    static uint32_t found = ~0u;
    if (found != ~0u) return found;
    found = 0;
    for (auto& s : g_image->sections) {
        if (s.name != "DSOUND") continue;
        const uint8_t* p = static_cast<const uint8_t*>(HostPtr(s.va));
        for (uint32_t o = 0; o + sizeof kStart <= s.vsize; o++)
            if (std::memcmp(p + o, kStart, sizeof kStart) == 0) {
                if (found) {
                    LOGE("dsound: I3DL2 listener conversion found twice, not used");
                    return found = 0;
                }
                found = s.va + o;
            }
    }
    if (!found) LOGE("dsound: I3DL2 listener conversion not found, the reverb keeps the image's settings");
    return found;
}

// (this, DSI3DL2LISTENER, apply): as DSOUND commits a listener (0x2225A2 in CMR): its conversion runs on a copy of
// the reverb's first 0x118 state bytes, then only the flags word, with bit 2 set (the reverb takes new parameters,
// fading across), and the parameters at 0x118..0x220 go back to the state segment for the GP. Writing the whole
// converted copy instead changed the delay lines under the running reverb: a stale stretch of engine sound could
// then loop on through pauses.
HLE_STDCALL(IDirectSound_SetI3DL2Listener, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_gp) {
        // the stand-in reverb takes the listener as floats; room, room HF, reflections and reverb are integers
        float p[12];
        for (int i = 0; i < 12 && a[1]; i++) {
            uint32_t v = R32(a[1] + i * 4);
            bool integer = i < 2 || i == 5 || i == 7;
            p[i] = integer ? float(int32_t(v)) : F32(v);
        }
        if (a[1]) dsp::legacy::SetListener(p);
        return 0;
    }
    uint32_t fn = ListenerToReverb();
    if (!fn || !a[1] || g_reverb_effect < 0 || size_t(g_reverb_effect) >= g_effect_states.size()) return 0;
    constexpr uint32_t kRead = 0x118, kFlags = 0x10, kParams = 0x118, kParamsEnd = 0x220, kCopy = 0x224;
    constexpr uint32_t kNewParameters = 4;
    const Segment& seg = g_effect_states[g_reverb_effect];
    if (seg.bytes < kParamsEnd) return 0;
    static uint32_t scratch = 0;  // the conversion's object, its listener, the state copy
    if (!scratch) scratch = mem::AllocSystem(4 + 4 + 48 + kCopy);
    uint32_t listener = scratch + 4, state = scratch + 56;
    std::memset(HostPtr(state), 0, kCopy);
    std::memcpy(HostPtr(state), HostPtr(seg.at), kRead);
    W32(scratch, state);
    W32(listener, 0);
    std::memcpy(HostPtr(listener + 4), HostPtr(a[1]), 48);
    uint32_t ecx = ctx.ecx;
    // DSPDUMP set: the I3DL2 "generic" and "mountains" presets converted on copies of the whole state, for testing
    // the reverb offline
    static bool dumped = false;
    if (std::getenv("DSPDUMP") && !dumped) {
        dumped = true;
        static const uint32_t kPresets[2][12] = {
            {uint32_t(-1000), uint32_t(-100), 0, 0x3FBEB852 /* 1.49 */, 0x3F547AE1 /* 0.83 */, uint32_t(-2602),
             0x3BE56042 /* 0.007 */, 200, 0x3C343958 /* 0.011 */, 0x42C80000 /* 100 */, 0x42C80000, 0x459C4000 /* 5000 */},
            {uint32_t(-1000), uint32_t(-2500), 0, 0x3FBEB852 /* 1.49 */, 0x3E570A3D /* 0.21 */, uint32_t(-2780),
             0x3E99999A /* 0.3 */, uint32_t(-2014), 0x3DCCCCCD /* 0.1 */, 0x41D80000 /* 27 */, 0x42C80000, 0x459C4000}};
        static const char* kNames[2] = {"dsp_reverb_generic.bin", "dsp_reverb_mountains.bin"};
        uint32_t copy = mem::AllocSystem(seg.bytes);
        for (int k = 0; k < 2; k++) {
            std::memcpy(HostPtr(copy), HostPtr(seg.at), seg.bytes);
            std::memcpy(HostPtr(listener + 4), kPresets[k], 48);
            W32(scratch, copy);
            ctx.ecx = scratch;
            CallGuest(ctx, fn, {listener, copy});
            if (FILE* f = std::fopen(kNames[k], "wb")) {
                std::fwrite(HostPtr(copy), 1, seg.bytes, f);
                std::fclose(f);
            }
        }
        W32(scratch, state);
        std::memcpy(HostPtr(listener + 4), HostPtr(a[1]), 48);
    }
    ctx.ecx = scratch;
    CallGuest(ctx, fn, {listener, state});
    ctx.ecx = ecx;
    W32(seg.at + kFlags, R32(state + kFlags) | kNewParameters);
    std::memcpy(HostPtr(seg.at + kParams), HostPtr(state + kParams), kParamsEnd - kParams);
    // each listener that differs from the last, whole, to replay a stage's reverb offline
    static uint32_t last[12] = {};
    if (std::memcmp(last, HostPtr(a[1]), 48) != 0) {
        std::memcpy(last, HostPtr(a[1]), 48);
        auto i = [&](int k) { return int32_t(last[k]); };
        auto f = [&](int k) { return F32(last[k]); };
        LOGI("dsound: I3DL2 listener room %d HF %d rolloff %g decay %g HF ratio %g reflections %d delay %g reverb %d "
             "delay %g diffusion %g density %g HF reference %g",
             i(0), i(1), f(2), f(3), f(4), i(5), f(6), i(7), f(8), f(9), f(10), f(11));
    }
    return 0;
}
HLE_STDCALL(IDirectSound_SetPosition, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) g_listener[i] = F32(a[1 + i]);
    return 0;
}
HLE_STDCALL(IDirectSound_SetOrientation, 8) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) {
        g_front[i] = F32(a[1 + i]);
        g_top[i] = F32(a[4 + i]);
    }
    return 0;
}
HLE_STDCALL(DirectSoundDoWork, 0) {
    g_pumped = NowMs();
    std::vector<Done> done;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        done.assign(g_done.begin(), g_done.end());
        g_done.clear();
    }
    Run(ctx, done);
    return 0;
}
// starts together every voice played or unpaused with the synch playback flag
HLE_STDCALL(IDirectSound_SynchPlayback, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (auto& [obj, v] : g_voices) v.sync = false;
    return 0;
}

// DSOUND's GP program, which the library downloads to P:0 when it starts the GP: found in its section by its first
// words, and taken up to the effects chain's empty entry at P:0x171 (nop, rts) that the image's effects replace
static std::vector<uint8_t> GpProgram() {
    static const uint32_t kStart[] = {0x0BF080, 0x000155, 0x300600, 0x310000};
    constexpr uint32_t kWords = 0x173;
    for (auto& s : g_image->sections) {
        if (s.name != "DSOUND") continue;
        const uint8_t* p = static_cast<const uint8_t*>(HostPtr(s.va));
        for (size_t o = 0; o + kWords * 4 <= s.vsize; o += 4) {
            if (std::memcmp(p + o, kStart, sizeof kStart) != 0) continue;
            uint32_t entry[2];
            std::memcpy(entry, p + o + (kWords - 2) * 4, 8);
            if (entry[0] == 0 && entry[1] == 0xC) return {p + o, p + o + kWords * 4};
        }
    }
    return {};
}

// The descriptor keeps the image's effect list so the title finds its indices. State segments hold the image's plain
// state, kept in step with the GP's X memory (xdk/dsp_image.cpp); code, Y memory and scratch point at zeroed memory.
// loc: DSEFFECTIMAGELOC, the I3DL2 reverb's index first
static uint32_t EffectsDesc(const std::vector<uint8_t>& img, const char* name, uint32_t loc) {
    constexpr int kStateSegment = 1;
    XDK_REQUIRE("dsound", XDK_KEY(EffectsImage_Skip0), XDK_KEY(EffectsImage_Skip1), XDK_KEY(EffectsImage_Desc));
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= img.size()) std::memcpy(&v, &img[o], 4); return v; };
    size_t t = XDK_EffectsImage_Desc + size_t(u32(XDK_EffectsImage_Skip0) + u32(XDK_EffectsImage_Skip1)) * 4;
    uint32_t count = std::min<uint32_t>(u32(t), 64);
    LOGI("dsound: effects image %s, %u effects", name, count);
    uint32_t desc = mem::AllocSystem(8 + std::max(count, 1u) * 32);
    W32(desc, count);
    W32(desc + 4, u32(t + 4));
    std::vector<Segment> states;
    for (uint32_t i = 0; i < count; i++) {
        size_t m = t + 8 + i * 32;
        uint32_t d = desc + 8 + i * 32;
        for (int k = 0; k < 4; k++) {
            uint32_t size = u32(m + k * 8 + 4);
            uint32_t seg = mem::AllocSystem(std::max(size, 0x40u));
            std::memset(HostPtr(seg), 0, std::max(size, 0x40u));
            uint32_t at = u32(m + k * 8);
            bool inside = at <= img.size() && size <= img.size() - at;
            if (k == kStateSegment && inside) std::memcpy(HostPtr(seg), img.data() + at, size);
            if (k == kStateSegment) states.push_back({seg, size});
            W32(d + k * 8, seg);
            W32(d + k * 8 + 4, size);
        }
    }
    int32_t reverb = loc ? int32_t(R32(loc)) : -1;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_effect_states = states;
    g_reverb_effect = reverb;
    std::vector<uint32_t> addresses;
    for (const Segment& s : states) addresses.push_back(s.at);
    if (g_gp) dsp::Load(img, t, addresses, reverb, GpProgram());
    else dsp::legacy::Load(img, t, addresses, reverb);
    return desc;
}

HLE_STDCALL(XAudioDownloadEffectsImage, 4) {
    std::string name = ReadCString(a[0]);
    std::vector<uint8_t> img;
    if (a[2] & 1) {
        for (auto& sec : g_image->sections)
            if (sec.name == name) img.assign(static_cast<uint8_t*>(HostPtr(sec.va)), static_cast<uint8_t*>(HostPtr(sec.va)) + sec.vsize);
    } else {
        ReadGameFile(name, img);
    }
    uint32_t desc = EffectsDesc(img, name.c_str(), a[1]);
    if (a[3]) W32(a[3], desc);
    return 0;
}

// (this, image, size, location, descriptor out): the title loaded the image itself
HLE_STDCALL(IDirectSound_DownloadEffectsImage, 5) {
    std::vector<uint8_t> img(static_cast<uint8_t*>(HostPtr(a[1])), static_cast<uint8_t*>(HostPtr(a[1])) + a[2]);
    uint32_t desc = EffectsDesc(img, "from memory", a[3]);
    if (a[4]) W32(a[4], desc);
    return 0;
}
// (this, effect, offset, data, size): reads the effect's state
HLE_STDCALL(IDirectSound_GetEffectData, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t state = EffectState(a[1], a[2], a[4]);
    if (!state || !a[3]) return kInvalidCall;
    std::memcpy(HostPtr(a[3]), HostPtr(state + a[2]), a[4]);
    return 0;
}
HLE_STDCALL(IDirectSound_CommitEffectData, 1) { return 0; }
// (this, DSOUTPUTLEVELS out, reset peaks): 16 dwords of meters, silent
HLE_STDCALL(IDirectSound_GetOutputLevels, 3) {
    if (a[1]) std::memset(HostPtr(a[1]), 0, 64);
    return 0;
}

// buffers

HLE_STDCALL(DirectSoundCreateBuffer, 2) { return NewBuffer(a[0], a[1]); }
HLE_STDCALL(IDirectSound_CreateSoundBuffer, 4) { return NewBuffer(a[1], a[2]); }

#define BUFFER(name, nargs)                                 \
    static uint32_t buf_##name(Voice& v, const Args& a);    \
    HLE_STDCALL(IDirectSoundBuffer_##name, nargs) {         \
        std::lock_guard<std::recursive_mutex> lock(g_lock); \
        Voice* v = Find(a[0]);                              \
        return v ? buf_##name(*v, a) : 0x80004005u;         \
    }                                                       \
    static uint32_t buf_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

BUFFER(Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    if (v.owned) mem::FreeContiguous(v.owned);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    mem::FreeSystem(obj);
    return 0;
}
BUFFER(Unlock, 5) { return 0; }
BUFFER(SetBufferData, 3) {
    if (a[1] == v.data && a[2] == v.bytes) return 0;
    // the library frees its own memory, and makes it again for a size with no data
    if (v.owned && v.owned != a[1]) {
        mem::FreeContiguous(v.owned);
        v.owned = 0;
    }
    v.data = a[1];
    if (!a[1] && a[2]) v.data = v.owned = mem::AllocContiguous(a[2], 0, 0xFFFFFFFF, 4);
    v.bytes = v.play_len = v.loop_len = a[2];
    v.play_start = v.loop_start = 0;
    v.pos = 0;
    ForgetBlocks(v);
    return 0;
}
BUFFER(Play, 4) {
    v.has_gain = false;  // a new sound does not ramp from the last one's levels
    v.playing = true;
    v.looping = a[3] & 1;
    if (a[3] & 2) v.pos = 0;
    v.sync = a[3] & 4;
    // while recording, every sound started, at its time in audio_rec.wav: which sample (its address), how long, at
    // what pitch and volume, to which bins. To tell whether a sound (CMR's backfires) is played at all.
    if (g_recorder.On()) {
        char t[200] = "";
        for (uint32_t i = 0, n = 0; i < v.nbins && n < sizeof(t) - 16; i++)
            n += uint32_t(std::snprintf(t + n, sizeof(t) - n, " %u:%d", v.bin[i], v.bin_mb[v.bin[i]]));
        uint32_t rate = v.freq ? v.freq : v.rate;
        LOGI("dsound: play at %.2f s: voice %08X sample %08X %u ms at %u Hz%s%s volume %.1f dB bins%s",
             g_recorder.Seconds(), v.obj, v.data + v.play_start, unsigned(1000.0 * FramesOf(v, v.play_len) / rate),
             rate, v.looping ? " looped" : "", v.is3d ? " 3D" : "", 20 * std::log10(std::max(v.volume, 1e-5f)), t);
    }
    return 0;
}
BUFFER(Stop, 1) {
    if (v.playing)
        for (const auto& [at, event] : v.notify)
            if (at == kOffsetStop) SignalHandle(event);
    v.playing = false;
    return 0;
}
// (this, time low, time high, flags): stops now, the release envelope is not reproduced
BUFFER(StopEx, 4) {
    v.playing = false;
    return 0;
}
// (this, flags): 0 resumes, 1 pauses, 2 holds until IDirectSound_SynchPlayback
BUFFER(Pause, 2) {
    v.paused = a[1] == 1;
    v.sync = a[1] == 2;
    return 0;
}
// envelopes are not played
BUFFER(SetEG, 2) { return 0; }
// (this, start, length) in bytes, 0 length plays to the end. The loop becomes the whole region.
BUFFER(SetPlayRegion, 3) {
    if (a[1] > v.bytes || a[2] > v.bytes - a[1]) return 0x88780032u;  // DSERR_INVALIDCALL
    v.play_start = a[1];
    v.play_len = v.loop_len = a[2] ? a[2] : v.bytes - a[1];
    v.loop_start = 0;
    v.pos = 0;
    ForgetBlocks(v);
    return 0;
}
BUFFER(SetLoopRegion, 3) {
    if (a[1] > v.play_len || a[2] > v.play_len - a[1]) return 0x88780032u;
    v.loop_start = a[1];
    v.loop_len = a[2] ? a[2] : v.play_len - a[1];
    return 0;
}
BUFFER(GetStatus, 2) {
    W32(a[1], (v.playing ? 1u : 0u) | (v.playing && v.looping ? 4u : 0u));  // DSBSTATUS_PLAYING, DSBSTATUS_LOOPING
    return 0;
}
BUFFER(GetCurrentPosition, 3) {
    uint32_t p = BytesOf(v, uint32_t(v.pos));
    if (a[1]) W32(a[1], p);
    if (a[2]) W32(a[2], v.play_len ? (p + BytesOf(v, 64 * 4)) % v.play_len : 0);
    return 0;
}
BUFFER(SetCurrentPosition, 2) {
    v.pos = FramesOf(v, a[1]);
    return 0;
}
BUFFER(Lock, 8) {
    uint32_t off = a[1], n = a[2];
    if (a[7] & 1) n = v.bytes;  // DSBLOCK_ENTIREBUFFER
    if (!v.bytes) return 0x80004005u;  // E_FAIL
    off %= v.bytes;
    uint32_t first = std::min(n, v.bytes - off);
    W32(a[3], v.data + off);
    W32(a[4], first);
    if (a[5]) W32(a[5], n > first ? v.data : 0);
    if (a[6]) W32(a[6], n > first ? n - first : 0);
    return 0;
}
BUFFER(SetVolume, 2) {
    v.volume = Db(int32_t(a[1]));
    return 0;
}
BUFFER(SetFrequency, 2) {
    v.freq = a[1];
    return 0;
}
BUFFER(SetFilter, 2) { return 0; }
BUFFER(SetHeadroom, 2) {
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
BUFFER(SetMixBinVolumes, 2) {
    SetBinVolumes(v, a[1]);
    return 0;
}
BUFFER(SetMixBins, 2) {
    SetBins(v, a[1]);
    return 0;
}
BUFFER(SetMaxDistance, 3) {
    v.max_dist = F32(a[1]);
    return 0;
}
BUFFER(SetMinDistance, 3) {
    v.min_dist = F32(a[1]);
    return 0;
}
BUFFER(SetPosition, 5) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(a[1 + i]);
    return 0;
}
BUFFER(SetVelocity, 5) { return 0; }
BUFFER(SetI3DL2Source, 3) { return 0; }
// (this, DS3DBUFFER, apply): position at +4, distances at +0x34 and +0x38, mode at +0x3C
BUFFER(SetAllParameters, 3) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(R32(a[1] + 4 + i * 4));
    v.min_dist = F32(R32(a[1] + 0x34));
    v.max_dist = F32(R32(a[1] + 0x38));
    v.mode = R32(a[1] + 0x3C);
    return 0;
}
// (this, points, count, apply)
BUFFER(SetRolloffCurve, 4) {
    v.curve.clear();
    for (uint32_t i = 0; a[1] && i < std::min<uint32_t>(a[2], 64); i++) v.curve.push_back(F32(R32(a[1] + i * 4)));
    return 0;
}
BUFFER(SetOutputBuffer, 2) { return 0; }  // the voice keeps its own mix bins
BUFFER(SetFormat, 2) {
    ReadFormat(v, a[1]);
    v.freq = 0;
    ForgetBlocks(v);
    return 0;
}

// streams, the object carries the XDK's own vtable

HLE_STDCALL(DirectSoundCreateStream, 2) {
    XDK_REQUIRE("dsound", XDK_KEY(CDirectSoundStream_Vtable), XDK_KEY(CDirectSoundStream_Vtable2),
                XDK_DATA(DSound_StreamVtable), XDK_DATA(DSound_StreamVtable2));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t desc = a[0];
    uint32_t obj = mem::AllocSystem(0x80);
    std::memset(HostPtr(obj), 0, 0x80);
    W32(obj + XDK_CDirectSoundStream_Vtable, kStreamVtable);
    W32(obj + XDK_CDirectSoundStream_Vtable2, kStreamVtable2);
    Voice& v = g_voices[obj];
    v.obj = obj;
    v.stream = true;
    uint32_t flags = R32(desc);
    v.max_packets = std::max<uint32_t>(R32(desc + 4), 1);
    ReadFormat(v, R32(desc + 8));
    v.callback = R32(desc + 12);
    v.context = R32(desc + 16);
    SetBins(v, R32(desc + 20));
    v.is3d = flags & 0x10;  // DSBCAPS_CTRL3D
    v.headroom = DefaultHeadroom(flags);
    W32(a[1], obj);
    return 0;
}

#define STREAM(name, nargs)                                     \
    static uint32_t str_##name(Voice& v, const Args& a);        \
    HLE_STDCALL(CDirectSoundStream_##name, nargs) {             \
        uint32_t r;                                             \
        std::vector<Done> flushed;                              \
        {                                                       \
            std::lock_guard<std::recursive_mutex> lock(g_lock); \
            Voice* v = Find(a[0]);                              \
            r = v ? str_##name(*v, a) : 0x80004005u;            \
            flushed.swap(t_flushed);                            \
        }                                                       \
        Run(ctx, flushed);                                      \
        return r;                                               \
    }                                                           \
    static uint32_t str_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

// the stream's callbacks still queued and those of its flushed packets, in order, go to the calling thread
static void FlushStream(Voice& v) {
    for (auto& pk : v.packets) Complete(v, pk, kFlushed);
    v.packets.clear();
    v.pos = 0;
    for (auto it = g_done.begin(); it != g_done.end();) {
        if (it->voice != v.obj) {
            ++it;
            continue;
        }
        t_flushed.push_back(*it);
        it = g_done.erase(it);
    }
    Deactivate(v);
}

STREAM(AddRef, 1) { return uint32_t(++v.refs); }
STREAM(SetFormat, 2) {
    FlushStream(v);
    ReadFormat(v, a[1]);
    v.freq = 0;  // the XDK sets the pitch back to the new format's own rate
    ForgetBlocks(v);
    return 0;
}
STREAM(Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    FlushStream(v);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    mem::FreeSystem(obj);
    return 0;
}
STREAM(GetInfo, 2) {
    uint32_t info = a[1];
    W32(info, 0x3);  // fixed sample size, fixed packet alignment
    W32(info + 4, v.tag == 0x69 ? 36u * v.channels : v.align);
    W32(info + 8, 0);
    W32(info + 12, 0);
    return 0;
}
STREAM(GetStatus, 2) {
    W32(a[1], v.packets.size() < v.max_packets ? 1u : 0u);  // XMEDIASTATUS_ACCEPT_INPUT
    return 0;
}
STREAM(Process, 3) {
    uint32_t in = a[1];
    if (!in) return 0;
    Packet pk{R32(in), R32(in + 4), R32(in + 8), R32(in + 12), R32(in + 16)};
    if (pk.completed_ptr) W32(pk.completed_ptr, 0);
    if (pk.status_ptr) W32(pk.status_ptr, kPacketPending);
    v.packets.push_back(pk);
    if (!v.active) v.has_gain = false;  // after going quiet a stream starts its levels fresh
    v.active = true;
    v.ending = false;
    return 0;
}
STREAM(Discontinuity, 1) {
    if (!v.packets.empty()) v.ending = true;
    else if (v.active) FlushStream(v);
    return 0;
}
STREAM(Flush, 1) {
    FlushStream(v);
    return 0;
}
STREAM(FlushEx, 4) {
    if (a[3] && !v.active) return 0;
    FlushStream(v);
    return 0;
}
STREAM(Pause, 2) {
    v.paused = a[1] == 1;
    v.sync = a[1] == 2;
    return 0;
}
STREAM(SetVolume, 2) {
    v.volume = Db(int32_t(a[1]));
    return 0;
}
STREAM(SetFrequency, 2) {
    v.freq = a[1];
    return 0;
}
STREAM(SetFilter, 2) { return 0; }
// the library's own writes voice registers a host stream does not have; envelopes are not played
HLE_STDCALL(IDirectSoundStream_SetEG, 2) { return 0; }
STREAM(SetHeadroom, 2) {
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
STREAM(SetMixBinVolumes, 2) {
    SetBinVolumes(v, a[1]);
    return 0;
}
STREAM(SetMixBins, 2) {
    SetBins(v, a[1]);
    return 0;
}
STREAM(SetMaxDistance, 3) {
    v.max_dist = F32(a[1]);
    return 0;
}
STREAM(SetMinDistance, 3) {
    v.min_dist = F32(a[1]);
    return 0;
}
STREAM(SetPosition, 5) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(a[1 + i]);
    return 0;
}
STREAM(SetVelocity, 5) { return 0; }
STREAM(SetI3DL2Source, 3) { return 0; }

// a voice's rolloff factor, for titles whose IDirectSoundBuffer_SetRolloffFactor and IDirectSoundStream_SetRolloffFactor
// exports the SDK finds no signature for (their hooks call this)
namespace xdk {
void SetVoiceRolloff(uint32_t obj, float factor) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (Voice* v = Find(obj)) {
        v->rolloff = factor;
    }
}
}  // namespace xdk

namespace xdk {
// (buffer, count, DSBPOSITIONNOTIFY: byte offset, event handle): for titles whose
// IDirectSoundBuffer_SetNotificationPositions export the SDK finds no signature for
void SetVoiceNotifications(uint32_t obj, uint32_t count, uint32_t positions) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Voice* v = Find(obj);
    if (!v) return;
    v->notify.clear();
    for (uint32_t i = 0; positions && i < std::min<uint32_t>(count, 64); i++)
        v->notify.emplace_back(R32(positions + i * 8), R32(positions + i * 8 + 4));
    LOG_FIRST(8, "dsound: voice %08X notifies %u positions, first at %08X", obj, count, count ? R32(positions) : 0);
}
}  // namespace xdk

namespace xdk {
// the stereo mix from the game's settings (src/common/game_info.cpp): amplitudes, 1 as the default mix has them
void SetEffects(bool gp) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_gp = gp;
    LOGI("dsound: effects image on %s", gp ? "the emulated GP" : "the hand-written stand-ins");
}
void SetMix(float dialogue, float front, float centre, float back, float sub, uint32_t sub_hz) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_mix = {dialogue, front, centre, back, sub, sub_hz};
    LOGI("dsound: mix dialogue %.2f front %.2f centre %.2f back %.2f sub %.2f low passed at %u Hz", dialogue, front,
         centre, back, sub, sub_hz);
}
}  // namespace xdk
