// HookUtil.h - standalone hook helpers for ShadowLimitFix
//
// ATTRIBUTION (original names preserved, see THIRD_PARTY.md):
//   - detour_thunk trampoline pattern: based on Microsoft Detours (MIT),
//     reference form as used by Community Shaders / Open Shaders
//     (github.com/alandtse/open-shaders, GPL-3.0 WITH Modding Exception).
//   - install_context_hook machinery: CommonLibSSE-NG Community-Shaders fork
//     (CharmedBaryon/CommonLibSSE-NG; alandtse fork), GPL-3.0-or-later WITH
//     Modding Exception.
// This plugin is standalone: REL-ID/engine-behavior comments are facts used
// for verification only, with no runtime dependency on any other mod.
// install_context_hook comes from the CommonLibSSE-NG CS fork
// (SKSE/ContextHook.h, requires SKSE_SUPPORT_XBYAK + vcpkg xbyak).
#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <SKSE/ContextHook.h>

#include <detours/detours.h>
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

struct ID3D11DeviceContext;  // post-light pass decl below (ShaderReplace.cpp owns the full d3d11 include)

// ---- BeginTechnique event ring buffer (crash forensics) ----
// Records the last N shader-swap decisions; dumped by the VEH filter in
// main.cpp so we can see exactly which pass crashed and whether the shadow
// check misfired.
struct BTEvent
{
	std::uint32_t vdesc;
	std::uint32_t pdesc;
	std::uint64_t dsv;      // OM-bound depth-stencil view at decision time
	std::uint8_t isShadow;  // IsShadowPass() result
	std::uint8_t skipPixel; // a_skipPixelShader
	std::uint8_t swapped;   // 1 = our VS/PS were bound
};
inline std::array<BTEvent, 64> g_btEvents{};
inline std::atomic<std::uint32_t> g_btIdx{ 0 };

inline void SLFRecordBT(std::uint32_t a_vdesc, std::uint32_t a_pdesc, void* a_dsv,
	bool a_isShadow, bool a_skipPixel, bool a_swapped)
{
	auto& e = g_btEvents[g_btIdx.fetch_add(1) % g_btEvents.size()];
	e.vdesc = a_vdesc;
	e.pdesc = a_pdesc;
	e.dsv = reinterpret_cast<std::uint64_t>(a_dsv);
	e.isShadow = a_isShadow ? 1 : 0;
	e.skipPixel = a_skipPixel ? 1 : 0;
	e.swapped = a_swapped ? 1 : 0;
}

inline void SLFDumpBT(std::FILE* a_f)
{
	const std::uint32_t n = g_btIdx.load(std::memory_order_relaxed);
	const std::uint32_t start = n > g_btEvents.size() ? n - g_btEvents.size() : 0;
	std::fprintf(a_f, "--- BT ring buffer (last %u of %u) ---\n",
		n > g_btEvents.size() ? static_cast<unsigned>(g_btEvents.size()) : n, static_cast<unsigned>(n));
	for (std::uint32_t i = start; i < n; i++) {
		const auto& e = g_btEvents[i % g_btEvents.size()];
		std::fprintf(a_f, "  [%4u] v=%08X p=%08X dsv=%016llX shdw=%d skip=%d swp=%d\n",
			i, e.vdesc, e.pdesc, e.dsv, e.isShadow, e.skipPixel, e.swapped);
	}
}

// Extended shadow-map DSV arrays (defined in P1_hooks.cpp). The engine
// renders shadow slices 0..N-1; slices >= 8 live in these arrays. Shared
// with ShaderReplace.cpp so IsShadowPass can match DSVs beyond slot 7.
namespace ShadowLimitFixNS::P1
{
	extern std::array<void*, 128> g_normalDepthBuffer;
	extern std::array<void*, 128> g_readOnlyDepthBuffer;

	// Material-pass shadow-array content readback (defined in P1_hooks.cpp).
	// Called from ShaderReplace.cpp right before our SLF PS samples t103.
	void DebugReadbackShadowArrayAtMaterialPass(::ID3D11DeviceContext* a_ctx);

	// Shadow-render trace (defined in P1_hooks.cpp): hooks ID3D11DeviceContext
	// OMSetRenderTargets (vtable[33]) / ClearDepthStencilView (vtable[53]) /
	// Draw (vtable[13]) and logs every call whose DSV texture IS the
	// kSHADOWMAPS array (what t103 samples). Answers: does the engine really
	// render shadow draws into our array, and is it cleared afterwards?
	void InstallShadowRenderTrace();

	// Material-pass sampling probe (defined in P1_hooks.cpp): hooks
	// ID3D11DeviceContext PSSetShaderResources (vtable[8] - verified against
	// SDK d3d11.h: VS/PS methods are interleaved, PSSetShaderResources sits
	// right after VSSetConstantBuffers at slot 8; fix10 23:4x REVERTED
	// fix8's wrong move to slot 16, which was PSSetConstantBuffers) and,
	// when the engine binds the shadow-mask texture to PS slot t14 (= this
	// material pass samples shadows), logs: the bound t103 depth-array view
	// range, a slice-content readback of the shadow canvas, engine shadow
	// globals (accum slot / mask index / shadow mask) and our scheduled
	// lights' descriptor state - the four facts that separate "canvas wrong"
	// from "material pass never samples the extended slices". Read-only.
	void InstallMaterialPassProbe();

