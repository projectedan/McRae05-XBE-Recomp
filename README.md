# Colin McRae Rally 2005 - XBE Recomp

An unofficial native PC port of the original Xbox version of Colin McRae Rally 2005, with extended graphics options and other enhancements.

This port was created using the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

## Installation

- Put your game files in the `assets` folder. This can be an ISO, or the extracted data files.
- Run `CMR2005.exe`. The first start compiles the game's code for your PC, this only happens once.
- Recomp settings can be adjusted in `settings.toml` beside the exe.

## Features

### Settings Overlay
Press **F1**, or **Back + Start** on a controller, to open the overlay menu over the game (the game blurs behind it).
Changes apply at once and are saved to `settings.toml`.

### Display
- **Display mode** - Fullscreen, borderless or windowed, at any resolution.
- **Frame pacing** - A steady 60 fps cap with even frame times (the game steps its physics once per frame, so it stays at 60).
- **Brightness and gamma** - Adjust the picture from 50% to 150%.
- **Frame counter** - Optional on-screen FPS.

### Graphics
- **Render resolution** - Renders well above the console's 640x480, scaled to your output resolution.
- **Anisotropic filtering** - Up to 16x.
- **Anti-aliasing** - FXAA or SMAA.
- **Shader cache** - Compiled shaders are kept in `shader_cache` beside the exe, so stutter from shader compiles only
  happens the first time each one is seen.
- **Fixes** - Corrected road and track rendering, brightness and gamma, depth of field, the loading screen logo and the
  speedometer's warning lamps.

### Audio
- The game's own audio effects (reverb, engine filtering) run as on the console, on an emulated Xbox audio DSP.
- Surround folded down to stereo; the mix levels can be adjusted in `settings.toml` (`mix_` keys).

### Controller
Supports Xbox, PlayStation and Switch controllers, with hot plugging and rumble.

### Mods
- **Texture packs** - PNG images in `textures\replacement` beside the exe replace the game's textures, at any
  resolution, and update while the game runs. Set `dump_textures = true` in `settings.toml` to save the game's own
  textures to `textures\dumps`, already named as their replacements must be.
- **File overrides** - Files in a `mods` folder beside the exe replace the game's files of the same path.

### Miscellaneous
- **Skipped logo** - The "Codemasters presents" screen at start is skipped (the Dolby and Codemasters videos still play).
- **Boot Splash** - Place a `splash.png` (or `.bmp`) beside the exe to display a splash screen at launch.

## System Requirements

### Minimum
- **OS**
  - Windows 10 64-bit
- **GPU** with support for Direct3D 12

## Building

### 1. Clone the Repository
Clone this repository using [Git](https://git-scm.com/).

### 2. Add the Required Game Files
Place your extracted game data files in the `assets` folder. `default.xbe` must be there, with the game's folders beside it.

### 3. Install Dependencies
Install Visual Studio 2022 with **Desktop development with C++**, **C++ Clang tools for Windows** and **C++ CMake tools for Windows** (`build.bat` offers to add the last two).

### 4. Build the Project
Run `build.bat`. The first build recompiles the game and takes a while. The exe is written to `out\build\release`.

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp), the recompiler and runtime this is built on.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the overridable function layout and instruction hooks.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
- [SMAA](https://github.com/iryoku/smaa) by Jorge Jimenez and others for the SMAA anti-aliasing option (`sdk/third_party/smaa`).
- [Lato](https://www.latofonts.com) and [Archivo Black](https://github.com/Omnibus-Type/ArchivoBlack) (SIL Open Font License) for the overlay menu on systems without Windows' own fonts, such as Proton (`sdk/third_party/fonts`).
- [xemu](https://github.com/xemu-project/xemu)'s DSP56300 interpreter and DSP DMA unit, which run the game's own audio effects (`sdk/third_party/xemu-dsp`).

## License

This port is free software under the [GNU General Public License, version 3](LICENSE). It includes xemu's DSP code
(GPL-2.0-or-later and LGPL-2.1-or-later, see `sdk/third_party/xemu-dsp`), so builds that contain it are distributed
under the GPL. The SDK in `sdk/` keeps its own MIT licence (`sdk/LICENSE`) for the parts that come from it. No game
code or data is included: you supply your own copy of the game.
