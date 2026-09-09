// Scheduler.cpp - P1c-1b: real shadow-caster scheduler (select N, slot them)
//
// ATTRIBUTION (original names preserved, see THIRD_PARTY.md): engine-state
// API shapes and scheduler concepts cross-verified against Community Shaders
// / Open Shaders (alandtse/open-shaders) ShadowEngineHooks.cpp and
// ShadowCasterClassifier.cpp (GPL-3.0 WITH Modding Exception). Re-implemented
// here; REL-ID facts only, no runtime dependency.
//
// Replaces CalculateActiveShadowCasters (ID 100419/107137). The engine calls// ResetCalculatedShadowCasterLights BEFORE this hook (vanilla flow), which
// clears slot state and installs the sun - so we only need to:
//   1. collect active shadow lights
//   2. sort by distance to the camera
//   3. slot the top N via GameSetShadowCasterSlot (engine API 99728/106365)
// The engine render loop then renders N true shadow maps.
//
// Engine state API shapes cross-verified against an upstream shadow-engine reference.
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "HookUtil.h"

#include <chrono>
#include <cstdio>
#include <unordered_set>
#include <array>

namespace ShadowLimitFixNS::P1
{
	// ---- Engine state wrappers (CS-verified IDs) ---------------------------

	static RE::ShadowSceneNode* GetShadowSceneNode()
	{
		static REL::RelocationID uid(513211, 390951);
		return *reinterpret_cast<RE::ShadowSceneNode**>(uid.address());
	}

	static RE::NiCamera* GetWorldCamera()
	{
		static REL::RelocationID uid(528087, 415032);
		auto* sg = *reinterpret_cast<RE::BSSceneGraph**>(uid.address());
		return sg ? sg->GetRuntimeData().camera.get() : nullptr;
	}

	static uint32_t* GetAccumLightSlot()
	{
		static REL::RelocationID uid(528091, 415036);
		return reinterpret_cast<uint32_t*>(uid.address());
	}

	static uint32_t* GetMaskIndex()
	{
		static REL::RelocationID uid(528091, 415036);
		return reinterpret_cast<uint32_t*>(uid.address() + 4);
	}

	// Engine's active shadow-caster bitmask (ORed per slot). VANILLA engine
	// state - CS only writes it too (ShadowScheduler.cpp:811
	// `*GetShadowMask() |= 1u << slot`). The engine's shadow logic keys off
	// this bitmask; without it, later shadow handling may skip lights even
	// though their shadow maps rendered.
	static uint32_t* GetShadowMask()
	{
		static REL::RelocationID uid(528093, 415038);
		return reinterpret_cast<uint32_t*>(uid.address());
	}

	static void GameSetShadowCasterSlot(RE::ShadowSceneNode* ssn, RE::BSLight* light, uint32_t index, uint32_t unk)
	{
		using F = void (*)(RE::ShadowSceneNode*, RE::BSLight*, uint32_t, uint32_t);
		static REL::Relocation<F> func{ REL::RelocationID(99728, 106365) };
		func(ssn, light, index, unk);
	}

	// Engine per-light activation (SkyrimSE+14A2530 / Address Library 106342).
	// Vanilla CalculateActiveShadowCasters called this right after UpdateCamera
	// for every accepted candidate (disasm SkyrimSE+14CC7C9). It allocates the
	// per-light culling process the accumulate walk appends casters to.
	static void GameEnableLight(RE::ShadowSceneNode* ssn, RE::BSLight* light)
	{
		using F = void (*)(RE::ShadowSceneNode*, RE::BSLight*);
		static REL::Relocation<F> func{ REL::RelocationID(99708, 106342) };
		func(ssn, light);
	}

	// ---- Scheduler ---------------------------------------------------------

	static constexpr uint32_t MAX_SCHEDULED_SHADOW_LIGHTS = 30;  // v6: up to 30 scene shadow lights (kSHADOWMAPS already patched to 127 slices)

	// v6.1 CRASH FIX (22:36 CTD, SkyrimSE+14A3FE5): the engine's
	// shadowLightsAccum array (ShadowSceneNode RUNTIME_DATA, +0x230) has a
	// FIXED capacity of ~8 entries (slot 0 = sun when active, point lights
	// 1..7; CS likewise only ever writes accum slots 0..7). GameSetShadow-
	// CasterSlot (Address Library 106365 / 0x14A3FB0) indexes that array
	// with the slot we pass and UNCONDITIONALLY writes back into whatever
	// pointer it read:  old = accum[slot]; if (old && old != light)
	// old->[0x520] = 0xFF. At slot >= capacity the read returns stale/
	// garbage memory and the [rcx+0x520]=0xFF store AVs (crash dump
	// register: rcx = 0x7FF7BC9F6DF0, inside the .text image).
	//
	// v6 raised MAX_SCHEDULED_SHADOW_LIGHTS 8 -> 30 and called this engine
	// API for every scheduled light -> nightly/indoor scenes slotted 10+
	// and blew the accumulator. Fix: lights with slot < this capacity go
	// through the FULL engine-native path (SetShadowCasterSlot + shadow
	// mask + global accum counter). Lights beyond it are shadow-map
	// producers only for OUR pipeline: we still write their descriptor
	// shadowmapIndex (= kSHADOWMAPS slice), still run the engine cull walk
	// (Accumulate -> per-light caster geometry the manual Render needs) and
	// still persist them to g_scheduledShadowLights for the render-loop
	// dispatch hook - but we NEVER touch the engine accumulator for them
	// (no GameSetShadowCasterSlot, no mask bit, no global counter bump).
	// That split is safe because our dispatch drives BSShadowLight::Render
	// from our own list, not from shadowLightsAccum.
	static constexpr uint32_t ENGINE_ACCUM_CAPACITY = 8;

	// ---- Descriptor readiness gate (v4, crash fix) ------------------------
	// Engine Accumulate (BSShadowLight vtable 09 -> 0x1511C80 / ID 108488,
	// verified against SkyrimSE 1.6.1170) iterates ALL of the light's
	// shadowmapDescriptors and per descriptor dereferences
	// descriptor->shaderAccumulator (descr+0x48) UNCONDITIONALLY:
	//   [acc+0x160] = *count+1 ; [acc+0x164] = 1<<*count ; [acc+0x168] = i ;
	//   [acc+0x12e] = 1 ; then 0x14F0920(light, &descr, &count, ...) which
	//   uses the accumulator again. NO null check anywhere -> a descriptor
	//   whose accumulator is still null AVs at mov [r8+0x160], eax (R8=0,
	//   SkyrimSE+1511CCA - the 15:15/15:31 crashes, identical signature).
	// The engine only accumulates a light once ITS per-descriptor accumulator
	// exists (vanilla coordinates this with internal phase bytes - e.g.
	// 0x1EC0350 gates the sun section of the original CalculateActive-
	// ShadowCasters). Right after a load/cell change the first scheduler run
	// lands BEFORE the engine rebuilt the sun's accumulators -> our v3
	// unconditional call crashed on frame 1. We gate on the same implicit
	// precondition: every descriptor must have its shaderAccumulator before
	// Accumulate may run. When not ready we defer (the engine finishes setup
	// over the next frames; the log proves when accumulate starts landing).
	// camera (descr+0x40) is only NiPointer retain/release'd inside the
	// per-descriptor worker (null-guarded there) so accumulate tolerates
	// camera==null, but Render needs it -> point lights gate on both.
	struct DescriptorReadiness
	{
		std::uint32_t total = 0;
		std::uint32_t accNull = 0;  // descriptors missing shaderAccumulator (crash precondition)
		std::uint32_t camNull = 0;  // descriptors missing camera (needed by Render)
	};

	static DescriptorReadiness GetDescriptorReadiness(RE::BSShadowLight* a_light)
	{
		DescriptorReadiness r;
		if (!a_light)
			return r;
		auto& descs = a_light->GetRuntimeData().shadowmapDescriptors;
		r.total = static_cast<std::uint32_t>(descs.size());
		for (const auto& d : descs) {
			if (d.shaderAccumulator.get() == nullptr)
				r.accNull++;
			if (d.camera.get() == nullptr)
				r.camNull++;
		}
		return r;
	}

	// fix19a (2026-09-04 02:1x): is the light's descriptor[0] camera frustum
	// still the engine's DEFAULT UNIT BOX (never placed)? Returns 1 = default
	// box (the cull walk inside Accumulate sees ~nothing -> sceneAccum stays 0
	// -> Render rasterizes 0 px -> t103 all-0% readback), 0 = some real
	// frustum was written, -1 = no descriptor[0] camera at all.
	// Same default-box predicate as the [CN] census in P1_hooks.cpp.
	static int CamFrustumDefault(RE::BSShadowLight* a_light)
	{
		if (!a_light)
			return -1;
		auto& descs = a_light->GetRuntimeData().shadowmapDescriptors;
		if (descs.empty() || !descs[0].camera)
			return -1;
		const auto& fr = descs[0].camera->GetRuntimeData2().viewFrustum;
		return (fr.fLeft == -1.0f && fr.fRight == 1.0f && fr.fTop == 1.0f &&
				   fr.fBottom == -1.0f && fr.fNear == 0.1f && fr.fFar == 1.0f) ?
			       1 :
			       0;
	}