	// Frame-quality probe (defined in P1_hooks.cpp, gated by SLF_QA_ENABLED).
	// GPU readback of the main render target -> per-second mean luma +
	// frame-to-frame flicker + lamp on/off step events, so build verdicts
	// come from DATA instead of the user's eyes. Inert until
	// ShadowLimitFix_QA.start appears in the game dir; auto-ends after 90 s
	// (or early on ShadowLimitFix_QA.stop). Idle cost = one file stat per
	// 500 ms.
	void InstallFrameQA();

	// ---- N-light shadow data channel (P1c-3 final step) ----
	// Filled every frame by the scheduler (Scheduler.cpp) from the engine's
	// per-light ShadowmapDescriptor; consumed by the replaced material
	// shaders (t103 = kSHADOWMAPS depth array SRV + this cbuffer).
	struct ShadowLightData
	{
		float proj[16];      // light transform, filled per type by the data
		                     // channel: omni/hemi = worldToCam AFFINE only
		                     // (paraboloid sample math needs light-space
		                     // position; perspective would corrupt it);
		                     // frustum (sun) = worldToCam x frustum proj.
		float pos[3];        // light world position
		float radius;        // light fade radius
		float shadowMapIndex; // slice into kSHADOWMAPS depth array
		float flags;         // 1 = matrix valid (enabled)
		float lightType;     // CS ShadowLightParam.x semantics: 0 = frustum
		                     // (sun), 1 = hemi (cone/spot, single paraboloid),
		                     // 2 = omni (dual-paraboloid front/back halves
		                     // stacked in one slice)
		float pad[2];
	};
	inline std::array<ShadowLightData, 128> g_shadowLights{};
	inline std::atomic<std::uint32_t> g_shadowLightCount{ 0 };

	// ---- P1c-2: per-light shadow render dispatch list ----
	// The render-loop hook (P1_hooks.cpp) replaces the engine's vanilla call
	// that was the ONLY producer of shadow-map depth (per-light Render). The
	// scheduler (Scheduler.cpp) persists the lights it slotted THIS frame
	// here; the render-loop hook then drives each one's engine
	// BSShadowLight::Render so the kSHADOWMAPS slices actually get depth.
	// Scheduler runs before the dispatch in the same engine frame (same
	// thread), so plain pointers valid until the next scheduler overwrite.
	struct ScheduledShadowLight
	{
		RE::BSShadowLight* light = nullptr;
		std::uint32_t slot = 0;
	};
	inline std::array<ScheduledShadowLight, 128> g_scheduledShadowLights{};
	inline std::atomic<std::uint32_t> g_scheduledShadowCount{ 0 };

	// fix45 (2026-09-08): post-resume dispatch grace counter (ticks).
	// The world-switch gate cooldown ending re-enables the scheduler's
	// register/extend writes AND the manual dispatch in the same frame.
	// The first post-resume dispatch froze inside a single shadow pass
	// (23:46 session: last OMSet->shadow 23:45:49.441, then OMSet stopped
	// advancing while shadow draws raced ~60k/s for 20+s - a Render on a
	// half-settled light, main image never produced = "freeze"). The
	// dispatch hook parks itself while this is >0 (scheduler still learns
	// the engine accumulator each frame); Scheduler.cpp arms it at resume.
	inline std::atomic<std::uint32_t> g_resumeGrace{ 0 };

	// fix46 (2026-09-08): true while the world-switch gate is frozen
	// (load UI open / cooldown). The scheduler's fill side is gated by
	// WorldSwitching(), but the manual dispatch (Hook_RenderShadowLights)
	// is NOT - on a cell transition WITHOUT a load menu (outdoor cell
	// boundary walk / teleport / camera jump) the scheduled list can still
	// hold the PREVIOUS frame's 21 lights whose engine objects the cell
	// unload just released -> dispatch Render on freed light -> clean exit,
	// no WER/CrashLogger dump (00:04:14 session, camera-jump gate fired
	// 00:04:14.218, died within ~1s). The dispatch hook parks itself while
	// this is true; the gate also zeroes the count the moment it freezes.
	inline std::atomic<bool> g_shadowWritesFrozen{ false };

	// ---- Post-light pass CPU data (SLF_POSTLIGHT_ENABLED) ----
	// Filled by the scheduler right after PublishShadowLightDataChannel:
	// one entry per EXTENDED light (scheduled index >= this frame's engine
	// count - engine's own 4+ are excluded, the engine already lit them).
	// The full-screen pass uploads these to its b9 cbuffer.
	struct PostLightCPU
	{
		float pos[3];
		float radius;
		float color[3];   // RE::NiLight::diffuse
		float intensity;  // 1.0 (tuning knob)
		float slice;      // kSHADOWMAPS slice (Step 2 shadow test)
		float lightType;  // 1 = hemi/paraboloid, 2 = omni (dual-paraboloid)
		float flags;      // 1 = valid
		float farDist;    // Step 2: paraboloid depth normalizer (shadow-camera
		                  // viewFrustum.fFar; fallback radius). depth =
		                  // saturate(length(lightSpace) / farDist), matching
		                  // CS ShadowLightParam.y semantics.
		float proj[16];   // Step 2: world -> light-space AFFINE transform
		                  // (shadow-camera worldToCam, w row zeroed) - CS
		                  // ShadowProj semantics: length(lightSpace) ==
		                  // distance to light; perspective would corrupt it.
	};
	inline std::array<PostLightCPU, 64> g_postLight{};
	inline std::atomic<std::uint32_t> g_postLightCount{ 0 };
	inline std::atomic<std::uint32_t> g_engineLightCount{ 0 };  // this frame's engine-scheduled shadow lights

