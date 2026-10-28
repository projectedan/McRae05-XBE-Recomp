# CMR2005 PC port: project overview

Colin McRae Rally 2005 (Xbox) statically recompiled for Windows with
[XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp). The recompiler and runtime are vendored in `sdk/` and
changed here as the port needs.

## Ground rules

- No game content goes in the repository: `assets/` (the user's game files), `generated/` (code recompiled from the
  game) and `out/` are ignored and never committed. The release ships tools only; each player's PC compiles the game
  from their own copy on first start.
- The project is GPLv3 (it includes xemu's DSP code). Third-party code and fonts keep their own licences, listed in
  `sdk/THIRD_PARTY.txt`.

## Layout

- `src/game/hooks.cpp`: hooks that replace or wrap recompiled game functions, each with a comment saying why.
- `src/common/`: the title's settings, build ID and the settings overlay (`menu.cpp`, ImGui).
- `sdk/runtime/`: the runtime the recompiled code runs on: Xbox kernel, D3D8 on Direct3D 12 (`xdk/d3d8.cpp`,
  `gpu/`), DirectSound and the emulated audio DSP (`xdk/dsound.cpp`, `xdk/dsp_image.cpp`), input and settings.
- `sdk/tools/recomp/`: the recompiler; `sdk/harness/`: a headless Linux build for automated testing.
- `sdk/third_party/`: SMAA, xemu's DSP interpreter, the overlay's fallback fonts.
- `config/game.toml`: the recompiler's settings for this game.
- `tools/`: analysis scripts (audio recordings, DSP effects, UI texture references).

## Build and test

- `build.bat`: configures and builds `out/build/release/CMR2005.exe` and the recompiled `CMR2005.dll`. The first build
  takes a long time; later runtime changes take minutes.
- `release.bat`: packs the release zip (exe, compiler, licences; no game code).
- `capture.bat` (F3 saves a frame's draws and targets), `record.bat` (a multichannel audio recording) and `perf.bat`
  (frame timing) are the debugging tools.
- Logs go to `out/build/release/logs/`, and each starts with the build's commit; settings are in
  `out/build/release/settings.toml`.