	static void ScheduleShadowCasters()
	{
		auto* ssn = GetShadowSceneNode();
		auto* camera = GetWorldCamera();
		if (!ssn || !camera) {
			// No scheduler output this frame: the render-loop dispatch hook
			// must not replay a stale list (pointers may have been freed by
			// a cell change since the last successful schedule).
			ShadowLimitFixNS::P1::g_scheduledShadowCount.store(0, std::memory_order_release);
			return;
		}

		auto& lights = ssn->GetRuntimeData().activeShadowLights;

		// ---- Sun / directional shadow light: engine-native accumulation ----
		// Runs BEFORE the point-light empty guard: vanilla accumulated the sun
		// every frame the scheduler ran, independent of nearby point lights.
		// ResetCalculatedShadowCasterLights (ran before this hook) installed
		// the sun's descriptors; the engine's own shadow pipeline dispatches
		// the sun LATER in the frame through a path we do NOT replace. But
		// vanilla's CalculateActiveShadowCasters was what accumulated the
		// sun's casters (the cull walk that fills its per-light geometry).
		// Since we fully replaced that function, the sun's own renders have
		// been running empty (D3D trace 14:03: clears+draws on the shadow
		// array, content 0%). Restore the accumulation here, before any point
		// light, same slot-0 semantics as the engine (CS renders sun first /
		// slot 0 likewise). Arg shape = the engine's own SE/AE call (global
		// count by ref, channel 0, culling scene default); vrFlag default 0
		// outside VR.
		//
		// v4 GATE: only when every descriptor has its shaderAccumulator
		// (see GetDescriptorReadiness). On the first frame(s) after a load
		// the engine has not rebuilt the sun's accumulators yet - the v3
		// unconditional call AV'd there (R8=0 at SkyrimSE+1511CCA). Defer
		// until the engine catches up; log one full detail on the first defer
		// and a short line every 64th thereafter so the catch-up moment is
		// visible in the log (accNull flips 1 -> 0).
		{
			auto* sun = ssn->GetRuntimeData().sunShadowDirLight;
			if (!sun) {
				static std::uint32_t s_sunNone = 0;
				if ((s_sunNone++ & 0x3Fu) == 0)
					SKSE::log::info("[SLF] scheduler: no sun shadow light this frame");
			} else {
				const auto rd = GetDescriptorReadiness(sun);
				const std::uint32_t smc = static_cast<std::uint32_t>(sun->shadowMapCount);
				// Accumulate (0x1511C80) loops i in [0, shadowMapCount) and
				// reads descriptor[i] - so the descriptor array must cover
				// shadowMapCount entries (shadowMapCount > size() would walk
				// OOB). Engine keeps both in sync once setup completes.
				const bool ready = rd.total != 0 && rd.accNull == 0 && smc <= rd.total;
				if (ready) {
					sun->Accumulate(*GetAccumLightSlot(), 0, nullptr);
					static std::uint32_t s_sunLog = 0;
					if ((s_sunLog++ & 0x3Fu) == 0) {
						auto& srtd = sun->GetRuntimeData();
						SKSE::log::info("[SLF] scheduler: sun accumulated (descs={} smc={} idx0={} slotCtr={} sceneAccum={})",
							rd.total, smc, srtd.shadowmapDescriptors[0].shadowmapIndex,
							*GetAccumLightSlot(),
							static_cast<std::uint32_t>(srtd.sceneAccumArray.size()));
					}
				} else if (rd.total == 0) {
					// No shadow-map descriptors yet: engine has not installed
					// the sun's shadow maps (normal for the very first frames
					// after a load). Nothing to accumulate into.
					static std::uint32_t s_sunNoDesc = 0;
					if ((s_sunNoDesc++ & 0x3Fu) == 0)
						SKSE::log::info("[SLF] scheduler: sun defer - no descriptors yet (engine not ready)");
				} else {
					// Descriptors exist but accumulator(s) missing (or count
					// mismatch): exactly the v3 crash state - must NOT call
					// Accumulate.
					static std::uint32_t s_sunDefer = 0;
					const std::uint32_t n = ++s_sunDefer;
					if (n == 1)
						SKSE::log::info("[SLF] scheduler: sun DEFER accumulate (crash guard) total={} smc={} accNull={} camNull={} - engine rebuilding shadow state after load",
							rd.total, smc, rd.accNull, rd.camNull);
					else if ((n & 0x3Fu) == 0)
						SKSE::log::info("[SLF] scheduler: sun still deferring (total={} smc={} accNull={} camNull={})",
							rd.total, smc, rd.accNull, rd.camNull);
				}
			}
		}

		if (lights.empty()) {
			ShadowLimitFixNS::P1::g_scheduledShadowCount.store(0, std::memory_order_release);
			return;
		}

		const RE::NiPoint3 camPos = camera->world.translate;

		// Collect + sort by squared distance to camera.
		std::vector<RE::BSShadowLight*> ranked;
		std::vector<float> distSq;
		ranked.reserve(lights.size());
		distSq.reserve(lights.size());
		for (auto& sp : lights) {
			if (!sp || !sp->light)
				continue;
			const RE::NiPoint3 pos = sp->light->world.translate;
			const float dx = pos.x - camPos.x;
			const float dy = pos.y - camPos.y;
			const float dz = pos.z - camPos.z;
			ranked.push_back(sp.get());
			distSq.push_back(dx * dx + dy * dy + dz * dz);
		}

		// Insertion sort by distance (small N, keeps it simple).
		for (size_t i = 1; i < ranked.size(); i++) {
			size_t j = i;
			while (j > 0 && distSq[j] < distSq[j - 1]) {
				std::swap(ranked[j], ranked[j - 1]);
				std::swap(distSq[j], distSq[j - 1]);
				j--;
			}
		}

		// ---- v6.3/v6.4 anti-churn: stable ordering + full roster ----------
		// Flicker report (user: "灯一闪一闪", only while WALKING, the light
		// glow itself; fix directive: "应该让所有灯常亮"): raw per-frame
		// distance re-sort makes lights swap slots - and worse, cross the
		// ENG/SLF boundary (engine-accumulator path vs self-managed, v6.1) -
		// constantly while the player moves. A light alternating between
		// full-engine activation (mask bit + accum slot + counters) and
		// self-managed mode frame to frame toggles its engine shadow
		// membership -> the glow/brightness strobes.
		// v6.4 membership rule: EVERY active light below the budget is a
		// roster member every frame (22 active << MAX_SCHEDULED_SHADOW_LIGHTS
		// 30 - nothing may be culled by distance or by UpdateCamera). The
		// ordering below only stabilizes SLOT ASSIGNMENT (keeps last frame's
		// lights in their previous relative order so each keeps its slice);
		// it no longer decides who is lit. The sole membership veto left is
		// the readiness gate (engine-state safety). Game thread only.
		static std::vector<RE::BSShadowLight*> s_prevSlotted;  // last frame's chosen (slot order)
		{
			std::vector<RE::BSShadowLight*> coherent;
			std::vector<float> coherentDist;
			coherent.reserve(ranked.size());
			coherentDist.reserve(ranked.size());
			// 1) previously slotted lights still present this frame, in the
			// previous slot order (stable identity + stable slot + stable
			// ENG/SLF path across frames).
			for (auto* prev : s_prevSlotted) {
				for (size_t i = 0; i < ranked.size(); i++) {
					if (ranked[i] == prev) {
						coherent.push_back(prev);
						coherentDist.push_back(distSq[i]);
						break;
					}
				}
			}
			// 2) the remainder, distance order (newcomers / previously
			// rejected lights fill only leftover budget).
			for (size_t i = 0; i < ranked.size(); i++) {
				bool dup = false;
				for (auto* c : coherent) {
					if (c == ranked[i]) {
						dup = true;
						break;
					}
				}
				if (!dup) {
					coherent.push_back(ranked[i]);
					coherentDist.push_back(distSq[i]);
				}
			}
			ranked.swap(coherent);
			distSq.swap(coherentDist);
			// Rebuild this frame's prev list from the accepted lights below.
			s_prevSlotted.clear();
		}

		// Slot the top N with ENGINE-NATIVE slotting + accumulation.
		// Engine already reset slot state (ResetCalculatedShadowCasterLights
		// ran before this hook). NOTE: do NOT touch BSShadowLight::maskIndex -
		// it is a 0..3 shadow mask channel used by the VANILLA material shaders
		// (4-entry technique table, no bounds check). Writing >3 would crash.
		// Real >4-light visibility requires the shader replacement (P1c-3),
		// which reads our ShadowLightData instead of the engine mask.
		//
		// Slice 0 is reserved for the sun shadow (engine slot-0 semantics,
		// CS renders sun first at Lights[0]); point lights slot from 1 when a
		// sun shadow exists, else from 0. Without the reservation the sun's
		// own dispatch and our manual point dispatch would alias slice 0.
		const bool sunActive = ssn->GetRuntimeData().sunShadowDirLight != nullptr;
		const uint32_t slotBase = sunActive ? 1u : 0u;
		uint32_t slot = slotBase;
		uint32_t slotted = 0;

		for (size_t i = 0; i < ranked.size() && slotted < MAX_SCHEDULED_SHADOW_LIGHTS; i++) {
			auto* light = ranked[i];
			auto& lrtd = light->GetRuntimeData();

			// v6 (real N-slice): accept multi-descriptor lights (parabolic /
			// omni, shadowMapCount 2). ALL descriptors share ONE slice: the
			// engine's parabolic Render draws a single paraboloid map and
			// copies descriptor[0]'s renderTarget into [1]; CS mirrors the
			// shadowmapIndex [0]->[1] for the same reason (Hook_Parabolic-
			// Render). Splitting slices across descriptors would alias the
			// next light's map. Every descriptor is written to the same slot
			// below; engine Accumulate walks ALL descriptors, so each must be
			// mounted (readiness gate) before we slot.
			const std::uint32_t nd = static_cast<std::uint32_t>(lrtd.shadowmapDescriptors.size());
			if (nd == 0) {
				static std::uint32_t s_noDesc = 0;
				if ((s_noDesc++ & 0x1FFu) == 0)
					SKSE::log::info("[SLF] scheduler: light without shadow descriptors (nd=0) - not shadow-capable");
				continue;
			}
			if (slot >= 127) {
				SKSE::log::info("[SLF] scheduler: slice budget exhausted at slot {}", slot);
				break;
			}

			// v4 GATE (same crash precondition as the sun): Accumulate +
			// the engine's per-light Render both need descriptor[0] fully set
			// up (shaderAccumulator non-null = accumulate safe; camera
			// non-null = Render can rebuild the view-proj). Check BEFORE any
			// engine state is written (GameSetShadowCasterSlot etc.) so a
			// not-yet-ready light is left completely untouched - the engine
			// slots it again on a later frame once its shadow state exists.
			// Vanilla reached accumulate only after its own readiness checks
			// (vtable16 cast-shadow test + culling walk at 0x14CC71F+), which
			// our distance-only scheduler bypasses.
			{
				// ENGINE-NATIVE PER-LIGHT MOUNT (vtable slot 0x10 = disp 0x80 =
				// UpdateCamera, CLib BSShadowLight.h:129). Vanilla
				// CalculateActiveShadowCasters called this for every candidate
				// at SkyrimSE+14CC728 BEFORE slotting: the override
				// (BSShadowFrustumLight 0x151AC50 / directional 0x1512230)
				// lazily creates + mounts each descriptor's shaderAccumulator
				// (descr+0x48; mount sites 0x151ADDB point-light / 0x151474B
				// sun). Replacing 107137 without re-issuing this call left every
				// point-light descriptor with a null accumulator forever
				// (log: accNull=1 constant, camNull=0 - camera is mounted
				// elsewhere, only the accumulator lives here) so the readiness
				// gate below deferred forever. Restore the engine call; returns
				// false = light can't shadow this frame (vanilla skips it).
				// v6.4 (user: "应该让所有灯常亮"): UpdateCamera is no longer a
				// MEMBERSHIP veto. Roster = every active light up to
				// MAX_SCHEDULED_SHADOW_LIGHTS (22 active << 30 budget, so all
				// of them must stay lit every frame); membership churn was the
				// strobe source (16/22 slotted, rejected lights re-entering
				// frames later -> glow flicker while walking). The call is
				// still issued for its side effect (lazily mounts descriptor
				// shaderAccumulator); a false return means the engine would
				// not shadow it THIS frame (frustum/state), but we let the
				// readiness gate below be the only hard veto - it guards
				// engine state we would actually corrupt (null accumulator
				// + Accumulate = CTD). Lights the gate defers are retried
				// every frame (roster is stable, nothing distance-shuffles
				// them out).
				if (!light->UpdateCamera(camera)) {
					static std::uint32_t s_skipCam = 0;
					if ((s_skipCam++ & 0xFFu) == 0)
						SKSE::log::info("[SLF] scheduler: UpdateCamera false (light=0x{:x} dyn={}) - keep in roster, gate decides",
							reinterpret_cast<uintptr_t>(light), light->dynamic ? 1 : 0);
				}
				// v4 GATE must run BEFORE GameEnableLight: enable allocates
				// the per-light culling process the accumulate walk appends
				// casters to and must only run for lights that actually get
				// slotted this frame. v6.2-diag called it for every
				// UpdateCamera-passing candidate (~20 lights incl. ones the
				// gate then deferred) - allocating/releasing engine culling
				// state for non-slotted lights every frame churned engine
				// light handling while walking (glow flicker). v6.3/6.4:
				// gate first, enable only the survivors (roster keeps all
				// active lights; the gate is now the sole membership veto).
				const auto pd = GetDescriptorReadiness(light);
				const std::uint32_t smc = static_cast<std::uint32_t>(light->shadowMapCount);
				if (pd.accNull != 0 || pd.camNull != 0 || pd.total == 0 || smc != pd.total) {
					static std::uint32_t s_skipReady = 0;
					if ((s_skipReady++ & 0xFFu) == 0)
						SKSE::log::info("[SLF] scheduler: skipped light not engine-ready (total={} smc={} accNull={} camNull={}) - defer to later frame",
							pd.total, smc, pd.accNull, pd.camNull);
					continue;
				}
				// ENGINE-NATIVE ACTIVATION (SkyrimSE+14CC7C9 / 0x14A2530 /
				// Address Library 106342). Vanilla called it right after
				// UpdateCamera, before SetShadowCasterSlot. Allocates the
				// per-light culling process the accumulate walk appends
				// casters to.
				GameEnableLight(ssn, light);
			}

			// ---- v6.1: split engine-accumulator vs self-managed ----
			// Lights inside ENGINE_ACCUM_CAPACITY use the full engine path
			// (SetShadowCasterSlot writes shadowLightsAccum[slot]; the mask
			// bits and the global accum counter mirror vanilla/CS). Lights
			// beyond it must NOT call GameSetShadowCasterSlot - the engine
			// accumulator is a fixed ~8-entry array and slot >= 8 indexes
			// OOB -> stale-pointer store crash (22:36 CTD, see constant
			// above). Their shadows are produced by OUR render-loop dispatch
			// (g_scheduledShadowLights) which never reads shadowLightsAccum.
			const bool inEngineAccum = slot < ENGINE_ACCUM_CAPACITY;
			if (inEngineAccum) {
				GameSetShadowCasterSlot(ssn, light, slot, 1);
				// Engine's active-shadow-caster bitmask (vanilla global; the
				// engine's shadow pipeline keys off these bits). Bits beyond
				// the accumulator's capacity are meaningless to the engine
				// (it never set them) and could make engine loops probe
				// shadowLightsAccum[8+] - keep the mask inside the range we
				// actually populate.
				*GetShadowMask() |= 1u << slot;
			}
			// Shadow-map slice into kSHADOWMAPS. The vanilla scheduler writes
			// the descriptor's shadowmapIndex; ours must too (the engine's
			// depth-target state picks the slice from this field). v6: write
			// EVERY descriptor to the same slot - parabolic/omni share one
			// paraboloid map across descriptors (see the nd block above).
			// Self-managed lights (slot >= 8) still need this: it is what
			// routes their Render into the correct kSHADOWMAPS slice.
			for (auto& d : lrtd.shadowmapDescriptors)
				d.shadowmapIndex = slot;

			// ENGINE-NATIVE caster accumulation (BSShadowLight vtable 09).
			// Vanilla's CalculateActiveShadowCasters ran this cull walk per
			// slotted light right after slotting - it appends the visible
			// shadow casters to the light so its shadow-map Render has
			// geometry to draw. We fully replaced that function and skipped
			// this -> every slice rendered empty (RL 0%, verified 14:03).
			// Arg shape = the engine's own SE/AE call (CS passes slot for both
			// the global count and the channel on this same virtual; culling
			// scene null = engine default). Game thread, like vanilla.
			{
				std::uint32_t accumSlot = slot;
				light->Accumulate(accumSlot, slot, nullptr);
				// v6.1: the global accum counter tracks engine accumulator
				// usage only (vanilla/CS semantics, reset each frame by the
				// engine before this hook). Self-managed lights must not
				// inflate it - engine code that iterates up to this count
				// would walk past shadowLightsAccum's real entries.
				if (inEngineAccum)
					*GetAccumLightSlot() += light->shadowMapCount;
			}

			// P1c-2 persist: the render-loop dispatch hook (P1_hooks.cpp)
			// renders exactly this list, in this order.
			auto& entry = ShadowLimitFixNS::P1::g_scheduledShadowLights[slotted];
			entry.light = light;
			entry.slot = slot;
			slotted++;
			slot++;
			// v6.3: record this frame's accepted list (slot order) so the
			// next frame's coherence pass can keep these lights in place.
			s_prevSlotted.push_back(light);
		}

		const size_t n = slotted;
		// P1c-2: publish this frame's list to the dispatch hook. (Counter was
		// already advanced per light by shadowMapCount above.)
		ShadowLimitFixNS::P1::g_scheduledShadowCount.store(
			static_cast<std::uint32_t>(n), std::memory_order_release);
		// Post-light pass (2026-09-06): the engine-scheduled prefix length is
		// the exclude boundary for the full-screen pass (engine already lit
		// these - extended lights = scheduled[engineCount..count)).
		ShadowLimitFixNS::P1::g_engineLightCount.store(
			static_cast<std::uint32_t>(n), std::memory_order_release);

		// ---- N-light data channel (P1c-3 final step, consumed by the
		// replaced material shaders: t103 depth-array SRV + this cbuffer).
		// Pure read from the engine's per-light ShadowmapDescriptor - zero
		// behavior change to rendering. CS reads descriptors[0] directly
		// (ShadowCasterManager.cpp:449) - renderTarget=kNONE is normal, the
		// shadowmapIndex is the live field (we write it in the slot loop).
		// lightTransform is NOT written by the engine (stays 0) - the real
		// view-proj is rebuilt from the descriptor's NiCamera
		// (worldToCam + viewFrustum, both engine-computed).
		{
			std::uint32_t count = 0;
			for (size_t i = 0; i < n; i++) {
				auto* light = ShadowLimitFixNS::P1::g_scheduledShadowLights[i].light;
				if (!light)
					continue;
				auto& rtd = light->GetRuntimeData();
				const auto& descs = rtd.shadowmapDescriptors;
				if (descs.empty())
					continue;
				const auto& d0 = descs[0];
				auto& ld = g_shadowLights[count];
				ld.pos[0] = light->light->world.translate.x;
				ld.pos[1] = light->light->world.translate.y;
				ld.pos[2] = light->light->world.translate.z;
				ld.radius = light->light->GetLightRuntimeData().radius.x;
				ld.shadowMapIndex = static_cast<float>(d0.shadowmapIndex);
				ld.flags = 0.0f;
				// Rebuild light view-proj from the shadow camera:
				// lightViewProj = worldToCam * proj(frustum), row-major
				// (v' = v * view * proj), matching the engine's row-vector
				// convention. flags=1 means the matrix is valid.
				if (d0.camera) {
					const float* view = &d0.camera->GetRuntimeData().worldToCam[0][0];
					const auto& fr = d0.camera->GetRuntimeData2().viewFrustum;
					const float l = fr.fLeft, r = fr.fRight, t = fr.fTop, b = fr.fBottom;
					const float n = fr.fNear, f = fr.fFar;
					float proj[16]{};
					if (fr.bOrtho) {
						proj[0] = 2.0f / (r - l);
						proj[5] = 2.0f / (t - b);
						proj[10] = 1.0f / (f - n);
						proj[12] = -(r + l) / (r - l);
						proj[13] = -(t + b) / (t - b);
						proj[14] = -n / (f - n);
						proj[15] = 1.0f;
					} else {
						proj[0] = 2.0f * n / (r - l);
						proj[5] = 2.0f * n / (t - b);
						proj[8] = (r + l) / (r - l);
						proj[9] = (t + b) / (t - b);
						proj[10] = f / (f - n);
						proj[11] = 1.0f;
						proj[14] = -n * f / (f - n);
					}
					// view (row-major float[4][4]) x proj
					for (int r2 = 0; r2 < 4; r2++)
						for (int c = 0; c < 4; c++) {
							float s = 0.0f;
							for (int k = 0; k < 4; k++)
								s += view[r2 * 4 + k] * proj[k * 4 + c];
							ld.proj[r2 * 4 + c] = s;
						}
					ld.flags = 1.0f;
				} else {
					for (int k = 0; k < 16; k++)
						ld.proj[k] = 0.0f;
				}
				count++;
			}
			g_shadowLightCount.store(count, std::memory_order_release);

			// Data-channel health (every 256th frame): how many lights our
			// PS actually receives, and how many have valid matrices (flags=1).
			// count==8 + all flags==1 = the shader gets 8 usable shadows.
			static uint32_t dcFrame = 0;
			if ((dcFrame++ & 0xFFu) == 0) {
				uint32_t valid = 0;
				for (uint32_t k = 0; k < count; k++) {
					if (ShadowLimitFixNS::P1::g_shadowLights[k].flags > 0.5f)
						valid++;
				}
				SKSE::log::info("[SLF] SLF data channel: {} lights, {} valid matrices",
					count, valid);
			}
		}

		// Log every 64th frame.
		static uint32_t frame = 0;
		if ((frame++ & 0x3Fu) == 0) {
			SKSE::log::info("[SLF] P1c-1b scheduler: {} lights, slotted {} (first dist {:.1f})",
				lights.size(), n, std::sqrt(distSq.empty() ? 0.0f : distSq[0]));

			// P1c-3 data-channel diagnostics: descriptor[0].shadowmapIndex is
			// the live field (written by our slot loop above). The view-proj
			// is rebuilt from the shadow camera (worldToCam + frustum).
			// sceneAccum/geom sizes are read AFTER the engine-native
			// Accumulate above - >0 proves the cull walk appended casters
			// (the RL=0% bug was exactly an empty caster list).
			// v6.5-diag: print ALL slotted lights (was first 4). User report:
			// "不是所有的灯都亮" - need to tell lit vs unlit lights apart by
			// their engine state. Engine UpdateCamera override 0x151AC50 has
			// TWO skip paths that leave the descriptor camera frustum at the
			// default unit box (l=-1 r=1 t=1 b=-1 n=0.1 f=1 o=1):
			//   gate A: 0x151AC80 test [BSLight+0xf4],1 -> jne 0x151AF5B
			//            (BSLight+0xf4 bit0 = engine "wants this light to
			//            shadow this frame" hint; set -> frustum block skipped)
			//   gate B: 0x151AC93 cmp [this+0x46]=dynamic,0 -> je 0x151AD59
			//            (static lights skip the camera frustum update block)
			// niF4 prints gate A's byte so we can see which lights the engine
			// itself is ignoring - those are the "not lit" ones.
			for (size_t i = 0; i < n; i++) {
				auto* light = ShadowLimitFixNS::P1::g_scheduledShadowLights[i].light;
				if (!light)
					continue;
				auto& rtd = light->GetRuntimeData();
				const auto& descs = rtd.shadowmapDescriptors;
				const std::uint32_t accumSize = static_cast<std::uint32_t>(rtd.sceneAccumArray.size());
				if (!descs.empty()) {
					const auto& d0 = descs[0];
					const bool hasCam = d0.camera != nullptr;
					const bool hasCull = d0.cullingProcess != nullptr;
					const bool hasAcc = d0.shaderAccumulator != nullptr;
					const std::uint32_t slot = ShadowLimitFixNS::P1::g_scheduledShadowLights[i].slot;
					// v6.1: engine-accumulator light vs self-managed (slice
					// only, never touches the engine accum array). Proves the
					// >8 split is live in-game.
					const char* path = slot < ENGINE_ACCUM_CAPACITY ? "ENG" : "SLF";
					// gate A byte: engine 0x151AC80 reads BSLight+0xf4 (the
					// object behind light->light, which the disasm confirmed
					// is [this+0x48]). Print raw byte, bit0 is the gate.
					std::uint32_t niF4 = 0xFFFFFFFF;
					if (light->light) {
						// NiPointer -> raw -> byte probe at +0xf4 (gate A).
						niF4 = *reinterpret_cast<const std::uint8_t*>(
							reinterpret_cast<const std::uint8_t*>(light->light.get()) + 0xf4);
					}
					if (hasCam) {
						const auto& fr = d0.camera->GetRuntimeData2().viewFrustum;
						// v6.2-diag: root-cause the all-0% readback. Lights whose
						// shadow NiCamera was never placed (static lights skip the
						// camera-config block of UpdateCamera, engine 0x151AC50
						// gate at [this+0x46]=BSLight::dynamic) keep the default
						// unit frustum AND a stale world transform -> every caster
						// is culled during Accumulate (sceneAccum=0) and every
						// Render rasterizes zero pixels. Print the flags that
						// decide that gate + the camera placement vs the light.
						const bool isDir = light->GetIsDirectionalLight();
						const bool isPara = light->GetIsParabolicLight();
						const bool isFrus = light->GetIsFrustumLight();
						const auto& lpos = light->light ? light->light->world.translate : RE::NiPoint3{};
						const auto& cpos = d0.camera->world.translate;
						// worldToCam[0][0..2]: identity-ish (1,0,0) means the
						// view matrix was never rebuilt for this light.
						const auto& w2c = d0.camera->GetRuntimeData().worldToCam;
						SKSE::log::info("[SLF]   light#{} slot={} [{}] shIdx={} cam=1 acc={} cull={} sceneAccum={} dyn={} pt={} pst={} fc={:#x} niF4={:#02x} en={} cm={:#x} sgi={} type={}{}{} frm(l={:.1f} r={:.1f} t={:.1f} b={:.1f} n={:.1f} f={:.1f} o={}) w2c0=({:.2f},{:.2f},{:.2f}) lpos=({:.1f},{:.1f},{:.1f}) cpos=({:.1f},{:.1f},{:.1f})",
							i, slot, path,
							d0.shadowmapIndex, hasAcc ? 1 : 0, hasCull ? 1 : 0, accumSize,
							light->dynamic ? 1 : 0, light->pointLight ? 1 : 0, light->portalStrict ? 1 : 0,
							static_cast<unsigned>(light->frustrumCull), niF4,
							d0.isEnabled ? 1 : 0, static_cast<unsigned>(d0.cullingMode),
							static_cast<unsigned>(rtd.sceneGraphIndex),
							isDir ? 'D' : '-', isPara ? 'P' : '-', isFrus ? 'F' : '-',
							fr.fLeft, fr.fRight, fr.fTop, fr.fBottom,
							fr.fNear, fr.fFar, fr.bOrtho ? 1 : 0,
							w2c[0][0], w2c[0][1], w2c[0][2],
							lpos.x, lpos.y, lpos.z,
							cpos.x, cpos.y, cpos.z);
						// Matrix sanity: print the rebuilt view-proj key rows
						// (diagonal + translation). Garbage/zero values here =
						// the camera or frustum was not yet set by the engine
						// at scheduler time.
						const auto& ld = ShadowLimitFixNS::P1::g_shadowLights[i];
						SKSE::log::info("[SLF]     vp[0]=({:.3f},{:.3f},{:.3f},{:.3f}) vp[5]={:.3f} vp[10]={:.3f} vp[12..14]=({:.1f},{:.1f},{:.1f})",
							ld.proj[0], ld.proj[1], ld.proj[2], ld.proj[3],
							ld.proj[5], ld.proj[10], ld.proj[12], ld.proj[13], ld.proj[14]);
					} else {
						SKSE::log::info("[SLF]   light#{} slot={} [{}] shadowmapIndex={} cam=0 sceneAccum={} (no camera yet)",
							i, slot, path,
							d0.shadowmapIndex, accumSize);
					}
				} else {
					SKSE::log::info("[SLF]   light#{} slot={} no descriptors sceneAccum={}",
						i, ShadowLimitFixNS::P1::g_scheduledShadowLights[i].slot, accumSize);
				}
			}
		}
	}