	void RunPostLightPass(::ID3D11DeviceContext* a_ctx);  // ShaderReplace.cpp
	void ForceLampDimmersOne();  // Scheduler.cpp - restore lodDimmer=1 on all active lamps (fix38/39)
	void SunArmedAccumulateRealSlot();  // Scheduler.cpp - fix54: re-run armed caster walk on the sun against the engine's REAL accum slot right before dispatch renders it (CS SetupSunLight alignment)
}

// P1c-3 shader replacement pipeline gate.
// DISABLED (02:52) - direction rejected: the pipeline used CS Lighting.hlsl
// as the compile source, which is fundamentally a CS-dependent shader
// (CS cbuffers b4/b5/b6/b12 + CS texture slots). Without the full CS runtime
// it renders BLACK (verified 02:50). This plugin is INDEPENDENT - no CS, ENB
// only. Any future shader work must use engine-semantics shaders written
// from scratch, NOT CS sources. Engine-side 8-light rendering stays on.
// (2026-09-08: all P1C3 blocks removed from the build; no consumer remains.)

// SLF_B2B_ENABLED - the SLF-B consumer-side bytecode patch engine
// (SlfBytecodePatch.h splices a per-light shadow payload into engine
// material PS shadow units). 2026-09-04 23:1x: A/B isolation control
// (0 = engine bytecode passes through untouched) VERDICT from user test:
// backlit black on skin/fur CLEARED with the patch off -> B2B was the black
// source. Root cause found offline (audit over 1249 runtime variants):
// the tail mov's dst WRITE MASK was hardcoded .w, but only 58% of engine
// variants write their mask DP4 dst to .w; 42% write X/Y/Z -> the spliced
// mask landed in an untouched lane -> engine light code multiplied garbage
// -> black on backlit surfaces. Fix: tail mov dst write-mask now replicates
// each variant's own mask-DP4 dst lane (SlfBytecodePatch.h, verified
// 1249/1249 wm-ok). Ground flicker is a separate consumer-side issue
// (engine t14 4-channel mask rotation) and persists with B2B on or off.
//
// 2026-09-05 22:0x A/B round 2 (night scenes): after B4e + B4d were both
// removed (md5 3cc7f995 = B4c v2 + B2B + B4a + probes only), user reported
// the night picture still wrong ("整个不对了" - all lights dark). The only
// remaining engine-render-modifying mechanism is this splice (4574/4574
// Lighting PS variants patched at load). Toggling to 0 isolates it:
//   0 = engine PS untouched -> vanilla 8-light shadows (no t102/t103)
//   1 = spliced payload consumer (SLF shadows on vanilla PS)
//
// 2026-09-06 22:4x A/B round 2 VERDICT (B2B=0 build md5 008cb1de, user run
// 22:37-22:40, data from ShadowLimitFix.log): scheduler fully healthy the
// whole session (23 scheduled/23 published/23 valid matrices, 120 s solid,
// every light pinned to its own slice 8.., dispatch drawing into all
// slices), yet the user sees "灯不全亮，走近才亮" (lights only light up
// when approached). With NO consumer (B2B=0 + SLF_PS=0) the picture can
// ONLY show the engine's vanilla t14 4-channel mask rotation - the known
// ">4 shadow casters in one space -> engine re-picks which 4 get slots
// each frame, near lights win -> light pops in/out, walk closer and it
// turns on" artifact (recorded verbatim in the fix6 note above). The 23
// pinned slices we render have no consumer and cannot appear. Therefore
// B2B is NOT the flicker/darkness culprit - it is the ONLY consumer path
// that keeps engine material bytes exact. The 2026-09-04 backside-black
// root cause (hardcoded .w tail-mov mask dst vs 42% of variants writing
// X/Y/Z) is fixed (1249/1249 wm-ok) but has NEVER run in a real game.
// 2026-09-05 23:5x REAL-GAME VERDICT (md5 b1ffd9e0, dark/multi-light
// scene, user + QA log): B2B=1 = PICTURE WRECKED - whole frame blackened,
// armor lost metalness/specular, shadow + light flicker on camera turn.
// Root cause chain (all from log data, not eyeballs):
//   * payload indexes LightRec[i] by the engine light-loop counter i, but
//     engine cb2[15+i] holds the per-SURFACE nearby-light set in VIEW
//     space (all light z in [-65,+7] around cam, changes every draw),
//     while LightRec holds the 23 global SCHEDULE-order lights in WORLD
//     space -> B5 probe: 48191/48191 reads matched=0, dist 162-418u.
//   * B4a fills all 23 records enabled=1 -> payload gate fully open ->
//     every engine light shadow-samples the WRONG light's slice -> dark
//     scene = mass false shadowing (full black + flicker). Bright scenes
//     mask it (ambient covers the error) - explains why md5 7205f125
//     "looked OK" in the QA-bright scene but dies here.
//   * The enabled-gate-fail -> full-black failure mode is recorded as a
//     known hazard in the ShaderReplace.cpp header comment block.
// FIX DIRECTION (next round, needs corrected probe data first): cb2 pos
// is VIEW space -> match probe must transform (or compare in view space);
// and/or switch payload to a per-surface aligned index buffer instead of
// the global schedule-order LightRec. NOT blind-fixed here (B4e lesson).
// This build = B2B OFF (engine bytecode pristine = normal picture) so the
// user can play while the corrected view-space match probe collects data.
// (2026-09-08: all SLF_B2B_ENABLED blocks removed from the build.)

