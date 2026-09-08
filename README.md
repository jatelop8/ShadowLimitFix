# ShadowLimitFix

A standalone SKSE plugin that stabilizes Skyrim SE/AE/VR shadow-lighting
behavior: it keeps engine point-light shadows from flickering while the
player moves, and fixes the engine's distance-based lamp fade (the
"walk up and the lamp pops on/off" behavior). No Community Shaders runtime,
ENB-coexistent.

> **Status note:** this plugin was developed through a long data-driven
> investigation of Skyrim's shadow-light scheduling. It complements
> [Native Mesh Light Flicker Fix](https://www.nexusmods.com/skyrimspecialedition/mods/186432)
> (by nicola89b): NMLFF expands the engine's per-geometry render-pass light
> storage and keeps the light-selection list coherent across frames, while
> ShadowLimitFix pins lamp `lodDimmer` to 1.0 so the engine never
> distance-fades lamps. They fix different mechanisms and work well together.

## What it does

| Mechanism | Engine behavior fixed |
|---|---|
| **Lamp dimmer pin** | The engine zeroes each lamp's `lodDimmer` when the player exceeds a ~330–455-unit LOD distance, then snaps it back to 1 on approach — the "lamp suddenly turns on/off" pop. ShadowLimitFix restores `lodDimmer = 1.0` for every active lamp at the last moment before the engine reads lamp data (Lighting `BeginTechnique`), every frame. |
| **Shadow-engine observation** | Hooks `CalculateActiveShadowCasters` / shadow render paths for diagnostics and stability work; publishes the N-light shadow data channel infrastructure. |

## Architecture (what is inside)

- **`src/Scheduler.cpp`** — shadow-caster scheduler thunk: engine accumulator
  registration, extended-light scheduling, per-frame lamp-dimmer restore.
- **`src/ShaderReplace.cpp`** — engine render-path observation
  (`BSShader::LoadShaders`, `BeginTechnique`), shadow-pass detection,
  `CreatePixelShader` bytecode capture, per-material diagnostics.
- **`src/P1_hooks.cpp`** — P1 hook installation framework (render-loop,
  QA probes, extended shadow buffers).
- **`docs/SLF_127_Architecture.md`** — design notes on the extended
  shadow-map architecture explored during development.

This plugin is standalone: engine-behavior references in comments are
Address-Library REL-ID facts used for verification only, with **no runtime
dependency** on Community Shaders or any other mod.

## Requirements

- Skyrim SE 1.5.97 / AE 1.6.1170 / VR
- Address Library for SKSE Plugins
- (optional, recommended) Native Mesh Light Flicker Fix for per-geometry
  multi-light stability

## Building

```
cmake --preset NINJA \
  -DCOMMONLIB_DIR=<path to CommonLibSSE-NG sources> \
  -DVCPKG_PREFIX=<path to vcpkg installed/x64-windows-static-md> \
  -DPLUGIN_DEPLOY_DIR=<your mod folder>   # optional auto-deploy
cmake --build --preset NINJA
```

Requires: Visual Studio Build Tools, CMake 4.2+, Ninja, vcpkg (spdlog,
directxtk, detours, xbyak), and a CommonLibSSE-NG source tree that carries
`SKSE/ContextHook.h` (e.g. the Community Shaders fork).

## Credits / Third-party notices

See [THIRD_PARTY.md](THIRD_PARTY.md) for complete attribution of every
project whose patterns or bindings this plugin relies on — Community Shaders
/ Open Shaders (alandtse and contributors), Light Limit Fix (doodlez,
alandtse, jonahex, ProfJack), CommonLibSSE-NG, Microsoft Detours, and others.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
