// ShadowLimitFix.cpp - P0: engine hook target address verification
//
// ATTRIBUTION (original names preserved, see THIRD_PARTY.md): Address
// Library IDs cross-verified against Community Shaders / Open Shaders
// (alandtse/open-shaders) src/Features/LightLimitFix/ShadowEngineHooks.cpp
// (GPL-3.0 WITH Modding Exception). REL-ID facts; no runtime dependency.
//   CalculateActiveShadowCasters       (100419, 107137) - scene shadow light scheduler
//   CalculateActiveNonShadowCasterLights (100997, 107784) - per-surface light selection
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "HookUtil.h"  // defines ENABLE_P1B (must be included BEFORE the #if below)

namespace ShadowLimitFixNS
{
	// P1a: light-selection hooks (stub mode, no engine state change)
	namespace P1
	{
		void Install();
#if ENABLE_P1B
		void InstallExtendedBuffers(uint32_t a_shadowLightCount);
#endif
		void InstallScheduler();
		void InstallShaderHooks();
		void InstallSurfaceLightsHook();
		void InstallSceneLightsRewriteHook();
	}

	void Install()
	{
		SKSE::log::info("[SLF] P0: resolving engine hook targets...");

		bool allOk = true;

		// 1) Scene-level shadow caster scheduler (replace with our own later)
		try {
			REL::RelocationID sceneScheduler(100419, 107137);
			SKSE::log::info("[SLF]   CalculateActiveShadowCasters  @ {:016X}", sceneScheduler.address());
			if (!sceneScheduler.address()) {
				SKSE::log::error("[SLF]   FAILED: CalculateActiveShadowCasters address is null");
				allOk = false;
			}
		} catch (const std::exception& e) {
			SKSE::log::error("[SLF]   FAILED: CalculateActiveShadowCasters exception: {}", e.what());
			allOk = false;
		}

		// 2) Per-surface non-shadow light selection (extend 4 -> N lights)
		try {
			REL::RelocationID surfaceLights(100997, 107784);
			SKSE::log::info("[SLF]   CalculateActiveNonShadowCasterLights @ {:016X}", surfaceLights.address());
			if (!surfaceLights.address()) {
				SKSE::log::error("[SLF]   FAILED: CalculateActiveNonShadowCasterLights address is null");
				allOk = false;
			}
		} catch (const std::exception& e) {
			SKSE::log::error("[SLF]   FAILED: CalculateActiveNonShadowCasterLights exception: {}", e.what());
			allOk = false;
		}

		if (allOk) {
			SKSE::log::info("[SLF] P0 PASS - both hook targets resolved, ready for P1");
#if ENABLE_P1B
			// v8-exp2 (2026-09-03 11:45): crash isolation build.
			// crash-2026-09-03-11-33-06: RIP=0 null vtable call at
			// SkyrimSE.exe+14CC19E (call [r8+0x50] = BSShadowLight::Render,
			// vtable slot EMPTY) inside the engine's own shadow-light
			// dispatch loop (0x14CBFF0, arr = container+0x230, len +0x240).
			// v8-exp1 difference vs the STABLE v7-vanilla was NOT only the
			// P1b arrays: P1c-3 (LoadShaders/BeginTechnique/ResetState hooks
			// + bytecode-capture vtable[15] patch + BindCSConstantBuffers
			// overwriting the engine material PS b4/b12 every ResetState +
			// per-material cbuffer Map readback + bytecode dumps) was ALSO
			// live. With the engine dispatch finally running unmodified
			// (SLF_SKIP_VANILLA_DISPATCH=0), those D3D state interventions
			// corrupted the renderer -> garbage light object in the dispatch
			// array -> vtable[0x50]=0 -> null call.
			// v8-exp2 isolates the passive array component: P1b extended
			// buffers ONLY, scheduler detour AND P1c-3 shader hooks parked,
			// readback/manual-render off. Engine scheduling+dispatch+material
			// rendering 100% vanilla. If stable -> P1b arrays proven safe and
			// the killer was P1c-3's hot-path D3D writes.
			// fix64 step2 (2026-09-09): 127 -> 30. Hypothesis: the sun's
			// cascade render AV (WER: faulting RIP outside all modules =
			// call through a corrupted pointer) comes from the engine's
			// shadow state machine overflowing on SLF's 127-slice
			// expansion - CS ships ShadowLightCount=16 default and renders
			// the sun fine under its full engine-facing expansion. 30
			// slices still covers every scheduled light (21-24) while
			// staying inside the envelope the engine adapts to.
			//
			// fix73 (2026-09-09 23:4x): DISABLED - bisect verdict target.
			// crash-2026-09-09-23-29-05 (outdoors, Riverwood, fix72 md5
			// 696fe978, uptime 109s): AV executing HEAP (RIP=0x1287A87700,
			// 5.6MB past the R14 sign mesh) inside the engine lighting-pass
			// machinery (BSBatchRenderer_SetupAndDrawPass region uid
			// 107639-107644, EngineFixes hook on the stack) - the fix64
			// step2 "corrupted pointer" crash class REAPPEARING at count=30.
			// Same-run evidence: the ENGINE already renders the sun itself
			// every frame (64 shadow renders / 64 frames, engine slot
			// counter=2 = sun idx0/idx1) while SLF's own dispatch renders 0
			// of 1 scheduled lights - the 30-slice array + SelectDSB/DSV
			// redirect carries pure crash risk and ZERO current shadow
			// output. fix73 = engine-native kSHADOWMAPS (8 slices, no
			// SelectDSB redirect); scheduler/dimmer/indoor publish logic
			// unchanged. Bisect: outdoor crash gone => array/redirect was
			// the killer (extended lights then need the FULL CS-style
			// engine-state expansion, not just the texture); crash stays
			// => next isolate = InstallScheduler.
			// P1::InstallExtendedBuffers(30);
			// v10-phase1 (2026-09-03): scheduler restored - thunk runs the
			// ORIGINAL engine scheduler (func) then a register pass that
			// publishes the engine's shadowLightsAccum into our
			// g_scheduledShadowLights. The render-loop hook skips the
			// vanilla dispatch (SLF_SKIP_VANILLA_DISPATCH=1) and self-renders
			// that list (SLF_MANUAL_RENDER=1) - engine schedules, WE render,
			// so the engine's fixed 8-slot dispatch never touches the
			// 127-slice array (v8-exp2 crash site avoided).
			//
			// fix74 (2026-09-09 23:5x): DISABLED - bisect verdict step 2.
			// crash-2026-09-09-23-50-02 (indoor, Sleeping Giant Inn,
			// fix73 md5 0dfe1463, uptime 113s): AV executing HEAP
			// (RIP=0x1136471C00 = R14 CandleHornWall01 mesh +0x600) inside
			// the SAME engine lighting-pass machinery as the outdoor crash
			// (BSBatchRenderer_SetupAndDrawPass region uid 107639-107644),
			// NO SLF frames on the stack. fix73 = engine-native 8-slice
			// kSHADOWMAPS with NO render-loop rax=0 hook installed
			// (Hook_RenderShadowLights is only installed inside
			// InstallExtendedBuffers step 4 - fix73 skipped that whole
			// function), so the ENGINE's vanilla dispatch ran unmodified
			// against an accumulator the scheduler had flooded: the
			// ShadowSceneNode dump shows Active Shadow Lights=21 (SLF
			// scheduler publish, lightCount=21 BT rows from 23:49:56) and
			// the engine's <=8-slot shadow state machine walked all 21 ->
			// OOB descriptor state -> corrupted pointer in the lighting
			// pass. fix69 (30-slice array + rax=0 stop + scheduler) never
			// crashed indoors because rax=0 kept the engine dispatch off
			// the flooded accumulator. fix73 left HALF the v10
			// architecture (scheduler on, dispatch-stop off) and that
			// half-combination is the indoor killer.
			//
			// fix74 = InstallScheduler off => engine scheduling +
			// dispatch + rendering 100% vanilla (v7-vanilla proven state,
			// HookUtil.h:331: "outdoor sun shadow RETURNED"). Sun stays
			// (engine-rendered, fix73 outdoor verdict). Indoor shadow
			// lights return to the engine's native budget - extended
			// all-lights shadows then need the FULL CS-style coordinated
			// engine-state expansion (accumulator + channel map +
			// per-surface ceilings), not scheduler-plus-native-dispatch.
			//
			// fix75 (2026-09-10): RE-ENABLED stripped to ALWAYS_LIT-only.
			// fix74 user report: "not always-lit anymore, vanilla walk-up
			// behavior". The ALWAYS_LIT per-frame restore (fix34 fade
			// cache overwrite + fix38 lodDimmer pin) lived inside this
			// thunk, so disabling InstallScheduler killed "always lit"
			// along with the scheduler. The thunk now runs func() then
			// ONLY re-applies the lamp fade overrides (register/extend/
			// publish/pin/probes stripped inside) - engine state stays
			// 100% native (fix74 stability), lamps stay lit at any
			// distance (fix34/fix38 mechanism restored).
			P1::InstallScheduler();
			// Material-pass sampling probe (22:3x flicker diagnosis): logs
			// t103 view range + canvas content + engine shadow globals at
			// the t14-bind sampling moment. Read-only; strip anytime.
			// fix9 (23:3x): NOT installed here - this runs at kPostLoad when
			// BSGraphics::Renderer::GetRuntimeData().context is still null
			// (D3D device not created yet; SelectDSB first fires ~34s later
			// at 23:30:57 vs PostLoad 23:30:23) -> the probe silently
			// returned and logged nothing in fix8. Moved to kDataLoaded in
			// main.cpp where the renderer is guaranteed initialized.
			// P1::InstallMaterialPassProbe();
			// fix13 (2026-09-04 00:3x): UNPARKED. Parked in v8-exp2 (crash
			// isolation) when P1c-3's hot-path D3D writes were the suspect;
			// that crash context is gone (SLF_SKIP_VANILLA_DISPATCH=1 +
			// SLF_MANUAL_RENDER=1: WE render scheduled lights, the engine
			// dispatch never walks the 127-slice array; P1C3_ENABLED=0 drops
			// the VS/PS global-replacement half). fix12 turned SLF_PS_ENABLED
			// on + added the >4-light gate but FORGOT to unpark this call ->
			// BeginTechnique hook never installed -> CompileSLFPS/swap-hook/
			// shadow-trace never ran -> log had zero "SLF PS compiled" /
			// "t14-bind swap hook installed" lines and swaps stayed 0.
			// Now: BeginTechnique once-block compiles our PS, installs the
			// t14-bind swap hook and shadow trace; the gate (>4 scheduled
			// lights) keeps <=4-light scenes 100% engine PS.
			P1::InstallShaderHooks();
#if SLF_QA_ENABLED
			// Frame-quality probe (QA mode): GPU readback luma statistics so
			// build verdicts come from data, not the user's eyes. Inert until
			// ShadowLimitFix_QA.start appears next to SkyrimSE.exe.
			P1::InstallFrameQA();
#endif
			// B4c (2026-09-05): engine per-surface light batch rewrite
			// (CalculateActiveNonShadowCasterLights 100997/107784). Makes
			// the cb2 batch a consecutive prefix of g_scheduledShadowLights
			// so engine material-loop idx == t102 idx (B4b 序错位根因).
			// Flicker fix core - must be LIVE for the readback verdict.
			//
			// DISABLED (2026-09-05 23:5x, md5 d4eda656 -> 2026-09-06): B4c is
			// the ALIGNMENT PARTNER of the B2B payload - it exists to make
			// batch[i] == scheduled[i] so the payload's t102[i] read hits the
			// right light. With B2B OFF (engine PS bytecode pristine) B4c ON
			// is the WORST combination: the batch (and *shadowCount, lifted
			// past the <=4 clamp) gets rewritten to the scheduled prefix,
			// but the pristine engine PS samples the vanilla t14 4-channel
			// mask baked for the ENGINE'S OWN accumulated lights -> mask and
			// batch disagree -> wrong shadowing everywhere (user report on
			// d4eda656: picture still dark overall + flicker on turn +
			// lights not all on). B4c-v3 already proved the rewrite never
			// reaches draw-time cb2 anyway (that walk is 107300
			// sceneLights, the B4d seam). Baseline for this build = full
			// engine vanilla light batching + vanilla PS (only the 127-slice
			// array + background scheduler remain, both non-consuming).
			// P1::InstallSurfaceLightsHook();
			// B4d (2026-09-05): engine cb2 light-batch fill hook (AE 1.6.1170
			// RVA 0x14DD040, crashlog id 107300). B4c-v3 draw-time probe proved
			// the cb2 batch is NOT the 107784 output buffer - it is walked
			// from BSRenderPass::sceneLights by 107300. This hook rewrites
			// sceneLights[0..k) to the scheduled prefix right before that
			// walk, making cb2[i] == scheduled[i] actually hold at draw time.
			//
			// DISABLED (2026-09-06): field test showed it swaps the surface's
			// OWN lights (torches/candles, numLights=16) for the scheduled
			// prefix -> interior/night scenes lose ALL local lighting
			// (user: "所有灯都不亮"). The flicker reduction came at the cost
			// of breaking per-surface illumination. Re-enable only behind a
			// scene-type filter (outdoor/day where the sun dominates the
			// batch). Baseline = B4c v2 (de9ea01c) behaviour.
			// P1::InstallSceneLightsRewriteHook();
#else
			// P0 verified -> P1a: address-only (no engine interception)
			P1::Install();
#endif
		} else {
			SKSE::log::error("[SLF] P0 FAIL - check Address Library / game version");
		}
	}
}
