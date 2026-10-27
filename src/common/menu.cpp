// the port's settings menu, drawn over the game with ImGui: F1 or Back and Start together open and close it. The game
// gets no controller input while it is open, nor until every button is let go after it closes.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "common/cmr.h"
#include "core/log.h"

namespace cmr::menu {

namespace {

constexpr uint16_t kStart = 1 << 4, kBack = 1 << 5;

std::atomic<bool> g_open{false};
std::atomic<bool> g_swallow{false};  // closed: the game's input stays empty until every button is up

ImFont* g_body = nullptr;
bool g_focus_first = false;  // the next row takes the focus: the menu opened or the section changed
ImFont* g_title = nullptr;

void Toggle() {
    bool was = g_open.load();
    g_open = !was;
    if (was) g_swallow = true;
}

// CMR's menus: a dark brown band holding the white title, light grey below with dark brown text, brown for the
// current choice
const ImVec4 kInk{0.16f, 0.10f, 0.07f, 1.0f};
const ImVec4 kPaper{0.90f, 0.90f, 0.89f, 0.98f};
const ImVec4 kBand{0.10f, 0.06f, 0.04f, 1.0f};
const ImVec4 kAccent{0.42f, 0.31f, 0.25f, 1.0f};
const ImVec4 kSoft{0.42f, 0.31f, 0.25f, 0.25f};

void Style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.WindowBorderSize = 0;
    s.FrameRounding = 0;
    s.ItemSpacing = ImVec2(0, 0);
    s.WindowPadding = ImVec2(0, 0);
    s.SelectableTextAlign = ImVec2(0, 0.5f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = kInk;
    c[ImGuiCol_WindowBg] = kPaper;
    c[ImGuiCol_Header] = kAccent;
    c[ImGuiCol_HeaderHovered] = kSoft;
    c[ImGuiCol_HeaderActive] = kAccent;
    c[ImGuiCol_NavHighlight] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = kSoft;
}

// the display modes the game window's monitor lists, largest last, 640x480 and up
std::vector<std::pair<uint32_t, uint32_t>> Modes() {
    std::vector<std::pair<uint32_t, uint32_t>> modes;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(app::Window()), MONITOR_DEFAULTTOPRIMARY), &mi))
        return modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++)
        if (dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480) modes.emplace_back(dm.dmPelsWidth, dm.dmPelsHeight);
    std::sort(modes.begin(), modes.end());
    modes.erase(std::unique(modes.begin(), modes.end()), modes.end());
    return modes;
}

// what the menu holds between frames: the render thread's alone
struct State {
    bool was_open = false;
    bool changed = false;
    int tab = 0, shown_tab = -1;
    std::vector<std::pair<uint32_t, uint32_t>> modes;
};
State g_state;

// vsync and the audio mix stay in the settings file: the pacing wants vsync on, and the mix is fitted to the console's
const char* kTabs[] = {"DISPLAY", "GRAPHICS", "OVERLAY"};
constexpr int kTabCount = int(std::size(kTabs));

bool Pressed(ImGuiKey key) { return ImGui::IsKeyPressed(key, true); }

// one setting: its name on the left, its value on the right; -1 or +1 when stepped (left and right, A or a click
// steps on, a right click back), 0 otherwise
int Row(const char* label, const std::string& value, bool enabled = true) {
    ImGui::PushID(label);
    float h = ImGui::GetFontSize() * 1.9f;
    ImVec2 at = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    if (!enabled) ImGui::BeginDisabled();
    if (enabled && g_focus_first) {
        ImGui::SetKeyboardFocusHere();
        g_focus_first = false;
    }
    bool clicked = ImGui::Selectable("##row", false, ImGuiSelectableFlags_None, ImVec2(w, h));
    bool focused = enabled && ImGui::IsItemFocused();
    bool hovered = enabled && ImGui::IsItemHovered();
    if (!enabled) ImGui::EndDisabled();
    int step = 0;
    if (clicked) step = 1;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) step = -1;
    if (focused) {
        if (Pressed(ImGuiKey_LeftArrow) || Pressed(ImGuiKey_GamepadDpadLeft) || Pressed(ImGuiKey_GamepadLStickLeft))
            step = -1;
        if (Pressed(ImGuiKey_RightArrow) || Pressed(ImGuiKey_GamepadDpadRight) || Pressed(ImGuiKey_GamepadLStickRight))
            step = 1;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float pad = ImGui::GetFontSize() * 1.2f;
    ImU32 text = ImGui::GetColorU32(enabled ? kInk : ImVec4(kInk.x, kInk.y, kInk.z, 0.4f));
    if (focused) {
        dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), ImGui::GetColorU32(kAccent));
        text = IM_COL32(240, 238, 235, 255);
    }
    float ty = at.y + (h - ImGui::GetFontSize()) / 2;
    dl->AddText(ImVec2(at.x + pad, ty), text, label);
    std::string shown = focused ? "<  " + value + "  >" : value;
    float vw = ImGui::CalcTextSize(shown.c_str()).x;
    dl->AddText(ImVec2(at.x + w - pad - vw, ty), text, shown.c_str());
    ImGui::PopID();
    return enabled ? step : 0;
}