// SLF_PS_ENABLED - our OWN engine-semantics Lighting PS replacement
// (Shaders/Lighting_SLF.hlsl, re-implemented from the vanilla bytecode
// disassembly). Step 1: faithful vanilla re-implementation (screen should
// match vanilla); Step 2: N-light depth-array shadow sampling.
//
// fix12 ROUTE B RE-ENABLE (2026-09-03, user delegated the fix route to AI;
// verdict from the fix11 41k-line log: canvas side is 100% correct - all
// 24 lights render into 0x73555320 = kSHADOWMAPS t103 array slices - but
// the engine's VANILLA material PS consumes shadows ONLY via t14, a 2D
// 4-channel screen-space mask. The engine bakes the <=4 currently-rotated
// lights into it each frame -> >4 shadow lights in a space = channel
// rotation = the user-visible flicker ("游戏本身就有个BUG...阴影灯太多就闪").
// t103 is never bound/sampled by vanilla PS (ShaderReplace.cpp:1003 proves
// the plugin binds it itself) -> the ONLY extension path is the material
// PS sampling t103 directly (same route CS takes).
//
// WHY GATED (this is the fix for the 2026-09-02 disable reason, not a
// revert): the old enable swapped the SLF PS on EVERY t14 bind (152k+
// swaps, +32k/s -> effectively a global Lighting replacement -> material
// degradation on passes whose techniques our Step-1 re-implementation
// doesn't reproduce yet: parallax/normal-detail branches). fix12 swaps
// ONLY when more than 4 shadow lights are scheduled this frame
// (DoSLFShadowSwap gate on g_shadowLightCount): <=4 lights fit the vanilla
// t14 mask exactly, so those scenes stay 100% on the engine PS - zero
// material-regression risk in normal gameplay. The SLF PS only ever runs
// in the >4-light scenes that vanilla physically cannot shadow correctly.
//
// fix16 BLEED STOP (2026-09-04 00:4x): fix15 gate+data-channel live log
// verdict - 3/3 criteria PASSED (24 published/24 valid, BT lightCount=24
// shouldSwap=1 x2757, swap-on-t14-bind 85384 in 23s ~= 3700/s). The
// pipeline ENGINEERED works end-to-end. But the user's in-game read is
// three failures: material regression (the half-faithful Step-1 PS now
// really runs, fullscreen swap rate makes it unmissable), still-flicker,
// no >4 distinct shadows. Verdict: the swap-replacement approach is
// FATALLY FLAWED regardless of shadow correctness - any hand-written PS
// that replaces the engine PS shows material degradation until it is 100%
// faithful, which is a multi-month port (CS maintains theirs for years).
// Killed until a route that preserves engine material bytes exactly
// (e.g. compile-time source/bytecode injection, not
// runtime PS swap) is chosen. Render loop + data channel stay on (fix15
// PublishShadowLightDataChannel is inert without the PS consumer).
// (2026-09-08: all SLF_PS_ENABLED blocks removed from the build.)

