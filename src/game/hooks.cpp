// Hooks for Colin McRae Rally 2005 (USA) default.xbe, XDK 5849.
//
// A strong definition replaces the recompiled function at that address. Each one below is a DirectSound export the
// SDK's host has no version of; left as recompiled code they reach for the console's audio hardware. The names come
// from the export ids the image's .XTLID lists them under (the runtime logs them at boot as "dsound: exports running
// their original code") and from their code.

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "xdk/hle.h"

namespace xdk {
void SetVoiceRolloff(uint32_t obj, float factor);                             // xdk/dsound.cpp
void SetVoiceNotifications(uint32_t obj, uint32_t count, uint32_t positions);  // xdk/dsound.cpp
}

namespace {
uint32_t Arg(X86Context& ctx, int i) { return ::kernel::R32(ctx.esp + 4 + 4 * uint32_t(i)); }
float ArgF(X86Context& ctx, int i) {
    uint32_t b = Arg(ctx, i);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
}  // namespace

// B0074 IDirectSoundBuffer_SetNotificationPositions(buffer, count, positions): events the buffer sets as play passes
// each position. The co-driver's calls stream through buffers that wait for them; ignored, he never spoke.
X86_FUNC(sub_0021EB15) {
    (void)base;
    xdk::SetVoiceNotifications(Arg(ctx, 0), Arg(ctx, 1), Arg(ctx, 2));
    ::kernel::Return(ctx, 0, 3);
}

// B007A IDirectSoundBuffer_SetRolloffFactor(buffer, float factor, apply): how fast the voice fades with distance. Left
// at the default factor, the game's 3D sounds (the engine among them) faded to near silence.
X86_FUNC(sub_0021F889) {
    (void)base;
    xdk::SetVoiceRolloff(Arg(ctx, 0), ArgF(ctx, 1));
    ::kernel::Return(ctx, 0, 3);
}

// B009C IDirectSoundStream_SetRolloffFactor(stream, float factor, apply)
X86_FUNC(sub_0021F934) {
    (void)base;
    xdk::SetVoiceRolloff(Arg(ctx, 0), ArgF(ctx, 1));
    ::kernel::Return(ctx, 0, 3);
}

// .XTLID 0x30005 XOnlineGetUsers(users, out count): the Xbox Live accounts saved on the console. The SDK leaves Xbox
// Live off, so the library's object is null and this fails with XONLINE_E_NOT_INITIALIZED; the UIX library (the Live
// friends and notification UI the frontend creates) then fails to start and the main menu used its null object.
// On an Xbox with no Live account this succeeds with none: clear the 0x700 byte list and return zero users.
X86_FUNC(sub_0027E1B6) {
    (void)base;
    uint32_t users = Arg(ctx, 0), count = Arg(ctx, 1);
    if (users) std::memset(::kernel::HostPtr(users), 0, 0x700);
    if (count) ::kernel::W32(count, 0);
    LOG_FIRST(2, "cmr: XOnlineGetUsers, no Xbox Live accounts");
    ::kernel::Return(ctx, 0, 2);
}

// UIX notifier setup (GAME\Source\UIXNotifier.cpp): starts the Xbox Live UI library with d:\Data\UIX\cmr5skin.uix,
// then loads the friend request and game invite icons. The frontend calls it every frame until the UIX start succeeds
// (the flag at 0x5F8A44), but with no Xbox Live the start always fails with XONLINE_E_NOT_INITIALIZED, so every call
// leaked two icons: about 1 KB of the game's heap each time, out of memory after roughly 30 minutes at 60 fps.
// Try it once, then leave the notifier off.
X86_EXTERN_FUNC(__imp__sub_0016EA10);
X86_FUNC(sub_0016EA10) {
    static bool tried = false;
    if (!tried) {
        tried = true;
        __imp__sub_0016EA10(ctx, base);
        LOGI("cmr: Xbox Live notifier start tried once, %s", ::kernel::R32(0x5F8A44) ? "running" : "off");
        return;
    }
    ::kernel::Return(ctx, 0, 0);
}

// The engine's cull mode setter (cached at 0x31C8B8, applied as D3DRS_CULLMODE): materials with bit 31 of their flags
// ask for D3DCULL_CW, which is how the track and terrain surfaces draw. The host culls those triangles as clockwise
// and the ground vanished, while the console shows it. Nothing else in the game culls, and culling only ever removes
// hidden faces, so the setter always turns it off.
X86_EXTERN_FUNC(__imp__sub_001838D0);
X86_FUNC(sub_001838D0) {
    if (Arg(ctx, 0)) LOG_FIRST(1, "cmr: culling (D3DRS_CULLMODE %X) kept off", Arg(ctx, 0));
    ::kernel::W32(ctx.esp + 4, 0);
    __imp__sub_001838D0(ctx, base);
}

// B003B IDirectSound_CreateSoundStream(dsound, desc, out stream, unknown): the host's DirectSoundCreateStream(desc, out)
X86_FUNC(sub_00220824) {
    (void)base;
    uint32_t r = ::kernel::CallGuest(ctx, SYM(DirectSoundCreateStream), {Arg(ctx, 1), Arg(ctx, 2)});
    ::kernel::Return(ctx, r, 4);
}

// The rally's display ramp (GAME: the sun glare): each entry is glare + i * brightness / 128, where the brightness at
// 0x4E7AC0 comes from the stage's lighting file (187 in Spain, a 1.46 boost). With it the PC picture is far brighter than
// the console's and clips to white; the ramp is built with 128 here, which keeps the glare and drops the boost.
X86_EXTERN_FUNC(__imp__sub_0006C040);
X86_FUNC(sub_0006C040) {
    constexpr uint32_t kBrightness = 0x4E7AC0;
    uint32_t saved = ::kernel::R32(kBrightness);
    float neutral = 128.0f;
    uint32_t bits;
    std::memcpy(&bits, &neutral, 4);
    ::kernel::W32(kBrightness, bits);
    __imp__sub_0006C040(ctx, base);
    ::kernel::W32(kBrightness, saved);
    LOG_FIRST(1, "cmr: rally display ramp built without the stage's brightness boost");
}

// XBE_GRASS_PROBE: the grass's view volume. The grass pieces (sub_0005C8D0 then sub_0005CFC0, quads through EndPush)
// are each tested against the object at [viewport + 0x7A20] (sub_000D77C0), the viewports 0x8B70 bytes apart from
// [0x5F8330]. Logs that object's first 0x100 bytes as floats and the positions at +0x6B50 and +0x6D50 now and then.
X86_EXTERN_FUNC(__imp__sub_0005C8D0);
X86_FUNC(sub_0005C8D0) {
    static const bool probe = std::getenv("XBE_GRASS_PROBE") != nullptr;
    static uint32_t calls = 0, logged = 0;
    uint32_t vp = Arg(ctx, 0);
    if (probe && vp == 0 && ++calls % 300 == 0 && logged < 4) {
        logged++;
        uint32_t rec = ::kernel::R32(0x5F8330) + vp * 0x8B70, fr = ::kernel::R32(rec + 0x7A20);
        auto f = [](uint32_t a) {
            uint32_t b = ::kernel::R32(a);
            float v;
            std::memcpy(&v, &b, 4);
            return v;
        };
        LOGI("cmr: grass probe %u: +6B50 %.2f %.2f %.2f, +6D50 %.2f %.2f %.2f, volume %08X", logged, f(rec + 0x6B50),
             f(rec + 0x6B54), f(rec + 0x6B58), f(rec + 0x6D50), f(rec + 0x6D54), f(rec + 0x6D58), fr);
        for (uint32_t o = 0; fr && o < 0x100; o += 0x20)
            LOGI("cmr:   +%02X %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f", o, f(fr + o), f(fr + o + 4),
                 f(fr + o + 8), f(fr + o + 12), f(fr + o + 16), f(fr + o + 20), f(fr + o + 24), f(fr + o + 28));
    }
    __imp__sub_0005C8D0(ctx, base);
}

// The speedometer's lamp labels (Turbo, Water, Oil) are sprites of the HUD atlas from texel 39.5 to 83.5, 44 pixels
// wide; their dark dot starts at texel 39, so the game cuts half its first column, which the console's 640x480 hides
// and render scale 3 shows as a flat edge. Each label starts a pixel and a texel further left: the whole dot, the text
// where it was. sub_0017FC90(sprites, count) draws 0x28 byte records: colour, u0 u1 v0 v1, x0 x1 y0 y1, z.
X86_EXTERN_FUNC(__imp__sub_0017FC90);
X86_FUNC(sub_0017FC90) {
    uint32_t sprites = Arg(ctx, 0), count = Arg(ctx, 1);
    auto f = [](uint32_t a) {
        uint32_t b = ::kernel::R32(a);
        float v;
        std::memcpy(&v, &b, 4);
        return v;
    };
    auto w = [](uint32_t a, float v) {
        uint32_t b;
        std::memcpy(&b, &v, 4);
        ::kernel::W32(a, b);
    };
    for (uint32_t i = 0; i < count && i < 64; i++) {
        uint32_t s = sprites + i * 0x28;
        float u0 = f(s + 4), u1 = f(s + 8), x0 = f(s + 0x14), x1 = f(s + 0x18);
        if (u0 <= 0 || std::fabs(u1 / u0 - 83.5f / 39.5f) > 1e-4f || std::fabs(x1 - x0 - 44) > 1e-3f) continue;
        w(s + 4, u0 * 38.5f / 39.5f);
        w(s + 0x14, x0 - 1);
        LOG_FIRST(1, "cmr: speedometer lamp labels start a texel earlier, their dots whole");
    }
    __imp__sub_0017FC90(ctx, base);
}
