# Third-Party Notices & Attribution

ShadowLimitFix is a standalone SKSE plugin, but its development relied on
studying, cross-verifying, and re-implementing patterns from the following
projects. Per their licenses, the original names, authors, and licenses are
preserved here.

This project is licensed under **GPL-3.0-or-later** (see `LICENSE`). Several
dependencies below are statically linked and therefore make this plugin a
combined work under their terms.

## Community Shaders / Open Shaders

- **Project:** Community Shaders (upstream)
- **Repository:** https://github.com/community-shaders/skyrim-community-shaders
- **Nexus:** https://www.nexusmods.com/skyrimspecialedition/mods/86492
- **Fork used for reference:** Open Shaders — https://github.com/alandtse/open-shaders (Nexus 180419)
- **Maintainer / authors:** alandtse (Alan Tse) and the Community Shaders contributors
- **License:** GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception

**What was taken / referenced (all re-implemented, no code copied verbatim):**
- Engine hook target Address Library IDs and behavioral verification
  (`CalculateActiveShadowCasters`, `RenderShadowLights`, shadow engine hooks)
- The `detour_thunk` trampoline pattern (itself based on Microsoft Detours)
- Shadow-engine scheduling / classification concepts
  (`ShadowEngineHooks.cpp`, `ShadowCasterClassifier.cpp` references)
- Shader-compilation mechanism concept (ShaderCache)

## Light Limit Fix

- **Project:** Light Limit Fix - Community Shaders (Pre-1.4 Only)
- **Nexus:** https://www.nexusmods.com/skyrimspecialedition/mods/99548
- **Authors:** doodlez, alandtse, jonahex, ProfJack
- **License:** GPL-3.0-or-later (as part of Community Shaders)

**What was referenced:** shadow-map-slice shadow test math direction
(`lit = mapDepth > receiverDepth`) and the "shadow limit is not yet
unlocked" engine constraint used to validate this plugin's architecture.

## CommonLibSSE-NG

- **Project:** CommonLibSSE-NG
- **Repository:** https://github.com/CharmedBaryon/CommonLibSSE-NG
- **Original CommonLibSSE:** https://github.com/Ryan-rsm-McKenzie/CommonLibSSE (c) 2018 Ryan-rsm-McKenzie, MIT
- **Fork used for build:** the Community-Shaders fork bundled with Open Shaders
  (`alandtse/CommonLibSSE-NG`), which carries `SKSE/ContextHook.h`
- **Maintainers:** Charmed Baryon (NG), Ryan-rsm-McKenzie (original), powerof3, alandtse
- **License:** GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception
  (originally MIT; the fork used here is under the GPL terms — see its EXCEPTIONS.md)

**What was used:** the entire engine API binding layer (statically linked).

## Microsoft Detours

- **Project:** Detours
- **Repository:** https://github.com/microsoft/Detours
- **Author:** Microsoft
- **License:** MIT

**What was used:** the hook library (vcpkg `detours`), the base of the
`detour_thunk` pattern.

## Other build-time dependencies (via vcpkg)

| Library | Author | License |
|---|---|---|
| spdlog | Gabi Melman (gabime) | MIT |
| xbyak | Mitsuteru Nakao (herumi) | BSD-3-Clause |
| DirectXTK / DirectXMath | Microsoft | MIT |

## Related own projects

Mechanisms ported within this author's own GPL-3.0 project family:
- DynamicWetness (cbuffer/data-channel patterns cross-referenced during
  development of this plugin's resource set)

---

If you believe any attribution is missing or incorrect, please open an issue.