// P1b compile gate - shared across translation units.
// Set to 1 to build the P1b extended-buffer engine modifications in.
//
// v7-obs 2026-09-03 11:19: user reports OUTDOOR SUN SHADOWS STILL DEAD
// with scheduler restored to vanilla func() (thunk -> func only). Root
// cause found: ENABLE_P1B was still 1, so the render-loop hook
// (Hook_RenderShadowLights) kept forcing ctx.Rax=0 EVERY frame - the CS
// "skip vanilla per-light dispatch" that is only valid when OUR scheduler
// replaces the whole chain. With the scheduler asleep AND rax=0 forced,
// NOBODY calls light->Render -> every kSHADOWMAPS slice stays empty
// (readback 0%, engine slot counter = 2 proves vanilla scheduler ran).
//
// v7-vanilla observation build: ENABLE_P1B=0 -> ZERO hooks installed
// (P1a logs only). Engine 100% vanilla. RESULT 2026-09-03 11:2x (user:
// "有了"): outdoor sun shadow RETURNED -> our P1b hook chain kills engine
// shadow rendering; engine vanilla path is fully alive underneath.
//
// v8-exp1: re-enable P1b (array expansion + DSV redirect + select hooks
// are passive - they only enlarge physical resources and route views; they
// do NOT touch the shadow LIGHT pipeline) but STOP forcing ctx.Rax=0 at
// the render-loop call site (SLF_SKIP_VANILLA_DISPATCH=0 below). Engine
// vanilla scheduler (func() in the thunk) + vanilla per-light Render
// dispatch run unmodified. Our own scheduler stays parked. If shadows
// return here, the array component is proven safe and rax=0 was the sole
// killer -> next step patches engine-side capacity (AccumulatedLightsArray
// + light-count limit) for >8 lights instead of replacing the scheduler.
//
// v8-exp2 (2026-09-03 12:04): CRASH VERDICT - P1b expansion IS the killer.
// v8-exp2 isolated the array component (scheduler detour, P1c-3 shader
// hooks, manual render, readback ALL off; SLF_SKIP_VANILLA_DISPATCH=0 lets
// the engine dispatch). Crash crash-2026-09-03-12-04-19.log = SAME point
// as v8-exp1: null vtable call [r8+0x50] at SkyrimSE.exe+14CC19E
// (uid107133+0x1AE; crash-log frame[0] return addr = +0x1B2 / 0x14CC1A2,
// the instruction right after the call) INSIDE the engine shadow-light
// dispatch fn 0x14CBFF0 (uid 107133, pdata span 0x25C; byte-verified
// 2026-09-03: 41 FF 50 50 @0x14CC19E, r8=[rax] vtable, rax = light object
// returned by the helper called at 0x14CC17D = the hooked site fn+0x18D;
// vtable slot 0x50 = idx 0x0A = BSShadowLight::Render). With dispatch
// genuinely running, the P1b count-patch 8->127 creates more shadow-map
// slices than the engine's fixed 8-slot state machine can address ->
// OOB/garbage state -> garbage light object in the dispatch array ->
// vtable[0x50]=0. CS only survives this because it ALSO expands the
// accumulator array + blend table + clamps with 30+ coordinated hooks;
// a 5-hook slice expansion is structurally half a patch.
//
// v10-phase1 (2026-09-03, user decision: all-lights shadow, 30-concurrent /
// 127-capacity, FULL self-managed dispatch, phased verification): re-enable
// the P1b arrays but pair them with the CS-verified dispatch ownership:
// the scheduler thunk calls the ORIGINAL engine scheduler first (func()
// restores every engine state side effect - sun accumulate, descriptor
// rebuild, mask/counters) then a register pass publishes the engine's
// shadowLightsAccum list to g_scheduledShadowLights; the render-loop hook
// skips the VANILLA dispatch (SLF_SKIP_VANILLA_DISPATCH=1) and drives each
// light's Render itself (SLF_MANUAL_RENDER=1). Engine 8-slot dispatch loop
// never runs against the 127-slice array -> the v8-exp2 crash site is never
// reached. Phase 1 output must equal v7-vanilla (sun + engine-budget
// lights); phase 2 appends SLF-managed lights at slots 8..29.
#define ENABLE_P1B 1

// Skip the vanilla shadow-light render dispatch at the render-loop call
// site (Hook_RenderShadowLights sets ctx.Rax=0). This recipe was cross-verified upstream,
// where it is ONLY valid because CS fully owns scheduling + rendering.
// v10-phase1: 1 - the engine scheduler still runs (func() inside the
// thunk) but the DISPATCH is ours (SLF_MANUAL_RENDER=1) - the exact CS
// split. With rax=0 forced and our register pass publishing the engine's
// accumulator list, every light the engine scheduled still gets its
// per-light Render, just driven by our loop instead of the engine's
// 8-slot loop (which cannot address the 127-slice array, v8-exp2 crash).
//
// fix20 (2026-09-04): 0 - render-producer isolation experiment. fix19o
// (manual render 24/24, 3.94ms/frame into kSHADOWMAPS slices 0-27) is
// verified CORRECT on the readback side, but the user still sees
// flicker ("地面一块亮一块黑"). Since SLF_PS_ENABLED=0 the vanilla
// material PS consumes shadows ONLY via the t14 screen-space 4-channel
// mask (baked by the engine's utility pass from the engine's OWN
// <=4-light registration) - our extended renders have no consumer and
// may desync the engine's bake table -> this flip returns the DISPATCH
// to the engine (vanilla producer, vanilla camera placement, vanilla
// registration) to isolate whether OUR dispatch aggravates the flicker
// vs the vanilla 4-slot rotation baseline.
//
// fix20 (2026-09-04 16:41 CRASH, crash-2026-09-04-16-41-43, RIP=0 null
// deref at SkyrimSE.exe+14CC1A2 = same engine dispatch loop as
// crash-2026-09-03-12-04-19 at 14CC19E, 4 bytes apart): letting the
// ENGINE dispatch while P1B's 127-slice array is installed is the
// KNOWN-fatal combo (v8-exp2 verdict, Scheduler.cpp v10 rationale) -
// the engine's fixed 8-slot state machine cannot address the 127-slice
// array. Producer-isolation via this macro is IMPOSSIBLE without also
// removing P1B itself (which would remove SLF). Verdict: the engine
// can NEVER be the dispatcher; WE must own dispatch. Flicker root cause
// stays at the consumer end (engine t14 4-channel mask), not producible
// by any isolation run. ROLLED BACK to 1.
#define SLF_SKIP_VANILLA_DISPATCH 1

// P1b full mode - real depth-buffer expansion (>8 slices) + render-loop hook.
// slice=8 mechanism verified in-game (00:37) -> enable full mode for slice=9.
#define P1B_FULL 1