	// =====================================================================
	// v10-phase1c (2026-09-03): CS-style AppendVirtual caster-collection
	// chain - minimal port of an upstream shadow-classifier reference.
	//
	// Why it exists: engine scheduler func() accumulates the SUN only
	// (disasm: the single vtable09 call inside uid107137 sits at 0x14CC5E5,
	// args = global counter, 0, null). Its per-light segment just Enables +
	// slots. The cull walk that collects each NON-sun light's casters runs
	// at render-dispatch time in vanilla (ShadowSceneNode::AccumulateLight
	// uid 99753/106401, inside the dispatch fn we now skip via ctx.Rax=0),
	// so once we own the dispatch nobody collects casters -> geomList stays
	// empty -> manual Render has nothing to rasterize (the RL=0% bug).
	//
	// CS solves collection by hooking BSCullingProcess::AppendVirtual
	// (vtable slot 0x18 on BOTH culling-process vtables) and attaching the
	// culled geometry onto the light currently being accumulated. A bare
	// Accumulate without this chain has no geometry output path (the
	// phase1b verdict). This is the minimal subset: no static/dynamic
	// split-cache, no contribution cull, no cull-pool guard - just
	// "which light is accumulating now" + heal-attach during the walk.
	// ---------------------------------------------------------------------
	// Threading: the engine's DrawWorld cull jobs walk the same vtable
	// slots from worker threads. CurrentCullLight() is thread-id gated so
	// a worker thread never mistakes itself for our accumulate owner (CS
	// ShadowCasterClassifier.cpp:31-57, same shape).
	// =====================================================================

