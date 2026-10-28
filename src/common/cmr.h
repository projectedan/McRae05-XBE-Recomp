#pragma once
#include <cstdint>

#include "core/input.h"

// what the port's own pieces share: the settings game_info.cpp keeps, the menu's hooks
namespace cmr {

// the stereo mix, each a percent of the default level (100), and where the low frequency channel's low pass starts
// (0 keeps its whole range)
extern uint32_t g_mix_dialogue, g_mix_front, g_mix_centre, g_mix_back, g_mix_sub, g_mix_sub_hz;
// 0 none, 1 frames a second, 2 with the frame time
extern uint32_t g_show_fps;
// 1: a few seconds after start, a reminder of how to open the menu
extern uint32_t g_menu_hint;
void ApplyMix();
const char* BuildId();

// menu.cpp: the port's settings menu over the game, F1 or Back and Start together
namespace menu {
void Window(void* hwnd);
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
void Draw(double fps);
void Pad(int port, input::Pad& pad);
}  // namespace menu

}  // namespace cmr