void Heading(const char* text) {
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.6f));
    ImGui::SetCursorPosX(ImGui::GetFontSize() * 1.2f);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));
}

template <typename T>
int Index(const std::vector<T>& list, const T& v) {
    auto it = std::find(list.begin(), list.end(), v);
    return it == list.end() ? 0 : int(it - list.begin());
}

template <typename T>
T Step(const std::vector<T>& list, const T& now, int step) {
    int n = int(list.size());
    return list[size_t(((Index(list, now) + step) % n + n) % n)];
}

std::string OnOff(bool v) { return v ? "On" : "Off"; }

void Display(State& st) {
    app::Settings& s = app::Get();
    Heading("Screen");
    static const char* kModeNames[] = {"Fullscreen", "Borderless", "Windowed"};
    if (int step = Row("Display mode", kModeNames[uint32_t(s.display_mode)])) {
        s.display_mode = app::DisplayMode((uint32_t(s.display_mode) + 3 + step) % 3);
        // borderless covers the monitor at the desktop's size
        if (s.display_mode == app::DisplayMode::Borderless) s.width = s.height = 0;
        app::ApplyDisplay();
        st.changed = true;
    }
    bool sized = s.display_mode != app::DisplayMode::Borderless;
    std::vector<std::pair<uint32_t, uint32_t>> res = st.modes;
    res.insert(res.begin(), {0u, 0u});
    std::pair<uint32_t, uint32_t> now{s.width, s.height};
    app::Mode desk = app::Desktop();
    char text[64];
    if (now.first) std::snprintf(text, sizeof text, "%u x %u", now.first, now.second);
    else std::snprintf(text, sizeof text, "Desktop (%u x %u)", desk.width, desk.height);
    if (int step = Row("Resolution", text, sized)) {
        auto next = Step(res, now, step);
        s.width = next.first;
        s.height = next.second;
        app::ApplyDisplay();
        st.changed = true;
    }
    Heading("Rendering");
    static const std::vector<uint32_t> kPercents = {50, 67, 75, 100, 125, 150, 200};
    uint32_t rh = app::Output().height * s.render_percent / 100;
    std::snprintf(text, sizeof text, "%u%%  (%up)", s.render_percent, rh);
    if (int step = Row("Render resolution", text)) {
        s.render_percent = Step(kPercents, s.render_percent, step);
        st.changed = true;
    }
}

void Graphics(State& st) {
    app::Settings& s = app::Get();
    Heading("Textures");
    static const std::vector<uint32_t> kAniso = {0, 2, 4, 8, 16};
    char text[32];
    if (s.aniso) std::snprintf(text, sizeof text, "%ux", s.aniso);
    else std::snprintf(text, sizeof text, "As the game asks");
    if (int step = Row("Anisotropic filtering", text)) {
        s.aniso = Step(kAniso, s.aniso, step);
        st.changed = true;
    }
    if (Row("Texture packs", OnOff(s.custom_textures))) {
        s.custom_textures = !s.custom_textures;
        st.changed = true;
    }
    Heading("Menus and HUD");
    if (Row("Sharper 2D", OnOff(s.sharp_2d))) {
        s.sharp_2d = !s.sharp_2d;
        st.changed = true;
    }
}

void Overlay(State& st) {
    Heading("Performance");
    static const char* kFps[] = {"Off", "Frames a second", "With frame time"};
    if (int step = Row("Frame counter", kFps[std::min(g_show_fps, 2u)])) {
        g_show_fps = (std::min(g_show_fps, 2u) + 3 + uint32_t(step)) % 3;
        st.changed = true;
    }
}