// P1c-2 manual render dispatch: at the render-loop call site we replaced,
// ctx.Rax=0 alone skips the vanilla dispatch and leaves the kSHADOWMAPS
// array with NO depth producer (RL readback = 0% across all slices,
// verified 14:03). When set, the hook additionally drives each scheduled
// light's engine BSShadowLight::Render (vtable 0A) itself - the same
// per-light render the vanilla dispatch performed (pattern confirmed in
// the engine's own call graph; other lighting mods replace that same call site and
// calls Light->Render manually). Set 0 to revert to skip-only.
//
// v8-exp2 (2026-09-03): forced 0. With SLF_SKIP_VANILLA_DISPATCH=0 the
// engine itself dispatches; running OUR manual dispatch on top was a
// duplicate-render configuration (and the dispatch source
// g_scheduledShadowLights is empty anyway while the scheduler is parked).
// The manual dispatch only ever makes sense paired with rax=0 (skip).
//
// v10-phase1 (2026-09-03): 1 - paired with SLF_SKIP_VANILLA_DISPATCH=1.
// The scheduler thunk now runs func() (engine schedule) + a register pass
// that publishes the engine's shadowLightsAccum lights into
// g_scheduledShadowLights, so the manual dispatch renders exactly the
// lights the engine would have dispatched (same objects, same
// descriptor[0].shadowmapIndex slices) - but the loop is ours, so the
// engine's fixed 8-slot dispatch never touches the 127-slice array.
//
// fix20 (2026-09-04): was 0 - render-producer isolation via
// engine-dispatch = KNOWN FATAL (crash-2026-09-04-16-41-43, see note on
// SLF_SKIP_VANILLA_DISPATCH). ROLLED BACK: manual dispatch is the ONLY
// safe producer for the 127-slice array. 1.
#define SLF_MANUAL_RENDER 1

// v10-phase2 gate (2026-09-03 17:2x FLIPPED - phase1 + phase1c both passed
// in-game: phase1 indoor test 16:55 = 4/4 lights rendered with the CS
// AppendVirtual collection chain live, geom=712/68/33/40, 0 errors; user
// visual confirm "有影子的"). SLF-managed extension pass (Scheduler.cpp
// ExtendScheduledLights): after the engine scheduler (func) +
// RegisterEngineAccumLights publish the engine's <=8 lights
// (shadowLightsAccum, slices 0..7), this pass appends active lights the
// engine did NOT slot and gives them real shadow maps at slices 8..29 via
// the pure SLF path: UpdateCamera (mounts shaderAccumulator) -> readiness
// gate -> GameEnableLight (allocates the culling process) -> descriptors
// shadowmapIndex = slot -> ARMED engine cull-walk Accumulate (current-cull
// light + heal-attach chain, phase1c fix 17:2x) -> append to
// g_scheduledShadowLights after the engine prefix. NEVER writes the engine
// accumulator / mask / global counter for slot >= 8 (v6.1 OOB lesson:
// GameSetShadowCasterSlot at slot >= 8 crashes).
// 2026-09-03 ISOLATION TEST VERDICT (fix6, md5 02a44a22, 23:08 user run):
// EXTEND=0 with canvas pinning -> SelectDSB 100% clean (94 manual renders
// all sub->slot, zero drift; 36 engine renders untouched sub=0). User still
// reports flicker = "游戏本身就有个BUG，一个空间里阴影灯太多就闪" ->
// that is the VANILLA 4-slot ROTATION artifact (engine re-picks which 4
// lights hold shadow slots each frame when a space holds >4 casters ->
// light pops in/out, near lights win = "走近点才会亮"). Canvas pinning
// cannot cure slot rotation; ONLY a permanent slot per light can
// (EXTEND=1 -> no rotation -> no pop). Every earlier EXTEND=1 build was
// polluted by canvas drift (fix6 cured) - this is the FIRST clean full
// config test: 24 lights, each pinned to its own slice 8..29 forever.
//
// fix20 (2026-09-04): was 0 - rolled back to 1 after the 16:41 crash
// (see SLF_SKIP_VANILLA_DISPATCH note): engine dispatch is fatal with
// the extended array, so the extension pass (slices 8-29, our producer)
// is the live config going forward. fix19o = 1/1/1 stable (no crash).
#define SLF_P2_EXTEND 1

// v10-selfcheck (2026-09-03): read-only runtime diagnostics that replace
// eyeball-only phase2 verdicts. Three probes run inside the manual dispatch
// (P1_hooks.cpp RenderScheduledShadowLightsDispatch, every 64th frame):
//   SC-A  descriptor/slice audit - one light whose descriptors all share a
//         single shadowmapIndex (dual-paraboloid halves overwrite each
//         other -> shadow missing half) or two distinct lights mapped to
//         one slice (later render overwrites earlier -> light loses shadow).
//   SC-B  per-light shadow-camera placement vs light position + frustum
//         state (the old check only printed light#0 - eyeball territory).
//         dir (sun) is skipped: its ortho camera sits at the scene center.
//   SC-C  dispatch cost in ms via QPC (perf budget for the 127-light goal).
// All probes are read-only; rendering behavior is untouched. Flip to 0 to
// strip every probe back to the exact phase2 baseline build.
#define SLF_SELFCHECK 1

