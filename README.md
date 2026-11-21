# rsdk-air

One native app (C++20, SDL2) that bundles the RSDKv3, RSDKv4 and RSDKv5U engines and
Sonic 3 A.I.R., and asks at startup which game to play. Targets Windows, Linux, macOS
and Android. No game data is included: you provide your own files.

Our own code is under the [MIT License](LICENSE). The bundled engines keep their own
licenses (non-commercial RSDK decompilation licenses, GPL-3.0 for Sonic 3 A.I.R.); see
[`ROADMAP/2026-11-20/upstreams-and-licensing.md`](ROADMAP/2026-11-20/upstreams-and-licensing.md).

Current plan: [`ROADMAP/2026-11-20/`](ROADMAP/2026-11-20/overview.md).

## Build

Needs CMake 3.22+, a C++20 compiler and, on Linux, the SDL2 build dependencies
(X11/Wayland, ALSA/PulseAudio dev packages; see `.github/workflows/build.yml`).

```bash
git clone --recursive <this repository>
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
./build/bin/rsdk-air --list
./build/bin/rsdk-air --game sonic2                         # the game's own menus
./build/bin/rsdk-air --game sonic2 -- --slot 0 --player 1  # straight into save slot 0 as Tails
```

Android (JDK 17, Android SDK with NDK 27.2.12479018 and CMake 3.22.1):

```bash
cd android && ./gradlew assembleRelease
```

`-DRSDKAIR_ENABLE_PLUS=ON` (CMake) / `-PrsdkairEnablePlus=true` (Gradle) turns on Plus DLC
content. Only for builds you keep for yourself: never distribute them.

## Layout

| Path | What |
| --- | --- |
| `include/rsdkair/engine.h` | The C ABI every engine module exports |
| `src/core/` | Launcher logic without SDL: game catalog, data detection, ROM checks, module loading, child processes |
| `src/launcher/` | The `rsdk-air` executable (`libmain.so` on Android), with the cover-mosaic picker (Dear ImGui) |
| `art/` | Picker covers, with their provenance in `art/README.md`; removable, the picker falls back to neutral art |
| `assets/fonts/` | Inter (SIL OFL 1.1), the picker's font |
| `engines/` | Engine modules: `rsdkv3/` (Sonic CD), `rsdkv4/` (Sonic 1, Sonic 2); `deps/` (ogg, vorbis, theora, GLEW); `dummy/` is test-only |
| `android/` | Gradle project: `LauncherActivity` + `GameActivity` in a `:game` process |
| `third_party/` | SDL2, Dear ImGui, stb and the engines, as pinned git submodules |
| `site/` | Landing page, deployed by `release.yml` |
| `.github/workflows/` | `build.yml` (push to main, or manual), `android.yml` (called by `build.yml`, or a push with `[android]` in the commit message) and `release.yml` (manual) |

## Project conventions

- [`AGENTS.md`](AGENTS.md) — rules for agents (`CLAUDE.md` symlinks to it).
- [`ROADMAP/`](ROADMAP/) — project plan by date, with Mermaid Gantt diagrams.
- [`RECORD/`](RECORD/) — project memory (`.WIP` append-only, `.completed` read-only).

All code and documentation in this repository are written in English.