void FrameCounter(double fps) {
    if (!g_show_fps) return;
    ImGui::SetNextWindowPos(ImVec2(12, 12));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    if (g_show_fps >= 2 && fps > 0) ImGui::Text("%.0f fps  %.1f ms", fps, 1000.0 / fps);
    else ImGui::Text("%.0f fps", fps);
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

// every pad's buttons into ImGui's gamepad keys
void FeedGamepad() {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    uint16_t buttons = 0;
    uint8_t analog[8] = {};
    int lx = 0, ly = 0;
    for (int port = 0; port < 4; port++) {
        input::Pad p = input::Read(port);
        if (!p.connected) continue;
        buttons |= p.buttons;
        for (int i = 0; i < 8; i++) analog[i] = std::max(analog[i], p.analog[i]);
        if (std::abs(p.lx) > std::abs(lx)) lx = p.lx;
        if (std::abs(p.ly) > std::abs(ly)) ly = p.ly;
    }
    auto key = [&](ImGuiKey k, bool down) { io.AddKeyEvent(k, down); };
    key(ImGuiKey_GamepadDpadUp, buttons & 1);
    key(ImGuiKey_GamepadDpadDown, buttons & 2);
    key(ImGuiKey_GamepadDpadLeft, buttons & 4);
    key(ImGuiKey_GamepadDpadRight, buttons & 8);
    key(ImGuiKey_GamepadStart, buttons & kStart);
    key(ImGuiKey_GamepadBack, buttons & kBack);
    key(ImGuiKey_GamepadFaceDown, analog[0] > 0x80);
    key(ImGuiKey_GamepadFaceRight, analog[1] > 0x80);
    key(ImGuiKey_GamepadFaceLeft, analog[2] > 0x80);
    key(ImGuiKey_GamepadFaceUp, analog[3] > 0x80);
    key(ImGuiKey_GamepadR1, analog[4] > 0x80);  // black is on the right shoulder
    key(ImGuiKey_GamepadL1, analog[5] > 0x80);
    auto stick = [&](ImGuiKey k, float v) { io.AddKeyAnalogEvent(k, v > 0.5f, std::clamp(v, 0.0f, 1.0f)); };
    stick(ImGuiKey_GamepadLStickLeft, -lx / 32767.0f);
    stick(ImGuiKey_GamepadLStickRight, lx / 32767.0f);
    stick(ImGuiKey_GamepadLStickUp, ly / 32767.0f);
    stick(ImGuiKey_GamepadLStickDown, -ly / 32767.0f);
}

void Menu(State& st) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 screen = io.DisplaySize;
    // the screen dimmed behind it
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(0, 0, 0, 110));
    float fs = ImGui::GetFontSize();
    ImVec2 size(std::min(screen.x * 0.9f, fs * 30), std::min(screen.y * 0.9f, fs * 22));
    ImGui::SetNextWindowPos(ImVec2((screen.x - size.x) / 2, (screen.y - size.y) / 2));
    ImGui::SetNextWindowSize(size);
    if (!st.was_open) ImGui::SetNextWindowFocus();
    ImGui::Begin("##cmr_menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetWindowPos();
    float pad = fs * 1.2f;

    // the band: the title, then the sections, the current one underlined
    float band = (g_title ? g_title->FontSize : fs * 2) * 1.35f;
    dl->AddRectFilled(at, ImVec2(at.x + size.x, at.y + band), ImGui::GetColorU32(kBand));
    ImFont* title = g_title ? g_title : ImGui::GetFont();
    float tsize = g_title ? g_title->FontSize : fs * 2;
    dl->AddText(title, tsize, ImVec2(at.x + pad, at.y + (band - tsize) / 2), IM_COL32(232, 230, 226, 255), "SETTINGS.");

    if (Pressed(ImGuiKey_GamepadL1) || Pressed(ImGuiKey_PageUp)) st.tab = (st.tab + kTabCount - 1) % kTabCount;
    if (Pressed(ImGuiKey_GamepadR1) || Pressed(ImGuiKey_PageDown)) st.tab = (st.tab + 1) % kTabCount;
    float x = at.x + pad, y = at.y + band + fs * 0.5f;
    for (int i = 0; i < kTabCount; i++) {
        ImVec2 ts = ImGui::CalcTextSize(kTabs[i]);
        ImVec2 a(x - fs * 0.3f, y - fs * 0.2f), b(x + ts.x + fs * 0.3f, y + ts.y + fs * 0.3f);
        bool hover = ImGui::IsMouseHoveringRect(a, b);
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) st.tab = i;
        ImU32 col = ImGui::GetColorU32(i == st.tab ? kInk : ImVec4(kAccent.x, kAccent.y, kAccent.z, hover ? 1.0f : 0.7f));
        dl->AddText(ImVec2(x, y), col, kTabs[i]);
        if (i == st.tab) dl->AddRectFilled(ImVec2(x, y + ts.y + fs * 0.1f), ImVec2(x + ts.x, y + ts.y + fs * 0.22f), col);
        if (i < kTabCount - 1) dl->AddText(ImVec2(x + ts.x + fs * 0.6f, y), ImGui::GetColorU32(kSoft), "-");
        x += ts.x + fs * 1.8f;
    }

    // the section's settings
    int tab = st.tab;
    float top = band + fs * 2.2f, foot = fs * 2.0f;
    ImGui::SetCursorPos(ImVec2(0, top));
    if (!st.was_open || tab != st.shown_tab) g_focus_first = true;
    st.shown_tab = tab;
    ImGui::PushID(tab);
    switch (tab) {
        case 0: Display(st); break;
        case 1: Graphics(st); break;
        default: Overlay(st); break;
    }
    ImGui::PopID();

    // the keys, and the build
    float fy = at.y + size.y - foot;
    dl->AddLine(ImVec2(at.x + pad, fy), ImVec2(at.x + size.x - pad, fy), ImGui::GetColorU32(kSoft));
    ImU32 dim = ImGui::GetColorU32(ImVec4(kInk.x, kInk.y, kInk.z, 0.6f));
    const char* keys = "Left/Right change    LB/RB section    B or Esc close";
    dl->AddText(ImVec2(at.x + pad, fy + (foot - fs) / 2), dim, keys);
    char build[48];
    std::snprintf(build, sizeof build, "build %s", BuildId());
    float bw = ImGui::CalcTextSize(build).x;
    dl->AddText(ImVec2(at.x + size.x - pad - bw, fy + (foot - fs) / 2), dim, build);

    if (Pressed(ImGuiKey_GamepadFaceRight)) Toggle();
    ImGui::End();
}

}  // namespace

