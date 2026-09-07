# SLF 127-Light Shadow Architecture — CS Mechanism Study & Staged Plan

Date: 2026-09-03 | Status: PLANNING (user decision: all lights cast real shadows, target 127 slices)
Baseline: v9-stable = v7-vanilla equivalent (ENABLE_P1B=0, zero hooks, user-confirmed stable "有了")

## 1. Why the 5-hook expansion crashed (v8-exp2 verdict)

count-patch 8->127 + DSV redirect + SelectDSB alone, WITH the engine dispatch
left running, crashed twice at the SAME byte (null vtable call [r8+0x50] at
SkyrimSE.exe+14CC19E = uid107133+0x1AE inside dispatch fn 0x14CBFF0;
crash-log frame[0] return addr = 0x14CC1A2/+0x1B2, the instr after the call,
BSShadowLight::Render slot 0x0A -> RIP=0). Root cause: a slice expansion
without the coordinated scheduler is a structurally HALF patch. The engine's
fixed 8-slot state machine cannot address the extra slices -> OOB/garbage
state -> garbage light object in the dispatch array -> empty vtable slot.

## 2. What Community Shaders actually does (the full mechanism)

CS LightLimitFix / ShadowCasterManager (SCM) = 8840 lines across 16 files:

| Module | Lines | Role |
|---|---|---|
| ShadowScheduler.cpp | 3350 | ScheduleShadowCasters core: geometry hashing, light transitions, per-light redraw budget, dispatch |
| ShadowEngineHooks.cpp | 1554 | Every engine touchpoint (17 hooks) |
| ShadowAtlas.cpp | 1116 | Variable-tile atlas for point/spot shadows |
| ShadowCasterManager.cpp | 650 | Feature lifecycle / settings |
| ShadowCasterInternal.h | 628 | Shared state (s_lights pool etc.) |
| ShadowCasterClassifier.cpp | 478 | Light classification (static/dynamic etc.) |
| ShadowRenderer.cpp | 380 | Per-light render setup |
| ShadowSlotAllocator.cpp | 358 | Slot allocation |
| ShadowBudget.cpp | 326 | Redraw budget formulas |

CS defaults: ShadowLightCount=16, ShadowAtlas=true, AtlasResolution=8192.
Note: CS does NOT directly expand kSHADOWMAPS to N slices for large N -- it keeps
the engine array small and packs many lights into ONE 8192 atlas texture. Direct
slice expansion to 127 would cost ~127 full shadow-map slices of VRAM.

### 2.1 The 17 engine hooks (ShadowEngineHooks.cpp) -- full list

PHYSICAL RESOURCES (only needed when ShadowLightCount > 8, "needExtraBuffers"):
1.  Creation-loop count patch 8->N   uid(100458,107175) = BSShaderRenderTargets::Create (CS Hooks.cpp:1137; AE fn 0x14CF210, pdata span 0x125B); pattern "C7 44 24 68 08 00 00 00" hits ONLY once in-fn at AE +0x9E6 = 0x14CFBF6 (SE-era fn+0x3E6, VR +0xC91); whole-.text scan: 2 hits total, the other (0xD42660) has no Address-Library uid
2.  Hook_CreateNormalDepthBuffer     uid(75469,77255)   @ +0x172 rel -- redirect R9 (DSV store ptr) to our array for views >= 8
3.  Hook_CreateReadOnlyDepthBuffer   uid(75469,77255)   @ +0x191 rel -- same for readOnly DSVs
4.  Hook_SetupGameArray              uid(75469,77255)   @ +0x220 rel -- after loop, sync first 8 into engine's DepthStencilData[4]
5.  Hook_DeleteDepthBuffers          uid(75628,0)/AE 3 paths -- release extended buffers at shutdown
6.  Hook_SelectDepthBuffer1          uid(75580,77386)   @ +0x154 rel -- draw-time type-4 redirect (renderer R8 -> RBX)
7.  Hook_SelectDepthBuffer2          uid(75462,77247)   @ +0x135 rel -- second select site (RBP -> R14)
8.  Hook_AccumulatedLightsArray      uid(99686,106320)  @ +0x354 SE (AE 0x381/0x3A8) -- extend accum array (8+1 sentinel) -> (N+1)*2; bump RDI/RDX (SE) or RDI only (AE)
9.  Hook_OverwriteShadowMapIndex     uid(100820,107604) @ +0xBE (RenderCascade) -- force per-light assigned slot (engine recomputes from global counter -> slot corruption)
10. Hook_RenderShadowLightsWithUtilityShader uid(100423,107141) -- MUST NO-OP: indexes 4-entry blend table by maskIndex; extended slots (>=4) + 0xFF teardown sentinel -> OOB
11. Hook_StartGroupingAlphas         uid(100874,107670) -- alpha GeometryGroup ceiling guard (extended pool can exceed engine bump-alloc)

