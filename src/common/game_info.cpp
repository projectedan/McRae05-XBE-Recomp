#include "app/app.h"
#include "core/log.h"

#if __has_include("build_id.h")
#include "build_id.h"  // CMakeLists.txt: the commit this build came from
#else
#define CMR_BUILD_ID "unknown"
#endif

namespace {
void Init() { LOGI("cmr2005: build %s", CMR_BUILD_ID); }
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
    .options = {},                        // game settings kept in the settings file
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