void Window(void*) {
    // Segoe UI for the settings, Arial Black for the title as CMR's heavy Helvetica, sized for the monitor
    ImGuiIO& io = ImGui::GetIO();
    float px = std::max(16.0f, float(app::Desktop().height) / 48.0f);
    std::filesystem::path fonts = std::filesystem::path(_wgetenv(L"WINDIR") ? _wgetenv(L"WINDIR") : L"C:\\Windows") / "Fonts";
    auto add = [&](const char* name, float size) -> ImFont* {
        std::filesystem::path p = fonts / name;
        std::error_code ec;
        return std::filesystem::is_regular_file(p, ec) ? io.Fonts->AddFontFromFileTTF(p.string().c_str(), size) : nullptr;
    };
    g_body = add("segoeui.ttf", px);
    if (!g_body) g_body = io.Fonts->AddFontDefault();
    g_title = add("ariblk.ttf", px * 2.0f);
    io.FontDefault = g_body;
    Style();
}

bool Message(void*, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    if (msg == WM_KEYDOWN && wp == VK_F1) {
        Toggle();
        *result = 0;
        return true;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE && g_open) {
        Toggle();
        *result = 0;
        return true;
    }
    // Alt+Enter: windowed and back
    if ((msg == WM_SYSKEYDOWN || msg == WM_SYSCHAR) && wp == (msg == WM_SYSCHAR ? '\r' : VK_RETURN) && (lp & (1 << 29))) {
        if (msg == WM_SYSKEYDOWN && !(lp & (1 << 30))) {
            app::Settings& s = app::Get();
            s.display_mode = s.display_mode == app::DisplayMode::Windowed ? app::DisplayMode::Borderless
                                                                          : app::DisplayMode::Windowed;
            if (s.display_mode == app::DisplayMode::Borderless) s.width = s.height = 0;
            app::ApplyDisplay();
            app::Save();
        }
        *result = 0;
        return true;
    }
    return false;
}

void Draw(double fps) {
    State& st = g_state;
    FeedGamepad();
    bool open = g_open;
    if (open && !st.was_open) {
        st.modes = Modes();
        st.changed = false;
    }
    if (!open && st.was_open && st.changed) {
        app::Save();
        LOGI("menu: settings saved");
    }
    // the game blurred behind it, eased in and out over about a sixth of a second
    float blur = app::g_backdrop_blur.load(), want = open ? 1.0f : 0.0f;
    blur += (want - blur) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
    app::g_backdrop_blur = std::fabs(want - blur) < 0.01f ? want : blur;
    if (open) Menu(st);
    st.was_open = open;
    FrameCounter(fps);
}

void Pad(int port, input::Pad& pad) {
    // Back and Start together, on any controller, open and close the menu
    static uint16_t last[4] = {};
    if (port >= 0 && port < 4) {
        uint16_t both = kStart | kBack;
        bool now = (pad.buttons & both) == both, before = (last[port] & both) == both;
        if (now && !before) Toggle();
        last[port] = pad.buttons;
    }
    if (g_open) {
        pad = input::Pad{.connected = pad.connected};
        return;
    }
    if (g_swallow) {
        bool any = pad.buttons || pad.lx > 8000 || pad.lx < -8000 || pad.ly > 8000 || pad.ly < -8000;
        for (uint8_t a : pad.analog) any |= a > 0x20;
        if (!any) g_swallow = false;
        else pad = input::Pad{.connected = pad.connected};
    }
}

}  // namespace cmr::menu