SCHEDULER REPLACEMENT (the heart):
12. Hook_CalculateActiveShadowCasters uid(100419,107137) -- FULL detour thunk -> ScheduleShadowCasters() (engine vanilla scheduler NEVER runs)
13. Hook_RenderShadowLights          uid(100415,107133) @ +0x146 SE (+0x18D AE) -- call-site inside render loop: ctx.Rax=0 (skip vanilla dispatch) + RenderScheduledShadowLights()
14. Hook_CalculateActiveLightsForSurface uid(100997,107784) -- FULL replace + RET at +5 (10/11 args, stack layout) -- surface light list injection

FIXUPS (correctness / crash guards):
15. vtable slot3 IsShadowLight -> custom (all 4 shadow light types) -- makes converted lights non-shadow for engine
16. Hook_ParabolicRender vtable 0x0A -- copy cascade0 shadowmapIndex -> cascade1 (teardown frees right slot)
17. Stealth detection: Hook_UpdateLightLevelPlayer / Hook_CheckLightLevelPlayer (GetLightLevel 38900/39946 + GetLuminanceAtPoint 99725/106362)
18. Convert light lifecycle: Hook_ConvertLights_Remove(99697)/ClearLightArrays(99704)/ResetScene(99741)/AccumulateLight(99753)/ShadowLightCtor(100810)/AddLight(99692)/SetLight(101302)
19. Focus-shadow suppression: byte patches (xor eax) @ 0x14ea854 (AE>=1.7.99) / uid 10209+10207 + global gate uid3(513201,390932)=0
20. Atlas: Hook_UpdateViewPort (75455/77240) -- map engine rect into atlas tile

### 2.2 The critical insight for SLF

SLF's "engine-native chain + passive arrays" direction is DEAD (v8-exp2 proved it).
CS works because it FULLY OWNS scheduling:
- CalculateActiveShadowCasters -> ScheduleShadowCasters (engine never schedules)
- render-loop call site -> rax=0 + RenderScheduledShadowLights (engine never dispatches)
- surface lights -> fully replaced hook
- ALL engine 8/4 hard limits coordinated (accum array, blend table, alpha pool, focus shadows)

SLF independence constraint: SLF cannot link CS runtime. Any port must be a
self-contained reimplementation using the same REL IDs / mechanisms.

## 3. What "127 lights" really costs

- CS default is 16 with an 8192 atlas. Direct 127-slice array expansion is
  VRAM-prohibitive unless slices are small (e.g. 512x512 tiles in an atlas).
- User requirement "ALL lights cast shadows" realistically means: every light in
  the cell (inn ~22-30) gets a real shadow map, not literally 127 simultaneous.
- 127 = the slice/atlas capacity ceiling we should BUILD for (CS-style atlas or
  array with small slices), not necessarily per-frame simultaneous count.

## 4. Staged implementation plan (each stage = build + in-game verify)

Stage A (v10): Reimplement CS scheduler-ownership core in SLF:
  - ScheduleShadowCasters (simplified: all active lights get slots, time-coherent
    persistence, no atlas yet -- direct slices with count capped to safe VRAM)
  - Hook_CalculateActiveShadowCasters full detour
  - Hook_RenderShadowLights rax=0 + own per-light Render dispatch
  - OverwriteShadowMapIndex + AccumulatedLightsArray + utility-shader no-op
  - Keep count modest (16-32) first; verify sun + 8+ indoor lights stable
Stage B (v11): Surface-light replacement hook (per-surface >4 contribution)
Stage C (v12): VRAM strategy -- either variable-tile atlas (CS ShadowAtlas port)
  or small-slice array to reach 127 capacity
Stage D (v13): Focus-shadow suppression + stealth fix + lifecycle crash guards
  (ConvertLights/ClearLightArrays/AccumulateLight TOCTOU etc.) -- port from CS

## 5. Open questions for the user before Stage A

1. Target: 127 = hard ceiling to build, or per-frame simultaneous count?
2. Direct array slices (simple, VRAM heavy) vs CS-style 8192 atlas (complex, VRAM
   efficient)? SLF has no CS runtime so atlas must be reimplemented (~1500 lines).
3. Accept per-frame redraw budget (CS model: not every light re-renders every
   frame; time-coherent) or demand every light re-renders when it moves?