	static std::atomic<RE::BSShadowLight*> s_currentCullLight{ nullptr };
	static std::atomic<std::uint32_t> s_cullThreadId{ 0 };

	static void SetCurrentCullLight(RE::BSShadowLight* a_light)
	{
		// Thread id first on arm, cleared first on disarm: a concurrent job
		// thread never sees itself as the accumulate owner.
		if (a_light) {
			s_cullThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
			s_currentCullLight.store(a_light, std::memory_order_relaxed);
		} else {
			s_cullThreadId.store(0, std::memory_order_relaxed);
			s_currentCullLight.store(nullptr, std::memory_order_relaxed);
		}
	}

	static RE::BSShadowLight* CurrentCullLight()
	{
		if (s_cullThreadId.load(std::memory_order_relaxed) != GetCurrentThreadId())
			return nullptr;
		return s_currentCullLight.load(std::memory_order_relaxed);
	}

	// Armed while accumulating a light whose geomList is empty (fresh light
	// or post-clear frame): the append hook then re-attaches casters via
	// the engine's own AttachGeometry. A light created after scene attach
	// never gets geometry from the engine's once-per-geometry latch, so
	// without the heal it casts nothing forever (CS comment, :59-62).
	static std::atomic<bool> s_accumRebuildAttach{ false };

	// Dedupes the re-attach: dual-paraboloid walks append the same geometry
	// once per half and the engine pair-insert has no dedupe of its own.
	// Accumulate-thread only (guarded by CurrentCullLight above).
	static std::unordered_set<const RE::BSGeometry*> s_healAttached;

	// Engine wrappers (CS ShadowEngineHooks.cpp ids, SE/AE order).
	static void SLFGameAttachGeometry(RE::BSLight* a_light, RE::BSGeometry* a_geom)
	{
		using F = void (*)(RE::BSLight*, RE::BSGeometry*);
		static REL::Relocation<F> func{ REL::RelocationID(101296, 108283) };
		func(a_light, a_geom);
	}

	static bool SLFGameLightIsInRange(RE::BSLight* a_light, const RE::NiBound* a_bound, RE::NiLight* a_niLight, float a_scale)
	{
		using F = bool (*)(RE::BSLight*, const RE::NiBound*, RE::NiLight*, float);
		static REL::Relocation<F> func{ REL::RelocationID(101299, 108286) };
		return func(a_light, a_bound, a_niLight, a_scale);
	}

	// Append-side heal, shared by both hooks: mid-accumulate on a light that
	// needs its geomList rebuilt -> attach this caster (in-range only).
	static void SLFHealAttachCaster(RE::BSGeometry& a_visible)
	{
		RE::BSShadowLight* light = CurrentCullLight();
		if (!light || !s_accumRebuildAttach.load(std::memory_order_relaxed))
			return;
		if (!s_healAttached.insert(&a_visible).second)
			return;  // already attached this walk
		auto* ni = light->light.get();
		if (!ni)
			return;
		if (!SLFGameLightIsInRange(static_cast<RE::BSLight*>(light), &a_visible.worldBound, ni, 1.0f))
			return;
		SLFGameAttachGeometry(static_cast<RE::BSLight*>(light), &a_visible);
	}