// SLF_QA_ENABLED - frame-quality probe (P1_hooks.cpp QaProbe section).
// 2026-09-06: user asked to replace eyeball verdicts with pure data
// ("你能不能仅用数据检测不要我亲眼看"). The probe hooks into the existing
// context vtable chain (OMSetRenderTargets + Draw ticks), picks the largest
// non-shadow RTV as "the picture", and while <game dir>/ShadowLimitFix_QA.start
// exists copies it to a staging texture every ~100 ms and computes:
//   mean luma / min / max per 2 s bucket,
//   flicker%  = fraction of samples whose mean luma or >5% of grid points
//               moved >=8% vs the previous sample,
//   lamp steps = single-sample mean-luma jumps >=25% (light on/off events).
// Auto-stops after 90 s or on ShadowLimitFix_QA.stop, then logs a summary.
// A/B builds are compared numerically. 0 = compile the whole probe out.
#define SLF_QA_ENABLED 1

// SLF_B5_MATCH_PROBE - CPU-side position-match simulation for the B2B
// payload index fix (2026-09-05). The engine PS light loop walks
// i = 0..min(cb2[29].x,7) and reads light i's position from cb2[15+i]
// (proven by disasm: `ftoi r6.w, r2.z; add r9, -v2, cb2[r6.w+15].xyz`),
// while the B2B payload indexes LightRec[i] in GLOBAL SCHEDULE order ->
// systematic mismatch ([B4c][draw] log evidence). Proposed fix: in the
// payload, read cb2[15+i].xyz and nearest-neighbour match it against
// LightRec[] instead of hard-indexing i. This probe simulates that match
// on the CPU at draw time (every 256th patched draw) and reports how many
// engine cb2 slots find a unique nearby LightRec entry + distance spread,
// to validate viability + pick the shader-side distance threshold BEFORE
// any bytecode payload change. 0 = compile the probe out.
#define SLF_B5_MATCH_PROBE 1

// SLF_POSTLIGHT_ENABLED - POST-LIGHT MULTI-LAMP PASS (2026-09-06, consumer
// side of the >4-light goal).
//
// WHY (after fix16 killed whole-PS runtime swap - 4491 engine PS variants,
// hand-written PS always regresses materials): the engine physically
// consumes only 4 shadow lights/frame (t14 = screen-space 4-channel mask,
// cb2 slots <= 7 diffuse with <= 4 shadowed, PS variants cannot express
// more). even full lighting-overhaul mods do NOT unlock this ("the shadow limit
// is not yet unlocked" - its extra lights are shadowless diffuse via a
// clustered shader REPLACEMENT).
//
// SLF already renders 4 engine + 19 extended lights into the 127-slice
// kSHADOWMAPS array every frame with NO consumer - the extended 19 are
// invisible (never in engine cb2). This pass is the consumer: after the
// engine finishes the main scene (first ImageSpace BeginTechnique), run our
// own full-screen pass over kMAIN (color + depth):
//   depth -> world pos (+ normal via screen-space difference),
//   for each EXTENDED light (scheduled index >= engine count; the engine's
//   own 4 are excluded - the engine already lit them, no double light):
//     diffuse += lightColor * NdotL * engine-style attenuation (* shadow
//     from t103 slice in Step 2),
//   out = in + albedoApprox * diffuseAcc.
// Engine PS untouched -> zero material regression; ENB untouched (pass
// runs before engine image-space post).
// 0 = compile the whole pass out (baseline probes only).
// fix24 (2026-09-07): PPT isolation experiment #1 - SLF confirmed as the
// stutter source (user A/B test: DLL renamed off = smooth, on = PPT in the
// multi-lamp room after a fast-travel round trip). Suspect #1 = this pass:
// RunPostLightPass mutates OM/PS/SRV/RS/BS/DSS/viewport mid-frame (at the
// first ImageSpace BeginTechnique) and never restores engine state, which
// can desync the engine's render-state cache -> batch merging collapses ->
// lighting pass count explodes (~49/frame stuttering vs ~13 smooth) ->
// engine CPU stalls. Set 0 to isolate: if the room is smooth with this off,
// the pass (or its trigger/state handling) owns the PPT.
// fix25 (2026-09-07): VERDICT - user A/B: pass OFF (fix24) = smooth, so the
// pass owns the PPT. Root fix = full state save/restore inside
// RunPostLightPass (IA/VS/PS/CB9/SRV0-1/sampler/OM-RTV+DSV/BS/DSS/RS/VP/
// scissor), restoring engine cache == device before we return. Re-enabled.
// fix36 (2026-09-07): user verified lamps with NON-overlapping ranges are
// ALL lit + shadowed + stable by the engine alone (layout solves the budget
// competition). Post pass no longer needed for that state - disable it
// (also removes its full-screen copy from the frame, the suspected
// tear-line amplifier). Redline issue parked.
// (2026-09-08: all SLF_POSTLIGHT_ENABLED blocks removed from the build.)

