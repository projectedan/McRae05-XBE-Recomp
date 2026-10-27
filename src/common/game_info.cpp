#include "app/app.h"
#include "core/log.h"

#if __has_include("build_id.h")
#include "build_id.h"  // CMakeLists.txt: the commit this build came from
#else
#define CMR_BUILD_ID "unknown"
#endif

namespace xdk {
void SetMix(float dialogue, float front, float centre, float back, float sub, uint32_t sub_hz);  // xdk/dsound.cpp
}

namespace {
// the stereo mix, in the settings file: each a percent of the default level (100), mix_sub_hz where the low frequency
// channel's low pass starts (0 keeps its whole range)
uint32_t g_mix_dialogue = 100, g_mix_front = 100, g_mix_centre = 100, g_mix_back = 100, g_mix_sub = 100,
         g_mix_sub_hz = 180;

void Init() {
    LOGI("cmr2005: build %s", CMR_BUILD_ID);
    auto pc = [](uint32_t v, float base) { return base * float(v) / 100.0f; };
    xdk::SetMix(pc(g_mix_dialogue, 1.78f), pc(g_mix_front, 1.0f), pc(g_mix_centre, 0.707f), pc(g_mix_back, 0.707f),
                pc(g_mix_sub, 0.5f), g_mix_sub_hz);
}
}  // namespace

// Colin McRae Rally 2005 (USA, Xbox): what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Colin McRae Rally 2005",   // window and splash
    .short_title = L"CMR 2005",           // message boxes
    .name = L"CMR2005",                   // window classes, crash dumps, handover event
    .log = "cmr2005",                     // log file prefix
    .settings = "settings.toml",          // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put Colin McRae Rally 2005 in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "CMR2005"}},  // each recompiled xbe and its exe, for title launches
    .options = {{"mix_dialogue", nullptr, &g_mix_dialogue}, {"mix_front", nullptr, &g_mix_front},
                {"mix_centre", nullptr, &g_mix_centre},     {"mix_back", nullptr, &g_mix_back},
                {"mix_sub", nullptr, &g_mix_sub},           {"mix_sub_hz", nullptr, &g_mix_sub_hz}},
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = Init,                         // after the settings load, before the window and boot
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = nullptr,                    // the game window is up
    .message = nullptr,                   // window messages first; true takes one
    .ui = nullptr,                        // ImGui drawing each frame
    .pad = nullptr,                       // changes a polled pad
    .rumble = nullptr,                    // changes a rumble before it is set
};