	// BSCullingProcess::AppendVirtual hook - parabolic culling vtable
	// (point/omni lights' walk). Always passes through: SLF v1 adds the heal
	// only, never filters, so vanilla culling semantics are untouched.
	struct Hook_ParabolicCullAppend
	{
		static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
		{
			SLFHealAttachCaster(a_visible);
			func(a_this, a_visible, a_alphaGroupIndex);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Same for the base culling vtable (frustum/spot lights' walk + the
	// engine's own room/scene culls - those read CurrentCullLight()==null
	// and fall straight through unchanged).
	struct Hook_BaseCullAppend
	{
		static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
		{
			SLFHealAttachCaster(a_visible);
			func(a_this, a_visible, a_alphaGroupIndex);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void InstallCasterCullHook()
	{
		SKSE::log::info("[SLF] v10-phase1c installing AppendVirtual caster hooks (parabolic + base, slot 0x18)");
		stl::write_vfunc<0x18, Hook_ParabolicCullAppend>(RE::VTABLE_BSParabolicCullingProcess[0]);
		stl::write_vfunc<0x18, Hook_BaseCullAppend>(RE::VTABLE_BSCullingProcess[0]);
		SKSE::log::info("[SLF] v10-phase1c AppendVirtual hooks installed");
	}

	struct Hook_CalculateActiveShadowCasters
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// v7-obs (2026-09-03): isolation experiment. Since v6.1 our thunk has
	// fully REPLACED the engine's CalculateActiveShadowCasters and never
	// calls the original - but the original is what installs the engine
	// state the shadow render pass consumes: it accumulates the sun (prologue
	// 0x14CC5E5, cullingScene=null there too), walks ssn's light list
	// ([rdi+0x158]), per-light UpdateCamera->init->Enable->SetSlot->mask->
	// bounding->Accumulate (vtable09 @ 0x14CC9CE), registers lights into the
	// global render array (0x1414cec70 when [light+0x138]!=0) and writes the
	// tail state (ssn+0x248 + global shadow-light counter @0x14CCA54/0x68).
	// Skipping ALL of that left the engine's render pass with an empty list
	// (t103 0%, sun included, user: no shadows outdoors either). So:
	//   1) call the original first - engine-native shadows (sun + engine
	//      budget) come back untouched;
	//   2) our SLF extension scheduler (v6.4 roster) is parked for this
	//      run - one variable at a time. It returns as a post-original
	//      pass over lights the engine did NOT slot (t103 high slices +
	//      self-managed dispatch) once the engine path is confirmed alive.
	// ---------------------------------------------------------------------
	// v10-phase1 register pass: after the engine's own scheduler (func())
	// returns, ssn->shadowLightsAccum[0..n] holds the engine-selected
	// shadow lights (slot 0 = sun when active, point lights follow). Only
	// the SUN is accumulated inside func() (0x14CC5E5 - the single vtable09
	// call in uid107137); non-directional lights are Enabled+slotted only,
	// and their cull walk runs in phase1c below via the CS AppendVirtual
	// chain (this file, RegisterEngineAccumLights). We publish
	// that exact list to g_scheduledShadowLights so the render-loop hook
	// (rax=0 skip + SLF_MANUAL_RENDER) drives each light's
	// BSShadowLight::Render itself.
	//
	// v10 rationale (v8-exp2 verdict): with the count-patch 8->127 live,
	// letting the ENGINE dispatch (SLF_SKIP_VANILLA_DISPATCH=0) crashes
	// (crash-2026-09-03-12-04-19: null vtable call [r8+0x50] inside the
	// engine dispatch loop - the engine's fixed 8-slot state machine cannot
	// address the 127-slice array). CS survives by OWNING the dispatch
	// (rax=0 + its own Render loop). Phase 1 replicates exactly that split:
	// engine schedules (func), WE render (self-dispatch over the same light
	// list the engine would have dispatched). Output must equal v7-vanilla
	// (sun + engine-budget lights) while proving the extended-array +
	// self-dispatch combo is stable; the full v6.4 roster scheduler
	// (ScheduleShadowCasters above) returns in phase 2 as a post-original
	// pass over lights the engine did NOT slot (SLF path, slots 8..29).
#if SLF_ALWAYS_LIT
	// fix34: force the engine's light-LOD fade-end cache to "never fade" so
	// lamps stay lit at any distance (no walk-up pop-in). The engine
	// recomputes this cache every frame (Sky::UpdateLightLODFadeDistances),
	// so we re-apply every frame in the scheduler thunk (CS's own pattern:
	// "a per-frame writer must re-apply every frame to stay live").
	static void ForceLightsAlwaysLit()
	{
		static REL::Relocation<float*> p{ REL::RelocationID(527669, 414583) };
		if (p.get() && *p < 1.0e8f)
			*p = 1.0e8f;  // fade-end squared -> ~10k units, effectively never fades
	}
	// fix38 (2026-09-07): DIM probe VERDICT - lodDimmer is a binary 0/1
	// switch (probe: 0.000->1.000 at dist ~330/455). The engine zeros it in
	// BSShadowLight::UpdateCamera's LOD sub-test (CS ShadowScheduler.cpp:1379
	// comment). The fade-cache override (fix34) is useless because
	// UpdateCamera ran inside func() BEFORE we wrote the cache. Fix: after
	// func() re-set every active lamp's lodDimmer to 1 every frame (CS
	// restores per-lamp in its own loops - we do it for ALL active lamps).
	// If the engine re-zeros later in the frame (shadow render path) we will
	// need a second restore point; this first build proves the mechanism.
	static void ForceLampDimmersOneImpl()
	{
		auto* ssn = GetShadowSceneNode();
		if (!ssn)
			return;
		for (auto& sp : ssn->GetRuntimeData().activeShadowLights) {
			if (sp && sp->lodDimmer < 1.0f)
				sp->lodDimmer = 1.0f;
		}
		for (auto& sp : ssn->GetRuntimeData().activeLights) {
			if (sp && sp->lodDimmer < 1.0f)
				sp->lodDimmer = 1.0f;
		}
	}
	void ForceLampDimmersOne()
	{
		ForceLampDimmersOneImpl();
	}
#endif

#if SLF_DIMMER_PROBE
	// fix37: track each active shadow lamp's lodDimmer (BSLight @0x14, the
	// engine's distance-based dimming) vs player distance; log a JUMP when a
	// lamp's lodDimmer changes > 0.15 between samples - a step (not a smooth
	// fade) is the "lamp pops when approaching" source. Throttled by caller.
	static void DimmerJumpProbe()
	{
		auto* ssn = GetShadowSceneNode();
		auto* cam = GetWorldCamera();
		if (!ssn || !cam)
			return;
		struct LP
		{
			RE::BSLight* l;
			float lod;
			float dist;
		};
		static std::vector<LP> s_prev;
		std::vector<LP> cur;
		auto& aS = ssn->GetRuntimeData().activeShadowLights;
		const auto& c = cam->world.translate;
		for (auto& sp : aS) {
			if (!sp)
				continue;
			auto* l = sp.get();  // BSShadowLight : BSLight (has lodDimmer)
			const auto& p = l->worldTranslate;
			const float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
			cur.push_back({ l, l->lodDimmer, std::sqrt(dx * dx + dy * dy + dz * dz) });
		}
		for (auto& cc : cur) {
			for (auto& pp : s_prev) {
				if (pp.l == cc.l && std::fabs(cc.lod - pp.lod) > 0.15f) {
					SKSE::log::info("[SLF][DIM] lamp=0x{:x} lod {:.3f}->{:.3f} at dist {:.1f}->{:.1f}",
						reinterpret_cast<std::uintptr_t>(cc.l),
						pp.lod, cc.lod, pp.dist, cc.dist);
				}
			}
		}
		s_prev = std::move(cur);
	}
#endif

#if SLF_PIN_FIXED_LIGHTS > 0
	// fix29 (2026-09-07): pin a FIXED set of shadow lights. User target:
	// exactly 4 normal shadow lights (stable, no churn) + unlimited diffuse
	// for everything else. Right after func() filled shadowLightsAccum, we
	// rebuild it to sun + up to N engine-ready point lights with frame-to-
	// frame INHERITANCE: a pinned light stays pinned as long as the engine
	// still has it ready this frame (it re-mounts descriptors for the picks
	// inside func()); only lights the engine truly dropped are replaced by
	// new engine picks. Walking inside one room therefore never swaps the 4
	// -> no flicker; a real scene/cell change swaps them (acceptable).
	// Rebuild (clear+push) so no engine walk hits a null hole. Game thread.
	static void ShadowPinFixedLights()
	{
		auto* ssn = GetShadowSceneNode();
		if (!ssn)
			return;
		auto& accum = ssn->GetRuntimeData().shadowLightsAccum;
		RE::BSShadowLight* sun = nullptr;
		std::vector<RE::BSShadowLight*> cand;  // engine-ready point lights
		for (auto* l : accum) {
			if (!l)
				continue;
			if (l->GetIsDirectionalLight())
				sun = l;
			else
				cand.push_back(l);
		}
		static std::vector<RE::BSShadowLight*> s_pinned;
		std::vector<RE::BSShadowLight*> pinned;
		// 1) inherit last frame's pinned lights the engine still has ready.
		for (auto* p : s_pinned) {
			for (auto* c : cand) {
				if (c == p) {
					pinned.push_back(p);
					break;
				}
			}
			if (pinned.size() >= static_cast<size_t>(SLF_PIN_FIXED_LIGHTS))
				break;
		}
		// 2) top up from the engine's own picks (its order = importance).
		for (auto* c : cand) {
			bool dup = false;
			for (auto* q : pinned) {
				if (q == c) {
					dup = true;
					break;
				}
			}
			if (!dup) {
				pinned.push_back(c);
				if (pinned.size() >= static_cast<size_t>(SLF_PIN_FIXED_LIGHTS))
					break;
			}
		}
		// 3) rebuild the accumulator: sun + pinned only.
		const std::uint32_t before = static_cast<std::uint32_t>(accum.size());
		accum.clear();
		if (sun)
			accum.push_back(sun);
		for (auto* p : pinned)
			accum.push_back(p);
		s_pinned = std::move(pinned);
		static std::uint32_t s_log = 0;
		if ((s_log++ & 0x3Fu) == 0) {
			char line[256];
			int off = 0;
			off += std::snprintf(line + off, sizeof(line) - off, "pinned={} (sun={})",
				static_cast<std::uint32_t>(accum.size()), sun ? 1 : 0);
			for (auto* p : accum) {
				if (!p || p == sun)
					continue;
				off += std::snprintf(line + off, sizeof(line) - off, " [0x%llx@(%.0f,%.0f,%.0f)]",
					reinterpret_cast<unsigned long long>(p),
					p->light->world.translate.x, p->light->world.translate.y,
					p->light->world.translate.z);
			}
			SKSE::log::info("[SLF][PIN] {} (accum {} -> {})", line, before,
				static_cast<std::uint32_t>(accum.size()));
		}
	}
#endif

	// fix31 (2026-09-07): ACTIVATION-LAYER probe. User's lamps are now all
	// non-casting (engine shadow layer irrelevant), yet lamps still flicker
	// on/off while moving. The residual sources must be the engine's light
	// ACTIVATION management (portal / light enable state -> a lamp leaving
	// activeLights is engine-disabled -> it stops being lit by everything,
	// engine AND our post pass, because our data source is engine-managed)
	// or the per-surface diffuse budget. Track membership churn of
	// ShadowSceneNode.activeLights / activeShadowLights frame to frame and
	// log which lamps enter/leave, so we can SEE a flickering lamp being
	// deactivated/reactivated by the engine (the data decides whether the
	// fix is an independent light source independent of engine activation).
	static void ActiveLightProbe()
	{
		auto* ssn = GetShadowSceneNode();
		if (!ssn)
			return;
		struct Entry
		{
			uintptr_t addr;
			float x, y, z;
		};
		static std::vector<Entry> s_prevA, s_prevS;
		auto& aL = ssn->GetRuntimeData().activeLights;
		auto& aS = ssn->GetRuntimeData().activeShadowLights;
		std::vector<Entry> curA, curS;
		for (auto& sp : aL) {
			if (!sp)
				continue;
			curA.push_back({ reinterpret_cast<uintptr_t>(sp.get()),
				sp->worldTranslate.x, sp->worldTranslate.y, sp->worldTranslate.z });
		}
		for (auto& sp : aS) {
			if (!sp)
				continue;
			curS.push_back({ reinterpret_cast<uintptr_t>(sp.get()),
				sp->worldTranslate.x, sp->worldTranslate.y, sp->worldTranslate.z });
		}
		auto churn = [](const std::vector<Entry>& prev, const std::vector<Entry>& cur) {
			std::uint32_t in = 0, out = 0;
			std::string det;
			for (auto& c : cur) {
				bool seen = false;
				for (auto& p : prev) {
					if (p.addr == c.addr) {
						seen = true;
						break;
					}
				}
				if (!seen) {
					in++;
					if (in <= 3) {
						char b[96];
						const auto a64 = static_cast<std::uint64_t>(c.addr);
						std::snprintf(b, sizeof(b), " +0x%llx@(%.0f,%.0f,%.0f)",
							a64, static_cast<double>(c.x), static_cast<double>(c.y),
							static_cast<double>(c.z));
						det += b;
					}
				}
			}
			for (auto& p : prev) {
				bool still = false;
				for (auto& c : cur) {
					if (c.addr == p.addr) {
						still = true;
						break;
					}
				}
				if (!still) {
					out++;
					if (out <= 3) {
						char b[96];
						const auto a64 = static_cast<std::uint64_t>(p.addr);
						std::snprintf(b, sizeof(b), " -0x%llx@(%.0f,%.0f,%.0f)",
							a64, static_cast<double>(p.x), static_cast<double>(p.y),
							static_cast<double>(p.z));
						det += b;
					}
				}
			}
			return std::make_pair(in + out, det);
		};
		SKSE::log::info("[SLF][ACT] activeLights={} shadow={}",
			static_cast<std::uint32_t>(curA.size()), static_cast<std::uint32_t>(curS.size()));
		{
			auto [tot, det] = churn(s_prevA, curA);
			if (tot > 0)
				SKSE::log::info("[SLF][ACT]   lights churn={}{}", tot, det);
		}
		{
			auto [tot, det] = churn(s_prevS, curS);
			if (tot > 0)
				SKSE::log::info("[SLF][ACT]   shadow churn={}{}", tot, det);
		}
		s_prevA = std::move(curA);
		s_prevS = std::move(curS);
	}

	static void RegisterEngineAccumLights()
	{
		auto* ssn = GetShadowSceneNode();
		if (!ssn) {
			ShadowLimitFixNS::P1::g_scheduledShadowCount.store(0, std::memory_order_release);
			return;
		}

		auto& accum = ssn->GetRuntimeData().shadowLightsAccum;
		std::uint32_t n = 0;
		// fix27 (2026-09-07): accumulator-churn probe - the engine rewrites
		// shadowLightsAccum (its per-frame pick, the ONLY pool the engine's
		// cb2 shadow-lamp selection reads) inside func(). The user's residual
		// shadow flicker = these picks swapping while walking. Before we
		// freeze/rewrite the array, log who is in it across frames (identity
		// hash + world pos + engine order) every 128th call so the swap
		// pattern (which lamps trade places, how often) is DATA, not guess.
		static std::array<RE::BSShadowLight*, 16> s_prevAccum{};
		static std::uint32_t s_prevAccumN = 0;
		static std::uint32_t s_accDiag = 0;
		static std::uint32_t s_churnTotal = 0;
		// fix69: sampling 128->32 frames - a sub-128-frame churn cycle (the
		// 'lamp pops at fixed angle' signature) was invisible at 128.
		if (((s_accDiag++) & 0x1Fu) == 0) {
			std::uint32_t accN = 0;
			char accLine[512];
			int accOff = 0;
			std::uint32_t churn = 0;
			for (auto* l : accum) {
				if (!l || accN >= 16)
					break;
				accOff += std::snprintf(accLine + accOff, sizeof(accLine) - accOff,
					"[%u:0x%llx@(%.0f,%.0f,%.0f)%c]",
					accN, reinterpret_cast<unsigned long long>(l),
					l->light->world.translate.x, l->light->world.translate.y,
					l->light->world.translate.z,
					l->GetIsDirectionalLight() ? 'd' : (l->GetIsParabolicLight() ? 'p' : '?'));
				bool seenPrev = false;
				for (std::uint32_t k = 0; k < s_prevAccumN; k++) {
					if (s_prevAccum[k] == l) {
						seenPrev = true;
						break;
					}
				}
				if (!seenPrev)
					churn++;
				s_prevAccum[accN] = l;
				accN++;
			}
			s_prevAccumN = accN;
			if (accN > 0) {
				s_churnTotal += churn;
				SKSE::log::info("[SLF][ACC] n={} churnNow={} churnTot={} {}", accN, churn, s_churnTotal, accLine);
			}
		}
		// The engine scheduler (func) writes the SAME sun pointer into
		// shadowLightsAccum twice ([0]==[1], 13:40 phase1b log). Publishing
		// the duplicate would double-render; accumulating it would be the
		// same-frame double-accumulate that fail-fast crashed phase1b.
		std::unordered_set<RE::BSShadowLight*> seen;
		for (auto* light : accum) {
			if (!light)
				continue;
			if (!seen.insert(light).second)
				continue;  // duplicate entry (engine sun double-write)
			// Render needs the descriptor camera and the target slice comes
			// from descriptor[0].shadowmapIndex (engine-written during
			// slotting), so skip lights whose descriptors are not mounted yet.
			auto& rtd = light->GetRuntimeData();
			auto& descs = rtd.shadowmapDescriptors;
			if (descs.empty()) {
				// [SLF][R1] probe (2026-09-06): the engine mounts shadowmap
				// descriptors for only ~4 of ~10 accumulated lights (budget
				// cap); the rest are silently rejected here. Log each
				// rejected light's identity + world pos every 256th frame so
				// we can judge whether raising the engine budget would
				// actually light more of the scene (near lights rejected =
				// visible win; only far/tiny lights rejected = no win).
				static std::uint32_t s_skipLog = 0;
				if ((s_skipLog++ & 0xFFu) == 0) {
					const char t = light->GetIsDirectionalLight() ? 'd' :
						(light->GetIsParabolicLight() ? 'p' :
						(light->GetIsFrustumLight() ? 'f' : '?'));
					SKSE::log::info("[SLF][R1] skip: light=0x{:x} type={} dyn={} descs={} smc={} pos=({:.0f},{:.0f},{:.0f})",
						reinterpret_cast<uintptr_t>(light), t, light->dynamic ? 1 : 0,
						static_cast<std::uint32_t>(descs.size()),
						static_cast<std::uint32_t>(light->shadowMapCount),
						light->light->world.translate.x, light->light->world.translate.y,
						light->light->world.translate.z);
				}
				continue;
			}
			const auto& d0 = descs[0];

			// ---- caster collection: v10-phase1c (CS AppendVirtual chain) ----
			// func() accumulated the SUN itself, so it is published but never
			// re-accumulated here (its geometry is already collected by the
			// engine's own walk inside func()).
			//
			// Non-directional engine lights were only Enabled+slotted by
			// func(); in vanilla their cull walk runs inside the render
			// dispatch (ShadowSceneNode::AccumulateLight uid 99753/106401)
			// that we skip via ctx.Rax=0. phase1b proved a bare Accumulate
			// collects nothing without the AppendVirtual hook chain (CS
			// ShadowCasterClassifier.cpp:383/437 + GameAttachGeometry), so we
			// run that walk here, CS-style, with the current-cull-light armed:
			// the hooks heal-attach every visible caster onto the light's
			// geomList - exactly what our manual Render rasterizes.
			const bool isDir = light->GetIsDirectionalLight();
			if (!isDir) {
				const auto pd = GetDescriptorReadiness(light);
				const std::uint32_t smc = static_cast<std::uint32_t>(light->shadowMapCount);
				const std::uint32_t idx = d0.shadowmapIndex;
				if (pd.accNull == 0 && pd.camNull == 0 && pd.total > 0 &&
					smc == pd.total && idx != 0xFFFFFFFFu) {
					// fix19a obs: engine func() already ran its UpdateCamera
					// for accumulator lights (vanilla CalculateActiveShadowCasters
					// call site 0x14CC728). Was the frustum actually PLACED, or
					// is it still the unit box despite pointers being mounted?
					// [CN] at render time says dyn=1p 88/88 DEF=1; this capture
					// answers whether that predates Accumulate (engine vfunc
					// never fills parabolic frustums) or happens later.
					static std::uint32_t s_regCamLog = 0;
					if ((s_regCamLog++ & 0xFFu) == 0)
						SKSE::log::info("[SLF] reg pre-acc: light=0x{:x} type={} dyn={} camDflt={} idx0={} smc={} geom={} sceneAccum={}",
							reinterpret_cast<uintptr_t>(light),
							light->GetIsDirectionalLight() ? 'd' :
								(light->GetIsParabolicLight() ? 'p' :
								(light->GetIsFrustumLight() ? 'f' : '?')),
							light->dynamic ? 1 : 0, CamFrustumDefault(light), idx, smc,
							static_cast<std::uint32_t>(light->geomList.size()),
							static_cast<std::uint32_t>(light->GetRuntimeData().sceneAccumArray.size()));
					SetCurrentCullLight(light);
					struct ClearCullLight
					{
						~ClearCullLight() { SetCurrentCullLight(nullptr); }
					} clearGuard;
					s_healAttached.clear();
					// Heal-attach only when the list is empty (fresh/cleared
					// light); a populated list is the engine's own state.
					s_accumRebuildAttach.store(light->geomList.empty(), std::memory_order_relaxed);
					{
						// Throwaway count ref: the engine registers the light
						// into shadowLightsAccum through this ref. func() already
						// owns those slots, so never pass the real global counter.
						std::uint32_t localSlot = idx;
						light->Accumulate(localSlot, 0, nullptr);
					}
					s_accumRebuildAttach.store(false, std::memory_order_relaxed);

					static std::uint32_t s_accLog = 0;
					if ((s_accLog++ & 0x3Fu) == 0)
						SKSE::log::info("[SLF] v10-phase1c accumulate: light=0x{:x} idx={} sceneAccum={} geom={}",
							reinterpret_cast<uintptr_t>(light), idx,
							static_cast<std::uint32_t>(rtd.sceneAccumArray.size()),
							static_cast<std::uint32_t>(light->geomList.size()));
				} else {
					// Descriptor readiness gate (v4 AV guard): the engine mounts
					// shaderAccumulator over the first frames after load/cell
					// change; Accumulate AVs on a null accumulator.
					static std::uint32_t s_defLog = 0;
					if ((s_defLog++ & 0xFFu) == 0)
						SKSE::log::info("[SLF] v10-phase1c defer accumulate light=0x{:x} (accNull={} camNull={} total={} smc={} idx={:#x})",
							reinterpret_cast<uintptr_t>(light), pd.accNull, pd.camNull, pd.total, smc, idx);
				}
			}

			// [SLF][R1] published-side portrait (throttled): position of
			// each light the engine DID slot, to contrast with the rejected
			// ones above (near=kept vs near=rejected answers the budget-raise
			// ROI question).
			{
				static std::uint32_t s_pubLog = 0;
				if ((s_pubLog++ & 0x1FFu) == 0) {
					SKSE::log::info("[SLF][R1] pub: light=0x{:x} slot={} pos=({:.0f},{:.0f},{:.0f})",
						reinterpret_cast<uintptr_t>(light), n,
						light->light->world.translate.x, light->light->world.translate.y,
						light->light->world.translate.z);
				}
			}

			auto& e = ShadowLimitFixNS::P1::g_scheduledShadowLights[n];
			e.light = light;
			e.slot = n;
			n++;
			if (n >= ShadowLimitFixNS::P1::g_scheduledShadowLights.size())
				break;
		}
		ShadowLimitFixNS::P1::g_scheduledShadowCount.store(n, std::memory_order_release);
		// Post-light pass (2026-09-06): THIS function appends the engine's own
		// accumulator lights (scheduled[0..n-1]) - they are the prefix that
		// ExtendScheduledLights() builds on and that the engine already lit
		// via cb2, so the full-screen consumer pass must EXCLUDE them. The
		// old store for g_engineLightCount sat in the dead ScheduleShadowCasters()
		// (never called since v10) -> it stayed 0 -> the pass re-lit the engine
		// lights on top of the engine's own lighting. Store the real prefix
		// length here, at the single place engine lights are published.
		ShadowLimitFixNS::P1::g_engineLightCount.store(n, std::memory_order_release);

		// ---- v10-phase3 caster rotation probe (2026-09-06) ----
		// The vanilla shadow flicker = the engine RE-SELECTING its per-frame
		// caster set (shadowLightsAccum, this exact list) as the player moves;
		// churn of the set/order = shadow pop. This block is a pure observer:
		// it snapshots the caster POINTERS the engine scheduler produced this
		// frame (accum order, sun-deduped - identity is unambiguous, no
		// coordinate space involved) and reports a rolling 2 s window:
		//   churnF = frames where the set or order changed
		//   add/rem = lights entering/leaving the set over the window
		//   ordF   = frames where the ORDER changed but the set did not
		// A high churnF% while the player idles = vanilla rotation bug body;
		// near-0 while idle but high while walking = motion-triggered set
		// churn (candidate for a stable-sort fix).
		{
			using Clock = std::chrono::steady_clock;
			static std::array<uintptr_t, 128> s_prev{};
			static std::uint32_t s_prevN = 0;
			static bool s_hasPrev = false;
			static auto s_winStart = Clock::now();
			static std::uint32_t s_winFrames = 0, s_winChurn = 0, s_winOrd = 0;
			static std::uint32_t s_winAdd = 0, s_winRem = 0;

			const std::uint32_t cn = n;  // engine-only caster count (pre-Extend)
			if (s_hasPrev && cn == s_prevN) {
				// Same cardinality: set + order comparison.
				bool sameSet = true, sameOrder = true;
				for (std::uint32_t i = 0; i < cn; i++) {
					const uintptr_t p = reinterpret_cast<uintptr_t>(
						ShadowLimitFixNS::P1::g_scheduledShadowLights[i].light);
					bool found = false;
					for (std::uint32_t j = 0; j < s_prevN; j++) {
						if (s_prev[j] == p) {
							found = true;
							if (i != j)
								sameOrder = false;
							break;
						}
					}
					if (!found) {
						sameSet = false;
						s_winAdd++;
					}
				}
				for (std::uint32_t j = 0; j < s_prevN; j++) {
					bool found = false;
					for (std::uint32_t i = 0; i < cn; i++) {
						if (s_prev[j] == reinterpret_cast<uintptr_t>(
								ShadowLimitFixNS::P1::g_scheduledShadowLights[i].light)) {
							found = true;
							break;
						}
					}
					if (!found)
						s_winRem++;
				}
				if (!sameSet) {
					s_winChurn++;
				} else if (!sameOrder) {
					s_winChurn++;
					s_winOrd++;
				}
			} else if (s_hasPrev) {
				// Cardinality changed: count the symmetric difference.
				s_winChurn++;
				for (std::uint32_t j = 0; j < s_prevN; j++) {
					bool found = false;
					for (std::uint32_t i = 0; i < cn; i++) {
						if (s_prev[j] == reinterpret_cast<uintptr_t>(
								ShadowLimitFixNS::P1::g_scheduledShadowLights[i].light)) {
							found = true;
							break;
						}
					}
					if (!found)
						s_winRem++;
				}
				for (std::uint32_t i = 0; i < cn; i++) {
					bool found = false;
					const uintptr_t p = reinterpret_cast<uintptr_t>(
						ShadowLimitFixNS::P1::g_scheduledShadowLights[i].light);
					for (std::uint32_t j = 0; j < s_prevN; j++) {
						if (s_prev[j] == p) {
							found = true;
							break;
						}
					}
					if (!found)
						s_winAdd++;
				}
			}
			// Roll the snapshot.
			for (std::uint32_t i = 0; i < cn && i < 128; i++)
				s_prev[i] = reinterpret_cast<uintptr_t>(
					ShadowLimitFixNS::P1::g_scheduledShadowLights[i].light);
			s_prevN = cn;
			s_hasPrev = true;
			s_winFrames++;
			// Flush every 2 s.
			const double winS = std::chrono::duration<double>(Clock::now() - s_winStart).count();
			if (winS >= 2.0) {
				SKSE::log::info("[SLF][RT] rot: win={:.1f}s fr={} churnF={} ({:.0f}%) add={} rem={} ordF={} curN={}",
					winS, s_winFrames, s_winChurn,
					s_winFrames ? 100.0 * double(s_winChurn) / double(s_winFrames) : 0.0,
					s_winAdd, s_winRem, s_winOrd, cn);
				// [SLF][MOT] camera-motion ground truth (2026-09-06): the
				// 12:xx session "I did not move at all" contradicted 100 % RT
				// churn right after the QA window. Root cause (user, 09-06
				// 15:10): Skyrim AUTO-ROTATES the camera while idle - the
				// camera TRANSLATE never changes (moved ~0) but the VIEW
				// swings, so the engine re-picks its caster set every frame
				// (churn 100 %) and the scheduler's nearest-light cycles
				// A/B/C/D (the room's 4 walls). This probe therefore reports
				// BOTH translate motion and ROTATION:
				//   moved/speed = camera displacement (u, u/s; -1 = first)
				//   rot = rotation-matrix delta norm (unitless; ~0 still,
				//         >>0.02 turning - idling auto-rotate swings this
				//         several orders above stillness)
				//   yaw = Euler Z delta in deg (rough heading readout;
				//         rot is the authoritative turn detector)
				// Same 2 s cadence as the churn window for line-by-line
				// alignment of motion vs churn vs QA flicker buckets.
				{
					static RE::NiPoint3 s_motPrev{};
					static bool s_motHas = false;
					static double s_motPrevSec = 0.0;
					static float s_prevRot[9] = {};
					static float s_prevYawZ = 0.f;
					auto* motCam = GetWorldCamera();
					if (motCam) {
						const RE::NiPoint3& mp = motCam->world.translate;
						const auto& mr = motCam->world.rotate;
						const double sec = std::chrono::duration<double>(
							Clock::now().time_since_epoch()).count();
						float moved = -1.f;
						float speed = -1.f;
						float rotDelta = -1.f;
						float yawDeg = 0.f;
						if (s_motHas) {
							const float dx = mp.x - s_motPrev.x;
							const float dy = mp.y - s_motPrev.y;
							const float dz = mp.z - s_motPrev.z;
							moved = std::sqrt(dx * dx + dy * dy + dz * dz);
							const double dt = sec - s_motPrevSec;
							if (dt > 0.001)
								speed = float(moved / dt);
							// rotation matrix delta norm (entry[i][j] diff)
							float acc = 0.f;
							for (int r = 0; r < 3; r++)
								for (int c = 0; c < 3; c++) {
									const float d = mr.entry[r][c] - s_prevRot[r * 3 + c];
									acc += d * d;
								}
							rotDelta = std::sqrt(acc);
							// yaw via ToEulerAnglesXYZ: a_z = atan2(m[0][1],
							// m[0][0]) = local-X horizontal heading (valid
							// while the camera stays level - the idle
							// auto-rotate case). NiMatrix3 stores the
							// previous snapshot below.
							RE::NiPoint3 e1{};
							if (mr.ToEulerAnglesXYZ(e1)) {
								const float yawNow = e1.z * 57.29578f;
								float dyaw = yawNow - s_prevYawZ;
								while (dyaw > 180.f)
									dyaw -= 360.f;
								while (dyaw < -180.f)
									dyaw += 360.f;
								yawDeg = dyaw;
								s_prevYawZ = yawNow;
							}
						}
						s_motPrev = mp;
						s_motPrevSec = sec;
						for (int r = 0; r < 3; r++)
							for (int c = 0; c < 3; c++)
								s_prevRot[r * 3 + c] = mr.entry[r][c];
						s_motHas = true;
						SKSE::log::info("[SLF][MOT] cam=({:.0f},{:.0f},{:.0f}) moved={:.1f}u speed={:.1f}u/s rot={:.3f} yaw={:.1f}",
							mp.x, mp.y, mp.z, moved, speed, rotDelta, yawDeg);
					}
				}
				s_winStart = Clock::now();
				s_winFrames = s_winChurn = s_winOrd = s_winAdd = s_winRem = 0;
			}
		}

		static std::uint32_t s_log = 0;
		if ((s_log++ & 0x3Fu) == 0) {
			SKSE::log::info("[SLF] v10 register: {} engine-accumulated lights published for self-dispatch (accum size={})",
				n, static_cast<std::uint32_t>(accum.size()));
			for (std::uint32_t i = 0; i < n && i < 8; i++) {
				auto& e = ShadowLimitFixNS::P1::g_scheduledShadowLights[i];
				auto& rtd = e.light->GetRuntimeData();
				SKSE::log::info("[SLF]   slot {} light=0x{:x} descs={} sceneAccum={} idx0={}",
					e.slot, reinterpret_cast<uintptr_t>(e.light),
					static_cast<std::uint32_t>(rtd.shadowmapDescriptors.size()),
					static_cast<std::uint32_t>(rtd.sceneAccumArray.size()),
					rtd.shadowmapDescriptors.empty() ? -1 : static_cast<int32_t>(rtd.shadowmapDescriptors[0].shadowmapIndex));
			}
		}
	}

	// ---------------------------------------------------------------------
	// v10-phase2 extension pass (gate: SLF_P2_EXTEND). Compile-gated at the
	// CALL SITE only - the function body stays compiled every build so
	// syntax/type errors surface now, not when the gate flips.
	//
	// After func() + RegisterEngineAccumLights(), g_scheduled[0..N-1] holds
	// the engine-scheduled lights (their descriptors carry engine-written
	// shadowmapIndex 0..7). This pass appends the active lights the engine
	// did NOT slot, giving them REAL shadow maps at slices 8..29 so the
	// all-lights goal (user decision 2026-09-03) moves past the engine's
	// ~8-light budget.
	//
	// Pure SLF path (mirrors the v6.4 self-managed roster, now running
	// AFTER the engine instead of replacing it - the engine just rebuilt
	// descriptors inside func(), which is exactly the state v6 lacked and
	// the reason v6's sceneAccum stayed 0):
	//   UpdateCamera (side effect: lazily mounts descriptor
	//                 shaderAccumulator, same as engine 0x14CC728)
	//   -> readiness gate (accNull/camNull/smc mismatch defer - the v4
	//      crash guard: Accumulate AVs on a null accumulator)
	//   -> GameEnableLight (allocates the per-light culling process the
	//      accumulate walk appends casters to)
	//   -> descriptor shadowmapIndex = slot (routes Render into the slice)
	//   -> light->Accumulate (engine cull walk -> sceneAccum geometry the
	//      manual Render rasterizes)
	//   -> append {light, slot} to g_scheduledShadowLights
	// NEVER calls GameSetShadowCasterSlot / writes mask / bumps the global
	// accum counter for these lights: slot >= 8 indexes shadowLightsAccum
	// OOB (fixed ~8-entry array, v6.1 22:36 CTD). Slices 0..7 belong to the
	// engine path; extension starts at 8 and only grows -> zero overlap.
	static void ExtendScheduledLights()
	{
		const std::uint32_t base = ShadowLimitFixNS::P1::g_scheduledShadowCount.load(std::memory_order_acquire);
		if (base >= MAX_SCHEDULED_SHADOW_LIGHTS)
			return;  // engine already filled the budget

		auto* ssn = GetShadowSceneNode();
		auto* camera = GetWorldCamera();
		if (!ssn || !camera)
			return;

		auto& lights = ssn->GetRuntimeData().activeShadowLights;
		if (lights.empty())
			return;

		// Lights the engine already handles (func product). Extension skips
		// them - accumulating a second time would duplicate sceneAccum
		// geometry (Render would draw casters twice).
		auto& engineAccum = ssn->GetRuntimeData().shadowLightsAccum;

		const RE::NiPoint3 camPos = camera->world.translate;

		// Collect candidates: active lights absent from the engine accum
		// list, distance-sorted (insertion sort, small N).
		std::vector<RE::BSShadowLight*> cand;
		std::vector<float> distSq;
		cand.reserve(lights.size());
		distSq.reserve(lights.size());
		for (auto& sp : lights) {
			if (!sp || !sp->light)
				continue;
			bool engineHandled = false;
			for (auto* ea : engineAccum) {
				if (ea == sp.get()) {
					engineHandled = true;
					break;
				}
			}
			if (engineHandled)
				continue;
			const RE::NiPoint3 pos = sp->light->world.translate;
			const float dx = pos.x - camPos.x;
			const float dy = pos.y - camPos.y;
			const float dz = pos.z - camPos.z;
			cand.push_back(sp.get());
			distSq.push_back(dx * dx + dy * dy + dz * dz);
		}
		for (size_t i = 1; i < cand.size(); i++) {
			size_t j = i;
			while (j > 0 && distSq[j] < distSq[j - 1]) {
				std::swap(cand[j], cand[j - 1]);
				std::swap(distSq[j], distSq[j - 1]);
				j--;
			}
		}

		std::uint32_t slot = ENGINE_ACCUM_CAPACITY;  // 8: engine path owns 0..7
		std::uint32_t written = 0;
		for (auto* light : cand) {
			if (base + written >= MAX_SCHEDULED_SHADOW_LIGHTS)
				break;
			auto& lrtd = light->GetRuntimeData();
			const std::uint32_t nd = static_cast<std::uint32_t>(lrtd.shadowmapDescriptors.size());
			if (nd == 0)
				continue;

			// Mount shaderAccumulator (engine does this inside its own
			// slotting via UpdateCamera; the engine never ran it for these
			// lights because it did not slot them).
			// fix19a (02:1x): the return value was previously DISCARDED and
			// the readiness gate only checks pointer nullness (accNull/
			// camNull) -- so a light whose UpdateCamera refused or left the
			// default unit frustum still passes straight into Accumulate and
			// then renders 0 px. Capture return + frustum state right after
			// the call: uc=0 = engine vfunc rejected this light; uc=1 with
			// camDflt=1 = vfunc "succeeded" but never filled the frustum
			// (parabolic static-skip inside the engine impl). Both point to
			// "we must place the shadow camera ourselves" in the next fix.
			const bool ucOk = light->UpdateCamera(camera);
			{
				static std::uint32_t s_ucLog = 0;
				if ((s_ucLog++ & 0x3Fu) == 0)
					SKSE::log::info("[SLF] p2 uc: light=0x{:x} uc={} type={} dyn={} nd={} camDflt={} idx0={} sceneAccum={} geom={}",
						reinterpret_cast<uintptr_t>(light), ucOk ? 1 : 0,
						light->GetIsDirectionalLight() ? 'd' :
							(light->GetIsParabolicLight() ? 'p' :
							(light->GetIsFrustumLight() ? 'f' : '?')),
						light->dynamic ? 1 : 0,
						static_cast<std::uint32_t>(light->GetRuntimeData().shadowmapDescriptors.size()),
						CamFrustumDefault(light),
						light->GetRuntimeData().shadowmapDescriptors.empty() ? -1 :
							static_cast<int32_t>(light->GetRuntimeData().shadowmapDescriptors[0].shadowmapIndex),
						static_cast<std::uint32_t>(light->GetRuntimeData().sceneAccumArray.size()),
						static_cast<std::uint32_t>(light->geomList.size()));
			}

			const auto pd = GetDescriptorReadiness(light);
			const std::uint32_t smc = static_cast<std::uint32_t>(light->shadowMapCount);
			if (pd.accNull != 0 || pd.camNull != 0 || pd.total == 0 || smc != pd.total) {
				static std::uint32_t s_p2Def = 0;
				if ((s_p2Def++ & 0xFFu) == 0)
					SKSE::log::info("[SLF] p2 extend: defer light=0x{:x} (total={} smc={} accNull={} camNull={})",
						reinterpret_cast<uintptr_t>(light), pd.total, smc, pd.accNull, pd.camNull);
				continue;
			}

			GameEnableLight(ssn, light);
			// No GameSetShadowCasterSlot / mask / counter for slot >= 8.
			for (auto& d : lrtd.shadowmapDescriptors)
				d.shadowmapIndex = slot;
			// v10-phase2 fix (2026-09-03 17:2x, before first P2_EXTEND=1 build):
			// the accumulate MUST run with the current-cull-light ARMED, same
			// as the phase1c register pass (Scheduler.cpp:919-935). Without
			// SetCurrentCullLight + s_accumRebuildAttach the AppendVirtual
			// hooks (Hook_Parabolic/BaseCullAppend) see no owning light and
			// drop every caster -> geomList stays empty -> Render rasterizes
			// 0 px (the phase1b no-op, exactly). A fresh SLF light has an
			// empty geomList, so rebuild-attach arms and heals every visible
			// caster on the first walk (CS :938-940).
			SetCurrentCullLight(light);
			struct ClearCullLight
			{
				~ClearCullLight() { SetCurrentCullLight(nullptr); }
			} clearGuard;
			s_healAttached.clear();
			s_accumRebuildAttach.store(light->geomList.empty(), std::memory_order_relaxed);
			{
				// Throwaway count ref (same rule as phase1c): slots 8..29 are
				// NEVER registered into the engine accumulator (v6.1 OOB
				// lesson), so pass a local copy - never the real counter.
				std::uint32_t localSlot = slot;
				light->Accumulate(localSlot, 0, nullptr);
			}
			s_accumRebuildAttach.store(false, std::memory_order_relaxed);

			auto& e = ShadowLimitFixNS::P1::g_scheduledShadowLights[base + written];
			e.light = light;
			e.slot = slot;
			written++;
			slot++;
		}

		if (written > 0)
			ShadowLimitFixNS::P1::g_scheduledShadowCount.store(base + written, std::memory_order_release);

		static std::uint32_t s_p2Log = 0;
		if (written > 0 && (s_p2Log++ & 0x3Fu) == 0) {
			SKSE::log::info("[SLF] p2 extend: +{} SLF lights (total scheduled now {})", written, base + written);
			for (std::uint32_t i = 0; i < written; i++) {
				auto& e = ShadowLimitFixNS::P1::g_scheduledShadowLights[base + i];
				auto& rtd = e.light->GetRuntimeData();
				SKSE::log::info("[SLF]   slot {} light=0x{:x} descs={} sceneAccum={} geom={}",
					e.slot, reinterpret_cast<uintptr_t>(e.light),
					static_cast<std::uint32_t>(rtd.shadowmapDescriptors.size()),
					static_cast<std::uint32_t>(rtd.sceneAccumArray.size()),
					static_cast<std::uint32_t>(e.light->geomList.size()));
			}
		}
	}

	// ---------------------------------------------------------------------
	// fix15 (2026-09-04 00:4x): N-light data channel on the LIVE path.
	//
	// The v6.4 roster scheduler (ScheduleShadowCasters, which contained the
	// ONLY g_shadowLights fill + g_shadowLightCount store) is DEAD CODE since
	// the v10 re-architecture (engine func() + RegisterEngineAccumLights +
	// ExtendScheduledLights) - it has zero call sites. Consequence (fix14-diag
	// log, 3970x "lightCount=0 scheduled=24"): g_shadowLightCount stayed 0
	// forever -> the fix12 gate SLFShouldSwap() (= >4) never opened -> 0 swaps
	// -> vanilla 4-channel t14 mask -> flicker. This function re-publishes the
	// data channel (g_shadowLights + g_shadowLightCount) from the LIVE
	// scheduled list after phase2, exactly as ScheduleShadowCasters used to.
	// Must run AFTER RegisterEngineAccumLights + ExtendScheduledLights and
	// BEFORE the material pass (RenderScheduledShadowLightsDispatch consumes
	// the same list; BeginTechnique reads g_shadowLightCount per draw).
	static void PublishShadowLightDataChannel()
	{
		const std::uint32_t n = ShadowLimitFixNS::P1::g_scheduledShadowCount.load(std::memory_order_acquire);
		if (n == 0)
			return;
		std::uint32_t count = 0;
		for (std::uint32_t i = 0; i < n; i++) {
			auto* light = ShadowLimitFixNS::P1::g_scheduledShadowLights[i].light;
			if (!light)
				continue;
			auto& rtd = light->GetRuntimeData();
			const auto& descs = rtd.shadowmapDescriptors;
			if (descs.empty())
				continue;
			const auto& d0 = descs[0];
			auto& ld = ShadowLimitFixNS::P1::g_shadowLights[count];
			ld.pos[0] = light->light->world.translate.x;
			ld.pos[1] = light->light->world.translate.y;
			ld.pos[2] = light->light->world.translate.z;
			ld.radius = light->light->GetLightRuntimeData().radius.x;
			ld.shadowMapIndex = static_cast<float>(d0.shadowmapIndex);
			ld.flags = 0.0f;
			// CS ShadowLightParam.x semantics (LightLimitFix.hlsli:469+):
			//   0 = frustum (sun CSM - not in this list, directional skipped)
			//   1 = hemi   (single paraboloid fills the slice - cone/spot)
			//   2 = omni   (dual paraboloid, front/back halves in ONE slice)
			// The engine renders every non-sun shadowing light as a
			// paraboloid; the descriptor count (2 halves vs 1) tells the
			// slice layout. flags=1 below still means "matrix valid".
			ld.lightType = (descs.size() >= 2) ? 2.0f : 1.0f;
			// Rebuild the light transform from the shadow camera (engine
			// fills worldToCam + viewFrustum during UpdateCamera; lightTransform
			// itself is never written by the engine).
			if (d0.camera) {
				const float* view = &d0.camera->GetRuntimeData().worldToCam[0][0];
				if (ld.lightType > 0.5f) {
					// hemi/omni (paraboloid): consumer only needs the
					// world -> light-space AFFINE transform (rotation +
					// translation, w stays 1) so |light-space pos| ==
					// distance to light. CS GetOmnidirectionalShadow does
					// mul(ShadowProj, pos) then uses length() as the depth
					// to compare against the paraboloid-rendered slice, so
					// ShadowProj must preserve distance - NO perspective.
					// The paraboloid projection itself lives in the
					// render/sample math, not in this matrix.
					for (int k = 0; k < 16; k++)
						ld.proj[k] = view[k];
					ld.proj[3] = 0.0f;
					ld.proj[7] = 0.0f;
					ld.proj[11] = 0.0f;
					ld.proj[15] = 1.0f;
				} else {
					// frustum (sun): world -> clip, consumer divides by w.
					const auto& fr = d0.camera->GetRuntimeData2().viewFrustum;
					const float l = fr.fLeft, r = fr.fRight, t = fr.fTop, b = fr.fBottom;
					const float fn = fr.fNear, ff = fr.fFar;
					float proj[16]{};
					if (fr.bOrtho) {
						proj[0] = 2.0f / (r - l);
						proj[5] = 2.0f / (t - b);
						proj[10] = 1.0f / (ff - fn);
						proj[12] = -(r + l) / (r - l);
						proj[13] = -(t + b) / (t - b);
						proj[14] = -fn / (ff - fn);
						proj[15] = 1.0f;
					} else {
						proj[0] = 2.0f * fn / (r - l);
						proj[5] = 2.0f * fn / (t - b);
						proj[8] = (r + l) / (r - l);
						proj[9] = (t + b) / (t - b);
						proj[10] = ff / (ff - fn);
						proj[11] = 1.0f;
						proj[14] = -fn * ff / (ff - fn);
					}
					for (int r2 = 0; r2 < 4; r2++)
						for (int c = 0; c < 4; c++) {
							float s = 0.0f;
							for (int k = 0; k < 4; k++)
								s += view[r2 * 4 + k] * proj[k * 4 + c];
							ld.proj[r2 * 4 + c] = s;
						}
				}
				ld.flags = 1.0f;
			} else {
				for (int k = 0; k < 16; k++)
					ld.proj[k] = 0.0f;
			}
			count++;
		}
		ShadowLimitFixNS::P1::g_shadowLightCount.store(count, std::memory_order_release);

		// Data-channel health (every 64th frame, fix15: 256->64 so the value
		// is visible in short test runs): lights the PS receives + valid mats.
		static uint32_t dcFrame = 0;
		if ((dcFrame++ & 0x3Fu) == 0) {
			uint32_t valid = 0;
			for (uint32_t k = 0; k < count; k++) {
				if (ShadowLimitFixNS::P1::g_shadowLights[k].flags > 0.5f)
					valid++;
			}
			SKSE::log::info("[SLF] SLF data channel (live): {} lights scheduled, {} published, {} valid matrices",
				n, count, valid);
		}
	}


	// fix41 (2026-09-08): true while the world is loading/switching (fast
	// travel, cell transition load, main menu). During this window the engine
	// rebuilds shadow state and SLF must not write engine shadow data.
	// LoadingMenu covers fast travel / loads; a huge per-frame camera jump
	// (checked at low rate) catches quick-travel teleports without the menu.
	// fix42 (2026-09-08): gate with a cooldown tail. fix41 only froze while
	// the load UI was open; a save/load closes LoadingMenu the instant the
	// world is placed while cells/NPCs/shadow state are still streaming in,
	// and SLF resumed full engine-state writes the same frame -> engine
	// dispatch hit freed objects (SkyrimSE+14F3E4A call [rax+0x10], WER
	// 0xc0000005, no CrashLogger dump: 22:27/22:31/22:44 sessions, also with
	// AdvancedSkinFix disabled). Keep ALL SLF writes frozen for ~120
	// scheduler invocations (~2 s) after the load UI closes or a camera
	// jump, then resume.
	// fix44 (2026-09-08): cooldown 120 -> 360 ticks. A heavy save's cell
	// streaming continues well past LoadingMenu close; the 2 s freeze ended
	// while cells/NPCs were still spawning and SLF's first resumed writes
	// (RegisterEngineAccumLights / publish) hit half-built shadow state ->
	// clean exit, no WER/CrashLogger dump (23:28 session: gate resumed
	// 23:28:53.387, died 18 ms later). 360 ticks ~6-9 s covers the stream-in.
	static bool WorldSwitching()
	{
		enum class Gate : std::uint8_t { kNone, kOpen, kCooldown };
		static constexpr std::uint32_t kCooldownTicks = 360;
		static Gate s_gate = Gate::kNone;
		static std::uint32_t s_cooldown = 0;
		static std::uint32_t s_log = 0;

		// fix46 (2026-09-08): the moment the gate freezes, the scheduled
		// dispatch list may still hold THIS frame's lights (filled before
		// the transition was detected). The cell unload then releases those
		// engine objects while the manual dispatch (which does NOT consult
		// WorldSwitching) renders the stale list -> UAF with a clean exit
		// (00:04:14 session). Zero the list and flag the dispatch hook so
		// nothing renders a freed light during the freeze.
		const auto freeze = [] {
			ShadowLimitFixNS::P1::g_scheduledShadowCount.store(0, std::memory_order_release);
			ShadowLimitFixNS::P1::g_shadowWritesFrozen.store(true, std::memory_order_release);
		};

		auto* ui = RE::UI::GetSingleton();
		if (ui && (ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) ||
					  ui->IsMenuOpen(RE::MainMenu::MENU_NAME))) {
			// Load UI open: keep frozen (menu may reopen mid-transition).
			if (s_gate != Gate::kOpen) {
				s_gate = Gate::kOpen;
				freeze();
				if ((s_log++ & 0x1Fu) == 0)
					SKSE::log::info("[SLF] world-switch gate OPEN (load UI), shadow writes frozen");
			}
			return true;
		}
		if (s_gate == Gate::kOpen) {
			// Load UI just closed: world placed but still streaming in.
			// Do NOT resume this frame - hold a cooldown tail instead.
			s_gate = Gate::kCooldown;
			s_cooldown = kCooldownTicks;
			freeze();
			SKSE::log::info("[SLF] world-switch gate: load UI closed, shadow writes frozen {} ticks", kCooldownTicks);
			return true;
		}
		// Camera jump check (8 scheduler ticks ~ a few frames, cheap).
		static RE::NiPoint3 s_lastCam{ 0.f, 0.f, 0.f };
		static std::uint32_t s_tick = 0;
		if ((s_tick++ & 0x7u) == 0) {
			bool jumped = false;
			if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
				const auto p = pc->GetPosition();
				const float dx = p.x - s_lastCam.x;
				const float dy = p.y - s_lastCam.y;
				const float dz = p.z - s_lastCam.z;
				const float d2 = dx * dx + dy * dy + dz * dz;
				if (d2 > 2500.f * 2500.f)  // >2500 units between ticks
					jumped = true;
				s_lastCam = p;
			}
			if (jumped) {
				s_gate = Gate::kCooldown;
				s_cooldown = kCooldownTicks;
				freeze();
				SKSE::log::info("[SLF] world-switch gate: camera jump, shadow writes frozen {} ticks", kCooldownTicks);
				return true;
			}
		}
		if (s_gate == Gate::kCooldown) {
			if (s_cooldown > 0) {
				if (--s_cooldown == 0) {
					s_gate = Gate::kNone;
					SKSE::log::info("[SLF] world-switch gate: cooldown over, shadow writes resumed");
					// fix45 (2026-09-08): the FIRST post-resume dispatch
					// froze inside a single shadow pass (23:46 session:
					// cooldown over 23:46:04.405 -> 0.4s later OMSet->shadow
					// stopped advancing while draw counts raced ~60k/s for
					// 20+s; main image never produced). The scheduler
					// re-learns the engine accumulator this frame but the
					// per-light Render state (accum/geom/camera) was just
					// rebuilt by the load - park the manual dispatch for
					// ~96 ticks so the engine's own frames settle it first.
					// fix46: hand the freeze flag to the resume grace (it
					// keeps the dispatch parked while the scheduler learns).
					ShadowLimitFixNS::P1::g_shadowWritesFrozen.store(false, std::memory_order_release);
					ShadowLimitFixNS::P1::g_resumeGrace.store(96, std::memory_order_release);
				} else if ((s_log++ & 0x3Fu) == 0) {
					SKSE::log::info("[SLF] world-switch gate: cooldown {} ticks left, writes frozen", s_cooldown);
				}
				return true;
			}
		}
		return false;
	}

	void Hook_CalculateActiveShadowCasters::thunk()
	{
		// fix41 (2026-09-08): fast-travel / world-switch crash guard. During
		// a load or a huge per-frame camera jump the engine is rebuilding
		// shadow state; our post-func writes (dimmer/fade traversal, pinned
		// accumulator rebuild, extended schedules) then race the rebuild and
		// the engine's next-frame dispatch can hit empty vtable slots
		// (crash 2026-09-08-22-01-10 SkyrimSE+14CD743 call [rax+0x30]).
		// While switching we run the engine scheduler untouched and skip ALL
		// SLF engine-state writes; the normal path resumes next frame.
		if (WorldSwitching()) {
			func();
			return;
		}
		// fix75 (2026-09-10): thunk stripped to ALWAYS_LIT-only restore.
		// fix74 proved engine-native scheduling/dispatch = stable (sun
		// stays, engine-rendered) but the walk-up lamp behavior RETURNED
		// (user: "not always-lit anymore, vanilla walk-up"): the ALWAYS_LIT
		// per-frame restore (fix34 fade cache + fix38 dimmer pin) lived
		// inside this thunk, so disabling InstallScheduler killed "always
		// lit" along with the scheduler. This thunk runs the engine
		// scheduler untouched (func) then ONLY re-applies the lamp fade
		// overrides - no register/extend/publish/pin/probe writes, so the
		// engine state stays 100% native (fix74 stability preserved).
		func();
#if SLF_ALWAYS_LIT
		// fix34: lamps never fade (see macro comment).
		ForceLightsAlwaysLit();
		// fix38: engine zeroed lodDimmer in func()'s UpdateCamera LOD test -
		// restore every active lamp to full intensity (see macro comment).
		ForceLampDimmersOne();
#endif
#if SLF_DIMMER_PROBE
		// fix37: lodDimmer jump probe (8-frame throttle).
		{
			static std::uint32_t s_dp = 0;
			if ((s_dp++ & 0x7u) == 0)
				DimmerJumpProbe();
		}
#endif
		// fix76 (2026-09-10): scene-level lamp audit probe RESTORED (read-
		// only). fix75 stripped ActiveLightProbe along with the scheduler
		// post-processing; the user then added ~6 new lamps in-game and
		// asked "how many lamps can you detect?". [ACT] lines print the
		// engine ShadowSceneNode activeLights / activeShadowLights counts
		// every 32nd scheduler tick (fix69 sampling) + churn detail
		// (addr@worldpos) so newly added lamps show up as "+" entries
		// with coordinates. Pure observation - zero engine-state writes.
		{
			static std::uint32_t s_act = 0;
			if ((s_act++ & 0x1Fu) == 0)
				ActiveLightProbe();
		}
		// fix75: v10 register/extend/publish STRIPPED - they flooded the
		// engine accumulator past its <=8-slot state machine under native
		// dispatch (crash-2026-09-09-23-50-02, Sleeping Giant Inn, 21
		// active shadow lights). Engine scheduling/dispatch is 100% native;
		// extended all-lights shadows need the FULL CS-style engine-state
		// expansion (accumulator + channel map + per-surface ceilings)
		// later, not this half-measure.
		// fix75: activation/perf probes stripped with the post-processing.
	}

	void InstallScheduler()
	{
		// fix75: ALWAYS_LIT-only scheduler. InstallCasterCullHook (the CS
		// AppendVirtual caster-collection chain) NOT installed - it only
		// existed to feed RegisterEngineAccumLights/ExtendScheduledLights,
		// which are stripped from the thunk. The detour below runs the
		// engine scheduler untouched (func) then re-applies the lamp fade
		// overrides every frame (fix34/fix38), restoring "always lit"
		// under 100% native engine scheduling/dispatch.
		SKSE::log::info("[SLF] fix75 installing ALWAYS_LIT scheduler (engine func() + lamp fade restore only)...");
		stl::detour_thunk<Hook_CalculateActiveShadowCasters>(REL::RelocationID(100419, 107137));
		SKSE::log::info("[SLF] fix75 ALWAYS_LIT scheduler installed");
	}
}