// fix28 (2026-09-07): user directive - real shadow projection is NOT the
// goal; the goal is "any number of lights in one space, stable, no crash".
// The ONLY remaining flicker = the engine swapping its shadow pick while
// the player walks (verified: ACC n=1 churn per walk step; still when
// standing). Kill engine point-light shadow projection entirely: after the
// engine scheduler runs, strip every non-directional light from
// shadowLightsAccum (the pool the engine's cb2 shadow-lamp selection reads)
// so the engine projects ONLY the sun (fixed, never swaps). Point lights
// still shine (engine diffuse + our post pass) but never cast -> nothing to
// swap -> no flicker; and with zero point-light shadow pressure the engine
// stays stable even with 1000+ lights (future: full-list diffuse pass).
// fix29 (2026-09-07): user refined the target - keep FOUR normal shadow
// lights (fixed, no churn), everything else unlimited diffuse incl. any
// light count. SLF pins a fixed set of up to N engine-ready point lights
// in shadowLightsAccum (frame-to-frame inheritance: as long as a pinned
// light is still engine-ready this frame it stays - walking inside a room
// never swaps the 4 -> no flicker; only a real scene change swaps them).
// fix41 (2026-09-08): DISABLED - proven ineffective in-game (candidates
// sparse: engine only ever had 1 shadow light in the test room, so nothing
// to pin; "忽亮忽灭" continued) AND the per-frame clear+push of the
// engine's shadowLightsAccum is a crash contributor: next frame the engine
// scheduler (func) runs on OUR rebuilt list whose entries lack engine
// internal state -> dispatch hits empty vtable slots
// (crash 2026-09-08-22-01-10: SkyrimSE+14CD743 call [rax+0x30] during a
// fast-travel load, the same dispatch region as the SLF-era 14CC19E crash).
#define SLF_PIN_FIXED_LIGHTS 0

// fix34 (2026-09-07): "lamp lights up only when you walk close" root cause
// = the engine's per-frame LIGHT LOD FADE: lamps farther than the interior
// light-fade distance (InteriorData lightFadeStart/End) are faded out; walk
// into range and the lamp pops on. CS reads the same engine cache
// (REL 527669/414583, GetLightLODEndFadeSquared - "recomputed each frame by
// Sky::UpdateLightLODFadeDistances"). Fix: overwrite that squared fade-end
// cache with a huge value every frame (CS's own per-frame re-apply pattern)
// so no lamp ever fades -> all lamps lit at any distance, no walk-up pop.
#define SLF_ALWAYS_LIT 1

// fix37 (2026-09-07): lodDimmer-jump probe. User: lamps pop (sudden step,
// not smooth) when walking closer/farther, even though the lamp is always
// there. Suspect = the engine's per-lamp lodDimmer (BSLight @0x14), the
// distance-based dimming coefficient. Log frame-to-frame jumps > 0.15 with
// player-lamp distance, so the step distance/source becomes DATA.
#define SLF_DIMMER_PROBE 1

namespace stl
{
	// Replace a whole engine function with a thunk struct via Detours.	// The thunk struct must provide:
	//   static void thunk()          - our replacement entry
	//   static inline REL::Relocation<decltype(thunk)> func;  - original pointer
	template <class T>
	long detour_thunk(REL::RelocationID a_relId)
	{
		T::func = a_relId.address();
		if (const long rc = DetourTransactionBegin(); rc != NO_ERROR)
			return rc;
		if (const long rc = DetourUpdateThread(GetCurrentThread()); rc != NO_ERROR) {
			DetourTransactionAbort();
			return rc;
		}
		if (const long rc = DetourAttach(reinterpret_cast<PVOID*>(&T::func), reinterpret_cast<PVOID>(T::thunk)); rc != NO_ERROR) {
			DetourTransactionAbort();
			return rc;
		}
		return DetourTransactionCommit();
	}

	// Same as detour_thunk but for a raw image-base-relative address (no
	// Address Library ID). Used by B4d (2026-09-05): the cb2 light-batch
	// fill function (crashlog id 107300) has no AL ID - SE/AE/VR offline
	// databases both miss it - so AE 1.6.1170 hooks it as base + RVA after a
	// prologue byte sanity check. Caller must gate this per runtime/version.
	template <class T>
	long detour_thunk_addr(std::uintptr_t a_addr)
	{
		T::func = a_addr;
		if (const long rc = DetourTransactionBegin(); rc != NO_ERROR)
			return rc;
		if (const long rc = DetourUpdateThread(GetCurrentThread()); rc != NO_ERROR) {
			DetourTransactionAbort();
			return rc;
		}
		if (const long rc = DetourAttach(reinterpret_cast<PVOID*>(&T::func), reinterpret_cast<PVOID>(T::thunk)); rc != NO_ERROR) {
			DetourTransactionAbort();
			return rc;
		}
		return DetourTransactionCommit();
	}

	// Replace one vtable slot with a thunk struct (CS include/PCH.h pattern,
	// ShadowCasterClassifier.cpp InstallCasterCullHook). The thunk struct
	// must provide:
	//   static void thunk(Self*, Args...)                 - our replacement
	//   static inline REL::Relocation<decltype(thunk)> func;  // original
	template <std::size_t a_idx, class T>
	void write_vfunc(REL::VariantID a_vtblId)
	{
		REL::Relocation<std::uintptr_t> vtbl{ a_vtblId };
		T::func = vtbl.write_vfunc(a_idx, T::thunk);
	}
}
