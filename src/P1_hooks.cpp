// P1_hooks.cpp - P1: engine hook installation framework
// NOT yet in CMakeLists.txt - enabled only after P0 passes in-game.
//
// ATTRIBUTION (original names preserved, see THIRD_PARTY.md):
//   Hook targets and install patterns cross-verified against Community
//   Shaders / Open Shaders (github.com/alandtse/open-shaders, GPL-3.0 WITH
//   Modding Exception) - ShadowEngineHooks.cpp / LightLimitFix reference.
//   REL-ID facts; no runtime dependency on any other mod.
//   src/Features/LightLimitFix/ShadowEngineHooks.cpp
//
// Phases:
//   P1a (Install): the two light-selection hooks as no-op stubs. Confirm in
//       log that both fire and the game stays stable. NO engine state change.
//   P1b (InstallExtendedBuffers): kSHADOWMAPS slice expansion + depth-buffer
//       redirects + render-loop hook. Real engine-state modification (8 slots).
//       Enabled only after P1a verified.
//
// P1b enable procedure (AFTER P1a verified in-game):
//   1. Set ENABLE_P1B=1 in HookUtil.h
//   2. Call InstallExtendedBuffers(8) from ShadowLimitFix.cpp Install()
//   3. Rebuild + restart game (P1b modifies engine state, restart required to revert)
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <Windows.h>
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <string>

#include "HookUtil.h"  // includes SKSE/ContextHook.h (install_context_hook)

namespace ShadowLimitFixNS::P1
{
	// ---------------------------------------------------------------------
	// P1b: render-loop call site (ID 100415/107133).
	// Non-VR: force ctx.Rax = 0 so the engine skips "call [r8+0x50]" - with
	// rax != 0 r8 gets a stale pointer and crashes (CS Ghidra-verified).
	// Also counts shadow-map renders per interval + reads the engine slot
	// counter to prove how many shadow maps are actually rendered.
	// ---------------------------------------------------------------------
	static uint32_t* GetAccumLightSlotCount()
	{
		static REL::RelocationID uid(528091, 415036);
		return reinterpret_cast<uint32_t*>(uid.address());
	}

	// Read back the kSHADOWMAPS depth array while the shadow render loop is
	// active and log how many slices contain geometry = how many shadow maps
	// are really rendered (the visible-shadow count for our SLF PS). Cheap:
	// 256x256 center block per slice, throttled to every 512th loop entry.
	static int32_t GetDepthTargetType();
	static int32_t GetDepthTargetSubIndex();
	static void RenderLoopShadowCheck()
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer)
			return;
		auto* ctx = reinterpret_cast<::ID3D11DeviceContext*>(renderer->GetRuntimeData().context);
		auto* dev = reinterpret_cast<::ID3D11Device*>(renderer->GetRuntimeData().forwarder);
		if (!ctx || !dev)
			return;
		auto& dsd = renderer->GetDepthStencilData();

		// ---- What depth target is the engine rendering to RIGHT NOW? ----
		// This decides whether our t103 (depthSRV texture) can ever see the
		// shadow geometry. Print type/sub + the OM-bound DSV's texture.
		const int gType = GetDepthTargetType();
		const int gSub = GetDepthTargetSubIndex();
		::ID3D11DepthStencilView* boundDSV = nullptr;
		ctx->OMGetRenderTargets(0, nullptr, &boundDSV);
		const bool hasBound = boundDSV != nullptr;
		::ID3D11Resource* srvTex = nullptr;
		::D3D11_TEXTURE2D_DESC srvTd{};
		if (auto* srv = dsd.depthStencils[4].depthSRV) {
			reinterpret_cast<::ID3D11ShaderResourceView*>(srv)->GetResource(&srvTex);
			if (srvTex)
				reinterpret_cast<::ID3D11Texture2D*>(srvTex)->GetDesc(&srvTd);
		}
		if (hasBound) {
			::ID3D11Resource* bres = nullptr;
			boundDSV->GetResource(&bres);
			::D3D11_TEXTURE2D_DESC btd{};
			if (bres) {
				reinterpret_cast<::ID3D11Texture2D*>(bres)->GetDesc(&btd);
				SKSE::log::info("[SLF] RL: type={} sub={} boundDSV tex={} fmt={} {}x{} slices={} (SRV tex={})",
					gType, gSub,
					reinterpret_cast<void*>(bres),
					static_cast<int>(btd.Format), btd.Width, btd.Height, btd.ArraySize,
					srvTex ? (reinterpret_cast<void*>(bres) == reinterpret_cast<void*>(srvTex) ? "SAME" : "DIFFERENT") : "?");
				bres->Release();
			}
			boundDSV->Release();
		} else {
			SKSE::log::info("[SLF] RL: type={} sub={} NO DSV bound (shadow pass not active here)", gType, gSub);
		}
		if (srvTex)
			srvTex->Release();

		// ---- Content check on the SRV texture (what t103 sees) ----
		if (!srvTex)
			return;
		::ID3D11Texture2D* tex = reinterpret_cast<::ID3D11Texture2D*>(srvTex);
		tex->AddRef();
		static ::ID3D11Texture2D* s_staging = nullptr;
		if (!s_staging) {
			::D3D11_TEXTURE2D_DESC sd = srvTd;
			sd.Width = 256;
			sd.Height = 256;
			sd.Usage = ::D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
			sd.MiscFlags = 0;
			if (FAILED(dev->CreateTexture2D(&sd, nullptr, &s_staging))) {
				tex->Release();
				return;
			}
		}
		constexpr UINT kBlock = 256;
		const UINT ox = srvTd.Width > kBlock ? (srvTd.Width - kBlock) / 2 : 0;
		const UINT oy = srvTd.Height > kBlock ? (srvTd.Height - kBlock) / 2 : 0;
		::D3D11_BOX box{};
		box.left = ox;
		box.top = oy;
		box.right = ox + kBlock;
		box.bottom = oy + kBlock;
		box.front = 0;
		box.back = 1;
		const std::uint32_t nSlices = std::min<std::uint32_t>(srvTd.ArraySize, 32u);  // v6: prove >8-slice depth (scheduler now slots up to 30)
		std::string line;
		for (std::uint32_t slice = 0; slice < nSlices; slice++) {
			ctx->CopySubresourceRegion(s_staging, slice, 0, 0, 0, tex, slice, &box);
			::D3D11_MAPPED_SUBRESOURCE ms{};
			if (FAILED(ctx->Map(s_staging, slice, ::D3D11_MAP_READ, 0, &ms)))
				continue;
			const std::uint16_t* p = static_cast<const std::uint16_t*>(ms.pData);
			std::uint32_t content = 0, samples = 0;
			for (std::uint32_t y = 0; y < kBlock; y += 8) {
				const std::uint16_t* row = p + static_cast<std::size_t>(y) * ms.RowPitch / 2;
				for (std::uint32_t x = 0; x < kBlock; x += 8) {
					const float df = static_cast<float>(row[x]) / 65535.0f;
					if (df > 0.001f && df < 0.999f)
						content++;
					samples++;
				}
			}
			ctx->Unmap(s_staging, slice);
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%s%u=%.0f%%", slice == 0 ? "" : " ", slice,
				samples > 0 ? content * 100.0f / static_cast<float>(samples) : 0.0f);
			line += buf;
		}
		SKSE::log::info("[SLF] RL shadow content (SRV tex): {}", line);
		tex->Release();
	}

	// ---------------------------------------------------------------------
	// P1b diagnosis (v3, definitive): read back the kSHADOWMAPS depth array
	// (the texture t103 samples) AT THE MATERIAL PASS - the exact moment our
	// SLF PS is about to sample it. This decides where the bug is:
	//   content > 0%  -> shadows ARE rendered into the array we bound, so
	//                     the bug is SLF PS sampling / projection.
	//   content ~ 0%  -> the engine renders shadows into a DIFFERENT
	//                     texture (or clears this one) - t103 binds the
	//                     wrong object and we must re-target it.
	// Throttled: every 128th material pass. 256x256 center block per slice.
	// ---------------------------------------------------------------------
	static void DebugReadbackShadowArraySlicesImpl(::ID3D11DeviceContext* a_ctx, const char* a_label);
	static void DebugMainDepthSanity(::ID3D11DeviceContext* a_ctx, const char* a_label);
	// SLF-B B4a: refill the LightRec SRV (t102) with this frame's real light
	// data (g_shadowLights). Defined in ShaderReplace.cpp (P1 namespace,
	// non-static - must be declared BEFORE the render-loop hook below uses
	// it, hence here at file scope rather than next to the other forward
	// declarations at line ~2619).
	void UpdateB2bLightRec(::ID3D11DeviceContext* a_ctx);
	// fix19e: manually clear an UNUSED slice (31) of the shadow array to
	// 1.0, then read it back with the same copy+map the readbacks use.
	// Verdict in ONE run:
	//   reads back 1.0 (0xFFFF) -> readback mechanism is PROVEN good, the
	//     engine's own clear(1.0)x300k+drawx68M on this texture are all
	//     no-ops -> the shadow render path itself is broken (next: why).
	//   reads back 0.0          -> the copy/map/interpretation of EVERY
	//     readback (RL, MAT, sanity) is broken -> rewrite the readback.
	static void DebugProbeClearTestSlice(::ID3D11DeviceContext* a_ctx, const char* a_label);
	// fix19f: end-to-end readback self-test that touches NO engine resource
	// and NO hooked vtable slot (UpdateSubresource/CopySubresourceRegion/
	// Map are not among our hooks). Writes known red into a 64x64 RGBA8
	// default texture, copies to staging, maps back. Variant A = whole
	// copy (no box). Variant B = 32x32 sub-box copy (the boxed style every
	// shadow/main-depth readback here uses). 
	//   A red + B red  -> copy+map proven good; every all-0.000 depth
	//     readback is a SOURCE-side issue (engine depth/typeless copy).
	//   A red + B 0    -> boxed sub-region copies fail silently (staging
	//     keeps driver-zero init) - THE root cause of every 0% so far.
	//   A 0            -> the copy+map chain itself is broken.
	static void DebugReadbackSelfTest(::ID3D11DeviceContext* a_ctx, const char* a_label);

	// ---------------------------------------------------------------------
	// fix19i: behavioural verdict (13:1x). Observation reached its end on
	// the ENGINE shadow-array texture (0x5c2d96e0, 4096x4096x127 fmt53):
	// every write is a no-op - probe-clear through the engine's OWN DSV
	// (bind=31+1, SAME texture) reads back 0x000000 and creating a SECOND
	// DSV on it fails (variant D x241) - while depthC/depthC2 prove our
	// own R24G8 textures clear/copy/read fine end-to-end. Instead of
	// observing a texture that rejects every write, reroute the shadow
	// render itself into OUR texture: replace g_normalDepthBuffer[0..63]
	// and depthStencils[4].views[0..7] with DSVs we create on a self-made
	// 2048x2048x64 R24G8 array. The engine's clear (via views[k]) and
	// draw (via SelectDSB -> g_normalDepthBuffer) then both land in OUR
	// texture; DebugReadbackSLFShadowArray reads it. Verdict:
	//   content > 0%  -> the engine texture is the broken link
	//                    (allocation/ghost); rerouting FIXES shadows
	//   content ~ 0%  -> the draws rasterize nothing (camera/frustum):
	//                    go fix the camera placement instead.
	static ::ID3D11Texture2D* s_slfTex = nullptr;
	static ::ID3D11DepthStencilView* s_slfDSV[64]{};
	static ::ID3D11ShaderResourceView* s_slfSRV = nullptr;
	static ::ID3D11Texture2D* s_slfStaging = nullptr;
	static std::uint32_t s_slfSlices = 0;

	// ---------------------------------------------------------------------
	// fix19j: READBACK METHOD SWEEP. fix19i's verdict was POISONED by a
	// readback artefact: the MAIN depth (2560x1440, in use, provably
	// non-empty every frame - the scene is on screen) reads all-0.000 with
	// the exact copy->staging->map used for every shadow readback, and the
	// rerouted SELF texture (2048x2048x64 R24G8, cleared 1.0 by two
	// different DSVs) also reads all-0 - while our tiny 64x64 R24G8
	// self-tests (depthC/depthC2) read 100% 0xFFFFFF. So "engine shadow
	// texture rejects writes" is a READBACK ARTEFACT, not a dead texture:
	// big/in-use textures read back 0, tiny fresh ones read fine.
	// Sweep staging format / box position on a fresh self 2048x2048x64 we
	// clear to 1.0 ourselves, then probe the MAIN depth + shadow array with
	// the variants, to find the combo that sees real content.
	[[maybe_unused]] static void DebugReadbackSweep(::ID3D11DeviceContext* a_ctx)
	{
		// fix19k: every 3rd sweep also runs FULL-subresource copies (heavy:
		// 16-64MB staging); every 9th sweep reads the engine shadow array
		// full-size too.
		static std::uint32_t s_sweepTick = 0;
		const bool doFull = ((s_sweepTick % 3u) == 1u);
		const bool doFullShadow = ((s_sweepTick % 9u) == 4u);
		s_sweepTick++;
		::ID3D11Device* dev = nullptr;
		a_ctx->GetDevice(&dev);
		if (!dev)
			return;
		// Box-copy w x h at (ox,oy) from tex slice -> 1-slice staging of the
		// given (same-family) format -> map. 0xA5 prefill detects a copy that
		// never lands; returns "F%/0%/nz%/raw".
		auto BoxRead = [&](::ID3D11Texture2D* tex, std::uint32_t slice, std::uint32_t ox,
							 std::uint32_t oy, std::uint32_t w, std::uint32_t h,
							 DXGI_FORMAT stgFmt) -> std::string {
			::D3D11_TEXTURE2D_DESC st{};
			st.Width = w; st.Height = h; st.MipLevels = 1; st.ArraySize = 1;
			st.Format = stgFmt; st.SampleDesc.Count = 1;
			st.Usage = ::D3D11_USAGE_STAGING; st.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
			::ID3D11Texture2D* sg = nullptr;
			if (FAILED(dev->CreateTexture2D(&st, nullptr, &sg)))
				return "stgfail";
			::D3D11_MAPPED_SUBRESOURCE wm{};
			if (SUCCEEDED(a_ctx->Map(sg, 0, ::D3D11_MAP_WRITE, 0, &wm)) && wm.pData) {
				std::memset(wm.pData, 0xA5, static_cast<std::size_t>(h) * wm.RowPitch);
				a_ctx->Unmap(sg, 0);
			}
			::D3D11_BOX bx{};
			bx.left = ox; bx.top = oy; bx.right = ox + w; bx.bottom = oy + h;
			bx.front = 0; bx.back = 1;
			a_ctx->CopySubresourceRegion(sg, 0, 0, 0, 0, tex, slice, &bx);
			::D3D11_MAPPED_SUBRESOURCE ms{};
			if (FAILED(a_ctx->Map(sg, 0, ::D3D11_MAP_READ, 0, &ms)) || !ms.pData) {
				sg->Release();
				return "mapfail";
			}
			const std::uint8_t* p = static_cast<const std::uint8_t*>(ms.pData);
			std::uint32_t n = 0, hiF = 0, hi0 = 0, nz = 0, pat = 0;
			for (std::uint32_t y = 0; y < h; y += 8) {
				for (std::uint32_t x = 0; x < w; x += 8) {
					std::uint32_t v = 0;
					std::memcpy(&v, p + static_cast<std::size_t>(y) * ms.RowPitch +
										  static_cast<std::size_t>(x) * 4, 4);
					if (v == 0xA5A5A5A5u)
						pat++;
					const std::uint32_t dH = (v >> 8) & 0xFFFFFFu;
					const std::uint32_t dL = v & 0xFFFFFFu;
					if (dH == 0xFFFFFFu || dL == 0xFFFFFFu)
						hiF++;
					else if (dH == 0u && dL == 0u)
						hi0++;
					if (dH || dL)
						nz++;
					n++;
				}
			}
			const std::uint32_t r0 = static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8) |
									 (static_cast<unsigned>(p[2]) << 16) | (static_cast<unsigned>(p[3]) << 24);
			a_ctx->Unmap(sg, 0);
			sg->Release();
			char b[128];
			std::snprintf(b, sizeof(b), "%u%%F/%u%%0/%u%%nz/r%08x%s", n ? hiF * 100u / n : 0,
				n ? hi0 * 100u / n : 0, n ? nz * 100u / n : 0, r0, pat ? " PAT!" : "");
			return b;
		};
		// fix19k: FULL-subresource copy variant. depthC/depthC2 (both read
		// back 100% FFF on 64x64) use CopySubresourceRegion(..., nullptr) -
		// whole subresource - while every BoxRead above passes an explicit
		// BOX. The one untested axis is "explicit box vs nullptr" on BIG
		// depth-format textures: stage a full-size 1-slice texture of the
		// same format family, 0xA5-prefill, copy with a nullptr box.
		auto BoxReadFull = [&](::ID3D11Texture2D* tex, std::uint32_t slice,
			DXGI_FORMAT a_stFmt = DXGI_FORMAT_UNKNOWN) -> std::string {
			::D3D11_TEXTURE2D_DESC sd{};
			tex->GetDesc(&sd);
			// fix19m+n: the staging format MUST belong to the SAME typeless
			// family as the source, or CopySubresourceRegion silently copies
			// nothing and the readback returns all-0. Format-family facts
			// (verified against dxgiformat.h): mainD desc fmt=44 =
			// R24G8_TYPELESS (D24 family); the engine shadow array desc
			// fmt=53 = R16_TYPELESS and its per-slice DSVs report fmt=55 =
			// D16_UNORM -> it is a 16-bit depth array, NOT D32 and NOT D24!
			// fix19l misread 53 as D32_FLOAT_S8X24 (which is really 20), so
			// fix19m's D32 branch never matched and the R24G8-family staging
			// kept mismatching the R16 source -> still all-0 everywhere.
			const DXGI_FORMAT sfmt = sd.Format;
			const bool sD32 = (sfmt == DXGI_FORMAT_R32G8X24_TYPELESS ||
							   sfmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
							   sfmt == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ||
							   sfmt == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT);
			const bool sR16 = (sfmt == DXGI_FORMAT_R16_TYPELESS ||
							   sfmt == DXGI_FORMAT_D16_UNORM ||
							   sfmt == DXGI_FORMAT_R16_UNORM);
			const bool sRgba = (sfmt == DXGI_FORMAT_R8G8B8A8_UNORM);
			::D3D11_TEXTURE2D_DESC st{};
			st.Width = sd.Width;
			st.Height = sd.Height;
			st.MipLevels = 1;
			st.ArraySize = 1;
			const DXGI_FORMAT selFmt = (a_stFmt != DXGI_FORMAT_UNKNOWN)
				? a_stFmt
				: (sD32 ? DXGI_FORMAT_R32G8X24_TYPELESS
						: sR16 ? DXGI_FORMAT_R16_TYPELESS
								: sRgba ? DXGI_FORMAT_R8G8B8A8_UNORM
										: DXGI_FORMAT_R24G8_TYPELESS);
			st.Format = selFmt;
			st.SampleDesc.Count = 1;
			st.SampleDesc.Quality = 0;
			st.Usage = ::D3D11_USAGE_STAGING;
			st.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
			::ID3D11Texture2D* sg = nullptr;
			if (FAILED(dev->CreateTexture2D(&st, nullptr, &sg)))
				return "stgfail";
			::D3D11_MAPPED_SUBRESOURCE wm{};
			if (SUCCEEDED(a_ctx->Map(sg, 0, ::D3D11_MAP_WRITE, 0, &wm)) && wm.pData) {
				std::memset(wm.pData, 0xA5, static_cast<std::size_t>(sd.Height) * wm.RowPitch);
				a_ctx->Unmap(sg, 0);
			}
			a_ctx->CopySubresourceRegion(sg, 0, 0, 0, 0, tex, slice, nullptr);
			::D3D11_MAPPED_SUBRESOURCE ms{};
			if (FAILED(a_ctx->Map(sg, 0, ::D3D11_MAP_READ, 0, &ms)) || !ms.pData) {
				sg->Release();
				return "mapfail";
			}
			const std::uint8_t* p = static_cast<const std::uint8_t*>(ms.pData);
			std::uint32_t n = 0, hiF = 0, hi0 = 0, nz = 0, pat = 0;
			const std::uint32_t w = sd.Width, h = sd.Height;
			// fix19m+n: D32-family staging is 8 bytes/texel (32-bit float
			// depth + 8-bit stencil + 24-bit padding); R16-family (D16) is
			// 2 bytes/texel with a 16-bit UNORM depth (1.0 = 0xFFFF); D24
			// and RGBA8 staging are 4 bytes/texel. D32 depth is a FLOAT bit
			// pattern (1.0 = 0x3F800000), NOT the D24 0xFFFFFF packing.
			const bool stD32 = (selFmt == DXGI_FORMAT_R32G8X24_TYPELESS ||
								selFmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
			const bool stR16 = (selFmt == DXGI_FORMAT_R16_TYPELESS ||
								selFmt == DXGI_FORMAT_D16_UNORM ||
								selFmt == DXGI_FORMAT_R16_UNORM);
			const std::uint32_t stride = stD32 ? 8u : (stR16 ? 2u : 4u);
			for (std::uint32_t y = 0; y < h; y += 8) {
				for (std::uint32_t x = 0; x < w; x += 8) {
					std::uint32_t v = 0;
					std::uint16_t v16 = 0;
					if (stR16) {
						std::memcpy(&v16, p + static_cast<std::size_t>(y) * ms.RowPitch +
											  static_cast<std::size_t>(x) * stride, 2);
						if (v16 == 0xA5A5u)
							pat++;
						if (v16 == 0xFFFFu)
							hiF++;
						else if (v16 == 0u)
							hi0++;
						if (v16)
							nz++;
					} else {
						std::memcpy(&v, p + static_cast<std::size_t>(y) * ms.RowPitch +
										  static_cast<std::size_t>(x) * stride, 4);
						if (v == 0xA5A5A5A5u)
							pat++;
						if (stD32) {
							if (v == 0x3F800000u)
								hiF++;
							else if (v == 0u)
								hi0++;
							if (v)
								nz++;
						} else {
							const std::uint32_t dH = (v >> 8) & 0xFFFFFFu;
							const std::uint32_t dL = v & 0xFFFFFFu;
							if (dH == 0xFFFFFFu || dL == 0xFFFFFFu)
								hiF++;
							else if (dH == 0u && dL == 0u)
								hi0++;
							if (dH || dL)
								nz++;
						}
					}
					n++;
				}
			}
			std::uint32_t r0 = 0;
			if (stR16)
				r0 = static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8);
			else
				r0 = static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8) |
					 (static_cast<unsigned>(p[2]) << 16) | (static_cast<unsigned>(p[3]) << 24);
			a_ctx->Unmap(sg, 0);
			sg->Release();
			char b[128];
			std::snprintf(b, sizeof(b), "%u%%F/%u%%0/%u%%nz/r%08x%s", n ? hiF * 100u / n : 0,
				n ? hi0 * 100u / n : 0, n ? nz * 100u / n : 0, r0, pat ? " PAT!" : "");
			return b;
		};
		// 1) Self 2048x2048x64 R24G8_TYPELESS (fix19i spec), clear slice 31
		//    to 1.0 through our own array-DSV, then read with variants.
		::D3D11_TEXTURE2D_DESC td{};
		td.Width = 2048; td.Height = 2048; td.MipLevels = 1; td.ArraySize = 64;
		td.Format = DXGI_FORMAT_R24G8_TYPELESS; td.SampleDesc.Count = 1;
		td.Usage = ::D3D11_USAGE_DEFAULT; td.BindFlags = ::D3D11_BIND_DEPTH_STENCIL;
		::ID3D11Texture2D* tex = nullptr;
		if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) {
			SKSE::log::warn("[SLF] RB-sweep: self 2048x64 tex create FAILED");
			dev->Release();
			return;
		}
		::D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
		dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		dv.ViewDimension = ::D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
		dv.Texture2DArray.FirstArraySlice = 31;
		dv.Texture2DArray.ArraySize = 1;
		::ID3D11DepthStencilView* dsv = nullptr;
		if (SUCCEEDED(dev->CreateDepthStencilView(tex, &dv, &dsv))) {
			a_ctx->ClearDepthStencilView(dsv, ::D3D11_CLEAR_DEPTH, 1.0f, 0);
			dsv->Release();
		}
		std::string line = "[SLF] RB-sweep self(2048x64@31 clear1.0): ";
		line += "v1typ=" + BoxRead(tex, 31, 0, 0, 256, 256, DXGI_FORMAT_R24G8_TYPELESS);
		line += " v2d24=" + BoxRead(tex, 31, 0, 0, 256, 256, DXGI_FORMAT_D24_UNORM_S8_UINT);
		line += " v3r24x=" + BoxRead(tex, 31, 0, 0, 256, 256, DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
		line += " v4ctr=" + BoxRead(tex, 31, 896, 896, 256, 256, DXGI_FORMAT_D24_UNORM_S8_UINT);
		line += " v5full=" + BoxRead(tex, 31, 0, 0, 2048, 2048, DXGI_FORMAT_D24_UNORM_S8_UINT);
		if (doFull)
			line += " v6fullN=" + BoxReadFull(tex, 31);
		tex->Release();
		SKSE::log::info("{}", line);
		// 2) RGBA8 control at the same 2048x2048x64 - does a BIG array
		//    texture copy back at all, or is it depth-format-specific?
		::D3D11_TEXTURE2D_DESC rtd = td;
		rtd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		rtd.BindFlags = ::D3D11_BIND_RENDER_TARGET;
		::ID3D11Texture2D* rtex = nullptr;
		if (SUCCEEDED(dev->CreateTexture2D(&rtd, nullptr, &rtex))) {
			const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
			::D3D11_RENDER_TARGET_VIEW_DESC rv{};
			rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			rv.ViewDimension = ::D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			rv.Texture2DArray.FirstArraySlice = 31;
			rv.Texture2DArray.ArraySize = 1;
			::ID3D11RenderTargetView* rtv = nullptr;
			if (SUCCEEDED(dev->CreateRenderTargetView(rtex, &rv, &rtv))) {
				a_ctx->ClearRenderTargetView(rtv, red);
				rtv->Release();
			}
			SKSE::log::info("[SLF] RB-sweep rgba(2048x64@31 clear red): {}",
				BoxRead(rtex, 31, 0, 0, 256, 256, DXGI_FORMAT_R8G8B8A8_UNORM));
			if (doFull)
				SKSE::log::info("[SLF] RB-sweep rgba fullN: {}", BoxReadFull(rtex, 31));
			rtex->Release();
		}
		// 3) Real engine textures through the same BoxRead: main-depth slot0
		//    (in use, non-empty) + shadow array slice 0/31 - whichever
		//    variant works on the self texture gets the truth out of these.
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (renderer) {
			auto& dsd = renderer->GetDepthStencilData();
			for (int slot = 0; slot < 2; slot++) {
				if (!dsd.depthStencils[slot].depthSRV)
					continue;
				::ID3D11Resource* mr = nullptr;
				reinterpret_cast<::ID3D11ShaderResourceView*>(dsd.depthStencils[slot].depthSRV)->GetResource(&mr);
				::ID3D11Texture2D* mtex = nullptr;
				if (mr && SUCCEEDED(mr->QueryInterface(__uuidof(::ID3D11Texture2D), reinterpret_cast<void**>(&mtex)))) {
					mr->Release();
					::D3D11_TEXTURE2D_DESC md{};
					mtex->GetDesc(&md);
					SKSE::log::info("[SLF] RB-sweep mainD slot{} (v2d24): {}", slot,
						BoxRead(mtex, 0, 0, 0, 256, 256, DXGI_FORMAT_D24_UNORM_S8_UINT));
					if (doFull)
						SKSE::log::info("[SLF] RB-sweep mainD slot{} fullN ({}x{} arr={} fmt={}): {}", slot,
							md.Width, md.Height, md.ArraySize, static_cast<int>(md.Format),
							BoxReadFull(mtex, 0));
					mtex->Release();
				} else if (mr) {
					mr->Release();
				}
			}
			if (auto* srv4 = dsd.depthStencils[4].depthSRV) {
				::ID3D11Resource* ar = nullptr;
				reinterpret_cast<::ID3D11ShaderResourceView*>(srv4)->GetResource(&ar);
				::ID3D11Texture2D* atex = nullptr;
				if (ar && SUCCEEDED(ar->QueryInterface(__uuidof(::ID3D11Texture2D), reinterpret_cast<void**>(&atex)))) {
					ar->Release();
					::D3D11_TEXTURE2D_DESC ad{};
					atex->GetDesc(&ad);
					SKSE::log::info("[SLF] RB-sweep shadowArr slice0 (v2d24): {} | slice31: {}",
						BoxRead(atex, 0, 0, 0, 256, 256, DXGI_FORMAT_D24_UNORM_S8_UINT),
						BoxRead(atex, 31, 0, 0, 256, 256, DXGI_FORMAT_D24_UNORM_S8_UINT));
					if (doFullShadow)
						SKSE::log::info("[SLF] RB-sweep shadowArr fullN slice0 ({}x{} arr={} fmt={}): {} | slice31: {}", ad.Width, ad.Height, ad.ArraySize,
							static_cast<int>(ad.Format), BoxReadFull(atex, 0), BoxReadFull(atex, 31));
					// fix19l: identity - is THIS (depthStencils[4].depthSRV's
					// texture) the same texture the engine's SelectDSB renders
					// shadow maps into (chosenTex)? Plus a D24-staging whole
					// copy variant (mainD fmt=44 reads fine via R24G8 staging;
					// try the concrete D24 format the other way around).
					if (doFullShadow)
						SKSE::log::info("[SLF] fix19l SRV4 tex=0x{:x} ({}x{} arr={} fmt={} bind={:#x}) d24stg0={} d24stg31={}",
							reinterpret_cast<uintptr_t>(atex), ad.Width, ad.Height, ad.ArraySize,
							static_cast<int>(ad.Format), ad.BindFlags,
							BoxReadFull(atex, 0, DXGI_FORMAT_D24_UNORM_S8_UINT),
							BoxReadFull(atex, 31, DXGI_FORMAT_D24_UNORM_S8_UINT));
					atex->Release();
				} else if (ar) {
					ar->Release();
				}
			}
		}
		// ---- fix19l observation blocks (all read-only) ----
		// (A) Decode engine DSV slots: g_normalDepthBuffer[k] views' DSV
		//     format/first-slice/underlying texture, to learn the true
		//     slot<->slice<->texture mapping of shadow rendering.
		if (doFullShadow) {
			if (auto* rA = RE::BSGraphics::Renderer::GetSingleton()) {
				auto& dA = rA->GetDepthStencilData();
				for (int k = 0; k < 8; k++) {
					auto* dsv = reinterpret_cast<::ID3D11DepthStencilView*>(g_normalDepthBuffer[k]);
					if (!dsv)
						continue;
					::D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
					dsv->GetDesc(&dd);
					::ID3D11Resource* dr = nullptr;
					dsv->GetResource(&dr);
					int fsl = -1, asz = -1;
					if (dd.ViewDimension == ::D3D11_DSV_DIMENSION_TEXTURE2DARRAY) {
						fsl = static_cast<int>(dd.Texture2DArray.FirstArraySlice);
						asz = static_cast<int>(dd.Texture2DArray.ArraySize);
					} else if (dd.ViewDimension == ::D3D11_DSV_DIMENSION_TEXTURE2D) {
						fsl = 0;
						asz = 1;
					}
					SKSE::log::info("[SLF] fix19l gNDB[{}]: dsv=0x{:x} tex=0x{:x} dim={} fsl={} asz={} fmt={}",
						k, reinterpret_cast<uintptr_t>(dsv), reinterpret_cast<uintptr_t>(dr),
						static_cast<int>(dd.ViewDimension), fsl, asz, static_cast<int>(dd.Format));
					if (dr)
						dr->Release();
				}
				for (int k = 0; k < 8; k++) {
					auto* dsv = reinterpret_cast<::ID3D11DepthStencilView*>(dA.depthStencils[4].views[k]);
					if (!dsv)
						continue;
					::D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
					dsv->GetDesc(&dd);
					::ID3D11Resource* dr = nullptr;
					dsv->GetResource(&dr);
					int fsl = -1, asz = -1;
					if (dd.ViewDimension == ::D3D11_DSV_DIMENSION_TEXTURE2DARRAY) {
						fsl = static_cast<int>(dd.Texture2DArray.FirstArraySlice);
						asz = static_cast<int>(dd.Texture2DArray.ArraySize);
					} else if (dd.ViewDimension == ::D3D11_DSV_DIMENSION_TEXTURE2D) {
						fsl = 0;
						asz = 1;
					}
					SKSE::log::info("[SLF] fix19l views4[{}]: dsv=0x{:x} tex=0x{:x} dim={} fsl={} asz={} fmt={}",
						k, reinterpret_cast<uintptr_t>(dsv), reinterpret_cast<uintptr_t>(dr),
						static_cast<int>(dd.ViewDimension), fsl, asz, static_cast<int>(dd.Format));
					if (dr)
						dr->Release();
				}
			}
		}
		// (B) Self-made 4096x4096 controls: does a 4096-depth whole-copy read
		//     back AT ALL on this driver, independent of the engine? Depth
		//     formats: concrete D24 and typeless R24G8 both cleared to 1.0.
		if (doFullShadow) {
			const auto mk4096 = [&](DXGI_FORMAT fmt, bool depth) -> std::string {
				::D3D11_TEXTURE2D_DESC bd{};
				bd.Width = 4096;
				bd.Height = 4096;
				bd.MipLevels = 1;
				bd.ArraySize = 1;
				bd.Format = fmt;
				bd.SampleDesc.Count = 1;
				bd.SampleDesc.Quality = 0;
				bd.Usage = ::D3D11_USAGE_DEFAULT;
				bd.BindFlags = depth ? ::D3D11_BIND_DEPTH_STENCIL : ::D3D11_BIND_RENDER_TARGET;
				::ID3D11Texture2D* bt = nullptr;
				if (FAILED(dev->CreateTexture2D(&bd, nullptr, &bt)))
					return "createfail";
				if (depth) {
					::D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
					// fix19n: map the typeless base to its concrete DSV
					// format per family (R24G8->D24, R16->D16).
					dv.Format = (fmt == DXGI_FORMAT_R24G8_TYPELESS) ? DXGI_FORMAT_D24_UNORM_S8_UINT
								: (fmt == DXGI_FORMAT_R16_TYPELESS) ? DXGI_FORMAT_D16_UNORM
																	: fmt;
					dv.ViewDimension = ::D3D11_DSV_DIMENSION_TEXTURE2D;
					::ID3D11DepthStencilView* bdsv = nullptr;
					if (SUCCEEDED(dev->CreateDepthStencilView(bt, &dv, &bdsv))) {
						a_ctx->ClearDepthStencilView(bdsv, ::D3D11_CLEAR_DEPTH, 1.0f, 0);
						bdsv->Release();
					}
				} else {
					const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
					::D3D11_RENDER_TARGET_VIEW_DESC rv{};
					rv.Format = fmt;
					rv.ViewDimension = ::D3D11_RTV_DIMENSION_TEXTURE2D;
					::ID3D11RenderTargetView* brv = nullptr;
					if (SUCCEEDED(dev->CreateRenderTargetView(bt, &rv, &brv))) {
						a_ctx->ClearRenderTargetView(brv, red);
						brv->Release();
					}
				}
				std::string r = BoxReadFull(bt, 0);
				bt->Release();
				return r;
			};
			SKSE::log::info("[SLF] fix19l big4096: rgba={} d24={} typ={} d16={}",
				mk4096(DXGI_FORMAT_R8G8B8A8_UNORM, false),
				mk4096(DXGI_FORMAT_D24_UNORM_S8_UINT, true),
				mk4096(DXGI_FORMAT_R24G8_TYPELESS, true),
				mk4096(DXGI_FORMAT_R16_TYPELESS, true));
		}
		// (C) Roll a whole-copy (nullptr box) scan across the shadow-array
		//     texture's slices - one slice every other sweep - to find where
		//     rendered shadow content actually lands.
		{
			static std::uint32_t s_shadowScan = 0;
			if ((s_sweepTick & 1u) == 0u) {
				if (auto* rC = RE::BSGraphics::Renderer::GetSingleton()) {
					auto& dC = rC->GetDepthStencilData();
					if (auto* s4c = dC.depthStencils[4].depthSRV) {
						::ID3D11Resource* ac = nullptr;
						reinterpret_cast<::ID3D11ShaderResourceView*>(s4c)->GetResource(&ac);
						::ID3D11Texture2D* atc = nullptr;
						if (ac && SUCCEEDED(ac->QueryInterface(__uuidof(::ID3D11Texture2D), reinterpret_cast<void**>(&atc)))) {
							ac->Release();
							::D3D11_TEXTURE2D_DESC adc{};
							atc->GetDesc(&adc);
							const std::uint32_t total = std::max(1u, adc.ArraySize);
							const std::uint32_t sl = s_shadowScan % total;
							s_shadowScan++;
							SKSE::log::info("[SLF] fix19l shadowArr scan tex=0x{:x} slice={}/{}: {}",
								reinterpret_cast<uintptr_t>(atc), sl, total, BoxReadFull(atc, sl));
							atc->Release();
						} else if (ac) {
							ac->Release();
						}
					}
				}
			}
		}
		dev->Release();
	}

	[[maybe_unused]] static bool EnsureSLFShadowTextures(::ID3D11Device* a_dev)
	{
		if (s_slfTex)
			return true;  // already rerouted (idempotent)
		::D3D11_TEXTURE2D_DESC td{};
		td.Width = 2048;
		td.Height = 2048;
		td.MipLevels = 1;
		td.ArraySize = 64;
		td.Format = DXGI_FORMAT_R24G8_TYPELESS;
		td.SampleDesc.Count = 1;
		td.SampleDesc.Quality = 0;
		td.Usage = ::D3D11_USAGE_DEFAULT;
		td.BindFlags = ::D3D11_BIND_DEPTH_STENCIL | ::D3D11_BIND_SHADER_RESOURCE;
		td.CPUAccessFlags = 0;
		td.MiscFlags = 0;
		::ID3D11Texture2D* tex = nullptr;
		if (FAILED(a_dev->CreateTexture2D(&td, nullptr, &tex))) {
			SKSE::log::error("[SLF] fix19i: self shadow tex create FAILED ({}x{}x{} fmt{})",
				td.Width, td.Height, td.ArraySize, static_cast<int>(td.Format));
			return false;
		}
		std::uint32_t made = 0;
		::D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};
		dsvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		dsvd.ViewDimension = ::D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
		dsvd.Texture2DArray.ArraySize = 1;
		for (std::uint32_t i = 0; i < 64; i++) {
			dsvd.Texture2DArray.FirstArraySlice = i;
			::ID3D11DepthStencilView* v = nullptr;
			const HRESULT vhr = a_dev->CreateDepthStencilView(tex, &dsvd, &v);
			if (SUCCEEDED(vhr) && v) {
				s_slfDSV[made++] = v;
			} else {
				SKSE::log::error("[SLF] fix19i: self DSV slice {} create FAILED hr=0x{:08X}", i, static_cast<unsigned>(vhr));
			}
		}
		::D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
		srvd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		srvd.ViewDimension = ::D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvd.Texture2DArray.FirstArraySlice = 0;
		srvd.Texture2DArray.ArraySize = 64;
		srvd.Texture2DArray.MipLevels = 1;
		::ID3D11ShaderResourceView* srv = nullptr;
		if (FAILED(a_dev->CreateShaderResourceView(tex, &srvd, &srv)) || !srv) {
			SKSE::log::error("[SLF] fix19i: self SRV create FAILED");
			srv = nullptr;
		}
		::D3D11_TEXTURE2D_DESC std2 = td;
		std2.Width = 256;
		std2.Height = 256;
		std2.MipLevels = 1;
		std2.ArraySize = std::min<std::uint32_t>(td.ArraySize, 32u);
		std2.Usage = ::D3D11_USAGE_STAGING;
		std2.BindFlags = 0;
		std2.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
		std2.MiscFlags = 0;
		::ID3D11Texture2D* stg = nullptr;
		if (FAILED(a_dev->CreateTexture2D(&std2, nullptr, &stg))) {
			SKSE::log::error("[SLF] fix19i: self staging create FAILED");
			stg = nullptr;
		}
		if (made < 8 || !stg) {
			SKSE::log::error("[SLF] fix19i: abort reroute (DSVs made={} staging={})", made, stg ? 1 : 0);
			for (std::uint32_t i = 0; i < made; i++) s_slfDSV[i]->Release();
			if (srv) srv->Release();
			tex->Release();
			return false;
		}
		// Commit: AddRef everything the engine will now hold, then swap the
		// DSV slots (g_normalDepthBuffer = draw-side SelectDSB target,
		// views[0..7] = clear-side). Keep the engine depthSRV untouched -
		// t103 still samples the engine texture; readback of OUR texture is
		// explicit below. The engine renderer is the sole COM owner of the
		// old slots; our s_slfDSV owns its own refs, engine refs are extra.
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			for (std::uint32_t i = 0; i < made; i++) s_slfDSV[i]->Release();
			if (srv) srv->Release();
			tex->Release();
			return false;
		}
		for (std::uint32_t i = 0; i < made; i++)
			s_slfDSV[i]->AddRef();  // +1 for the engine's ownership
		if (srv) srv->AddRef();
		if (stg) stg->AddRef();
		auto& dsd = renderer->GetDepthStencilData();
		for (std::uint32_t k = 0; k < made; k++) {
			g_normalDepthBuffer[k] = s_slfDSV[k];
			if (k < 8)
				dsd.depthStencils[4].views[k] = s_slfDSV[k];
		}
		s_slfTex = tex;
		s_slfSRV = srv;
		s_slfStaging = stg;
		s_slfSlices = made;
		SKSE::log::warn("[SLF] fix19i: REROUTED shadow render -> self 2048x2048x{} R24G8 tex=0x{:x} ({} DSVs, srv=0x{:x}, staging=0x{:x})",
			s_slfSlices, reinterpret_cast<uintptr_t>(tex), made, reinterpret_cast<uintptr_t>(srv),
			reinterpret_cast<uintptr_t>(stg));
		return true;
	}

	// Read back OUR rerouted shadow texture (only exists after a successful
	// EnsureSLFShadowTextures). Same mechanism/decoding as the engine-side
	// readbacks so verdicts are directly comparable. (Not called in fix19j -
	// superseded by DebugReadbackSweep while the readback artefact is open.)
	[[maybe_unused]] static void DebugReadbackSLFShadowArray(::ID3D11DeviceContext* a_ctx)
	{
		if (!s_slfTex || !s_slfStaging || s_slfSlices == 0)
			return;
		constexpr UINT kBlock = 256;
		const UINT ox = (2048u > kBlock) ? (2048u - kBlock) / 2 : 0;
		const UINT oy = ox;
		::D3D11_BOX box{};
		box.left = ox;
		box.top = oy;
		box.right = ox + kBlock;
		box.bottom = oy + kBlock;
		box.front = 0;
		box.back = 1;
		const std::uint32_t nSlices = std::min<std::uint32_t>(s_slfSlices, 32u);
		std::string line;
		for (std::uint32_t slice = 0; slice < nSlices; slice++) {
			::D3D11_MAPPED_SUBRESOURCE wms{};
			if (SUCCEEDED(a_ctx->Map(s_slfStaging, slice, ::D3D11_MAP_WRITE, 0, &wms)) && wms.pData) {
				std::memset(wms.pData, 0xA5, static_cast<std::size_t>(kBlock) * wms.RowPitch);
				a_ctx->Unmap(s_slfStaging, slice);
			}
			a_ctx->CopySubresourceRegion(s_slfStaging, slice, 0, 0, 0, s_slfTex, slice, &box);
			::D3D11_MAPPED_SUBRESOURCE ms{};
			if (FAILED(a_ctx->Map(s_slfStaging, slice, ::D3D11_MAP_READ, 0, &ms)) || !ms.pData)
				continue;
			const std::uint8_t* pRaw = static_cast<const std::uint8_t*>(ms.pData);
			std::uint32_t content = 0, samples = 0, pat = 0;
			float minD = 1.0f, maxD = 0.0f;
			for (std::uint32_t y = 0; y < kBlock; y += 8) {
				const std::uint8_t* row = pRaw + static_cast<std::size_t>(y) * ms.RowPitch;
				for (std::uint32_t x = 0; x < kBlock; x += 8) {
					std::uint32_t v = 0;
					std::memcpy(&v, row + static_cast<std::size_t>(x) * 4, 4);
					if (v == 0xA5A5A5A5u)
						pat++;
					const std::uint32_t dHi = (v >> 8) & 0xFFFFFFu;
					const std::uint32_t dLo = v & 0xFFFFFFu;
					const float df = static_cast<float>((std::max)(dHi, dLo)) / 16777215.0f;
					if (df > 0.001f && df < 0.999f)
						content++;
					if (df < minD) minD = df;
					if (df > maxD) maxD = df;
					samples++;
				}
			}
			a_ctx->Unmap(s_slfStaging, slice);
			char buf[72];
			std::snprintf(buf, sizeof(buf), "%s%u=%.0f%%m%.3f-%.3f%s", slice == 0 ? "" : " ", slice,
				samples ? content * 100.0f / static_cast<float>(samples) : 0.0f, minD, maxD,
				pat ? " PAT!" : "");
			line += buf;
		}
		SKSE::log::info("[SLF] SLF-shadow content (self tex): {}", line);
	}

	void DebugReadbackShadowArrayAtRenderLoop(::ID3D11DeviceContext* a_ctx)
	{
		// fix19o (2026-09-04): ALL readback diagnostics disabled.
		// ============================================================
		// fix19k proved the readback method (explicit D3D11_BOX + depth =
		// AMD all-zero artefact; nullptr full-subresource copy + matching
		// format-family staging = correct). fix19m/n then fixed the family
		// derivation and shadowArr (D16, fmt53) finally read real content
		// (0xFFFF clear + shadow shapes) - observation goal REACHED.
		// But every one of those readbacks is a SYNCHRONOUS 33MB+ copy
		// (full 4096x4096 D16 slice) + Map(READ) that force-flushes the
		// GPU pipeline on the render thread: user reported stutter ("一卡
		// 一卡") and material artefacts (armor) the moment D16 copies
		// became real (fix19n) instead of format-mismatch no-ops.
		// Verdicts banked so far (fix19l/n runs):
		//   - shadow array tex = depthStencils[4].depthSRV backing
		//     (SelectDSB chosenTex, fmt53 R16_TYPELESS, 4096x4096x127)
		//   - slices 0,4,5 + 20..30 carry content; high slices = our
		//     >8-light dispatch via g_normalDepthBuffer extended slots
		//     (the 6.9M "unres" OMSets that never match views[0..7])
		//   - main depth + engine shadow rendering were NEVER broken;
		//     every "engine texture all-0 = bad link" verdict from
		//     fix19c..fix19i was the explicit-box readback artefact.
		// fix19o therefore removes the diagnostic payload entirely so
		// the user can verify clean perf (no stutter / no material
		// artefacts) and the TRUE feature state (>8-light shadowing,
		// no flicker). Re-enable pieces here only when needed.
		(void)a_ctx;
	}

	// fix19d+e: copy+map the center 256x256 of every depth target slot 0-3
	// and report minD/maxD/content%. fix19e fixes the interpretation bugs:
	//  - R32_TYPELESS was read as (dword & 0xFFFFFF)/0xFFFFFF - WRONG for
	//    D32_FLOAT (bit pattern IS the float; 0.5 = 0x3F000000 -> &-mask = 0)
	//    and wrong for D24S8 packed in 32 bits (depth = high 24 bits, >> 8).
	//    Now read BOTH ways + dump raw first-pixels so the format is
	//    unambiguous.
	//  - ArraySize>1 slots (a2) were skipped - a dual-buffered main depth
	//    would never be sampled. Now read slice 0 and note the array size.
	static void DebugMainDepthSanity(::ID3D11DeviceContext* a_ctx, const char* a_label)
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer || !a_ctx)
			return;
		auto& dsd = renderer->GetDepthStencilData();
		::ID3D11Device* dev = nullptr;
		a_ctx->GetDevice(&dev);
		if (!dev)
			return;
		std::string line;
		char sb[220];
		for (int slot = 0; slot < 4; slot++) {
			auto* srv = dsd.depthStencils[slot].depthSRV;
			if (!srv) {
				std::snprintf(sb, sizeof(sb), "%s%d=nosrv", slot ? " " : "", slot);
				line += sb;
				continue;
			}
			::ID3D11Resource* res = nullptr;
			reinterpret_cast<::ID3D11ShaderResourceView*>(srv)->GetResource(&res);
			if (!res) {
				std::snprintf(sb, sizeof(sb), "%s%d=nores", slot ? " " : "", slot);
				line += sb;
				continue;
			}
			::ID3D11Texture2D* tex = nullptr;
			if (FAILED(res->QueryInterface(__uuidof(::ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
				res->Release();
				std::snprintf(sb, sizeof(sb), "%s%d=notex", slot ? " " : "", slot);
				line += sb;
				continue;
			}
			res->Release();
			::D3D11_TEXTURE2D_DESC td{};
			tex->GetDesc(&td);
			const std::uint32_t kBlock = 256;
			const std::uint32_t ox = td.Width > kBlock ? (td.Width - kBlock) / 2 : 0;
			const std::uint32_t oy = td.Height > kBlock ? (td.Height - kBlock) / 2 : 0;
			::D3D11_TEXTURE2D_DESC sd = td;
			sd.Width = kBlock;
			sd.Height = kBlock;
			sd.MipLevels = 1;
			sd.ArraySize = 1;
			sd.Usage = ::D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
			sd.MiscFlags = 0;
			::ID3D11Texture2D* st = nullptr;
			if (FAILED(dev->CreateTexture2D(&sd, nullptr, &st))) {
				std::snprintf(sb, sizeof(sb), "%s%d=nocreate", slot ? " " : "", slot);
				line += sb;
				tex->Release();
				continue;
			}
			::D3D11_BOX box{};
			box.left = ox;
			box.top = oy;
			box.right = ox + kBlock;
			box.bottom = oy + kBlock;
			box.front = 0;
			box.back = 1;
			// D3D11 CopySubresourceRegion returns void - failures are
			// silent (debug layer only). Mark the staging first so a failed
			// copy is visible as untouched pattern residue in the readback.
			::D3D11_MAPPED_SUBRESOURCE wms{};
			if (SUCCEEDED(a_ctx->Map(st, 0, ::D3D11_MAP_WRITE, 0, &wms)) && wms.pData) {
				std::memset(wms.pData, 0xA5, static_cast<std::size_t>(kBlock) * wms.RowPitch);
				a_ctx->Unmap(st, 0);
			}
			a_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, &box);
			::D3D11_MAPPED_SUBRESOURCE ms{};
			if (FAILED(a_ctx->Map(st, 0, ::D3D11_MAP_READ, 0, &ms))) {
				std::snprintf(sb, sizeof(sb), "%s%d=mapfail", slot ? " " : "", slot);
				line += sb;
				st->Release();
				tex->Release();
				continue;
			}
			// fix19g+n: decode per format family with the CORRECT texel
			// stride. Enum facts (dxgiformat.h): fmt44 = R24G8_TYPELESS
			// (D24 family, the main depth), fmt53 = R16_TYPELESS + DSV
			// fmt55 = D16_UNORM (the shadow array is 16-bit depth), fmt19/
			// 20 = R32G8X24/D32_FLOAT_S8X24. Pre-fix19g only fmt40 took a
			// float path; everything else fell into wrong-width branches,
			// so real depth read as garbage/0 - a pure artifact.
			const bool isF32S8 = (td.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
								  td.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
			const bool isD24 = (td.Format == DXGI_FORMAT_R24G8_TYPELESS ||
								td.Format == DXGI_FORMAT_D24_UNORM_S8_UINT);
			const bool isF32 = isF32S8 ||
							   (td.Format == DXGI_FORMAT_R32_TYPELESS ||
								td.Format == DXGI_FORMAT_R32_FLOAT);
			const bool isF16 = !isF32 && !isD24;
			// fix19n: bytes per texel follows the family - D32 = 8, R16/D16
			// = 2 (isF16 fallback), D24/R32 = 4.
			const std::uint32_t tstride = isF32S8 ? 8u : (isF16 ? 2u : 4u);
			const std::uint8_t* pRaw = static_cast<const std::uint8_t*>(ms.pData);
			const std::uint32_t rowB = ms.RowPitch;
			float minD = 1.0f, maxD = 0.0f;
			std::uint32_t content = 0, samples = 0;
			std::uint32_t nonZero = 0;
			for (std::uint32_t y = 0; y < kBlock; y += 8) {
				const std::uint8_t* row = pRaw + static_cast<std::size_t>(y) * rowB;
				for (std::uint32_t x = 0; x < kBlock; x += 8) {
					const std::uint8_t* t = row + static_cast<std::size_t>(x) * tstride;
					float df = 0.0f;
					if (isF16) {
						std::uint16_t v16 = 0;
						std::memcpy(&v16, t, 2);
						df = static_cast<float>(v16) / 65535.0f;
						if (v16) nonZero++;
				} else if (isD24) {
					std::uint32_t v32 = 0;
					std::memcpy(&v32, t, 4);
					// fix19h: accept BOTH depth placements. depthC proved
					// this AMD driver stores R24G8 depth LOW-24 on copy-back
					// (clear 1.0 -> raw 0x00FFFFFF); DXGI specs HIGH-24.
					const std::uint32_t dHi = (v32 >> 8) & 0xFFFFFFu;
					const std::uint32_t dLo = v32 & 0xFFFFFFu;
					df = static_cast<float>((std::max)(dHi, dLo)) / 16777215.0f;
					if (dHi || dLo) nonZero++;
				} else {
						std::uint32_t v32 = 0;
						std::memcpy(&v32, t, 4);
						std::memcpy(&df, &v32, 4);
						if (v32) nonZero++;
						if (!(df >= 0.0f && df <= 1.0f))
							continue;  // not a valid depth float
					}
					if (df < minD) minD = df;
					if (df > maxD) maxD = df;
					if (df > 0.001f && df < 0.999f) content++;
					samples++;
				}
			}
			const std::uint8_t* c0 = pRaw;
			const std::uint8_t* c1 = pRaw + (kBlock / 2) * rowB;
			const std::uint8_t* c2 = pRaw + (kBlock / 2) * rowB + (kBlock / 2) * tstride;
			auto rd = [&](const std::uint8_t* p) {
				std::uint32_t v = 0;
				std::memcpy(&v, p, std::min<std::size_t>(4, tstride));
				return v;
			};
			std::snprintf(sb, sizeof(sb), "%s%d[fmt%d %ux%u a%u s%ux%u r%08x %08x %08x %08x]=%.3f-%.3f/c%u%%/nz%u%%",
				slot ? " " : "", slot, static_cast<int>(td.Format), td.Width, td.Height, td.ArraySize,
				td.SampleDesc.Count, td.SampleDesc.Quality,
				rd(c0), rd(c1), rd(c2), rd(c1 + 4),
				minD, maxD,
				samples > 0 ? content * 100u / samples : 0u,
				samples > 0 ? nonZero * 100u / samples : 0u);
			line += sb;
			a_ctx->Unmap(st, 0);
			st->Release();
			tex->Release();
		}
		dev->Release();
		SKSE::log::info("[SLF] {} main-depth sanity: {}", a_label, line);
	}

	// fix19e: the one-run verdict probe. Slice 31 of the shadow array is not
	// used by the scheduler (max pin seen: slot 27), so clearing it to 1.0
	// cannot corrupt any rendered light. If the subsequent readback sees
	// 0xFFFF/1.0 the copy+map mechanism is GOOD and the engine's clears and
	// draws on this texture (fix19c trace: Clear x300k to 1.0, Draw x68M)
	// are all no-ops -> the shadow render path itself never executes. If it
	// still reads 0x0000 every readback this session has been lying to us.
	static void DebugProbeClearTestSlice(::ID3D11DeviceContext* a_ctx, const char* a_label)
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer || !a_ctx)
			return;
		auto& dsd = renderer->GetDepthStencilData();
		constexpr int kSlice = 31;
		const void* vp = static_cast<const void*>(dsd.depthStencils[4].views[kSlice]);
		if (!vp)
			vp = static_cast<const void*>(ShadowLimitFixNS::P1::g_normalDepthBuffer[kSlice]);
		if (!vp) {
			SKSE::log::info("[SLF] {} probe-clear slice {}: NO DSV", a_label, kSlice);
			return;
		}
		auto* dsv = reinterpret_cast<::ID3D11DepthStencilView*>(const_cast<void*>(vp));
		// fix19h: what subresource does the ENGINE's views[31] DSV actually
		// bind? If it is not slice 31 (placeholder / wrong first-slice) the
		// clear below lands elsewhere and reading slice 31 returns 0 no
		// matter what - an engine-DSV problem, not a texture problem.
		::D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};
		dsv->GetDesc(&dsvd);
		::ID3D11Resource* res = nullptr;
		dsv->GetResource(&res);
		::ID3D11Texture2D* tex = nullptr;
		if (!res || FAILED(res->QueryInterface(__uuidof(::ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
			if (res) res->Release();
			SKSE::log::info("[SLF] {} probe-clear slice {}: no tex", a_label, kSlice);
			return;
		}
		const std::uintptr_t dsvTexPtr = reinterpret_cast<std::uintptr_t>(res);  // dsv[31] backing
		// The texture the t103 SRV (main readback) samples - compare below.
		std::uintptr_t srvTexPtr = 0;
		if (auto* srv = dsd.depthStencils[4].depthSRV) {
			::ID3D11Resource* sres = nullptr;
			reinterpret_cast<::ID3D11ShaderResourceView*>(srv)->GetResource(&sres);
			if (sres) {
				srvTexPtr = reinterpret_cast<std::uintptr_t>(sres);
				sres->Release();
			}
		}
		res->Release();
		::D3D11_TEXTURE2D_DESC td{};
		tex->GetDesc(&td);
		// 1) Clear slice kSlice to depth 1.0 through the engine's own DSV.
		a_ctx->ClearDepthStencilView(dsv, ::D3D11_CLEAR_DEPTH, 1.0f, 0);
		// 2) Read it back with the exact copy+map used by the readbacks.
		::D3D11_TEXTURE2D_DESC sd = td;
		sd.Width = 256;
		sd.Height = 256;
		sd.MipLevels = 1;
		sd.ArraySize = 1;
		sd.Usage = ::D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = 0;
		::ID3D11Device* dev = nullptr;
		a_ctx->GetDevice(&dev);
		::ID3D11Texture2D* st = nullptr;
		if (!dev || FAILED(dev->CreateTexture2D(&sd, nullptr, &st))) {
			if (dev) dev->Release();
			tex->Release();
			SKSE::log::info("[SLF] {} probe-clear slice {}: staging create fail", a_label, kSlice);
			return;
		}
		dev->Release();
		::D3D11_BOX box{};
		box.left = 0;
		box.top = 0;
		box.right = 256;
		box.bottom = 256;
		box.front = 0;
		box.back = 1;
		// fix19h: D3D11 CopySubresourceRegion returns VOID - failures are
		// silent (debug layer only). Pre-fill the staging with 0xA5 so a
		// failed copy shows up as A5A5A5A5 residue instead of "all 0".
		::D3D11_MAPPED_SUBRESOURCE wms{};
		if (SUCCEEDED(a_ctx->Map(st, 0, ::D3D11_MAP_WRITE, 0, &wms)) && wms.pData) {
			std::memset(wms.pData, 0xA5, static_cast<std::size_t>(256) * wms.RowPitch);
			a_ctx->Unmap(st, 0);
		}
		a_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, kSlice, &box);
		::D3D11_MAPPED_SUBRESOURCE ms{};
		if (FAILED(a_ctx->Map(st, 0, ::D3D11_MAP_READ, 0, &ms)) || !ms.pData) {
			SKSE::log::info("[SLF] {} probe-clear slice {}: map fail", a_label, kSlice);
			st->Release();
			tex->Release();
			return;
		}
		// fix19g+n: decode per format family (NOTE: enum-corrected - the
		// shadow array is fmt53 = R16_TYPELESS/D16_UNORM, NOT R24G8 which is
		// fmt44; D24 depth sits in the low 24 bits on this AMD driver).
		// Pre-fix19g read this texture as uint16 with even stride = always
		// the LOW word of each texel (stencil | depth_byte0) which is 0 for
		// any depth < 1/256 -> the "0x0000=100%" verdicts were an
		// interpretation artifact. Also log the DSV's OWN backing texture
		// vs the tex we copy from, so "clear acted on a different texture
		// than readback" is ruled in/out.
		const std::uint8_t* pRaw = static_cast<const std::uint8_t*>(ms.pData);
		const std::uint32_t rowB = ms.RowPitch;
		const bool p24 = (td.Format == DXGI_FORMAT_R24G8_TYPELESS ||
						  td.Format == DXGI_FORMAT_D24_UNORM_S8_UINT);
		// fix19h: depthC proved this AMD driver stores R24G8 depth in the
		// LOW 24 bits on copy-back (clear 1.0 -> raw dword 0x00FFFFFF, NOT
		// DXGI's speced 0xFFFFFF00). Accept BOTH layouts for the 0xFFFFFF
		// (clear-1.0) verdict so the probe can never false-negative again.
		std::uint32_t allF = 0, all0 = 0, other = 0, samples = 0;
		float minD = 1.0f, maxD = 0.0f;
		for (std::uint32_t y = 0; y < 256; y += 8) {
			const std::uint8_t* row = pRaw + static_cast<std::size_t>(y) * rowB;
			for (std::uint32_t x = 0; x < 256; x += 8) {
				std::uint32_t dv = 0;
				float df = 0.0f;
				if (p24) {
					std::memcpy(&dv, row + static_cast<std::size_t>(x) * 4, 4);
					const std::uint32_t dHi = (dv >> 8) & 0xFFFFFFu;
					const std::uint32_t dLo = dv & 0xFFFFFFu;
					if (dHi == 0xFFFFFFu || dLo == 0xFFFFFFu) allF++;
					else if (dHi == 0u && dLo == 0u) all0++;
					else other++;
					df = static_cast<float>((std::max)(dHi, dLo)) / 16777215.0f;
				} else {
					std::memcpy(&dv, row + static_cast<std::size_t>(x) * 4, 4);
					if (dv == 0u) all0++;
					else other++;
					std::memcpy(&df, &dv, 4);
				}
				if (df < minD) minD = df;
				if (df > maxD) maxD = df;
				samples++;
			}
		}
		a_ctx->Unmap(st, 0);
		st->Release();
		tex->Release();
		SKSE::log::info("[SLF] {} probe-clear slice {}: fmt={} {}x{} arr={} sample={}x{} dsv=0x{:x} bind={} dsvTex=0x{:x}/srvTex=0x{:x} {} readback minD={:.3f} maxD={:.3f} 0xFFFFFF={}% 0x000000={}% other={}% raw={:08x} {:08x} {:08x} {:08x}",
			a_label, kSlice, static_cast<int>(td.Format), td.Width, td.Height, td.ArraySize,
			td.SampleDesc.Count, td.SampleDesc.Quality,
			reinterpret_cast<uintptr_t>(dsv),
			dsvd.ViewDimension == ::D3D11_DSV_DIMENSION_TEXTURE2D ? std::to_string(dsvd.Texture2D.MipSlice) :
				(dsvd.ViewDimension == ::D3D11_DSV_DIMENSION_TEXTURE2DARRAY ?
					(std::to_string(dsvd.Texture2DArray.FirstArraySlice) + "+" + std::to_string(dsvd.Texture2DArray.ArraySize)) : "?dim"),
			dsvTexPtr, srvTexPtr,
			(dsvTexPtr == srvTexPtr) ? "SAME" : "DIFFERENT",
			minD, maxD,
			samples > 0 ? allF * 100u / samples : 0u,
			samples > 0 ? all0 * 100u / samples : 0u,
			samples > 0 ? other * 100u / samples : 0u,
			static_cast<unsigned>(pRaw[0]) | (static_cast<unsigned>(pRaw[1]) << 8) | (static_cast<unsigned>(pRaw[2]) << 16) | (static_cast<unsigned>(pRaw[3]) << 24),
			static_cast<unsigned>(pRaw[4]) | (static_cast<unsigned>(pRaw[5]) << 8) | (static_cast<unsigned>(pRaw[6]) << 16) | (static_cast<unsigned>(pRaw[7]) << 24),
			static_cast<unsigned>(pRaw[8]) | (static_cast<unsigned>(pRaw[9]) << 8) | (static_cast<unsigned>(pRaw[10]) << 16) | (static_cast<unsigned>(pRaw[11]) << 24),
			static_cast<unsigned>(pRaw[12]) | (static_cast<unsigned>(pRaw[13]) << 8) | (static_cast<unsigned>(pRaw[14]) << 16) | (static_cast<unsigned>(pRaw[15]) << 24));

		// fix19h variant D: clear the SAME texture+slice through a DSV WE
		// create here (not the engine's views[31]). If this reads back
		// 0xFFFFFF the texture accepts clears fine and the engine DSV above
		// is the broken link (wrong binding). If this ALSO reads 0 the
		// texture itself rejects clears / copies - engine-side problem.
		{
			::ID3D11Device* dev3 = nullptr;
			a_ctx->GetDevice(&dev3);
			if (dev3) {
				::D3D11_DEPTH_STENCIL_VIEW_DESC md{};
				md.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
				md.ViewDimension = ::D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
				md.Texture2DArray.FirstArraySlice = static_cast<UINT>(kSlice);
				md.Texture2DArray.ArraySize = 1;
				::ID3D11DepthStencilView* mdsv = nullptr;
				const HRESULT odhr = dev3->CreateDepthStencilView(tex, &md, &mdsv);
				if (SUCCEEDED(odhr)) {
					a_ctx->ClearDepthStencilView(mdsv, ::D3D11_CLEAR_DEPTH, 1.0f, 0);
					::D3D11_TEXTURE2D_DESC msd = td;
					msd.Width = 256;
					msd.Height = 256;
					msd.MipLevels = 1;
					msd.ArraySize = 1;
					msd.Usage = ::D3D11_USAGE_STAGING;
					msd.BindFlags = 0;
					msd.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
					msd.MiscFlags = 0;
					::ID3D11Texture2D* mst = nullptr;
					if (SUCCEEDED(dev3->CreateTexture2D(&msd, nullptr, &mst))) {
						::D3D11_MAPPED_SUBRESOURCE mwms{};
						if (SUCCEEDED(a_ctx->Map(mst, 0, ::D3D11_MAP_WRITE, 0, &mwms)) && mwms.pData) {
							std::memset(mwms.pData, 0xA5, static_cast<std::size_t>(256) * mwms.RowPitch);
							a_ctx->Unmap(mst, 0);
						}
						a_ctx->CopySubresourceRegion(mst, 0, 0, 0, 0, tex, kSlice, &box);
						::D3D11_MAPPED_SUBRESOURCE mms{};
						if (SUCCEEDED(a_ctx->Map(mst, 0, ::D3D11_MAP_READ, 0, &mms)) && mms.pData) {
							const std::uint8_t* mp = static_cast<const std::uint8_t*>(mms.pData);
							std::uint32_t mF = 0, m0 = 0, mo = 0, mn = 0;
							for (std::uint32_t yy = 0; yy < 256; yy += 8) {
								const std::uint8_t* r2 = mp + static_cast<std::size_t>(yy) * mms.RowPitch;
								for (std::uint32_t xx = 0; xx < 256; xx += 8) {
									std::uint32_t v2 = 0;
									std::memcpy(&v2, r2 + static_cast<std::size_t>(xx) * 4, 4);
									const std::uint32_t hi = (v2 >> 8) & 0xFFFFFFu;
									const std::uint32_t lo = v2 & 0xFFFFFFu;
									if (hi == 0xFFFFFFu || lo == 0xFFFFFFu) mF++;
									else if (hi == 0u && lo == 0u) m0++;
									else mo++;
									mn++;
								}
							}
							char sb2[192];
							std::snprintf(sb2, sizeof(sb2), "[SLF] %s probe-ownDSV slice %d: %u%%F %u%%0 %u%%oth raw=%08x",
								a_label, kSlice,
								mn ? mF * 100u / mn : 0u, mn ? m0 * 100u / mn : 0u, mn ? mo * 100u / mn : 0u,
								static_cast<unsigned>(mp[0]) | (static_cast<unsigned>(mp[1]) << 8) |
									(static_cast<unsigned>(mp[2]) << 16) | (static_cast<unsigned>(mp[3]) << 24));
							SKSE::log::info("{}", sb2);
							a_ctx->Unmap(mst, 0);
						} else {
							SKSE::log::info("[SLF] {} probe-ownDSV slice {}: map fail", a_label, kSlice);
						}
						mst->Release();
					} else {
						SKSE::log::info("[SLF] {} probe-ownDSV slice {}: staging create fail", a_label, kSlice);
					}
					mdsv->Release();
				} else {
					SKSE::log::info("[SLF] {} probe-ownDSV slice {}: DSV create fail hr=0x{:08X}", a_label, kSlice, static_cast<unsigned>(odhr));
				}
				dev3->Release();
			}
		}
		// NOTE: no extra tex->Release() here - tex's ref from the QI above
		// was already released after the main probe readback (line ~705);
		// variant D only borrows tex while the DSV still holds a reference.
	}

	// fix19f: see forward declaration above. UpdateSubresource is NOT among
	// our hooked vtable slots, so this exercises the raw driver path.
	static void DebugReadbackSelfTest(::ID3D11DeviceContext* a_ctx, const char* a_label)
	{
		if (!a_ctx)
			return;
		::ID3D11Device* dev = nullptr;
		a_ctx->GetDevice(&dev);
		if (!dev)
			return;
		constexpr UINT kS = 64;
		::D3D11_TEXTURE2D_DESC td{};
		td.Width = kS;
		td.Height = kS;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = ::D3D11_USAGE_DEFAULT;
		td.BindFlags = 0;
		td.CPUAccessFlags = 0;
		td.MiscFlags = 0;
		::ID3D11Texture2D* texA = nullptr;
		if (FAILED(dev->CreateTexture2D(&td, nullptr, &texA))) {
			dev->Release();
			return;
		}
		std::uint8_t red[64 * 64 * 4];
		for (std::size_t i = 0; i < sizeof(red); i += 4) {
			red[i + 0] = 0xFF;
			red[i + 1] = 0x00;
			red[i + 2] = 0x00;
			red[i + 3] = 0xFF;
		}
		a_ctx->UpdateSubresource(texA, 0, nullptr, red, kS * 4, 0);
		auto makeStaging = [&](::ID3D11Texture2D** a_out) {
			::D3D11_TEXTURE2D_DESC sd = td;
			sd.Usage = ::D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
			return dev->CreateTexture2D(&sd, nullptr, a_out);
		};
		auto readPixels = [](::D3D11_MAPPED_SUBRESOURCE& a_ms, UINT a_w, UINT a_h,
							   std::uint32_t& a_rSum, std::uint32_t& a_n,
							   std::uint32_t& a_r0, std::uint32_t& a_g0, std::uint32_t& a_b0) {
			a_rSum = 0;
			a_n = 0;
			const auto* p = static_cast<const std::uint8_t*>(a_ms.pData);
			for (UINT y = 0; y < a_h; y += 4) {
				const auto* row = p + static_cast<std::size_t>(y) * a_ms.RowPitch;
				for (UINT x = 0; x < a_w; x += 4) {
					a_rSum += row[x * 4 + 0];
					a_n++;
				}
			}
			a_r0 = p[0];
			a_g0 = p[1];
			a_b0 = p[2];
		};
		std::string out;
		char sb[160];
		// Variant A: whole-texture copy (no box).
		::ID3D11Texture2D* stA = nullptr;
		if (SUCCEEDED(makeStaging(&stA))) {
			a_ctx->CopySubresourceRegion(stA, 0, 0, 0, 0, texA, 0, nullptr);
			::D3D11_MAPPED_SUBRESOURCE ms{};
			if (SUCCEEDED(a_ctx->Map(stA, 0, ::D3D11_MAP_READ, 0, &ms)) && ms.pData) {
				std::uint32_t rSum = 0, n = 0, r0 = 0, g0 = 0, b0 = 0;
				readPixels(ms, kS, kS, rSum, n, r0, g0, b0);
				std::snprintf(sb, sizeof(sb), "%s self-test: full=(%u,%u,%u)@(0,0) avgR=%u | ", a_label,
					r0, g0, b0, n ? rSum / n : 0);
				out += sb;
				a_ctx->Unmap(stA, 0);
			} else {
				out += std::string(a_label) + " self-test: full=mapfail | ";
			}
			stA->Release();
		} else {
			out += std::string(a_label) + " self-test: full=nocreate | ";
		}
		// Variant B: 32x32 sub-box copy (mirrors the boxed 256x256 copies
		// every shadow / main-depth readback in this plugin performs).
		::ID3D11Texture2D* stB = nullptr;
		if (SUCCEEDED(makeStaging(&stB))) {
			::D3D11_BOX box{};
			box.right = 32;
			box.bottom = 32;
			box.front = 0;
			box.back = 1;
			a_ctx->CopySubresourceRegion(stB, 0, 0, 0, 0, texA, 0, &box);
			::D3D11_MAPPED_SUBRESOURCE ms{};
			if (SUCCEEDED(a_ctx->Map(stB, 0, ::D3D11_MAP_READ, 0, &ms)) && ms.pData) {
				std::uint32_t rSum = 0, n = 0, r0 = 0, g0 = 0, b0 = 0;
				readPixels(ms, 32, 32, rSum, n, r0, g0, b0);
				std::snprintf(sb, sizeof(sb), "sub32=(%u,%u,%u)@(0,0) avgR=%u", r0, g0, b0, n ? rSum / n : 0);
				out += sb;
				a_ctx->Unmap(stB, 0);
			} else {
				out += "sub32=mapfail";
			}
			stB->Release();
		} else {
			out += "sub32=nocreate";
		}
		// Variant C (fix19g): depth-format chain self-test. Every shadow /
		// main-depth readback failed to see content (all-0) while RGBA8
		// copies (A/B above) succeed. The remaining difference is the
		// DEPTH format itself (fmt53 R24G8 / fmt44 R32G8X24). Build our
		// OWN R24G8 depth texture + DSV, clear it to 1.0 THROUGH the hooked
		// ClearDepthStencilView vtable slot (verifies the hook forwards
		// cleanly), copy to a TYPELESS staging, map, and decode as
		// dword>>8. 0xFFFFFF% high -> depth clear+copy+decode chain is
		// fine end to end -> engine-texture all-0 reads are a subresource/
		// texture mismatch. Still 0 -> depth-format copy or the clear hook
		// itself is broken.
		{
			::D3D11_TEXTURE2D_DESC dtd{};
			dtd.Width = 64;
			dtd.Height = 64;
			dtd.MipLevels = 1;
			dtd.ArraySize = 1;
			dtd.Format = DXGI_FORMAT_R24G8_TYPELESS;
			dtd.SampleDesc.Count = 1;
			dtd.Usage = ::D3D11_USAGE_DEFAULT;
			dtd.BindFlags = ::D3D11_BIND_DEPTH_STENCIL;
			dtd.CPUAccessFlags = 0;
			dtd.MiscFlags = 0;
			::ID3D11Texture2D* dtex = nullptr;
			if (SUCCEEDED(dev->CreateTexture2D(&dtd, nullptr, &dtex))) {
				::D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};
				dsvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
				dsvd.ViewDimension = ::D3D11_DSV_DIMENSION_TEXTURE2D;
				dsvd.Texture2D.MipSlice = 0;
				::ID3D11DepthStencilView* dsv = nullptr;
				if (SUCCEEDED(dev->CreateDepthStencilView(dtex, &dsvd, &dsv))) {
					a_ctx->ClearDepthStencilView(dsv, ::D3D11_CLEAR_DEPTH, 1.0f, 0);
					::D3D11_TEXTURE2D_DESC dsd2 = dtd;
					dsd2.Width = 64;
					dsd2.Height = 64;
					dsd2.MipLevels = 1;
					dsd2.Usage = ::D3D11_USAGE_STAGING;
					dsd2.BindFlags = 0;
					dsd2.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
					dsd2.MiscFlags = 0;
					::ID3D11Texture2D* dstg = nullptr;
					if (SUCCEEDED(dev->CreateTexture2D(&dsd2, nullptr, &dstg))) {
						a_ctx->CopySubresourceRegion(dstg, 0, 0, 0, 0, dtex, 0, nullptr);
						::D3D11_MAPPED_SUBRESOURCE ms{};
						if (SUCCEEDED(a_ctx->Map(dstg, 0, ::D3D11_MAP_READ, 0, &ms)) && ms.pData) {
							const std::uint8_t* p = static_cast<const std::uint8_t*>(ms.pData);
							std::uint32_t allF = 0, all0 = 0, other = 0, ns = 0;
							for (UINT yy = 0; yy < 64; yy += 4) {
								const std::uint8_t* row = p + static_cast<std::size_t>(yy) * ms.RowPitch;
								for (UINT xx = 0; xx < 64; xx += 4) {
									std::uint32_t v = 0;
									std::memcpy(&v, row + static_cast<std::size_t>(xx) * 4, 4);
									// fix19h: this AMD driver stores R24G8
									// depth in the LOW 24 bits on copy-back,
									// so clear-1.0 reads raw 0x00FFFFFF not
									// DXGI's speced 0xFFFFFF00. Accept both.
									const std::uint32_t dHi = (v >> 8) & 0xFFFFFFu;
									const std::uint32_t dLo = v & 0xFFFFFFu;
									if (dHi == 0xFFFFFFu || dLo == 0xFFFFFFu) allF++;
									else if (dHi == 0u && dLo == 0u) all0++;
									else other++;
									ns++;
								}
							}
							std::snprintf(sb, sizeof(sb), " | depthC: clear1.0 -> 0xFFFFFF=%u%% 0x0=%u%% oth=%u%% raw=%08x %08x",
								ns ? allF * 100u / ns : 0u, ns ? all0 * 100u / ns : 0u,
								ns ? other * 100u / ns : 0u,
								static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8) |
									static_cast<unsigned>(p[2]) << 16 | static_cast<unsigned>(p[3]) << 24,
								static_cast<unsigned>(p[4]) | (static_cast<unsigned>(p[5]) << 8) |
									static_cast<unsigned>(p[6]) << 16 | static_cast<unsigned>(p[7]) << 24);
							out += sb;
							a_ctx->Unmap(dstg, 0);
						} else {
							out += " | depthC=mapfail";
						}
						dstg->Release();
					} else {
						out += " | depthC=nocreate";
					}
					dsv->Release();
				} else {
					out += " | depthC=nodsv";
				}
				dtex->Release();
			} else {
				out += " | depthC=notex";
			}
		}
		// fix19i depthC2: does the ARRAY form work? The engine shadow
		// texture is a 127-slice ARRAY and our own-DSV attempt on it fails
		// every time (variant D), while depthC above proved the recipe on a
		// 1-slice (non-array) texture. Build a 2-slice array texture with an
		// ARRAY-style DSV on slice 1 - if that clears + reads back 1.0 the
		// array+array-DSV recipe is fine and the ENGINE texture itself is
		// the broken link (ghost/allocation), not our DSV usage.
		{
			::D3D11_TEXTURE2D_DESC atd{};
			atd.Width = 64;
			atd.Height = 64;
			atd.MipLevels = 1;
			atd.ArraySize = 2;
			atd.Format = DXGI_FORMAT_R24G8_TYPELESS;
			atd.SampleDesc.Count = 1;
			atd.SampleDesc.Quality = 0;
			atd.Usage = ::D3D11_USAGE_DEFAULT;
			atd.BindFlags = ::D3D11_BIND_DEPTH_STENCIL | ::D3D11_BIND_SHADER_RESOURCE;
			atd.CPUAccessFlags = 0;
			atd.MiscFlags = 0;
			::ID3D11Texture2D* atex = nullptr;
			if (SUCCEEDED(dev->CreateTexture2D(&atd, nullptr, &atex))) {
				::D3D11_DEPTH_STENCIL_VIEW_DESC adsvd{};
				adsvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
				adsvd.ViewDimension = ::D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
				adsvd.Texture2DArray.FirstArraySlice = 1;
				adsvd.Texture2DArray.ArraySize = 1;
				::ID3D11DepthStencilView* adsv = nullptr;
				const HRESULT d2hr = dev->CreateDepthStencilView(atex, &adsvd, &adsv);
				if (SUCCEEDED(d2hr)) {
					a_ctx->ClearDepthStencilView(adsv, ::D3D11_CLEAR_DEPTH, 1.0f, 0);
					::D3D11_TEXTURE2D_DESC asd2 = atd;
					asd2.Width = 64;
					asd2.Height = 64;
					asd2.MipLevels = 1;
					asd2.ArraySize = 1;
					asd2.Usage = ::D3D11_USAGE_STAGING;
					asd2.BindFlags = 0;
					asd2.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
					asd2.MiscFlags = 0;
					::ID3D11Texture2D* astg = nullptr;
					if (SUCCEEDED(dev->CreateTexture2D(&asd2, nullptr, &astg))) {
						a_ctx->CopySubresourceRegion(astg, 0, 0, 0, 0, atex, 1, nullptr);
						::D3D11_MAPPED_SUBRESOURCE ams{};
						if (SUCCEEDED(a_ctx->Map(astg, 0, ::D3D11_MAP_READ, 0, &ams)) && ams.pData) {
							const std::uint8_t* ap = static_cast<const std::uint8_t*>(ams.pData);
							std::uint32_t aF = 0, a0 = 0, ao = 0, an = 0;
							for (UINT yy = 0; yy < 64; yy += 4) {
								const std::uint8_t* r3 = ap + static_cast<std::size_t>(yy) * ams.RowPitch;
								for (UINT xx = 0; xx < 64; xx += 4) {
									std::uint32_t v3 = 0;
									std::memcpy(&v3, r3 + static_cast<std::size_t>(xx) * 4, 4);
									const std::uint32_t hi3 = (v3 >> 8) & 0xFFFFFFu;
									const std::uint32_t lo3 = v3 & 0xFFFFFFu;
									if (hi3 == 0xFFFFFFu || lo3 == 0xFFFFFFu) aF++;
									else if (hi3 == 0u && lo3 == 0u) a0++;
									else ao++;
									an++;
								}
							}
							std::snprintf(sb, sizeof(sb), " | depthC2(array64x2 slice1): clear1.0 -> 0xFFFFFF=%u%% 0x0=%u%% oth=%u%% raw=%08x",
								an ? aF * 100u / an : 0u, an ? a0 * 100u / an : 0u, an ? ao * 100u / an : 0u,
								static_cast<unsigned>(ap[0]) | (static_cast<unsigned>(ap[1]) << 8) |
									static_cast<unsigned>(ap[2]) << 16 | static_cast<unsigned>(ap[3]) << 24);
							out += sb;
							a_ctx->Unmap(astg, 0);
						} else {
							out += " | depthC2=mapfail";
						}
						astg->Release();
					} else {
						out += " | depthC2=nocreate";
					}
					adsv->Release();
				} else {
					char hb[64];
					std::snprintf(hb, sizeof(hb), " | depthC2=nodsv hr=0x%08X", static_cast<unsigned>(d2hr));
					out += hb;
				}
				atex->Release();
			} else {
				out += " | depthC2=notex";
			}
		}
		SKSE::log::info("[SLF] {}", out);
		texA->Release();
		dev->Release();
	}

	void DebugReadbackShadowArrayAtMaterialPass(::ID3D11DeviceContext* a_ctx)
	{
		// fix19o (2026-09-04): ALL readback diagnostics disabled.
		// fix19k proved the explicit D3D11_BOX readback (what the re-armed
		// 2026-09-05 version used: 256x256 boxed copies) is an AMD
		// ALL-ZERO artefact; fix19m/n used the correct method (full
		// subresource + matching format-family staging) and READ REAL
		// CONTENT (0xFFFF clear + shadow shapes, slices 0/4/5 + 20..30) -
		// "main depth + engine shadow rendering were NEVER broken".
		// The 09-05 dual-object probe (SAME tex 0x10bf7fa20) then proved
		// t103 binds the very render target the shadow draws use. The
		// "all 0% at material pass" verdicts of 08:50-09:30 were this
		// artefact, NOT a dead depth array. Observation goals are banked;
		// re-enabling only costs synchronous 33MB+ flushes (stutter) and
		// misleading all-zero logs. Keep disabled.
		(void)a_ctx;
	}

	static void DebugReadbackShadowArraySlicesImpl(::ID3D11DeviceContext* a_ctx, const char* a_label)
	{

		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer || !a_ctx)
			return;
		auto& dsd = renderer->GetDepthStencilData();
		// SLF-B B4 (2026-09-05 08:5x): dual-object probe. The 08:50 MP
		// readback of the depthSRV object showed all-1.0 (clear value) on
		// every slice WHILE 24-light shadow draws were running at ~180k/s
		// (OM-set confirms 125k binds to the array). Either the renders land
		// in a DIFFERENT texture than depthStencils[4].depthSRV (t103 bound
		// to the wrong object) or the depth writes never rasterize. Resolve
		// it by reading BOTH: texA = the depthSRV object (what t103 sees),
		// texB = the real render target (the DSV in g_normalDepthBuffer the
		// SelectDSB hook hands to shadow draws). Read texB, log the verdict.
		::ID3D11Texture2D* texA = nullptr;
		if (auto* srvA = dsd.depthStencils[4].depthSRV) {
			::ID3D11Resource* resA = nullptr;
			reinterpret_cast<::ID3D11ShaderResourceView*>(srvA)->GetResource(&resA);
			if (resA) {
				if (FAILED(resA->QueryInterface(__uuidof(::ID3D11Texture2D), reinterpret_cast<void**>(&texA))))
					texA = nullptr;
				resA->Release();
			}
		}
		::ID3D11Texture2D* texB = nullptr;  // real shadow render target
		if (auto* dsvB = ShadowLimitFixNS::P1::g_normalDepthBuffer[0]
				? reinterpret_cast<::ID3D11DepthStencilView*>(ShadowLimitFixNS::P1::g_normalDepthBuffer[0])
				: nullptr) {
			::ID3D11Resource* resB = nullptr;
			dsvB->GetResource(&resB);
			if (resB) {
				if (FAILED(resB->QueryInterface(__uuidof(::ID3D11Texture2D), reinterpret_cast<void**>(&texB))))
					texB = nullptr;
				resB->Release();
			}
		}
		::ID3D11Texture2D* tex = texB ? texB : texA;  // read the render target
		if (!tex) {
			if (texA) texA->Release();
			return;
		}
		::D3D11_TEXTURE2D_DESC td{};
		tex->GetDesc(&td);
		if (texA != tex) {
			::D3D11_TEXTURE2D_DESC tda{};
			texA->GetDesc(&tda);
			SKSE::log::warn("[SLF] {} render-target check: depthSRV-tex=0x{:x} ({}x{} arr={} fmt={}) != renderDSV-tex=0x{:x} ({}x{} arr={} fmt={})",
				a_label, reinterpret_cast<uintptr_t>(texA), tda.Width, tda.Height, tda.ArraySize, static_cast<int>(tda.Format),
				reinterpret_cast<uintptr_t>(tex), td.Width, td.Height, td.ArraySize, static_cast<int>(td.Format));
		} else {
			SKSE::log::info("[SLF] {} render-target check: SAME tex 0x{:x} ({}x{} arr={} fmt={})", a_label,
				reinterpret_cast<uintptr_t>(tex), td.Width, td.Height, td.ArraySize, static_cast<int>(td.Format));
		}
		if (texA && texA != tex)
			texA->Release();

		::ID3D11Device* dev = nullptr;
		a_ctx->GetDevice(&dev);
		if (!dev) {
			tex->Release();
			return;
		}

		// Staging is keyed to the texture it was created for: if the object
		// changed (depthSRV vs render target), rebuild it.
		static ::ID3D11Texture2D* s_staging = nullptr;
		static std::uintptr_t s_stagingTex = 0;
		if (!s_staging || s_stagingTex != reinterpret_cast<uintptr_t>(tex)) {
			if (s_staging)
				s_staging->Release();
			s_staging = nullptr;
			::D3D11_TEXTURE2D_DESC sd = td;
			sd.Width = 256;
			sd.Height = 256;
			sd.MipLevels = 1;
			sd.ArraySize = std::min<std::uint32_t>(td.ArraySize, 32u);  // v6: scan up to 32 slices
			sd.Usage = ::D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
			sd.MiscFlags = 0;
			if (FAILED(dev->CreateTexture2D(&sd, nullptr, &s_staging))) {
				dev->Release();
				tex->Release();
				return;
			}
			s_stagingTex = reinterpret_cast<uintptr_t>(tex);
		}
		dev->Release();

		constexpr std::uint32_t kBlock = 256;
		const std::uint32_t ox = td.Width > kBlock ? (td.Width - kBlock) / 2 : 0;
		const std::uint32_t oy = td.Height > kBlock ? (td.Height - kBlock) / 2 : 0;
		::D3D11_BOX box{};
		box.left = ox;
		box.top = oy;
		box.right = ox + kBlock;
		box.bottom = oy + kBlock;
		box.front = 0;
		box.back = 1;

		// v6.1: scan up to 32 slices like the RL readback (line ~164). The
		// hardcoded 8u predates v6's >8 scheduling - it masked whether
		// slices 8+ actually carry depth at the material pass.
		const std::uint32_t nSlices = std::min<std::uint32_t>(td.ArraySize, 32u);
		// fix19g+n: enum-corrected note - the shadow array texture is
		// fmt53 = R16_TYPELESS (16-bit, D16_UNORM DSVs), NOT fmt44 R24G8.
		// Decode per format family: R24G8/R24X8 -> (dword>>8); R16 -> u16.
		const bool is24 = (td.Format == DXGI_FORMAT_R24G8_TYPELESS ||
						   td.Format == DXGI_FORMAT_D24_UNORM_S8_UINT);
		const bool is32 = (td.Format == DXGI_FORMAT_R32_TYPELESS ||
						   td.Format == DXGI_FORMAT_R32_FLOAT ||
						   td.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
						   td.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
		std::string line;
		for (std::uint32_t slice = 0; slice < nSlices; slice++) {
			// fix19h: CopySubresourceRegion returns void; pre-fill staging
			// so a silent copy failure shows as 0xA5A5A5A5 pattern residue.
			::D3D11_MAPPED_SUBRESOURCE wms{};
			if (SUCCEEDED(a_ctx->Map(s_staging, slice, ::D3D11_MAP_WRITE, 0, &wms)) && wms.pData) {
				std::memset(wms.pData, 0xA5, static_cast<std::size_t>(kBlock) * wms.RowPitch);
				a_ctx->Unmap(s_staging, slice);
			}
			a_ctx->CopySubresourceRegion(s_staging, slice, 0, 0, 0, tex, slice, &box);
			::D3D11_MAPPED_SUBRESOURCE ms{};
			if (FAILED(a_ctx->Map(s_staging, slice, ::D3D11_MAP_READ, 0, &ms))) {
				SKSE::log::info("[SLF] {} shadow content: slice {} MAP fail", a_label, slice);
				continue;
			}
			const std::uint32_t bpp = is24 ? 4u : (is32 ? 4u : 2u);
			std::uint32_t content = 0, samples = 0, pat = 0;
			std::uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
			for (std::uint32_t y = 0; y < kBlock; y += 8) {
				const auto* rowb = static_cast<const std::uint8_t*>(ms.pData) + static_cast<std::size_t>(y) * ms.RowPitch;
				for (std::uint32_t x = 0; x < kBlock; x += 8) {
					float df = 0.0f;
					if (bpp == 2) {
						std::uint16_t v16 = 0;
						std::memcpy(&v16, rowb + static_cast<std::size_t>(x) * 2, 2);
						df = static_cast<float>(v16) / 65535.0f;
					} else {
						std::uint32_t v32 = 0;
						std::memcpy(&v32, rowb + static_cast<std::size_t>(x) * 4, 4);
						if (v32 == 0xA5A5A5A5u) pat++;  // pre-fill residue = copy never landed
						if (y == 0 && x == 0) r0 = v32;
						if (y == 0 && x == 8) r1 = v32;
						if (y == kBlock / 2 && x == 0) r2 = v32;
						if (y == kBlock / 2 && x == 8) r3 = v32;
						if (is24) {
							// fix19h: accept both depth placements (DXGI
							// high-24 vs this AMD driver's low-24 copy-back).
							const std::uint32_t dHi = (v32 >> 8) & 0xFFFFFFu;
							const std::uint32_t dLo = v32 & 0xFFFFFFu;
							df = static_cast<float>((std::max)(dHi, dLo)) / 16777215.0f;
						} else if (td.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
								 td.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
							std::memcpy(&df, &v32, 4);  // R32 float depth in low dword
						else
							std::memcpy(&df, &v32, 4);
					}
					if (df > 0.001f && df < 0.999f)
						content++;
					samples++;
				}
			}
			a_ctx->Unmap(s_staging, slice);
			char buf[64];
			if (bpp == 2 || pat == 0) {
				std::snprintf(buf, sizeof(buf), "%s%u=%.0f%%", slice == 0 ? "" : " ", slice,
					samples > 0 ? content * 100.0f / static_cast<float>(samples) : 0.0f);
			} else {
				// pattern residue (0xA5A5A5A5) = pre-fill survived = the
				// copy never landed (D3D11 CopySubresourceRegion is void).
				std::snprintf(buf, sizeof(buf), "%s%u=%.0f%%p%u/%08x%08x%08x%08x", slice == 0 ? "" : " ", slice,
					samples > 0 ? content * 100.0f / static_cast<float>(samples) : 0.0f, pat, r0, r1, r2, r3);
			}
			line += buf;
		}
		SKSE::log::info("[SLF] {} shadow content (t103 tex): {}", a_label, line);

		// fix19d (2026-09-04): audit ALL 8 slot DSVs, not just views[0].
		// fix19c trace showed clears(1.0)/draws hitting "the shadow array"
		// while EVERY readback of depthStencils[4].depthSRV read all-0.000.
		// If g_normalDepthBuffer[k] / views[k] (k>=1) point at a DIFFERENT
		// texture than the SRV, renders land elsewhere and this readback
		// can never see them - the whole 0% series would be a wrong-texture
		// read (fix19b's "assumption (a)" resurrected per-slice).
		{
			auto* srv = dsd.depthStencils[4].depthSRV;
			if (!srv)
				return;  // audit needs the engine SRV object
			::ID3D11Resource* srvRes = nullptr;
			reinterpret_cast<::ID3D11ShaderResourceView*>(srv)->GetResource(&srvRes);
			std::string av;
			char abuf[160];
			for (int k = 0; k < 8; k++) {
				::ID3D11Resource* vr = nullptr;
				::ID3D11Resource* er = nullptr;
				const void* vk = static_cast<const void*>(dsd.depthStencils[4].views[k]);
				if (vk) {
					auto* dv = reinterpret_cast<::ID3D11DepthStencilView*>(const_cast<void*>(vk));
					dv->GetResource(&vr);
				}
				if (ShadowLimitFixNS::P1::g_normalDepthBuffer[k]) {
					auto* ev = reinterpret_cast<::ID3D11DepthStencilView*>(ShadowLimitFixNS::P1::g_normalDepthBuffer[k]);
					ev->GetResource(&er);
				}
				const char* tag = "";
				if (vr == srvRes) tag = "=srv";
				else if (er == srvRes) tag = "=srvE";
				std::snprintf(abuf, sizeof(abuf), "%s%d:v0x%llx/e0x%llx%s", k == 0 ? "" : " ", k,
					reinterpret_cast<unsigned long long>(vr),
					reinterpret_cast<unsigned long long>(er), tag);
				av += abuf;
				if (vr) vr->Release();
				if (er) er->Release();
			}
			SKSE::log::info("[SLF] {} res audit8: srvTex=0x{:x} fmt={} {}x{} arr={} mips={} sample={}x{} usage={} bind=0x{:x} misc=0x{:x} |{}",
				a_label, reinterpret_cast<uintptr_t>(srvRes),
				static_cast<int>(td.Format), td.Width, td.Height, td.ArraySize, td.MipLevels,
				td.SampleDesc.Count, td.SampleDesc.Quality,
				static_cast<int>(td.Usage), td.BindFlags, td.MiscFlags, av);
			if (srvRes) srvRes->Release();
		}

		// fix19b: (b) full-slice scan of the first 8 slices. The center-256
		// block above can sit in an empty region (atlas gap / between a
		// dual-paraboloid light's two halves) and report 0% while the slice
		// actually carries depth. minD < 1.0 anywhere = pixels ARE written;
		// all-minD == 1.0 across all 8 = the renders rasterize nothing.
		if (td.ArraySize >= 8 && td.Width <= 4096) {
			static ::ID3D11Texture2D* s_stagingFull = nullptr;
			const UINT fw = std::min(td.Width, 2048u);
			const UINT fh = std::min(td.Height, 2048u);
			if (!s_stagingFull) {
				::D3D11_TEXTURE2D_DESC sd = td;
				sd.Width = fw;
				sd.Height = fh;
				sd.MipLevels = 1;
				sd.ArraySize = 8;
				sd.Usage = ::D3D11_USAGE_STAGING;
				sd.BindFlags = 0;
				sd.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
				sd.MiscFlags = 0;
				::ID3D11Device* dev2 = nullptr;
				a_ctx->GetDevice(&dev2);
				if (dev2) {
					dev2->CreateTexture2D(&sd, nullptr, &s_stagingFull);
					dev2->Release();
				}
			}
			if (s_stagingFull) {
				::D3D11_BOX fbox{};
				fbox.left = 0;
				fbox.top = 0;
				fbox.right = fw;
				fbox.bottom = fh;
				fbox.front = 0;
				fbox.back = 1;
				const bool fis24 = (td.Format == DXGI_FORMAT_R24G8_TYPELESS ||
									td.Format == DXGI_FORMAT_D24_UNORM_S8_UINT);
				const bool fis32 = (td.Format == DXGI_FORMAT_R32_TYPELESS ||
									td.Format == DXGI_FORMAT_R32_FLOAT ||
									td.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
									td.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
				const std::uint32_t fbpp = (fis24 || fis32) ? 4u : 2u;
				std::string fl;
				for (std::uint32_t slice = 0; slice < 8; slice++) {
					a_ctx->CopySubresourceRegion(s_stagingFull, slice, 0, 0, 0, tex, slice, &fbox);
					::D3D11_MAPPED_SUBRESOURCE fms{};
					if (FAILED(a_ctx->Map(s_stagingFull, slice, ::D3D11_MAP_READ, 0, &fms))) {
						fl += std::string(slice == 0 ? "" : " ") + std::to_string(slice) + ":mapfail";
						continue;
					}
					float minD = 1.0f, maxD = 0.0f;
					std::uint32_t fcontent = 0, fsamples = 0;
					for (std::uint32_t y = 0; y < fh; y += 16) {
						const auto* rowb = static_cast<const std::uint8_t*>(fms.pData) + static_cast<std::size_t>(y) * fms.RowPitch;
						for (std::uint32_t x = 0; x < fw; x += 16) {
							float df = 0.0f;
							if (fbpp == 2) {
								std::uint16_t v16 = 0;
								std::memcpy(&v16, rowb + static_cast<std::size_t>(x) * 2, 2);
								df = static_cast<float>(v16) / 65535.0f;
							} else {
								std::uint32_t v32 = 0;
								std::memcpy(&v32, rowb + static_cast<std::size_t>(x) * 4, 4);
								if (fis24) {
									const std::uint32_t dHi = (v32 >> 8) & 0xFFFFFFu;
									const std::uint32_t dLo = v32 & 0xFFFFFFu;
									df = static_cast<float>((std::max)(dHi, dLo)) / 16777215.0f;
								} else
									std::memcpy(&df, &v32, 4);
							}
							if (df < minD) minD = df;
							if (df > maxD) maxD = df;
							if (df > 0.001f && df < 0.999f)
								fcontent++;
							fsamples++;
						}
					}
					a_ctx->Unmap(s_stagingFull, slice);
					char fbuf[64];
					std::snprintf(fbuf, sizeof(fbuf), "%s%u:minD=%.3f/maxD=%.3f c=%u%%", slice == 0 ? "" : " ", slice,
						minD, maxD, fsamples > 0 ? fcontent * 100u / fsamples : 0u);
					fl += fbuf;
				}
				SKSE::log::info("[SLF] {} fullscan0-7: {}", a_label, fl);
			}
		}
		tex->Release();
	}

	// ---------------------------------------------------------------------
	// P1c-2: drive the engine's own per-light shadow render. The vanilla
	// call this hook replaces was the ONLY producer of kSHADOWMAPS depth;
	// skipping it (ctx.Rax=0) leaves every slice empty (RL readback = 0%,
	// verified 14:03). an upstream mod replaces that same call site (100415/107133) and
	// manually calls each scheduled light's BSShadowLight::Render - the
	// engine's per-light shadow render (vtable 0A in the CommonLib fork,
	// same vtable walk the vanilla dispatch performed). We replicate it with
	// OUR scheduler's list.
	//
	// Render arg = 0 for every light (the upstream in-game-verified recipe, sun
	// and points alike). The slice a light renders into is NOT selected by
	// this arg - it comes from the light's descriptor[0].shadowmapIndex
	// (which our scheduler wrote = slot) via the engine's depth-target
	// state. Passing the raw slot number here risks indexing past the
	// light's own descriptor list (size 1-2 in SE) -> OOB/crash; 0 is the
	// only shape proven in-game.
	// ---------------------------------------------------------------------
	// v10-phase2-fix2 (21:55, still flickering after the 21:33 pin fix):
	// SC-A audit showed descs=[0,slot] AFTER Render - the engine OVERWRITES
	// descriptor[0].shadowmapIndex back to 0 during Render, and the draw-time
	// slice selector (GetDepthTargetSubIndex global) does not honor the pin
	// either. Session evidence: 563/672 depth draws on slice 0, slices 8-27
	// never used -> every extended light stomps one map -> blocky flicker.
	// The SelectDepthBuffer hooks are the FINAL say on which DSV the engine
	// binds, so the fix is to remember which light we are Render-ing and
	// force the selector to that light's slot while it is active. dir (sun)
	// is skipped: not forced (vanilla path proven stable).
	static std::atomic<RE::BSShadowLight*> s_renderingLight{ nullptr };
	static std::atomic<std::uint32_t> s_renderingSlot{ 0xFFFFFFFFu };

	// ---------------------------------------------------------------------
	// fix69 (2026-09-09): flicker + sun-state probe. Indoor lamps 'pop
	// on/off' at a fixed angle - the scheduled set churns frame-to-frame
	// (a lamp leaves/enters or two swap order), or a lamp's shadow camera
	// alternates placed/unplaced (camSkip swings), or the accumulator the
	// engine feeds us changes membership. This probe diffs the scheduled
	// set + render outcome against the PREVIOUS dispatch call and logs
	// ONLY changes (event-driven, no per-frame spam), plus a throttled
	// baseline row that also carries the directional (sun) light's full
	// state. The sun is skipped by the render loop (fix64 step3), so this
	// row shows whether the ENGINE still maintains it (camera placed?
	// caster geometry present? lodDimmer alive?) while nobody renders its
	// cascade - data decides whether 'no sunlight' = parameters frozen by
	// the skipped sun render, or the sun never even enters our set.
	// ---------------------------------------------------------------------
	static void FlickerSunProbe(std::uint32_t n, std::uint32_t rendered,
		std::uint32_t dirSkipped, std::uint32_t camSkipped)
	{
		static std::uint32_t s_call = 0;
		const std::uint32_t call = ++s_call;
		static std::array<uintptr_t, 128> s_prevPtr{};
		static std::uint32_t s_prevN = 0;
		static std::uint32_t s_prevRendered = 0;
		static std::uint32_t s_prevCamSkip = 0;

		// fingerprint of the current set (order matters: a swap changes
		// which light owns which shadow slice even when the set is equal)
		std::array<uintptr_t, 128> curPtr{};
		curPtr.fill(0);
		for (std::uint32_t i = 0; i < n && i < curPtr.size(); i++) {
			auto& fs = ShadowLimitFixNS::P1::g_scheduledShadowLights[i];
			curPtr[i] = fs.light ? reinterpret_cast<uintptr_t>(fs.light) : 0;
		}
		const bool countSwung = n != s_prevN || rendered != s_prevRendered ||
			camSkipped != s_prevCamSkip;
		bool orderChanged = false;
		if (!countSwung) {
			for (std::uint32_t i = 0; i < n && i < s_prevPtr.size(); i++) {
				if (curPtr[i] != s_prevPtr[i]) {
					orderChanged = true;
					break;
				}
			}
		}
		if (countSwung || orderChanged) {
			std::string det;
			for (std::uint32_t i = 0; i < n; i++) {
				if (i < s_prevN && curPtr[i] != 0 && curPtr[i] == s_prevPtr[i])
					continue;
				auto& fs = ShadowLimitFixNS::P1::g_scheduledShadowLights[i];
				if (!fs.light)
					continue;
				char fb[150];
				std::snprintf(fb, sizeof(fb), "%s#%u[%s]0x%llx@(%.0f,%.0f,%.0f)",
					det.empty() ? "" : ",", fs.slot, fs.slot < 8u ? "E" : "S",
					static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(fs.light)),
					fs.light->light->world.translate.x,
					fs.light->light->world.translate.y,
					fs.light->light->world.translate.z);
				det += fb;
			}
			SKSE::log::info("[SLF][FLK] call={} set n={}->{} rendered={}->{} dirSkip={} camSkip={}->{}{}",
				call, s_prevN, n, s_prevRendered, rendered, dirSkipped,
				s_prevCamSkip, camSkipped, det.empty() ? "" : (" | " + det));
		}
		s_prevPtr = curPtr;
		s_prevN = n;
		s_prevRendered = rendered;
		s_prevCamSkip = camSkipped;

		// throttled baseline + sun state (every 32nd dispatch call)
		if ((call & 0x1Fu) == 1) {
			std::uint32_t sunSlot = 0xFFFFFFFFu;
			bool sunCamDflt = true;
			bool sunOrtho = false;
			float sfL = 0, sfR = 0, sfT = 0, sfB = 0, sfN = 0, sfF = 0;
			float sx = 0, sy = 0, sz = 0;
			std::uint32_t sunGeom = 0, sunAcc = 0, sunNd = 0, sunIdx0 = 0xFFFFFFFFu;
			float sunLod = -1.0f;
			for (std::uint32_t i = 0; i < n; i++) {
				auto& fs = ShadowLimitFixNS::P1::g_scheduledShadowLights[i];
				if (!fs.light || !fs.light->GetIsDirectionalLight())
					continue;
				sunSlot = fs.slot;
				sunLod = fs.light->lodDimmer;
				sunGeom = static_cast<std::uint32_t>(fs.light->geomList.size());
				auto& frtd = fs.light->GetRuntimeData();
				sunAcc = static_cast<std::uint32_t>(frtd.sceneAccumArray.size());
				auto& fdescs = frtd.shadowmapDescriptors;
				sunNd = static_cast<std::uint32_t>(fdescs.size());
				if (!fdescs.empty()) {
					sunIdx0 = fdescs[0].shadowmapIndex;
					if (fdescs[0].camera) {
						const auto& pfr = fdescs[0].camera->GetRuntimeData2().viewFrustum;
						sunCamDflt = pfr.fLeft == -1.0f && pfr.fRight == 1.0f &&
							pfr.fTop == 1.0f && pfr.fBottom == -1.0f &&
							pfr.fNear == 0.1f && pfr.fFar == 1.0f;
						sunOrtho = pfr.bOrtho != 0;
						sfL = pfr.fLeft; sfR = pfr.fRight; sfT = pfr.fTop;
						sfB = pfr.fBottom; sfN = pfr.fNear; sfF = pfr.fFar;
						const auto& cp = fdescs[0].camera->world.translate;
						sx = cp.x; sy = cp.y; sz = cp.z;
					}
				}
				break;
			}
			if (sunSlot != 0xFFFFFFFFu) {
				SKSE::log::info("[SLF][SUN] call={} n={} rendered={} sun slot={} idx0={} nd={} camDflt={} ortho={} fr(l={:.1f} r={:.1f} t={:.1f} b={:.1f} n={:.1f} f={:.1f}) cpos=({:.0f},{:.0f},{:.0f}) geom={} acc={} lod={:.2f}",
					call, n, rendered, sunSlot, sunIdx0, sunNd, sunCamDflt ? 1 : 0, sunOrtho ? 1 : 0,
					sfL, sfR, sfT, sfB, sfN, sfF, sx, sy, sz, sunGeom, sunAcc, sunLod);
			} else {
				SKSE::log::info("[SLF][SUN] call={} n={} rendered={} NO directional light in scheduled set",
					call, n, rendered);
			}
		}
	}

	// ---------------------------------------------------------------------
	// fix70 (2026-09-09): CS-recipe sun cascade render.
	//
	// The engine dispatch is stopped (Hook_RenderShadowLights sets rax=0),
	// so NOBODY renders the directional (sun) light's cascade depth maps
	// any more - fix69's SUN baseline showed the sun entering the scheduled
	// set outdoors with a LIVE ortho frustum (camDflt=0, engine maintains
	// it) but geom=0 and the render loop skipping it (fix64 step3
	// dirSkip) -> exterior scenes have no sun shadow, and per the user the
	// sunlight is gone too. Community Shaders solves the identical
	// architecture (rax=0 stops the engine walk, LLF renders from its own
	// list) by rendering the sun EXPLICITLY, first, slot 0
	// (ShadowScheduler.cpp ~3167: "sun.Render must be called explicitly...
	// Without this, exterior scenes render with no sun shadow").
	//
	// SLF's own fix48-fix64 sun-render attempts AV'd (corrupt-pointer
	// class) - but those ran under EngineFixes shadow hooks disabled and
	// pre-camDflt descriptor state, and CS proves the recipe sound. Render
	// under SEH: a repeat AV is caught, counted and (after 3) disables the
	// sun render for the session, degrading back to fix68 behavior instead
	// of crashing. __declspec(noinline) + no C++ objects so __try compiles
	// under /EHsc (same pattern as SnapshotLightD0).
	//
	// Returns: 0 = rendered OK, 1 = no directional in scheduled set,
	//          2 = camera not placed (engine not maintaining the sun yet),
	//          3 = AV caught.
	//
	// fix71 (2026-09-09, 22:5x): fix70's first REAL sun render froze the
	// whole game outdoors right after a world-switch resume (22:53:33
	// session - log ends at "fix45 diag: dispatch n=1", no fix45
	// pre-render row => the single scheduled light was the sun and the
	// hang is inside sun->Render). Diagnosis: the point-light loop always
	// pins every descriptor + publishes s_renderingLight/s_renderingSlot
	// so the SelectDepthBuffer hooks route the shadow draws to a valid
	// extended DSV slot; fix70's sun render did NEITHER, so SelectDSB ran
	// act=0 with the engine's stale sub-index global (post-world-switch
	// garbage / out-of-range slot) and OMSet bound an invalid/null DSV ->
	// GPU-side deadlock, frozen frame, no exception (SEH can't catch it).
	// Fix: render the sun through the SAME verified context as a point
	// light - pin descs to the engine-assigned sun slot (idx0, must be in
	// range), publish s_renderingLight/s_renderingSlot so SelectDSB forces
	// the canvas, clear afterwards, and log begin/end around Render so a
	// repeat hang pinpoints the exact statement (first 32 real renders
	// log every frame).
	static __declspec(noinline) std::uint32_t RenderSunCascadeSeh()
	{
		__try {
			const std::uint32_t n = ShadowLimitFixNS::P1::g_scheduledShadowCount.load(std::memory_order_acquire);
			for (std::uint32_t i = 0; i < n && i < 4; i++) {
				auto& s = ShadowLimitFixNS::P1::g_scheduledShadowLights[i];
				if (!s.light || !s.light->GetIsDirectionalLight())
					continue;
				// Same placement fence as the point-light loop (fix64 step3c):
				// a default unit-box frustum means the engine has not placed
				// the sun's shadow camera this frame - rendering now would
				// rasterize nothing (or spin). fix69 shows camDflt=0 with a
				// live ortho frustum outdoors, so this is normally passable.
				auto& rtd = s.light->GetRuntimeData();
				if (rtd.shadowmapDescriptors.empty() || !rtd.shadowmapDescriptors[0].camera)
					return 2;
				const auto& fr = rtd.shadowmapDescriptors[0].camera->GetRuntimeData2().viewFrustum;
				const bool dflt = fr.fLeft == -1.0f && fr.fRight == 1.0f &&
					fr.fTop == 1.0f && fr.fBottom == -1.0f &&
					fr.fNear == 0.1f && fr.fFar == 1.0f;
				if (dflt)
					return 2;
				// fix71: the sun's descriptor carries the engine-assigned
				// shadow canvas (idx0). Garbage/out-of-range after a
				// world-switch = do not render (SelectDSB would route to an
				// invalid DSV). The extended arrays hold 128 entries but only
				// `count` were created; anything >= count is a null/partial
				// DSV - cap to the safe engine band (0-7 vanilla mirrors are
				// always created) unless a valid descriptor index exists.
				const std::uint32_t idx0 = static_cast<std::uint32_t>(rtd.shadowmapDescriptors[0].shadowmapIndex);
				if (idx0 >= ShadowLimitFixNS::P1::g_scheduledShadowCount.load(std::memory_order_acquire) &&
					idx0 >= 8u)
					return 2;
				// fix72 (2026-09-09): CS SetupSunLight recipe - the sun's
				// caster geometry is ONLY collected inside the engine's
				// CalculateActiveShadowCasters (func, the single vtable09
				// call in uid107137) under an internal phase gate; with our
				// rax=0 render stop the engine never reaches that state and
				// fix69 measured the sun at geom=0/acc=0. CS accumulates the
				// sun itself every frame right before rendering it; do the
				// same here (only when empty - a double same-frame
				// Accumulate is the fail-fast crash RegisterEngineAccumLights
				// guards against). Gate on shaderAccumulator readiness like
				// the v4 crash fix. If the collection STILL comes up empty
				// the directional Render would rasterize against a
				// half-built caster set and spin (the fix70/fix71 freezes) -
				// skip instead of rendering blind.
				if (rtd.sceneAccumArray.empty()) {
					bool accReady = !rtd.shadowmapDescriptors.empty();
					for (auto& d : rtd.shadowmapDescriptors)
						if (d.shaderAccumulator.get() == nullptr)
							accReady = false;
					const std::uint32_t smc = static_cast<std::uint32_t>(s.light->shadowMapCount);
					if (accReady && smc <= rtd.shadowmapDescriptors.size()) {
						s.light->Accumulate(*GetAccumLightSlotCount(), 0, nullptr);
						static std::uint32_t s_sunAccLog = 0;
						if ((s_sunAccLog++ & 0x3Fu) == 0)
							SKSE::log::info("[SLF] fix72 sun: Accumulate filled acc={} geom={} idx0={}",
								static_cast<std::uint32_t>(rtd.sceneAccumArray.size()),
								static_cast<std::uint32_t>(s.light->geomList.size()), idx0);
					} else {
						static std::uint32_t s_sunAccDefer = 0;
						if ((s_sunAccDefer++ & 0x3Fu) == 0)
							SKSE::log::info("[SLF] fix72 sun: Accumulate deferred (accReady={} descs={} smc={})",
								accReady ? 1 : 0,
								static_cast<std::uint32_t>(rtd.shadowmapDescriptors.size()), smc);
					}
				}
				if (rtd.sceneAccumArray.empty() && s.light->geomList.empty()) {
					static std::uint32_t s_sunEmptyLog = 0;
					if ((s_sunEmptyLog++ & 0x3Fu) == 0)
						SKSE::log::info("[SLF] fix72 sun: no casters after Accumulate (acc={} geom={}) - render skipped",
							static_cast<std::uint32_t>(rtd.sceneAccumArray.size()),
							static_cast<std::uint32_t>(s.light->geomList.size()));
					return 2;
				}
				// Publish the render context exactly like the point-light
				// loop does, so SelectDepthBuffer1/2 force the canvas to the
				// sun's slot instead of trusting the engine's stale global.
				for (auto& d : rtd.shadowmapDescriptors)
					d.shadowmapIndex = idx0;
				s_renderingLight.store(s.light, std::memory_order_relaxed);
				s_renderingSlot.store(idx0, std::memory_order_relaxed);
				// Render arg 0 = the in-game-verified recipe shape (sun and
				// points alike; the slice comes from descriptor shadowmapIndex).
				std::uint32_t idx = 0;
				static std::uint32_t s_sunRenders = 0;
				const bool dbg = (++s_sunRenders <= 32u);
				if (dbg)
					SKSE::log::info("[SLF] fix72 sun: Render begin #{} slot={} idx0={} acc={} geom={} cam=0x{:x}",
						s_sunRenders, s.slot, idx0,
						static_cast<std::uint32_t>(rtd.sceneAccumArray.size()),
						static_cast<std::uint32_t>(s.light->geomList.size()),
						reinterpret_cast<uintptr_t>(rtd.shadowmapDescriptors[0].camera.get()));
				s.light->Render(idx);
				if (dbg)
					SKSE::log::info("[SLF] fix72 sun: Render end ok idx0={}",
						static_cast<std::uint32_t>(rtd.shadowmapDescriptors[0].shadowmapIndex));
				s_renderingLight.store(nullptr, std::memory_order_relaxed);
				s_renderingSlot.store(0xFFFFFFFFu, std::memory_order_relaxed);
				return 0;
			}
			return 1;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			// fix71: never leave the SelectDSB context published after an AV.
			s_renderingLight.store(nullptr, std::memory_order_relaxed);
			s_renderingSlot.store(0xFFFFFFFFu, std::memory_order_relaxed);
			return 3;
		}
	}

	static void RenderScheduledShadowLightsDispatch()
	{
		// fix46 (2026-09-08): the world-switch gate zeroes the scheduled
		// list the moment it freezes (see Scheduler.cpp freeze()) and this
		// hook must NOT render anything while the gate is frozen either -
		// on a no-load-menu cell transition (outdoor boundary walk /
		// teleport / camera jump) this hook keeps firing every frame while
		// the scheduler fill side is gated, and a list that was NOT yet
		// zeroed (or a stale local n from before the freeze) could render a
		// light the cell unload just released -> clean exit with no dump
		// (00:04:14 session, died ~1s after the camera-jump gate fired).
		// The flag is also kept true across the fix45 resume grace (the
		// scheduler learns the fresh accumulator while we stay parked).
		if (ShadowLimitFixNS::P1::g_shadowWritesFrozen.load(std::memory_order_acquire)) {
			static std::uint32_t s_frozenLog = 0;
			if ((s_frozenLog++ & 0xFFu) == 0)
				SKSE::log::info("[SLF] fix46 dispatch parked (world-switch gate frozen)");
			return;
		}

		const std::uint32_t n = ShadowLimitFixNS::P1::g_scheduledShadowCount.load(std::memory_order_acquire);
		// fix45 (2026-09-08): resume grace. After a world-switch gate
		// cooldown ends, the scheduler re-learns the engine accumulator
		// immediately but the per-light Render state was just rebuilt by
		// the load; the FIRST post-resume dispatch froze inside one shadow
		// pass (23:46 session, OMSet stopped at 3490 while draws raced
		// ~60k/s for 20+s). Park the dispatch while the grace counter is
		// armed (scheduler still fills the list each frame), then for the
		// first 16 live frames log every per-light Render so a repeat
		// freeze pinpoints the exact light.
		static std::uint32_t s_afterGrace = 0;
		{
			const std::uint32_t g = ShadowLimitFixNS::P1::g_resumeGrace.load(std::memory_order_acquire);
			if (g > 0) {
				if (g == 96)
					SKSE::log::info("[SLF] fix45 resume grace: manual dispatch parked {} ticks (load-settled render state)", g);
				ShadowLimitFixNS::P1::g_resumeGrace.store(g - 1, std::memory_order_release);
				s_afterGrace = 16;  // diagnostic window once we come back live
				return;
			}
		}
		const bool diagLights = s_afterGrace > 0;
		if (diagLights && (--s_afterGrace == 15))
			SKSE::log::info("[SLF] fix45 grace over - dispatch live, per-light diagnostic for {} frames", 16u);
		if (diagLights)
			SKSE::log::info("[SLF] fix45 diag: dispatch n={}", n);

		static std::uint32_t s_dispatchFrame = 0;
		const bool logNow = (++s_dispatchFrame & 0x3Fu) == 0;

		if (n == 0) {
			// Distinguishes "dispatch hook runs but scheduler produced 0"
			// (ordering/empty scene) from "hook never fires" - both look
			// identical in the shadow content readback.
			if (logNow)
				SKSE::log::info("[SLF] P1c-2 dispatch: 0 scheduled lights (scheduler empty/early-out)");
			return;
		}

		if (logNow) {
			// Pre-render accumulation health (v3): the scheduler now runs the
			// engine's own per-light Accumulate (cull walk) right after
			// slotting. geomList = attached caster meshes (what Render draws);
			// sceneAccumArray = engine accum list. Both >0 = the light has
			// geometry and Render should produce depth. All 0 = accumulate
			// still not reaching the light (descriptor/culling missing).
			for (std::uint32_t i = 0; i < n && i < 8; i++) {
				auto& s = ShadowLimitFixNS::P1::g_scheduledShadowLights[i];
				if (!s.light)
					continue;
				auto& rtd = s.light->GetRuntimeData();
				const std::uint32_t acc = static_cast<std::uint32_t>(rtd.sceneAccumArray.size());
				const std::uint32_t geom = static_cast<std::uint32_t>(s.light->geomList.size());
				const std::uint32_t nDesc = static_cast<std::uint32_t>(rtd.shadowmapDescriptors.size());
				void* shaderAcc = nDesc > 0 ? static_cast<void*>(rtd.shadowmapDescriptors[0].shaderAccumulator.get()) : nullptr;
				void* cam = nDesc > 0 ? static_cast<void*>(rtd.shadowmapDescriptors[0].camera.get()) : nullptr;
				// fix19b: PRE-render frustum state (mirrors the fix17b [CN]
				// post-render census). [CN] DEF=1 on nearly every light left
				// open whether the ENGINE scheduler placed no camera (static
				// lights skip engine UpdateCamera) or something in OUR
				// render/reset cycle degrades it. This line runs BEFORE our
				// Render -> pre vs [CN] post diff localizes the culprit.
				char preCam[200];
				bool preDflt = true;
				if (nDesc > 0 && rtd.shadowmapDescriptors[0].camera) {
					const auto& pfr = rtd.shadowmapDescriptors[0].camera->GetRuntimeData2().viewFrustum;
					const auto& pw2c = rtd.shadowmapDescriptors[0].camera->GetRuntimeData().worldToCam;
					preDflt = pfr.fLeft == -1.0f && pfr.fRight == 1.0f &&
						pfr.fTop == 1.0f && pfr.fBottom == -1.0f &&
						pfr.fNear == 0.1f && pfr.fFar == 1.0f;
					std::snprintf(preCam, sizeof(preCam), "DEF=%d w2c0=(%.2f,%.2f,%.2f)",
						preDflt ? 1 : 0, pw2c[0][0], pw2c[0][1], pw2c[0][2]);
				} else {
					std::snprintf(preCam, sizeof(preCam), "nocam");
				}
				SKSE::log::info("[SLF][PRE] slot {} [{}] dyn={} sceneAccum={} geom={} descs={} shaderAcc=0x{:x} cam=0x{:x} {}",
					s.slot, s.slot < 8u ? "ENG" : "SLF", s.light->dynamic ? 1 : 0, acc, geom, nDesc,
					reinterpret_cast<uintptr_t>(shaderAcc), reinterpret_cast<uintptr_t>(cam), preCam);
			}
		}

#if SLF_SELFCHECK
		// SC-C: QPC around the whole per-light render loop. Read-only; the
		// only cost is two counter reads per frame (sub-microsecond).
		static double s_scMs = 0.0;
		LARGE_INTEGER scF, scA, scB;
		QueryPerformanceFrequency(&scF);
		QueryPerformanceCounter(&scA);
#endif
		std::uint32_t rendered = 0;
		std::uint32_t dirSkipped = 0;
		std::uint32_t camSkipped = 0;
		std::array<std::uint32_t, 128> postIdx{};
		postIdx.fill(0xFFFFFFFFu);

		// fix70 (2026-09-09): render the sun's cascade FIRST, CS recipe
		// (see RenderSunCascadeSeh). fix64 step3 skipped the directional
		// light entirely (dirSkip below stays as the second fence so the
		// point-light loop never double-renders it). 3 consecutive AVs
		// disable the sun render for the session -> degrades to fix68.
		{
			static bool s_sunBroken = false;
			static std::uint32_t s_sunAv = 0;
			static std::uint32_t s_sunLog = 0;
			if (!s_sunBroken) {
				const std::uint32_t sunCode = RenderSunCascadeSeh();
				if (sunCode == 0) {
					rendered++;
					if (((s_sunLog++) & 0x3Fu) == 0)
						SKSE::log::info("[SLF] fix70 sun cascade rendered OK (count={} slot0 first pass)",
							ShadowLimitFixNS::P1::g_scheduledShadowCount.load(std::memory_order_acquire));
				} else if (sunCode == 3) {
					if (++s_sunAv >= 3) {
						s_sunBroken = true;
						SKSE::log::error("[SLF] fix70 sun Render AV x3 - sun render DISABLED for this session (fix68 behavior)");
					} else {
						SKSE::log::error("[SLF] fix70 sun Render AV #{} (caught by SEH) - sun render still attempted next frame", s_sunAv);
					}
				} else if (logNow) {
					SKSE::log::info("[SLF] fix70 sun skip code={} (1=no-dir 2=cam-not-placed 3=AV)", sunCode);
				}
			}
		}

		for (std::uint32_t i = 0; i < n; i++) {
			auto& s = ShadowLimitFixNS::P1::g_scheduledShadowLights[i];
			if (!s.light)
				continue;
			// fix64 step3 (2026-09-09), amended fix70: the sun is rendered
			// by the fix70 pre-loop block (RenderSunCascadeSeh, CS recipe)
			// and must NEVER render through this point-light loop - keep the
			// skip as the second fence against double-render. The fix48-fix64
			// AV history below is why the pre-loop render is SEH-guarded with
			// a 3-strike disable; fix70 re-tests the sun render under the
			// current (EngineFixes stock, camDflt-gated) state because CS
			// renders the sun explicitly in the same rax=0 architecture.
			if (s.light->GetIsDirectionalLight()) {
				dirSkipped++;
				continue;
			}
			// v10-phase2-fix (SC-A 21:33): engine Render selects the depth
			// slice from the light's descriptor shadowmapIndex, but for
			// slot>=8 lights that field read 0 at render time (engine
			// UpdateCamera/Accumulate zeroed it after our scheduler wrote
			// slot) -> every extended light rasterized into slice 0,
			// overwriting each other AND the engine's own slot-0 light:
			// blocky, flickering shadows. Pin EVERY descriptor to our slot
			// immediately before Render so each light owns its slice. A
			// dual-paraboloid light has 2 descriptors (front/back half) that
			// MUST share one shadowmapIndex (they split one map via their
			// port rects) - CS's render hook copies descs[0]->descs[1] for
			// exactly this reason (ShadowEngineHooks.cpp ~648). Pinning all
			// of them is the same thing in one step. dir (sun) skipped: not
			// rendered through this list and its descriptor layout differs.
			if (!s.light->GetIsDirectionalLight()) {
				auto& rtd = s.light->GetRuntimeData();
				for (auto& d : rtd.shadowmapDescriptors)
					d.shadowmapIndex = s.slot;
				// fix2: the engine ignores the descriptor field at draw time
				// (sub-index global), so ALSO announce this light to the
				// SelectDepthBuffer hooks, which force the slice to s.slot.
				s_renderingLight.store(s.light, std::memory_order_relaxed);
				s_renderingSlot.store(s.slot, std::memory_order_relaxed);
			}
			std::uint32_t idx = 0;  // verified-recipe arg (see comment above)
			if (diagLights) {
				auto& drtd = s.light->GetRuntimeData();
				SKSE::log::info("[SLF] fix45 pre-render #{} slot={} dyn={} acc={} geom={} nd={} idx0={}",
					i, s.slot, s.light->dynamic ? 1 : 0,
					static_cast<std::uint32_t>(drtd.sceneAccumArray.size()),
					static_cast<std::uint32_t>(s.light->geomList.size()),
					static_cast<std::uint32_t>(drtd.shadowmapDescriptors.size()),
					drtd.shadowmapDescriptors.empty() ? -1 :
						static_cast<int32_t>(drtd.shadowmapDescriptors[0].shadowmapIndex));
			}
			// fix64 step3c (2026-09-09): camDflt render-safety fence (the
			// fix51 gate, re-applied - the fix46 rollback dropped it and the
			// 16:50 session froze with slice-0 draws racing 162M: a light
			// whose shadow camera is still the engine's default unit box
			// (unplaced - post-load/cell-switch) makes engine Render spin on
			// the zero-size frustum). Skip until the engine UpdateCamera
			// places it. This is independent of the sun saga - pure render
			// safety for point lights.
			{
				auto& rtg = s.light->GetRuntimeData();
				const bool noDesc = rtg.shadowmapDescriptors.empty() ||
					!rtg.shadowmapDescriptors[0].camera;
				bool camDflt = true;
				if (!noDesc) {
					const auto& fr = rtg.shadowmapDescriptors[0].camera->GetRuntimeData2().viewFrustum;
					camDflt = fr.fLeft == -1.0f && fr.fRight == 1.0f &&
						fr.fTop == 1.0f && fr.fBottom == -1.0f &&
						fr.fNear == 0.1f && fr.fFar == 1.0f;
				}
				if (noDesc || camDflt) {
					camSkipped++;
					static std::uint32_t s_camSkipLog = 0;
					if ((s_camSkipLog++ & 0x3Fu) == 0)
						SKSE::log::info("[SLF] camDflt skip light#{} slot={} (shadow camera not placed)",
							i, s.slot);
					continue;
				}
			}
			s.light->Render(idx);   // engine virtual: draws this light's shadows
			if (diagLights)
				SKSE::log::info("[SLF] fix45 post-render #{} slot={} ok", i, s.slot);
			// fix19b: snapshot the descriptor slice Render left behind. The
			// re-pin loop below overwrites it, so THIS is the only moment the
			// engine's own write is observable. postIdx != s.slot means Render
			// internally re-selected the slice (e.g. wrote 0) -> it rasterized
			// into a DIFFERENT slice than the one our SelectDepthBuffer pin
			// aimed at, or the engine slot state machine advanced.
			{
				auto& prtd = s.light->GetRuntimeData();
				const auto& pdescs = prtd.shadowmapDescriptors;
				postIdx[i] = pdescs.empty() ? 0xFFFFFFFFu : static_cast<std::uint32_t>(pdescs[0].shadowmapIndex);
			}
			s_renderingLight.store(nullptr, std::memory_order_relaxed);
			s_renderingSlot.store(0xFFFFFFFFu, std::memory_order_relaxed);
			rendered++;
		}

		if (logNow) {
			// fix19b: post-render slice vs pinned slot, same 64-frame cadence
			// as [PRE] above so the rows correlate. "=S" = Render kept our
			// pin (good); a number = Render rewrote the slice to that value.
			std::string pr;
			for (std::uint32_t i = 0; i < n && i < 8; i++) {
				auto& s = ShadowLimitFixNS::P1::g_scheduledShadowLights[i];
				if (!s.light)
					continue;
				const std::uint32_t pi = postIdx[i];
				char pb[48];
				std::snprintf(pb, sizeof(pb), "%s[%u]", pr.empty() ? "" : " ", s.slot);
				pr += pb;
				if (pi == 0xFFFFFFFFu)
					pr += "=E";
				else if (pi == s.slot)
					pr += "=S";
				else
					pr += "=" + std::to_string(pi);
			}
			SKSE::log::info("[SLF][POST] rendered {} lights, post-render desc idx:{}", rendered, pr);
		}

		// fix69: frame-to-frame flicker/sun probe (event-driven, see def).
		FlickerSunProbe(n, rendered, dirSkipped, camSkipped);

		// fix3 (22:3x): the engine's per-light Render RESETS
		// descriptor[0].shadowmapIndex on extended (slot >= 8) lights - the
		// SC-A audit after the loop reads idx=[0,8] for slot-8 lights while
		// engine-owned slot 0-3 lights keep their index (they are in the
		// engine's own shadow list). That field is ALSO what the lighting
		// pass reads to sample this light's shadow from the t103 array, so
		// extended lights ended up sampling slice 0 = engine light #0's map:
		// blocky misattributed shadows that flicker as the engine re-zeroes
		// the field at a frame-varying time. Re-pin EVERY scheduled light's
		// descriptors to its own slot right after the render loop (and
		// before the lighting pass of this frame consumes them). dir (sun)
		// skipped: not in this list and its descriptor layout differs.
		for (std::uint32_t i = 0; i < n; i++) {
			auto& s = ShadowLimitFixNS::P1::g_scheduledShadowLights[i];
			if (!s.light || s.light->GetIsDirectionalLight())
				continue;
			auto& rtd = s.light->GetRuntimeData();
			for (auto& d : rtd.shadowmapDescriptors)
				d.shadowmapIndex = s.slot;
		}

		// fix17b (2026-09-04): full per-light shadow-camera census (the old
		// v6.2-diag block only printed light#0). Every 128th dispatch frame,
		// AFTER Render + re-pin, enumerate EVERY scheduled light: does its
		// descriptor camera carry a PLACED frustum or the engine's default
		// unit box (l=-1 r=1 t=1 b=-1 n=0.1 f=1 ortho=1, DEF=1 below)?
		// A default-box camera makes Render rasterize ~0 pixels, so the DEF
		// count = exactly how many of the n lights CANNOT produce depth this
		// frame. w2c0 = worldToCam[0][0..2] (still (1,0,0) = view matrix
		// never rebuilt = camera never placed). Answers (a) is the all-0%
		// readback caused by unplaced cameras and (b) which lights - static
		// (engine UpdateCamera 0x151AC50 gate B skips dynamic==0 lights) vs
		// dynamic - need placement in the next fix.
		{
			static std::uint32_t s_censusFrame = 0;
			if ((s_censusFrame++ & 0x7Fu) == 0) {
				for (std::uint32_t i = 0; i < n; i++) {
					auto& cs = ShadowLimitFixNS::P1::g_scheduledShadowLights[i];
					if (!cs.light)
						continue;
					auto& crtd = cs.light->GetRuntimeData();
					const auto& cdescs = crtd.shadowmapDescriptors;
					const std::uint32_t cAcc = static_cast<std::uint32_t>(crtd.sceneAccumArray.size());
					const std::uint32_t cGeom = static_cast<std::uint32_t>(cs.light->geomList.size());
					const std::uint32_t cNd = static_cast<std::uint32_t>(cdescs.size());
					char crow[360];
					if (!cdescs.empty() && cdescs[0].camera) {
						const auto& cd0 = cdescs[0];
						const auto& pfr = cd0.camera->GetRuntimeData2().viewFrustum;
						const auto& w2c = cd0.camera->GetRuntimeData().worldToCam;
						const bool dflt = pfr.fLeft == -1.0f && pfr.fRight == 1.0f &&
							pfr.fTop == 1.0f && pfr.fBottom == -1.0f &&
							pfr.fNear == 0.1f && pfr.fFar == 1.0f;
						std::snprintf(crow, sizeof(crow),
							"[SLF][CN] #%u slot=%u [%s] dyn=%d%s nd=%u idx0=%d acc=%u geom=%u DEF=%d frm(l=%.1f r=%.1f t=%.1f b=%.1f n=%.1f f=%.1f o=%d) w2c0=(%.2f,%.2f,%.2f) cpos=(%.1f,%.1f,%.1f)",
							i, cs.slot, cs.slot < 8u ? "ENG" : "SLF", cs.light->dynamic ? 1 : 0,
							cs.light->GetIsDirectionalLight() ? "d" :
								(cs.light->GetIsParabolicLight() ? "p" :
								(cs.light->GetIsFrustumLight() ? "f" : "?")),
							cNd, cdescs[0].shadowmapIndex, cAcc, cGeom, dflt ? 1 : 0,
							pfr.fLeft, pfr.fRight, pfr.fTop, pfr.fBottom,
							pfr.fNear, pfr.fFar, pfr.bOrtho ? 1 : 0,
							w2c[0][0], w2c[0][1], w2c[0][2],
							cd0.camera->world.translate.x,
							cd0.camera->world.translate.y,
							cd0.camera->world.translate.z);
					} else {
						std::snprintf(crow, sizeof(crow),
							"[SLF][CN] #%u slot=%u [%s] dyn=%d%s nd=%u NO-CAM acc=%u geom=%u",
							i, cs.slot, cs.slot < 8u ? "ENG" : "SLF", cs.light->dynamic ? 1 : 0,
							cs.light->GetIsDirectionalLight() ? "d" :
								(cs.light->GetIsParabolicLight() ? "p" :
								(cs.light->GetIsFrustumLight() ? "f" : "?")),
							cNd, cAcc, cGeom);
					}
					SKSE::log::info("{}", crow);
				}
			}
		}
#if SLF_SELFCHECK
		QueryPerformanceCounter(&scB);
		s_scMs += 1000.0 * static_cast<double>(scB.QuadPart - scA.QuadPart) /
			static_cast<double>(scF.QuadPart);
#endif

		if (logNow) {
			SKSE::log::info("[SLF] P1c-2 manual render dispatch: {} of {} scheduled lights", rendered, n);
			for (std::uint32_t i = 0; i < n && i < 4; i++) {
				auto& s = ShadowLimitFixNS::P1::g_scheduledShadowLights[i];
				if (!s.light)
					continue;
				const char* cls = s.light->GetIsDirectionalLight() ? "dir" :
					(s.light->GetIsParabolicLight() ? "para" :
					(s.light->GetIsOmniLight() ? "omni" : "frustum"));
				SKSE::log::info("[SLF]   slot {} type {}", s.slot, cls);
			}
#if SLF_SELFCHECK
			// -----------------------------------------------------------------
			// SC-A: slice/slot consistency audit (fix 21:33). Engine Render's
			// depth slice = descriptor[0].shadowmapIndex, which the dispatch
			// loop pins to each light's scheduled slot. Audit flags: (a) a
			// light whose descs[0].shadowmapIndex != its slot (renders into
			// another light's map -> overwrite/flicker, the 21:33 all-slice-0
			// bug) and (b) two lights mapped to one slice. descriptor[1] of a
			// dual-paraboloid light is deliberately NOT audited - it shares
			// descriptor[0]'s map via its port rect (0 in vanilla = normal).
			// Read-only; never mutates descriptor state.
			// -----------------------------------------------------------------
			std::uint32_t idxOwner[128];
			std::uint32_t idxCount[128];
			std::fill(std::begin(idxOwner), std::end(idxOwner), 0xFFFFFFFFu);
			std::fill(std::begin(idxCount), std::end(idxCount), 0u);
			for (std::uint32_t i = 0; i < n; i++) {
				auto& s = ShadowLimitFixNS::P1::g_scheduledShadowLights[i];
				if (!s.light)
					continue;
				const char* cls = s.light->GetIsDirectionalLight() ? "dir" :
					(s.light->GetIsParabolicLight() ? "para" :
					(s.light->GetIsOmniLight() ? "omni" : "frustum"));
				auto& rtd = s.light->GetRuntimeData();
				const auto nIdx = rtd.shadowmapDescriptors.size();
				if (nIdx == 0)
					continue;
				const std::uint32_t smc = static_cast<std::uint32_t>(s.light->shadowMapCount);
				std::string idxStr;
				std::uint32_t di = 0;
				for (auto& d : rtd.shadowmapDescriptors) {
					const std::uint32_t ix = static_cast<std::uint32_t>(d.shadowmapIndex);
					char ib[16];
					std::snprintf(ib, sizeof(ib), "%s%u", idxStr.empty() ? "" : ",", ix);
					idxStr += ib;
					// Only descriptor[0] owns the depth slice. descriptor[1] of
					// a dual-paraboloid light shares descriptor[0]'s map via
					// its port rect and stays 0 in vanilla - counting it here
					// would false-flag every para light as "sharing slice 0".
					if (di == 0 && ix < 128) {
						if (idxCount[ix] == 0)
							idxOwner[ix] = s.slot;
						idxCount[ix]++;
					}
					di++;
				}
				SKSE::log::info("[SLF][SC-A] slot {} type={} smc={} descs={} idx=[{}]",
					s.slot, cls, smc, nIdx, idxStr);
				// fix2 (21:55): the descs[0]!=slot WARN here was REMOVED - it
				// audited post-Render state and the engine legitimately
				// resets descriptor[0].shadowmapIndex to 0 during Render
				// (audit ran after the render loop, so it always fired and
				// said "0 != slot" even though the draw was correctly pinned).
				// The real draw slice is now forced + reported by the
				// SelectDSB hooks (sub=engine->slot), which is authoritative.
			}
			for (std::uint32_t ix = 0; ix < 128; ix++) {
				if (idxCount[ix] > 1)
					SKSE::log::warn("[SLF][SC-A] WARN slice {} shared by {} scheduled lights (incl. slot {}) - later render overwrites earlier",
						ix, idxCount[ix], idxOwner[ix]);
			}

			// -----------------------------------------------------------------
			// SC-B: per-light shadow-camera placement (fix 21:33). The shadow
			// camera is the light's real placement (engine UpdateCamera sits it
			// at the light; BSLight::worldTranslate is unmaintained, all-zero).
			// Flags: a camera still at world origin (never placed), a still
			// all-zero frustum (Render rasterizes 0 px), or two scheduled
			// lights whose cameras coincide (shadow frusta stacked -> all
			// shadows from one spot). dir (sun) skipped: its ortho camera
			// orbits the scene, not the light. Read-only.
			// -----------------------------------------------------------------
			// SC-B fix (21:33): BSLight::worldTranslate is NOT maintained by
			// the engine (read all-zero for every light -> false WARN spam).
			// The shadow camera IS the light's real placement (engine
			// UpdateCamera puts it at the light). So instead of a
			// light-vs-cam distance check, flag the two things that actually
			// break shadows here: (1) camera still at world origin = never
			// placed; (2) two scheduled lights whose cameras coincide =
			// their shadow frusta are stacked (all shadows from one spot).
			float seenCam[128][3];
			std::uint32_t nSeen = 0;
			for (std::uint32_t i = 0; i < n; i++) {
				auto& s = ShadowLimitFixNS::P1::g_scheduledShadowLights[i];
				if (!s.light)
					continue;
				const char* cls = s.light->GetIsDirectionalLight() ? "dir" :
					(s.light->GetIsParabolicLight() ? "para" :
					(s.light->GetIsOmniLight() ? "omni" : "frustum"));
				auto& rtd = s.light->GetRuntimeData();
				if (rtd.shadowmapDescriptors.empty())
					continue;
				const auto& cdx = rtd.shadowmapDescriptors[0];
				if (!cdx.camera)
					continue;
				const auto& cpos = cdx.camera->world.translate;
				const auto& pfr = cdx.camera->GetRuntimeData2().viewFrustum;
				const bool zeroFrustum = pfr.fLeft == 0.0f && pfr.fRight == 0.0f &&
					pfr.fTop == 0.0f && pfr.fBottom == 0.0f &&
					pfr.fNear == 0.0f && pfr.fFar == 0.0f;
				const float camDist = std::sqrt(cpos.x * cpos.x + cpos.y * cpos.y + cpos.z * cpos.z);
				SKSE::log::info("[SLF][SC-B] slot {} type={} cam=({:.0f},{:.0f},{:.0f}) dOrigin={:.1f} frustum(l={:.1f} r={:.1f} t={:.1f} b={:.1f} n={:.1f} f={:.1f} ortho={})",
					s.slot, cls, cpos.x, cpos.y, cpos.z, camDist,
					pfr.fLeft, pfr.fRight, pfr.fTop, pfr.fBottom, pfr.fNear, pfr.fFar,
					pfr.bOrtho ? 1 : 0);
				bool stacked = false;
				for (std::uint32_t k = 0; k < nSeen; k++) {
					const float dx = seenCam[k][0] - cpos.x;
					const float dy = seenCam[k][1] - cpos.y;
					const float dz = seenCam[k][2] - cpos.z;
					if (dx * dx + dy * dy + dz * dz < 1.0f)
						stacked = true;
				}
				if (nSeen < 128) {
					seenCam[nSeen][0] = cpos.x;
					seenCam[nSeen][1] = cpos.y;
					seenCam[nSeen][2] = cpos.z;
					nSeen++;
				}
				if (!s.light->GetIsDirectionalLight() && (zeroFrustum || camDist < 1.0f || stacked))
					SKSE::log::warn("[SLF][SC-B] WARN slot {} type {} zeroFrustum={} atOrigin={} camStacked={}",
						s.slot, cls, zeroFrustum ? 1 : 0, camDist < 1.0f ? 1 : 0, stacked ? 1 : 0);
			}

			// SC-C: dispatch cost summary + reset accumulator.
			SKSE::log::info("[SLF][SC-C] dispatch avg {:.2f} ms/frame over 64 frames, {} lights ({:.3f} ms/light)",
				s_scMs / 64.0, n, s_scMs / 64.0 / static_cast<double>(n));
			s_scMs = 0.0;
#endif
		}
	}

	static void Hook_RenderShadowLights(CONTEXT& ctx)
	{
		static uint32_t s_renderCount = 0;
		static uint32_t s_frame = 0;
		s_renderCount++;

		if ((++s_frame & 0x3Fu) == 0) {
			SKSE::log::info("[SLF] render loop: {} shadow renders / {} frames, engine slot counter = {}",
				s_renderCount, 64u, *GetAccumLightSlotCount());
			s_renderCount = 0;
		}
#if SLF_MANUAL_RENDER
		// P1c-2: produce the shadow depth the vanilla dispatch used to.
		// Must run BEFORE the readback below so RL% reflects this frame's
		// rendered content (not last frame's, which is stale).
		RenderScheduledShadowLightsDispatch();
#endif
		// Read the shadow array right here, right after our manual render
		// produced this frame's depth (throttled to every 32nd hit inside
		// the readback fn). Compare vs the MAT-pass readback:
		//   RL > 0%, MAT-pass ~ 0%  -> shadows ARE rendered but get
		//                               cleared/overwritten before the
		//                               material pass samples t103
		//   RL ~ 0%, MAT-pass ~ 0%  -> the manual dispatch never writes
		//                               the slices the readback sees
		//                               (wrong DSV texture/slice/camera)
		// fix17 (2026-09-04): re-enabled. v8-exp2 disabled this because the
		// crash then implicated hot-path D3D writes while the ENGINE
		// dispatch was running; since v10 the dispatch is OURS
		// (RenderScheduledShadowLightsDispatch) and SLF_PS_ENABLED=0 keeps
		// the material pass fully vanilla - the readback is the only tool
		// that can separate "rendered but cleared before MAT" from "never
		// rendered into the visible texture" (fix15 MAT log: all 0%).
		if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
			auto* rctx = reinterpret_cast<::ID3D11DeviceContext*>(renderer->GetRuntimeData().context);
			if (rctx)
				UpdateB2bLightRec(rctx);  // SLF-B B4a: push this frame's LightRec
		}
#if SLF_SKIP_VANILLA_DISPATCH
		ctx.Rax = 0;
#endif
	}

	// ---------------------------------------------------------------------
	// P1b: overwrite engine's shadowMapIndex writes with our slot index.
	// ---------------------------------------------------------------------
	static void Hook_OverwriteShadowMapIndex(CONTEXT& ctx)
	{
		_Unreferenced_parameter_(ctx);
	}

	// ---------------------------------------------------------------------
	// P1b: extended depth-buffer arrays + helpers (real implementation,
	// cross-verified against an upstream shadow-engine hooks reference). Non-static: shared with
	// ShaderReplace.cpp (IsShadowPass must match DSVs beyond slot 7).
	// ---------------------------------------------------------------------
	std::array<void*, 128> g_normalDepthBuffer{};
	std::array<void*, 128> g_readOnlyDepthBuffer{};

	// Engine depth-target index globals (type at addr, sub-index at addr+4).
	static int32_t GetDepthTargetType()
	{
		static REL::RelocationID uid(524780, 388826);
		return *reinterpret_cast<int32_t*>(uid.address());
	}

	static int32_t GetDepthTargetSubIndex()
	{
		static REL::RelocationID uid(524780, 388826);
		return *reinterpret_cast<int32_t*>(uid.address() + 4);
	}

	// Create-loop redirect: R12 (SE/AE) / R13 (VR) holds a_target * 0x13;
	// 4*19=76 identifies the shadow-map depth target. RDI (SE) / RBX (AE/VR)
	// is the loop index. Redirect R9 to our extended arrays so views >= 8
	// land there instead of writing OOB into the game struct.
	static void Hook_CreateNormalDepthBuffer(CONTEXT& ctx)
	{
		if (REL::Relocate(ctx.R12, ctx.R12, ctx.R13) != 4 * 19)
			return;
		const int idx = static_cast<int>(REL::Relocate(ctx.Rdi, ctx.Rbx, ctx.Rbx));
		ctx.R9 = reinterpret_cast<DWORD64>(&g_normalDepthBuffer[idx]);
	}

	static void Hook_CreateReadOnlyDepthBuffer(CONTEXT& ctx)
	{
		if (REL::Relocate(ctx.R12, ctx.R12, ctx.R13) != 4 * 19)
			return;
		const int idx = static_cast<int>(REL::Relocate(ctx.Rdi, ctx.Rbx, ctx.Rbx));
		ctx.R9 = reinterpret_cast<DWORD64>(&g_readOnlyDepthBuffer[idx]);
	}

	// After the creation loop finishes, sync the first 8 views into the
	// game's own DepthStencilData array (renderer in R15) so existing code
	// reading depthStencils[4].views[0..7] still works.
	static void Hook_SetupGameArray(CONTEXT& ctx)
	{
		if (REL::Relocate(ctx.R12, ctx.R12, ctx.R13) != 4 * 19)
			return;
		auto* renderer = reinterpret_cast<RE::BSGraphics::Renderer*>(ctx.R15);
		for (int i = 0; i < 8; i++) {
			renderer->GetDepthStencilData().depthStencils[4].views[i] = reinterpret_cast<ID3D11DepthStencilView*>(g_normalDepthBuffer[i]);
			renderer->GetDepthStencilData().depthStencils[4].readOnlyViews[i] = reinterpret_cast<ID3D11DepthStencilView*>(g_readOnlyDepthBuffer[i]);
		}
	}

	// Draw-time depth-buffer selection. Type 4 = shadow maps: route through
	// our extended arrays. Hook #1: renderer in R8, result -> RBX.
	static void Hook_SelectDepthBuffer1(CONTEXT& ctx)
	{
		auto* data = reinterpret_cast<RE::BSGraphics::RendererData*>(ctx.R8);
		const int type = GetDepthTargetType();
		const int sub = GetDepthTargetSubIndex();

		// fix6 (23:1x, fix5-diag data 91587d07): fix4's premise was WRONG.
		// fix5 attribution (act=1 vs act=0) proved every sub=4/5/6 select is
		// OUR manual dispatch (act=1, s_renderingLight set) - the engine
		// NEVER self-selects 4/5/6 while we render (all act=0 rows are sub=0).
		// The real bug: the engine's draw-time slice selector reads its OWN
		// sub-index global (GetDepthTargetSubIndex), which manual dispatch
		// never sets -> it carries stale engine values (4/5/6) -> the SAME
		// light renders into a DIFFERENT canvas across frames (log: slot 2 ->
		// sub 2/4/5, slot 0 -> sub 0/4/5/6) -> canvases overwrite each other
		// -> blocky flicker. fix4 narrowed forcing to slot>=8, leaving the
		// engine-owned 0-7 UNFORCED = drifting. Fix: while OUR dispatch
		// renders a light (act=1), ALWAYS pin the canvas to that light's
		// slot (0-127). Engine-path selects (act=0) stay untouched.
		const std::uint32_t actSlot = s_renderingSlot.load(std::memory_order_relaxed);
		const bool forcing = type == 4 &&
			s_renderingLight.load(std::memory_order_relaxed) != nullptr &&
			actSlot < 128u;
		const int effSub = forcing ? static_cast<int>(actSlot) : sub;

		// Diagnostic (every 64th type==4 call): which DSV did we route to?
		// This is the DEFINITIVE answer to "where does the engine render
		// shadow maps". Two paths: our g_normalDepthBuffer (redirected) or
		// the engine's depthStencils[4].views[sub] (vanilla). sub=X->Y:
		// X = engine's raw sub-index, Y = slot we forced (X==Y: no force).
		static uint32_t s_type4_count = 0;
		if (type == 4) {
			const uint32_t n = ++s_type4_count;
			if ((n & 0x1FFu) == 0) {  // 2026-09-06: 64 -> 512 (log flood)
				ID3D11DepthStencilView* chosen =
					(g_normalDepthBuffer[0] != nullptr)
						? (data->readOnlyDepth ? reinterpret_cast<ID3D11DepthStencilView*>(g_readOnlyDepthBuffer[effSub])
											  : reinterpret_cast<ID3D11DepthStencilView*>(g_normalDepthBuffer[effSub]))
						: (data->readOnlyDepth ? RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData().depthStencils[4].readOnlyViews[effSub]
											  : RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData().depthStencils[4].views[effSub]);
				ID3D11Resource* tex = nullptr;
				ID3D11Resource* srvTex = nullptr;
				if (chosen) chosen->GetResource(&tex);
				if (auto* dsd = &RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData(); dsd && dsd->depthStencils[4].depthSRV)
					reinterpret_cast<ID3D11ShaderResourceView*>(dsd->depthStencils[4].depthSRV)->GetResource(&srvTex);
				// fix5-diag (23:0x, flicker persists with EXTEND=0 + narrow force):
				// the frame-stable sub cycle 0,4,3,2,1 includes a sub=4 select
				// that manual dispatch (4 lights, slots 0-3) cannot produce.
				// Add attribution: act=1 -> select fired inside our manual
				// RenderScheduledShadowLightsDispatch (s_renderingLight set);
				// act=0 -> some ENGINE path selects this canvas on its own.
				// ro = which branch the hook routed (normal vs readOnly DSV
				// arrays - sub=4's DSV sits in a different memory region
				// (0x6dc7...) than slots 0-3 (0x5bb3...), suggesting the
				// readOnly path). This log change is read-only, no behavior
				// change; revert after diagnosis.
				const std::uint32_t actSlot2 = s_renderingSlot.load(std::memory_order_relaxed);
				const bool act2 = s_renderingLight.load(std::memory_order_relaxed) != nullptr;
				SKSE::log::info("[SLF] SelectDSB: type=4 #{} sub={}->{} act={} slot={} ro={} chosenDSV={} chosenTex={} SRVtex={} SAME={}",
					n, sub, effSub, act2 ? 1 : 0, actSlot2, data->readOnlyDepth ? 1 : 0,
					(void*)chosen, (void*)tex, (void*)srvTex,
					tex && srvTex && tex == srvTex ? "yes" : "NO");
				if (tex) tex->Release();
				if (srvTex) srvTex->Release();
			}
		}

		if (type == 4 && g_normalDepthBuffer[0]) {
			ctx.Rbx = data->readOnlyDepth ? reinterpret_cast<DWORD64>(g_readOnlyDepthBuffer[effSub]) : reinterpret_cast<DWORD64>(g_normalDepthBuffer[effSub]);
		} else {
			ctx.Rbx = data->readOnlyDepth ? reinterpret_cast<DWORD64>(RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData().depthStencils[type].readOnlyViews[effSub]) : reinterpret_cast<DWORD64>(RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData().depthStencils[type].views[effSub]);
		}
	}

	// Hook #2: VR: renderer in R14, result -> RBP; SE/AE: renderer in RBP,
	// result -> R14.
	static void Hook_SelectDepthBuffer2(CONTEXT& ctx)
	{
		const bool isVR = REL::Module::GetRuntime() == REL::Module::Runtime::VR;
		const bool readOnly = isVR ? reinterpret_cast<RE::BSGraphics::Renderer*>(ctx.R14)->GetRuntimeData().readOnlyDepth : reinterpret_cast<RE::BSGraphics::Renderer*>(ctx.Rbp)->GetRuntimeData().readOnlyDepth;

		const int type = GetDepthTargetType();
		const int sub = GetDepthTargetSubIndex();

		// fix6: same always-pin fix as hook #1 (see fix6 comment there) -
		// while OUR dispatch renders a light, force the canvas to that
		// light's slot; engine-path selects (act=0) untouched.
		const std::uint32_t actSlot = s_renderingSlot.load(std::memory_order_relaxed);
		const bool forcing = type == 4 &&
			s_renderingLight.load(std::memory_order_relaxed) != nullptr &&
			actSlot < 128u;
		const int effSub = forcing ? static_cast<int>(actSlot) : sub;

		DWORD64 result;
		if (type == 4 && g_normalDepthBuffer[0]) {
			result = readOnly ? reinterpret_cast<DWORD64>(g_readOnlyDepthBuffer[effSub]) : reinterpret_cast<DWORD64>(g_normalDepthBuffer[effSub]);
		} else {
			result = readOnly ? reinterpret_cast<DWORD64>(RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData().depthStencils[type].readOnlyViews[effSub]) : reinterpret_cast<DWORD64>(RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData().depthStencils[type].views[effSub]);
		}

		if (isVR)
			ctx.Rbp = result;
		else
			ctx.R14 = result;
	}

	// ---------------------------------------------------------------------
	// P1a entry: address verification only. The two selection hooks are NOT
	// intercepted yet.
	//
	// WHY: CalculateActiveNonShadowCasterLights populates per-surface light
	// data that the renderer consumes right after. Fully bypassing it in
	// stub mode leaves that data uninitialized -> null deref crash
	// (mov rdi,[r13+0x48], r13=0 at SkyrimSE.exe+14DF6E7, crash-2026-09-02).
	// CS intercepts it only because its scheduler fills the data. Our real
	// interception lands in P1c together with the actual scheduler.
	// ---------------------------------------------------------------------
	void Install()
	{
		SKSE::log::info("[SLF] P1a: selection hooks DEFERRED to P1c (bypassing the "
						"per-surface light population crashes the renderer)");
		SKSE::log::info("[SLF] P1a: P0 verified - trampoline allocated - ready for P1b");
	}

	// ---------------------------------------------------------------------
	// P1b entry: extend kSHADOWMAPS to N slices + redirect depth-buffer
	// creation + hook render loop. REAL engine-state modification.
	// Call only after P1a verified in-game. Requires game restart to revert.
	// Compile-time gated: set ENABLE_P1B=1 to build this in.
	// ---------------------------------------------------------------------
#if ENABLE_P1B
	void InstallExtendedBuffers(uint32_t a_shadowLightCount)
	{
		SKSE::log::info("[SLF] P1b installing extended buffers ({} shadow slices)...", a_shadowLightCount);
		const uint32_t count = a_shadowLightCount;

		// 1) Patch the depth-buffer creation-loop count 8 -> count.
		//    SE/VR pattern "C7 44 24 68 08 00 00 00"; AE same pattern at offset.
		//    Write all 4 bytes so values > 255 don't truncate.
		{
			static REL::RelocationID uid(100458, 107175);
			uintptr_t addr = uid.address() + REL::Relocate(0xD326 - 0xC940, 0xBF6 - 0x210, 0xc91);
			int immOff = REL::Relocate(4, 4, 3);
			REL::safe_write(addr + immOff, &count, sizeof(count));
			SKSE::log::info("[SLF]   kSHADOWMAPS creation-loop count patched to {}", count);
		}

#if P1B_FULL
		// 2..4 below are only needed when actually expanding beyond the
		// vanilla 8 slices. With count <= 8 the engine's own creation loop,
		// depth-stencil data and render loop all stay vanilla - installing
		// the render-loop rax=0 hook here breaks vanilla rendering
		// (crash: call [rax+0x20], rax=0 in d3d11.dll).

		// 2) Redirect depth-buffer pointer storage in the creation loop.
		{
			// Normal DSV creation
			static REL::RelocationID uid(75469, 77255);
			uintptr_t base = uid.address();
			uintptr_t off = REL::Relocate(0xB52 - 0x9E0, 0x2EB - 0x180, 0x1a0);
			int sz = REL::Relocate(7, 7, 8);
			if (!SKSE::stl::install_context_hook(base + off, sz, Hook_CreateNormalDepthBuffer, sz))
				SKSE::log::error("[SLF]   FAILED Hook_CreateNormalDepthBuffer");
		}
		{
			// ReadOnly DSV creation
			static REL::RelocationID uid(75469, 77255);
			uintptr_t base = uid.address();
			uintptr_t off = REL::Relocate(0xB71 - 0x9E0, 0x2FC - 0x180, 0x1c4);
			int sz = REL::Relocate(8, 7, 7);
			if (!SKSE::stl::install_context_hook(base + off, sz, Hook_CreateReadOnlyDepthBuffer, sz))
				SKSE::log::error("[SLF]   FAILED Hook_CreateReadOnlyDepthBuffer");
		}
		{
			// Sync first 8 slots into the game's DepthStencilData array
			static REL::RelocationID uid(75469, 77255);
			uintptr_t base = uid.address();
			uintptr_t off = REL::Relocate(0xC00 - 0x9E0, 0x384 - 0x180, 0x250);
			if (!SKSE::stl::install_context_hook(base + off, 8, Hook_SetupGameArray, 8))
				SKSE::log::error("[SLF]   FAILED Hook_SetupGameArray");
		}
		SKSE::log::info("[SLF]   depth-buffer redirects installed");

		// 3) Depth-buffer selection at draw time.
		{
			static REL::RelocationID uid(75580, 77386);
			uintptr_t base = uid.address();
			uintptr_t off = REL::Relocate(0x444 - 0x2F0, 0x704 - 0x5B0, 0x1c3);
			if (!SKSE::stl::install_context_hook(base + off, 21, Hook_SelectDepthBuffer1))
				SKSE::log::error("[SLF]   FAILED Hook_SelectDepthBuffer1");
		}
		{
			static REL::RelocationID uid(75462, 77247);
			uintptr_t base = uid.address();
			uintptr_t off = REL::Relocate(0x1A5 - 0x070, 0x985 - 0x850, 0x19c);
			int sz = REL::Relocate(10, 10, 0x2e);
			if (!SKSE::stl::install_context_hook(base + off, sz, Hook_SelectDepthBuffer2))
				SKSE::log::error("[SLF]   FAILED Hook_SelectDepthBuffer2");
		}
		SKSE::log::info("[SLF]   depth-buffer selection hooks installed");

		// 4) Render-loop call site - rax=0 skip (mandatory once array extended).
		{
			static REL::RelocationID uid(100415, 107133);
			uintptr_t addr = uid.address() + REL::Relocate(0xF76 - 0xE30, 0xC17D - 0xBFF0, 0x1CA);
			if (!SKSE::stl::install_context_hook(addr, 5, Hook_RenderShadowLights))
				SKSE::log::error("[SLF]   FAILED Hook_RenderShadowLights");
			SKSE::log::info("[SLF]   render-loop hook installed (rax=0 skip)");
		}
#endif  // P1B_FULL

		SKSE::log::info("[SLF] P1b slice patch installed ({} slices) - RESTART required to revert", count);
	}
#endif  // ENABLE_P1B

	// ---------------------------------------------------------------------
	// B4c (2026-09-05): per-surface light-batch rewrite.
	//
	// Target: engine CalculateActiveNonShadowCasterLights
	//   (Address Library SE 100997 / AE 107784, AE RVA 0x14FCF80 -
	//   verified 2026-09-05: module base 0x7FF77C7C0000 + install addr
	//   0x7FF77DCBCF80; the earlier "0x14FA570" in docs was wrong - that
	//   RVA sits inside the 107644 region). 107300 (material light setup,
	//   RVA 0x14DD040) is vtable-dispatched (.rdata vtable @ 0x1ABE4F0);
	//   107784 itself has NO direct E8 callers / data refs in the image.
	// The engine calls this per surface/material-pass and its return value
	// becomes the per-surface cb2 light batch: cb2[29].x = batch length,
	// cb2[29].y = shadow count, batch order = the order the material PS
	// light loop walks. Our B2b payload samples t102 (LightRec) at the
	// ENGINE LOOP INDEX (ftoi r18.x, r26.zzzz -> ld_structured t102[r18.x]),
	// so correctness requires:
	//
	//     batch[i]  ==  g_scheduledShadowLights[i]  (for every i in batch)
	//
	// because t102[i] is published from g_scheduledShadowLights[i] every
	// frame (scheduler -> g_shadowLights -> LightRec, index-preserving).
	// B4b's 92,041-readback proved the UNMODIFIED engine violates this
	// (engine picks its own per-surface order -> batch[i] != t102[i] ->
	// every shadow test reads another light's data = the flicker).
	//
	// The rewrite therefore fills the batch as the CONSECUTIVE PREFIX of
	// g_scheduledShadowLights (sun occupies index 0 when the engine
	// accumulated it, extended lights follow in render order), no
	// per-surface reordering and no interior skipping - any skip would
	// shift every later index and recreate the mismatch. *shadowCount is
	// set to the batch length (all prefix entries are shadow producers),
	// which lifts the vanilla <=4 clamp so >4-light surfaces stop rotating
	// through a 4-channel t14 bake.
	//
	// Diffuse-only lights that the vanilla list used to carry (tail
	// entries, j >= shadowCount) are NOT appended here: with the shadow
	// unit replaced by our unconditional payload the engine loop would run
	// the t102 test on them too, and t102[j] for j < 24 is an enabled
	// scheduled light - wrong shadow on a diffuse slot. Batch capacity is
	// ~7 lights (cb2[29].x <= 7 observed across 92k readbacks), so the
	// prefix usually fills it anyway. Shadows > 4 per surface come from
	// the payload's per-light depth test, not from t14 channels.
	static void Hook_CalculateActiveLightsForSurface(CONTEXT& ctx)
	{
		// x64 fastcall, context captured at function entry (patch at byte 0):
		//   a1 lightData  (RCX)  a2 lights**  (RDX)
		//   a3 maxCount   (R8)   a4 shadowCount* (R9)
		//   a5 ssn        [rsp+0x28]  a6 shaderProp [rsp+0x30]
		//   a7 addShadow  [rsp+0x38]  a8 useShadowSun [rsp+0x40]
		//   a9 firstPerson [rsp+0x48]  a10 fpMask    [rsp+0x50]
		//
		// Vanilla contract (verified against the 107784 disassembly):
		//   lights[0] is ALWAYS written (sun/cloud pointer, possibly null),
		//   the return value is ALWAYS >= 1. Callers (material pass
		//   setup, 107300 chain) consume lights[0] unconditionally, so a
		//   0-length batch corrupts downstream state -> AV (B4c crash
		//   2026-09-05-12-28-15: SkyrimSE+14DD5B7 mov rax,[rax], rax=0).
		auto** lights = reinterpret_cast<RE::BSLight**>(ctx.Rdx);
		auto*  shadowCount = reinterpret_cast<int*>(ctx.R9);
		const int maxCount = static_cast<int>(ctx.R8);

		auto* lightData = reinterpret_cast<RE::BSShaderPropertyLightData*>(ctx.Rcx);
		const auto ssnPtr = *reinterpret_cast<void**>(ctx.Rsp + 0x28);
		const auto shaderProp = *reinterpret_cast<void**>(ctx.Rsp + 0x30);
		const auto addShadow = *reinterpret_cast<const bool*>(ctx.Rsp + 0x38);
		const auto useShadowSunPtr = *reinterpret_cast<void**>(ctx.Rsp + 0x40);
		const auto firstPerson = *reinterpret_cast<const bool*>(ctx.Rsp + 0x48);
		const auto fpMask = *reinterpret_cast<const std::uint32_t*>(ctx.Rsp + 0x50);

		if (!lights || maxCount <= 0) {
			// No output buffer: nothing to write; the engine never calls
			// with this shape in practice, but keep the register contract
			// (a return of 0 here mirrors a degenerate vanilla result).
			ctx.Rax = 0;
			if (shadowCount)
				*shadowCount = 0;
			return;
		}

		// Resolve the sun/cloud light exactly like vanilla does:
		//   useShadowSun ? ssn.sunShadowDirLight(+0x210) : ssn.sunLight(+0x200)
		//   shaderProp flags kCloudLOD -> ssn.cloudLight(+0x208)
		// (offsets read from the 107784 disassembly; CommonLib layout agrees).
		RE::BSLight* sun = nullptr;
		if (ssnPtr && shaderProp) {
			auto* ssn = static_cast<RE::ShadowSceneNode*>(ssnPtr);
			auto* shader = static_cast<RE::BSLightingShaderProperty*>(shaderProp);
			const bool useShadowSun = useShadowSunPtr && *static_cast<const bool*>(useShadowSunPtr);
			sun = useShadowSun ? ssn->GetRuntimeData().sunShadowDirLight : ssn->GetRuntimeData().sunLight;
			if (shader->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kCloudLOD))
				sun = ssn->GetRuntimeData().cloudLight;
		}

		// ---- batch construction (B4c semantics) ----
		// Vanilla ALWAYS occupies lights[0] (sun/cloud pointer, possibly
		// null) and returns >= 1; callers consume lights[0] unconditionally.
		// To keep batch[i] == scheduled[i] (t102[i] is published from
		// scheduled[i], and the payload samples t102 at the ENGINE LOOP
		// INDEX), the batch is the CONSECUTIVE PREFIX of
		// g_scheduledShadowLights - scheduled[0] IS the sun when the engine
		// accumulated it (slot 0 = sun when active), so prefixing verbatim
		// satisfies both the alignment and the vanilla lights[0]=sun shape.
		//
		// Fallback (menu/load frames, scheduler not yet publishing): read
		// the engine's own shadowLightsAccum (the vanilla data source for
		// this function) and, if even that is empty, write lights[0] = sun
		// so the return is never 0 and lights[0] is never garbage - the
		// exact crash B4c first shipped with (scheduled=0 -> empty batch ->
		// SkyrimSE+14DD5B7 AV, rax=0).
		//
		// Diffuse-only lights are then appended from lightData->lights
		// exactly like vanilla/CS Step3 (skip frustrumCull==0xFF parabolic
		// markers and hidden NiLights). They do NOT run the shadow payload:
		// the engine's material loop only shadow-tests lights that carry a
		// shadow channel, and diffuse entries get none.
		// fix64 step3b (2026-09-09): CS-aligned batch (ShadowEngineHooks.cpp
		// 761-762): lights[0] = the RESOLVED sun source ALWAYS, *shadowCount
		// starts at 0 - the sun is a directional light on its own engine
		// path, never a shadow-array light. The pre-step3b prefix copy put
		// the scheduled list verbatim with nShadow INCLUDING the sun; the
		// material pass then shadow-tested the sun like a point light and
		// produced an empty cb2 ("BT diag lightCount=1 scheduled=1" vs
		// "cb2 per-surface: lights=0" - no sunlight anywhere). Shadow
		// point lights start at lights[1] = scheduled[1] (scheduled[0] IS
		// the sun when active), keeping the SLF-B payload aligned.
		*shadowCount = 0;
		lights[0] = sun;  // resolved sun source; may be null (vanilla writes it regardless)
		int added = 1;
		const std::uint32_t nSch = g_scheduledShadowCount.load(std::memory_order_acquire);
		auto* ssn = ssnPtr ? static_cast<RE::ShadowSceneNode*>(ssnPtr) : nullptr;

		if (addShadow) {
			if (nSch > 0) {
				int cap = std::min<int>(maxCount, static_cast<int>(nSch));
				for (int j = 0; j < cap && added < maxCount; j++) {
					auto* sl = g_scheduledShadowLights[j].light;  // BSShadowLight*
					if (!sl)
						break;  // null hole: keep prefix contiguous (never skip)
					if (reinterpret_cast<RE::BSLight*>(sl) == sun)
						continue;  // sun already occupies lights[0]
					lights[added++] = sl;
					(*shadowCount)++;
				}
			} else if (ssn) {
				// Scheduler idle (menu/load/first frame): vanilla data source.
				auto& accum = ssn->GetRuntimeData().shadowLightsAccum;
				for (const auto* sl : accum) {
					if (!sl || added >= maxCount)
						break;
					if (reinterpret_cast<RE::BSLight*>(const_cast<RE::BSShadowLight*>(sl)) == sun)
						continue;
					if (added >= 8)
						break;  // cb2 per-surface ceiling
					lights[added++] = const_cast<RE::BSShadowLight*>(sl);
					(*shadowCount)++;
				}
			}
		}

		// Diffuse-only lights (vanilla tail loop 0x14fd130 / CS Step3).
		if (added < maxCount && lightData) {
			for (auto* l : lightData->lights) {
				if (added >= maxCount)
					break;
				if (!l || l == sun)
					continue;
				// Skip parabolic shadow-caster markers and hidden lights.
				if (l->frustrumCull == 0xFFu)
					continue;
				if (l->light && l->light->GetFlags().any(RE::NiAVObject::Flag::kHidden))
					continue;
				// Skip if already in the batch (scheduled/accum may carry it).
				bool dup = false;
				for (int i = 0; i < added && !dup; i++)
					dup = (lights[i] == l);
				if (dup)
					continue;
				lights[added++] = l;
			}
		}

		// *shadowCount written directly during construction (fix64 step3b:
		// counts shadow point lights only, never the sun). added >= 1 always
		// (lights[0] = sun source, possibly null) - vanilla shape.
		ctx.Rax = static_cast<std::uint64_t>(added);

		// Per-surface batch diagnostics (every 1024th call). The first batch
		// light position is echoed so the existing cb2 readback line
		// "[SLF] cb2 light[0..2] pos=..." can be matched against it: B4c is
		// proven when cb2 light[0] pos == LightRec[0] pos (they were DIFF in
		// every B4b sample).
		static std::uint32_t s_calls = 0;
		if ((s_calls++ & 0x3FFu) == 0) {
			float px = 0.f, py = 0.f, pz = 0.f;
			std::uintptr_t l0 = 0;
			if (added > 0) {
				l0 = reinterpret_cast<std::uintptr_t>(lights[0]);
				// lights[0] is the first scheduled light; derive its world
				// position through the same accessor the readback uses.
				auto* sl0 = static_cast<RE::BSLight*>(lights[0]);
				if (sl0 && sl0->light) {
					px = sl0->light->world.translate.x;
					py = sl0->light->world.translate.y;
					pz = sl0->light->world.translate.z;
				}
			}
			SKSE::log::info(
				"[SLF][B4c] batch rewrite: maxCount={} scheduled={} added={} "
				"shadow*={} addShadow={} firstPerson={} fpMask={:#x} lightData={:x} ssn={:x} prop={:x} "
				"useShadowSun={:x} sun={:x} | light[0]={:x} pos=({:.1f},{:.1f},{:.1f})",
				maxCount, nSch, added,
				shadowCount ? *shadowCount : -1, addShadow ? 1 : 0, firstPerson ? 1 : 0, fpMask,
				reinterpret_cast<std::uintptr_t>(lightData),
				reinterpret_cast<std::uintptr_t>(ssnPtr),
				reinterpret_cast<std::uintptr_t>(shaderProp),
				reinterpret_cast<std::uintptr_t>(useShadowSunPtr),
				reinterpret_cast<std::uintptr_t>(sun),
				l0, px, py, pz);
		}
	}

	// B4c install: full-function replacement of
	// CalculateActiveNonShadowCasterLights (ID 100997/107784).
	// install_context_hook (context-capture stub, includeSize=0 -> original
	// prologue fully replaced) + a RET at addr+5 so RtlRestoreContext lands
	// on a return with our RAX = added batch count (same install pattern CS
	// uses for this exact function). Skipped on VR (arg layout differs: an
	// extra 11th stack arg at +0x58).
	void InstallSurfaceLightsHook()
	{
		if (REL::Module::GetRuntime() == REL::Module::Runtime::VR) {
			SKSE::log::info("[SLF][B4c] install skipped (VR runtime - different arg layout)");
			return;
		}
		REL::RelocationID uid(100997, 107784);
		const std::uintptr_t addr = uid.address();
		if (!addr) {
			SKSE::log::error("[SLF][B4c] FAILED: CalculateActiveNonShadowCasterLights address is null");
			return;
		}
		SKSE::log::info("[SLF][B4c] installing per-surface batch rewrite @ {:016X}", addr);
		if (!SKSE::stl::install_context_hook(addr, 5, Hook_CalculateActiveLightsForSurface)) {
			SKSE::log::error("[SLF][B4c] FAILED install_context_hook @ {:016X}", addr);
			return;
		}
		const uint8_t ret = 0xC3;
		REL::safe_write(addr + 5, &ret, 1);
		SKSE::log::info("[SLF][B4c] installed (RET at +5) - engine per-surface batch = SLF scheduled prefix");
	}

	// B4c-v3 cb2 readback probe (ShaderReplace.cpp): samples the PS slot-2
	// cbuffer via a staging copy. B4d calls it at 107300 (SetupGeometry)
	// entry to learn whether the cb2 batch is ALREADY filled before the
	// sceneLights walk - i.e. whether the real cb2 fill site is upstream.
	// Declared here (before SceneLightsRewriteHook uses it); defined in
	// ShaderReplace.cpp inside the same ShadowLimitFixNS::P1 namespace.
	void DrawTimePerSurfaceCB2(::ID3D11DeviceContext* a_ctx);

	// B4d (2026-09-05): engine cb2 light-batch fill hook.
	//
	// B4c rewrote the 107784 (CalculateActiveNonShadowCasterLights) output
	// buffer, but the B4c-v3 draw-time probe (md5 77f833ff) PROVED the
	// engine's cb2[15..17] batch is NOT that buffer: 6.2M patched draws
	// showed cb2[29]=(7,4) / chmap=(1,2,3,255) constant while the cb2 light
	// slots tracked a different per-draw set that never matched the
	// scheduled prefix. Offline RE of the crash chain (SkyrimSE+14DD5B7 AV,
	// rax=0 dereferencing a null sceneLights[0]) pinned the real cb2 data
	// source: the function at AE RVA 0x14DD040 (crashlog id 107300) walks
	// BSRenderPass::sceneLights and fills cb2[15..] from it. Evidence from
	// its prologue disassembly (1.6.1170):
	//   r12 = RDX arg2 = BSRenderPass* (kept across the whole function)
	//   [r12+0x1F] = numLights, [r12+0x20] = numShadowLights
	//   [r12+0x38] = sceneLights (BSLight**), [r12+0x08] = shaderProperty,
	//   [r12+0x10] = geometry          <- CommonLib BSRenderPass offsets 1:1
	// The engine batch order therefore IS sceneLights order, which is NOT
	// the scheduled order - the flicker root cause. B4d rewrites
	// sceneLights[0..k) to the scheduled prefix right before that walk so
	// cb2[i] == scheduled[i] (== t102[i] publishing order) finally holds.
	//
	// Safety: only swaps slots the engine already wrote (k <= numLights),
	// never grows the array, never touches numLights/numShadowLights. The
	// pass object is rebuilt by the engine per draw setup, so the rewrite
	// has no cross-draw persistence. Scheduled lights are shadow casters;
	// a diffuse-only pass that never shadow-tests them is unaffected.
	//
	// Scope: AE 1.6.1170 only (RVA verified against that exe; crashlog-id
	// numbering has no AL entry, so SE/VR and other AE minors are skipped).
	struct SceneLightsRewriteHook
	{
		// x64 fastcall: a1=RCX (this-like, flags @ +0x94), a2=RDX BSRenderPass*,
		// a3=R8D (u32, parked by the engine at [rsp+0x18]).
		static void thunk(void* a1, RE::BSRenderPass* a_pass, std::uint32_t a3)
		{
			if (a_pass) {
				const std::uint32_t nSch = g_scheduledShadowCount.load(std::memory_order_acquire);
				const std::uint8_t numLights = a_pass->numLights;
				if (nSch > 0 && numLights > 0 && a_pass->sceneLights) {
					// cb2 per-surface ceiling is 7 (cb2[29].x; engine PS does
					// min r3.xy, cb2[29].yxyy, l(4,7)) - rewriting more than
					// that never reaches cb2. Prefix must stay contiguous
					// (same rule as the B4c rewrite: no skipped holes).
					const int k = std::min({ static_cast<int>(nSch),
						static_cast<int>(numLights), 7 });
					for (int i = 0; i < k; i++) {
						auto* sl = g_scheduledShadowLights[i].light;  // BSShadowLight* -> BSLight* upcast
						if (!sl)
							break;
						a_pass->sceneLights[i] = sl;
					}
					// Throttled diagnostic: the position of sceneLights[0]
					// after the rewrite, matched against the [B4c][draw]
					// cb2[15] probe next test run - MATCH means the engine
					// cb2 batch is the scheduled prefix at draw time.
					static std::uint32_t s_log = 0;
					if (((s_log++) & 0xFFFu) == 0) {
						float px = 0.f, py = 0.f, pz = 0.f;
						std::uintptr_t sl0p = 0;
						if (auto* sl0 = a_pass->sceneLights[0]; sl0 && sl0->light) {
							sl0p = reinterpret_cast<std::uintptr_t>(sl0);
							px = sl0->light->world.translate.x;
							py = sl0->light->world.translate.y;
							pz = sl0->light->world.translate.z;
						}
						SKSE::log::info(
							"[SLF][B4d] sceneLights prefix: nSch={} numLights={} numShadow={} k={} "
							"pass={:x} | scene[0]={:x} pos=({:.1f},{:.1f},{:.1f})",
							nSch, numLights, a_pass->numShadowLights,
							k, reinterpret_cast<std::uintptr_t>(a_pass),
							sl0p, px, py, pz);
					}
				}
			}
			// B4d-diag (2026-09-06): the draw-time cb2 probe
			// ([B4c][draw] / [B5][match]) now fires from the generic Draw
			// hooks (P1_hooks.cpp) where the cb2 is read at the same
			// instant the PS consumes it; it no longer depends on this B4d
			// thunk (which is compiled out while B4d is disabled).
			return func(a1, a_pass, a3);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	void InstallSceneLightsRewriteHook()
	{
		if (REL::Module::GetRuntime() == REL::Module::Runtime::VR) {
			SKSE::log::info("[SLF][B4d] install skipped (VR runtime - no RVA database)");
			return;
		}
		if (REL::Module::GetRuntime() != REL::Module::Runtime::AE) {
			SKSE::log::info("[SLF][B4d] install skipped (SE runtime - address is AE-only)");
			return;
		}
		const auto ver = REL::Module::get().version();
		if (ver != REL::Version{ 1, 6, 1170, 0 }) {
			SKSE::log::info("[SLF][B4d] install skipped (AE {}.{}.{}.{} != 1.6.1170 - RVA not portable)",
				ver[0], ver[1], ver[2], ver[3]);
			return;
		}
		const std::uintptr_t addr = REL::Module::get().base() + 0x14DD040;
		// Prologue sanity (1.6.1170): 48 8B C4 = "mov rax, rsp". A mismatch
		// means the RVA no longer points at this function - refuse to hook.
		const auto* p = reinterpret_cast<const std::uint8_t*>(addr);
		if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0xC4) {
			SKSE::log::error("[SLF][B4d] FAILED: prologue mismatch @ {:016X} ({:02X} {:02X} {:02X}) - RVA out of date",
				addr, p[0], p[1], p[2]);
			return;
		}
		SKSE::log::info("[SLF][B4d] installing sceneLights prefix rewrite @ {:016X}", addr);
		const long rc = stl::detour_thunk_addr<SceneLightsRewriteHook>(addr);
		if (rc != NO_ERROR) {
			SKSE::log::error("[SLF][B4d] FAILED Detours rc={}", rc);
			return;
		}
		SKSE::log::info("[SLF][B4d] installed - engine cb2 batch = scheduled prefix");
	}


	// ---------------------------------------------------------------------
	// Shadow-render trace (13:22 diagnosis): does the engine really render
	// shadow draws into the kSHADOWMAPS array, and is it cleared afterwards?
	// MAT-pass readback shows the array EMPTY (0% across all 8 slices) even
	// though the engine selects type=4 DSVs on that texture (SAME=yes) - so
	// either the shadow draws never happen, they render elsewhere, or the
	// array is cleared between the shadow pass and the material pass.
	//
	// Hook ID3D11DeviceContext vtable (context singleton, same instance the
	// SLF swap hooks use): OMSetRenderTargets = [33], ClearDepthStencilView =
	// [53], Draw = [13]. Log only calls whose DSV texture IS the shadow
	// array (what t103 samples). Throttled - no per-draw spam.
	// ---------------------------------------------------------------------
	// SLF-B: zero-default t102 binding at draw time. Declared at P1 namespace
	// scope (NOT inside the anonymous namespace below - that would give it
	// internal linkage and fail to link against the definition in
	// ShaderReplace.cpp).
	void EnsureZeroBindForCurrentPS(::ID3D11DeviceContext* a_ctx);

	namespace
	{
		// [SLF-QA] frame-quality probe functions (defined at the end of this
		// anonymous namespace). Forward declarations so the hook thunks above
		// can call them - same-namespace entities, no linkage games.
		void QATickDraw(::ID3D11DeviceContext* a_ctx);
		void QANoteOMSetRT(::ID3D11DeviceContext* a_ctx, UINT a_numViews,
			::ID3D11RenderTargetView* const* a_rtvs, ::ID3D11DepthStencilView* a_dsv);

		using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(
			::ID3D11DeviceContext*, UINT, ::ID3D11RenderTargetView* const*, ::ID3D11DepthStencilView*);
		using ClearDepthStencilViewFn = void(STDMETHODCALLTYPE*)(
			::ID3D11DeviceContext*, ::ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
		using DrawFn = void(STDMETHODCALLTYPE*)(::ID3D11DeviceContext*, UINT, UINT);
		using DrawIndexedFn = void(STDMETHODCALLTYPE*)(
			::ID3D11DeviceContext*, UINT, UINT, INT);
		using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(
			::ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);

		OMSetRenderTargetsFn g_origOMSetRT = nullptr;
		ClearDepthStencilViewFn g_origClearDSV = nullptr;
		DrawFn g_origDraw = nullptr;
		DrawIndexedFn g_origDrawIndexed = nullptr;
		DrawIndexedInstancedFn g_origDrawIndexedInstanced = nullptr;

		// Cached resource (texture) behind depthStencils[4].depthSRV - the
		// very array our t103 samples. All shadow-DSV comparisons use this.
		::ID3D11Resource* ShadowArrayResource()
		{
			static ::ID3D11Resource* s_tex = []() -> ::ID3D11Resource* {
				if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
					auto& dsd = renderer->GetDepthStencilData();
					auto* srv = dsd.depthStencils[4].depthSRV;
					if (srv) {
						::ID3D11Resource* res = nullptr;
						reinterpret_cast<::ID3D11ShaderResourceView*>(srv)->GetResource(&res);
						if (res) {
							::D3D11_TEXTURE2D_DESC td{};
							reinterpret_cast<::ID3D11Texture2D*>(res)->GetDesc(&td);
							SKSE::log::info("[SLF] trace: shadow-array tex=0x{:x} {}x{} slices={}",
								reinterpret_cast<uintptr_t>(res), td.Width, td.Height, td.ArraySize);
							res->AddRef();
						}
						return res;
					}
				}
				return nullptr;
			}();
			return s_tex;
		}

		bool DSVIsShadowArray(::ID3D11DepthStencilView* a_dsv)
		{
			if (!a_dsv)
				return false;
			::ID3D11Resource* arr = ShadowArrayResource();
			if (!arr)
				return false;
			::ID3D11Resource* res = nullptr;
			a_dsv->GetResource(&res);
			if (!res)
				return false;
			const bool same = (res == arr);
			res->Release();
			return same;
		}

		std::uint32_t s_omShadow = 0;      // OMSet calls targeting the array
		std::uint32_t s_omShadowLogged = 0;
		std::uint32_t s_clearShadow = 0;   // ClearDepthStencilView on the array
		std::uint32_t s_clearShadowLogged = 0;
		std::uint32_t s_drawShadow = 0;    // Draws while the array is the OM DSV
		std::uint32_t s_drawShadowLogged = 0;
		std::atomic<bool> s_arrayIsOMDSV{ false };
		// fix19c: record the depth/flags of the last shadow-array clear.
		// fix19b fullscan read back all-0.000 - "never cleared" vs "cleared
		// to 0.0 and every draw frustum-culled" are two very different
		// verdicts, and the clear VALUE is what tells them apart.
		// (clearFlags: D3D11_CLEAR_DEPTH=0x1 / D3D11_CLEAR_STENCIL=0x2)
		float s_lastClearDepth = -1.0f;
		UINT s_lastClearFlags = 0;

		// ---- Per-slice activity on the shadow array (v3 diagnosis) ----
		// Resolve the OM-bound DSV to its kSHADOWMAPS slice by pointer-matching
		// against the game's depthStencils[4].views[0..7] (one DSV per array
		// slice). Cheap (8 compares, no COM). -1 = not the shadow array,
		// -2 = shadow texture but no slice match. 14:03 trace showed
		// clears+draws yet EVERY slice read back 0% - per-slice buckets answer
		// "which slices does the engine actually render into" without the RL
		// readback's timing ambiguity.
		int ShadowArraySlice(::ID3D11DepthStencilView* a_dsv)
		{
			if (!a_dsv)
				return -1;
			if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
				auto& dsd = renderer->GetDepthStencilData();
				for (int k = 0; k < 8; k++) {
					if (static_cast<void*>(dsd.depthStencils[4].views[k]) == static_cast<void*>(a_dsv))
						return k;
				}
			}
			// SLF-B B4 (2026-09-05): the unres flood (1.03M/frame in the
			// 23:45 log) is NOT a rendering fault - it is every shadow draw
			// whose DSV lives in OUR extended arrays (slices 8..127, the
			// SLF-managed lights). ShadowArraySlice only matched the engine's
			// views[0..7], so extended-slice draws fell through to
			// s_unknownSliceDraws. Match the extended arrays too (same
			// pointer test ShaderReplace.cpp IsShadowPass uses) so the
			// per-slice buckets show the REAL 24-light render distribution.
			if (DSVIsShadowArray(a_dsv)) {
				for (int k = 8; k < 128; k++) {
					if (ShadowLimitFixNS::P1::g_normalDepthBuffer[k] == static_cast<void*>(a_dsv) ||
						ShadowLimitFixNS::P1::g_readOnlyDepthBuffer[k] == static_cast<void*>(a_dsv))
						return k;
				}
				return -2;  // shadow texture, but not in either array set
			}
			return -1;
		}
		// SLF-B B4 fix (2026-09-05 07:5x): these buckets were std::array<,8>
		// while ShadowArraySlice's extended match (added the same night)
		// returns slices 8..127 for SLF-managed lights. Every extended-slice
		// OMSet/Clear/Draw then wrote OUT OF BOUNDS into the neighbouring
		// static counters (s_clearBySlice/s_drawBySlice/s_currentSlice/...),
		// corrupting the trace throttle state -> the trace flood (2ms of
		// back-to-back rows in the 07:46 log) and the hard exit with NO
		// CrashLogger dump (state corruption, not an AV). 128 buckets now
		// cover the full extended range.
		std::array<std::uint32_t, 128> s_omBySlice{};
		std::array<std::uint32_t, 128> s_clearBySlice{};
		std::array<std::uint32_t, 128> s_drawBySlice{};
		int s_currentSlice = -1;  // slice currently OM-bound (same context thread)
		std::uint32_t s_unknownSliceDraws = 0;  // shadow OM but slice unresolved

		void LogSliceActivity(const char* a_what)
		{
			std::string line;
			for (int k = 0; k < 8; k++) {
				if (s_omBySlice[k] == 0 && s_clearBySlice[k] == 0 && s_drawBySlice[k] == 0)
					continue;
				char buf[48];
				std::snprintf(buf, sizeof(buf), "%s%d=%u/%u/%u", line.empty() ? "" : " ", k,
					s_omBySlice[k], s_clearBySlice[k], s_drawBySlice[k]);
				line += buf;
			}
			// extended slices 8..127: list only the active ones (the 24-light
			// render distribution the B4 fix exists to see)
			for (int k = 8; k < 128; k++) {
				if (s_omBySlice[k] == 0 && s_clearBySlice[k] == 0 && s_drawBySlice[k] == 0)
					continue;
				char buf[48];
				std::snprintf(buf, sizeof(buf), "%s%d=%u/%u/%u", line.empty() ? "" : " ", k,
					s_omBySlice[k], s_clearBySlice[k], s_drawBySlice[k]);
				line += buf;
			}
			if (line.empty())
				line = "(no slice activity)";
			SKSE::log::info("[SLF] trace {} slice om/clear/draw: {} (+{} unres)", a_what, line, s_unknownSliceDraws);
		}

		void TraceOMShadow(uint32_t a_omCount, uint32_t a_clearCount, const char* a_what)
		{
			// Log both counters so the OMSet/Clear interleave is visible.
			// 2026-09-06: 32 -> 256 (the 32-step gate logged ~130 lines/s from
			// two render threads, ~1100 total lines/s across the file - the
			// spdlog I/O itself taxes the render loop and pollutes frame
			// timing; 256 keeps the same picture at ~16 lines/s).
			if ((a_omCount - s_omShadowLogged >= 256) || (a_clearCount - s_clearShadowLogged >= 256)) {
				s_omShadowLogged = a_omCount;
				s_clearShadowLogged = a_clearCount;
				SKSE::log::info("[SLF] trace {}: OMSet-shadow={} Clear-shadow={} Draw-shadow={} lastClearD={:.3f} clearF={}",
					a_what, a_omCount, a_clearCount, s_drawShadow, s_lastClearDepth, s_lastClearFlags);
				LogSliceActivity(a_what);
			}
		}

		void STDMETHODCALLTYPE HookOMSetRenderTargets(
			::ID3D11DeviceContext* a_ctx, UINT a_numViews,
			::ID3D11RenderTargetView* const* a_rtvs, ::ID3D11DepthStencilView* a_dsv)
		{
			QANoteOMSetRT(a_ctx, a_numViews, a_rtvs, a_dsv);
			const bool isShadow = DSVIsShadowArray(a_dsv);
			s_arrayIsOMDSV.store(isShadow, std::memory_order_release);
			s_currentSlice = isShadow ? ShadowArraySlice(a_dsv) : -1;
			if (isShadow) {
				s_omShadow++;
				if (s_currentSlice >= 0)
					s_omBySlice[s_currentSlice]++;
				TraceOMShadow(s_omShadow, s_clearShadow, "OMSet->array");
			}
			g_origOMSetRT(a_ctx, a_numViews, a_rtvs, a_dsv);
		}

		void STDMETHODCALLTYPE HookClearDepthStencilView(
			::ID3D11DeviceContext* a_ctx, ::ID3D11DepthStencilView* a_dsv,
			UINT a_clearFlags, FLOAT a_depth, UINT8 a_stencil)
		{
			const bool isShadow = DSVIsShadowArray(a_dsv);
			if (isShadow) {
				s_clearShadow++;
				s_lastClearDepth = a_depth;
				s_lastClearFlags = a_clearFlags;
				const int sl = ShadowArraySlice(a_dsv);
				if (sl >= 0)
					s_clearBySlice[sl]++;
				TraceOMShadow(s_omShadow, s_clearShadow, "CLEAR->array");
			}
			g_origClearDSV(a_ctx, a_dsv, a_clearFlags, a_depth, a_stencil);
		}

		// Draws while the shadow array is OM-bound: bucket into the slice that
		// was active at the last OMSet. Same-context-thread assumption as the
		// other trace counters (immediate context).
		inline void CountShadowDraw()
		{
			s_drawShadow++;
			if (s_currentSlice >= 0)
				s_drawBySlice[s_currentSlice]++;
			else
				s_unknownSliceDraws++;
			if (s_drawShadow - s_drawShadowLogged >= 4096) {
				s_drawShadowLogged = s_drawShadow;
				SKSE::log::info("[SLF] trace DRAW: shadow-draw total = {}", s_drawShadow);
				LogSliceActivity("DRAW");
			}
		}

		void STDMETHODCALLTYPE HookDraw(
			::ID3D11DeviceContext* a_ctx, UINT a_vertexCount, UINT a_startVertex)
		{
			EnsureZeroBindForCurrentPS(a_ctx);
			DrawTimePerSurfaceCB2(a_ctx);
			QATickDraw(a_ctx);
			if (s_arrayIsOMDSV.load(std::memory_order_acquire))
				CountShadowDraw();
			g_origDraw(a_ctx, a_vertexCount, a_startVertex);
		}

		// Skyrim renders nearly everything indexed (BSGeometry index buffers),
		// so the vanilla Draw hook alone is a blind spot for the shadow-map
		// passes. Count DrawIndexed / DrawIndexedInstanced too.
		void STDMETHODCALLTYPE HookDrawIndexed(
			::ID3D11DeviceContext* a_ctx, UINT a_indexCount,
			UINT a_startIndex, INT a_baseVertex)
		{
			EnsureZeroBindForCurrentPS(a_ctx);
			DrawTimePerSurfaceCB2(a_ctx);
			QATickDraw(a_ctx);
			if (s_arrayIsOMDSV.load(std::memory_order_acquire))
				CountShadowDraw();
			g_origDrawIndexed(a_ctx, a_indexCount, a_startIndex, a_baseVertex);
		}

		void STDMETHODCALLTYPE HookDrawIndexedInstanced(
			::ID3D11DeviceContext* a_ctx, UINT a_indexCountPerInstance,
			UINT a_instanceCount, UINT a_startIndexLocation,
			INT a_baseVertexLocation, UINT a_startInstanceLocation)
		{
			EnsureZeroBindForCurrentPS(a_ctx);
			DrawTimePerSurfaceCB2(a_ctx);
			QATickDraw(a_ctx);
			if (s_arrayIsOMDSV.load(std::memory_order_acquire))
				CountShadowDraw();
			g_origDrawIndexedInstanced(a_ctx, a_indexCountPerInstance,
				a_instanceCount, a_startIndexLocation, a_baseVertexLocation,
				a_startInstanceLocation);
		}

	// =====================================================================
	// [SLF-QA] frame-quality probe (2026-09-06) - replaces eyeball verdicts.
	// User mandate: "你能不能仅用数据检测不要我亲眼看" (judge builds from
	// data, not from what the user sees). This probe turns the rendered
	// picture into numbers by readback of the main render target:
	//   mean luma / min / max per 2 s bucket,
	//   flicker: fraction of samples where mean luma moved >=8% OR >5% of
	//            grid points moved >=8% vs the previous sample,
	//   lamp steps: single-sample mean-luma jumps >=25% (light on/off).
	// Trigger: <game dir>/ShadowLimitFix_QA.start appears (game dir = CWD,
	// the same relative path convention the plugin uses for INI files).
	// Auto-stops after 90 s or on ShadowLimitFix_QA.stop, logs a summary.
	// Idle cost: one FindFirstFileA per 500 ms on the render thread. While
	// sampling it performs one full-texture CopyResource+Map every ~100 ms
	// on the main RT (only ever when that RT is not OM-bound as the current
	// render target) - the ~1 ms/frame GPU cost only exists during a QA run.
	// A/B builds are then compared numerically instead of by eye.
	// ---------------------------------------------------------------------
#if SLF_QA_ENABLED
		// (QA entities are direct members of the enclosing anonymous
		// namespace - same layer as the hook thunks that call them)
		enum class QaPhase : std::uint8_t { Idle, Armed, Sampling, Done };
		QaPhase s_qaPhase = QaPhase::Idle;

			// ---- resources (cached across runs; released never - debug) ----
			::ID3D11Texture2D* s_qaMainTex = nullptr;    // chosen "the picture"
			::D3D11_TEXTURE2D_DESC s_qaMainDesc{};
			::ID3D11Texture2D* s_qaStage = nullptr;      // staging copy
			std::uintptr_t s_qaStageKey = 0;             // staging rebuild key
			std::uint64_t s_qaBestTexels = 0;            // Armed RTV picker

			// ---- timing ----
			double s_qaRunStart = 0;    // Armed entry (sec, QPC)
			double s_qaLastPoll = 0;    // start-file poll cadence (idle)
			double s_qaLastSample = 0;  // last CopyResource issue (sampling)
			double s_qaBucketT = 0;     // next 2 s flush boundary
			double s_qaFirstTick = 0;   // first Draw seen (auto-trigger base)
			double s_qaSceneReady = 0;  // 2026-09-06: first scheduler publish
			                            // of >=8 real casters (loading/menu
			                            // screens publish 0 - "first Draw" is
			                            // NOT "in game scene", QA sampled the
			                            // loading screen on 09:26)
			double s_qaLastAutoEnd = 0;    // 2026-09-06: AUTO loop - end time of
			                               // the last auto run (QPC sec); 0 =
			                               // never ran -> first run fires 20 s
			                               // after the scene goes live, then
			                               // repeats every kQaAutoGap so QA
			                               // monitors the WHOLE session (was:
			                               // one 90 s sample per process - left
			                               // a 49 min blind gap on 09-06 12:xx
			                               // while the user idled)
			constexpr double kQaCadence = 0.100;   // 10 Hz sampling
			constexpr double kQaDuration = 90.0;   // auto-stop after 90 s
			constexpr double kQaArmWindow = 1.0;   // RTV observation window
			constexpr double kQaAutoDelay = 20.0;  // first auto trigger: 20 s
			                                       // after the FIRST REAL SCENE
			                                       // LIGHT publish (not first
			                                       // Draw)
			constexpr double kQaAutoGap = 30.0;    // idle gap between AUTO runs
			                                       // (90 s sample + 30 s gap =
			                                       // 75 % session coverage)

			// ---- statistics ----
			std::uint32_t s_qaSamples = 0;
			double s_qaLumSum = 0;
			float s_qaLumMin = 1e30f;
			float s_qaLumMax = -1e30f;
			float s_qaPrevMean = -1.f;
			// per 2 s bucket
			std::uint32_t s_qaBkSamples = 0;
			double s_qaBkLumSum = 0;
			float s_qaBkMin = 1e30f;
			float s_qaBkMax = -1e30f;
			double s_qaBkFlickPtsSum = 0;   // per-sample point-flick %
			float s_qaBkDMeanPk = 0;        // max |delta mean| % in bucket
			// totals
			std::uint32_t s_qaFlickSamples = 0;  // flagged: dMean>=8% or pts>=5%
			std::uint32_t s_qaLampSteps = 0;     // |dMean| >= 25%
			std::uint32_t s_qaLampLogged = 0;
			bool s_qaRawLuma = false;            // fmt unsupported -> pseudo luma
			bool s_qaCopyPending = false;
			bool s_qaWantCopy = false;
			std::vector<float> s_qaPrevGrid;     // per-point luma of last sample
			std::uint32_t s_qaGridN = 0;
			bool s_qaWarnedFmt = false;

			double QaNow()
			{
				static const double s_freq = [] {
					LARGE_INTEGER f{};
					QueryPerformanceFrequency(&f);
					return double(f.QuadPart);
				}();
				LARGE_INTEGER c{};
				QueryPerformanceCounter(&c);
				return double(c.QuadPart) / s_freq;
			}

			bool QaFileExists(const char* a_name)
			{
				WIN32_FIND_DATAA fd{};
				const HANDLE h = FindFirstFileA(a_name, &fd);
				if (h == INVALID_HANDLE_VALUE)
					return false;
				FindClose(h);
				return true;
			}

			void QaRemoveFile(const char* a_name)
			{
				DeleteFileA(a_name);
			}

			int QaBytesPerTexel(DXGI_FORMAT a_fmt)
			{
				switch (a_fmt) {
				case DXGI_FORMAT_R32G32B32A32_FLOAT:
					return 16;
				case DXGI_FORMAT_R16G16B16A16_FLOAT:
				case DXGI_FORMAT_R16G16B16A16_UNORM:
				case DXGI_FORMAT_R16G16B16A16_UINT:
					return 8;
				case DXGI_FORMAT_R8G8B8A8_UNORM:
				case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				case DXGI_FORMAT_B8G8R8A8_UNORM:
				case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				case DXGI_FORMAT_B8G8R8A8_TYPELESS:
				case DXGI_FORMAT_B8G8R8X8_UNORM:
				case DXGI_FORMAT_R10G10B10A2_UNORM:
				case DXGI_FORMAT_R10G10B10A2_TYPELESS:
				case DXGI_FORMAT_R11G11B10_FLOAT:
					return 4;
				default:
					return 4;  // raw fallback assumption
				}
			}

			float QaHalfToFloat(std::uint16_t h)
			{
				const std::uint32_t sign = (std::uint32_t(h) & 0x8000u) << 16;
				const std::uint32_t e = (h >> 10) & 0x1Fu;
				const std::uint32_t m = h & 0x3FFu;
				std::uint32_t f = 0;
				if (e == 0x1F) {
					f = sign | 0x7F800000u | (m << 13);  // inf / nan
				} else if (e == 0) {
					if (m == 0) {
						f = sign;
					} else {
						std::uint32_t mm = m;
						int shift = 0;
						while (!(mm & 0x400u)) {
							mm <<= 1;
							shift++;
						}
						mm &= 0x3FFu;
						const std::uint32_t e2 = 127u - 15u - static_cast<std::uint32_t>(shift) + 1u;
						f = sign | (e2 << 23) | (mm << 13);
					}
				} else {
					f = sign | ((e - 15u + 127u) << 23) | (m << 13);
				}
				float out = 0.f;
				std::memcpy(&out, &f, sizeof(out));
				return out;
			}

			// Luma of one texel (0..1). -1 = format has no luma decode.
			float QaTexelLuma(const std::uint8_t* p, DXGI_FORMAT a_fmt)
			{
				switch (a_fmt) {
				case DXGI_FORMAT_R8G8B8A8_UNORM:
				case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				case DXGI_FORMAT_R8G8B8A8_TYPELESS: {
					const float r = p[0] / 255.f, g = p[1] / 255.f, b = p[2] / 255.f;
					return 0.299f * r + 0.587f * g + 0.114f * b;
				}
				case DXGI_FORMAT_B8G8R8A8_UNORM:
				case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				case DXGI_FORMAT_B8G8R8A8_TYPELESS:
				case DXGI_FORMAT_B8G8R8X8_UNORM: {
					const float b = p[0] / 255.f, g = p[1] / 255.f, r = p[2] / 255.f;
					return 0.299f * r + 0.587f * g + 0.114f * b;
				}
				case DXGI_FORMAT_R10G10B10A2_UNORM:
				case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
					const std::uint32_t v = *reinterpret_cast<const std::uint32_t*>(p);
					const float r = (v & 0x3FFu) / 1023.f;
					const float g = ((v >> 10) & 0x3FFu) / 1023.f;
					const float b = ((v >> 20) & 0x3FFu) / 1023.f;
					return 0.299f * r + 0.587f * g + 0.114f * b;
				}
				case DXGI_FORMAT_R16G16B16A16_FLOAT: {
					const auto* h = reinterpret_cast<const std::uint16_t*>(p);
					const float r = QaHalfToFloat(h[0]);
					const float g = QaHalfToFloat(h[1]);
					const float b = QaHalfToFloat(h[2]);
					return 0.299f * r + 0.587f * g + 0.114f * b;
				}
				case DXGI_FORMAT_R32G32B32A32_FLOAT: {
					const auto* f = reinterpret_cast<const float*>(p);
					return 0.299f * f[0] + 0.587f * f[1] + 0.114f * f[2];
				}
				case DXGI_FORMAT_R11G11B10_FLOAT: {
					const std::uint32_t v = *reinterpret_cast<const std::uint32_t*>(p);
					const auto dec = [](std::uint32_t bits, std::uint32_t eb, std::uint32_t mb) {
						const std::uint32_t e = (bits >> mb) & ((1u << eb) - 1u);
						const std::uint32_t m = bits & ((1u << mb) - 1u);
						if (e == 0)
							return m / float(1u << mb);       // small denorm-ish
						const std::uint32_t e2 = (e == (1u << eb) - 1u) ? 0xFFu : e + 127u - 15u;
						const std::uint32_t fbits = (e2 << 23) | (m << (23 - mb));
						float out = 0.f;
						std::memcpy(&out, &fbits, sizeof(out));
						return out;
					};
					const float r = dec(v & 0x7FFu, 5, 6);
					const float g = dec((v >> 11) & 0x7FFu, 5, 6);
					const float b = dec((v >> 22) & 0x3FFu, 5, 5);
					return 0.299f * r + 0.587f * g + 0.114f * b;
				}
				default:
					return -1.f;
				}
			}

			// Read the pending staging copy, aggregate one sample.
			static void QaMapReadSample(::ID3D11DeviceContext* a_ctx)
			{
				::D3D11_MAPPED_SUBRESOURCE ms{};
				if (FAILED(a_ctx->Map(s_qaStage, 0, ::D3D11_MAP_READ, 0, &ms))) {
					s_qaCopyPending = false;
					return;
				}
				const auto* base = static_cast<const std::uint8_t*>(ms.pData);
				const DXGI_FORMAT fmt = s_qaMainDesc.Format;
				const std::uint32_t W = s_qaMainDesc.Width;
				const std::uint32_t H = s_qaMainDesc.Height;
				const std::uint32_t bpp = static_cast<std::uint32_t>(QaBytesPerTexel(fmt));

				std::vector<float> cur;
				cur.reserve(s_qaGridN);
				float sum = 0.f;
				float mn = 1e30f;
				float mx = -1e30f;
				std::uint32_t n = 0;
				bool decodeOk = true;
				for (std::uint32_t y = 0; y < H; y += 16) {
					const auto* row = base + std::size_t(y) * ms.RowPitch;
					for (std::uint32_t x = 0; x < W; x += 16) {
						const auto* px = row + std::size_t(x) * bpp;
						float l = QaTexelLuma(px, fmt);
						if (l < 0.f) {
							if (!s_qaRawLuma) {
								s_qaRawLuma = true;
								SKSE::log::warn("[SLF-QA] fmt={} has no luma decode -> RAW pseudo-luma (relative only)",
									static_cast<int>(fmt));
							}
							l = float(*reinterpret_cast<const std::uint16_t*>(px) & 0xFFFFu) / 65535.f;
							decodeOk = false;
						}
						cur.push_back(l);
						sum += l;
						if (l < mn)
							mn = l;
						if (l > mx)
							mx = l;
						n++;
					}
				}
				a_ctx->Unmap(s_qaStage, 0);
				s_qaCopyPending = false;
				if (!decodeOk && !s_qaWarnedFmt) {
					s_qaWarnedFmt = true;
					SKSE::log::warn("[SLF-QA] luma values are RAW pseudo-luma (fmt {} unsupported) - absolute brightness invalid, flicker RELATIVE metrics still valid",
						static_cast<int>(fmt));
				}
				if (n == 0)
					return;
				s_qaGridN = n;

				// point flicker % vs previous grid
				float ptFlick = 0.f;
				if (s_qaPrevGrid.size() == cur.size()) {
					std::uint32_t moved = 0;
					for (std::size_t i = 0; i < cur.size(); i++) {
						if (std::fabs(cur[i] - s_qaPrevGrid[i]) > 0.08f)
							moved++;
					}
					ptFlick = 100.f * float(moved) / float(cur.size());
				}
				s_qaPrevGrid.swap(cur);

				const float mean = sum / float(n);
				s_qaSamples++;
				s_qaLumSum += mean;
				if (mn < s_qaLumMin)
					s_qaLumMin = mn;
				if (mx > s_qaLumMax)
					s_qaLumMax = mx;

				// bucket aggregation (signed delta % vs previous sample)
				double dMeanPctSigned = 0.0;
				float prevMean = -1.f;
				if (s_qaPrevMean >= 0.f) {
					prevMean = s_qaPrevMean;
					const double denom = std::max(1e-6, double(std::max(mean, s_qaPrevMean)));
					dMeanPctSigned = 100.0 * (double(mean) - s_qaPrevMean) / denom;
				}
				s_qaPrevMean = mean;
				const double dMeanPctAbs = std::fabs(dMeanPctSigned);
				s_qaBkSamples++;
				s_qaBkLumSum += mean;
				if (mn < s_qaBkMin)
					s_qaBkMin = mn;
				if (mx > s_qaBkMax)
					s_qaBkMax = mx;
				s_qaBkFlickPtsSum += ptFlick;
				if (dMeanPctAbs > s_qaBkDMeanPk)
					s_qaBkDMeanPk = float(dMeanPctAbs);

				// sample flag + lamp-step event
				if (dMeanPctAbs >= 8.0 || ptFlick >= 5.0)
					s_qaFlickSamples++;
				if (dMeanPctAbs >= 25.0) {
					s_qaLampSteps++;
					if (s_qaLampLogged < 30) {
						s_qaLampLogged++;
						SKSE::log::info("[SLF-QA] EVT t={:6.1f}s luma {:.3f}->{:.3f} (d{:+.0f}%){}",
							QaNow() - s_qaRunStart, prevMean, mean, dMeanPctSigned,
							s_qaLampLogged == 30 ? " ...(more suppressed)" : "");
					}
				}

				// 2 s bucket flush
				const double t = QaNow() - s_qaRunStart;
				if (t >= s_qaBucketT) {
					s_qaBucketT += 2.0;
					if (s_qaBkSamples > 0) {
						SKSE::log::info("[SLF-QA] t={:5.1f}s n={:3d} mean={:.3f} lo={:.3f} hi={:.3f} dMeanPk={:4.0f}% flickPts={:4.1f}%",
							s_qaBucketT - 2.0, s_qaBkSamples,
							s_qaBkLumSum / double(s_qaBkSamples), s_qaBkMin, s_qaBkMax,
							s_qaBkDMeanPk, s_qaBkFlickPtsSum / double(s_qaBkSamples));
					}
					s_qaBkSamples = 0;
					s_qaBkLumSum = 0;
					s_qaBkMin = 1e30f;
					s_qaBkMax = -1e30f;
					s_qaBkFlickPtsSum = 0;
					s_qaBkDMeanPk = 0.f;
				}
			}

			// Cadence-gated: read the pending copy, issue the next one. Only
			// ever runs when the main RT is NOT currently OM-bound as an RTV
			// (QANoteOMSetRT skips bindsMain moments; QATickDraw falls back
			// for the rare all-main-pass window).
			static void QaTrySample(::ID3D11DeviceContext* a_ctx)
			{
				if (!s_qaMainTex || !s_qaStage)
					return;
				const double now = QaNow();
				if (now - s_qaLastSample < kQaCadence)
					return;
				if (s_qaCopyPending)
					QaMapReadSample(a_ctx);
				a_ctx->CopyResource(s_qaStage, s_qaMainTex);
				s_qaCopyPending = true;
				s_qaWantCopy = false;
				s_qaLastSample = now;
			}

			static void QaStartSampling(::ID3D11DeviceContext* a_ctx)
			{
				::ID3D11Device* dev = nullptr;
				a_ctx->GetDevice(&dev);
				if (!dev) {
					SKSE::log::warn("[SLF-QA] arming failed: no D3D device");
					s_qaPhase = QaPhase::Idle;
					return;
				}
				::D3D11_TEXTURE2D_DESC sd = s_qaMainDesc;
				sd.Usage = ::D3D11_USAGE_STAGING;
				sd.BindFlags = 0;
				sd.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
				sd.MiscFlags = 0;
				if (s_qaStage && s_qaStageKey != reinterpret_cast<std::uintptr_t>(s_qaMainTex)) {
					s_qaStage->Release();
					s_qaStage = nullptr;
				}
				if (!s_qaStage) {
					if (FAILED(dev->CreateTexture2D(&sd, nullptr, &s_qaStage))) {
						SKSE::log::warn("[SLF-QA] staging texture create failed ({}x{} fmt={})",
							sd.Width, sd.Height, static_cast<int>(sd.Format));
						dev->Release();
						s_qaPhase = QaPhase::Idle;
						return;
					}
					s_qaStageKey = reinterpret_cast<std::uintptr_t>(s_qaMainTex);
				}
				dev->Release();
				SKSE::log::info("[SLF-QA] ==== SAMPLING START ==== main RTV tex=0x{:x} {}x{} fmt={} mips={} arr={} cadence={}ms dur={}s",
					reinterpret_cast<std::uintptr_t>(s_qaMainTex), s_qaMainDesc.Width, s_qaMainDesc.Height,
					static_cast<int>(s_qaMainDesc.Format), s_qaMainDesc.MipLevels, s_qaMainDesc.ArraySize,
					int(kQaCadence * 1000.0), int(kQaDuration));
				s_qaRunStart = QaNow();
				s_qaBucketT = 2.0;
				s_qaLastSample = s_qaRunStart;
				s_qaPhase = QaPhase::Sampling;
			}

			static void QaDumpAndReset(::ID3D11DeviceContext* a_ctx)
			{
				// drain a pending copy so the last sample counts
				if (s_qaCopyPending)
					QaMapReadSample(a_ctx);
				const double dur = QaNow() - s_qaRunStart;
				const double meanAll = s_qaSamples ? s_qaLumSum / double(s_qaSamples) : 0.0;
				const double flickPct = s_qaSamples ? 100.0 * double(s_qaFlickSamples) / double(s_qaSamples) : 0.0;
				SKSE::log::info("[SLF-QA] ========== SUMMARY ==========");
				SKSE::log::info("[SLF-QA] run={:.1f}s samples={} luma mean={:.4f} min={:.4f} max={:.4f}{}",
					dur, s_qaSamples, meanAll,
					s_qaLumMin > 1e29 ? 0.f : s_qaLumMin, s_qaLumMax < -1e29 ? 0.f : s_qaLumMax,
					s_qaRawLuma ? " (RAW pseudo-luma)" : "");
				SKSE::log::info("[SLF-QA] flicker: {}/{} samples ({}%) moved >=8% (mean or >5% of points)",
					s_qaFlickSamples, s_qaSamples, flickPct);
				SKSE::log::info("[SLF-QA] lamp steps (|dMean|>=25% single sample): {}", s_qaLampSteps);
				if (s_qaLampSteps > 0 && s_qaLampSteps > s_qaLampLogged)
					SKSE::log::info("[SLF-QA]   (+{} more lamp-step events not listed)", s_qaLampSteps - s_qaLampLogged);
				if (flickPct < 5.0 && s_qaLampSteps == 0)
					SKSE::log::info("[SLF-QA] VERDICT: STABLE - no measurable flicker, no lamp-level step");
				else if (s_qaLampSteps > 0 && flickPct < 20.0)
					SKSE::log::info("[SLF-QA] VERDICT: LAMP-STEP EVENTS present (picture level jumped; see EVT lines)");
				else
					SKSE::log::info("[SLF-QA] VERDICT: FLICKER - {}% of samples moved >=8%", flickPct);
				SKSE::log::info("[SLF-QA] ========== END ==========");
				// reset state (resources stay cached for the next run)
				s_qaPhase = QaPhase::Idle;
				s_qaPrevMean = -1.f;
				s_qaPrevGrid.clear();
				s_qaSamples = 0;
				s_qaLumSum = 0;
				s_qaLumMin = 1e30f;
				s_qaLumMax = -1e30f;
				s_qaFlickSamples = 0;
				s_qaLampSteps = 0;
				s_qaLampLogged = 0;
				s_qaBkSamples = 0;
				s_qaBkLumSum = 0;
				s_qaBkMin = 1e30f;
				s_qaBkMax = -1e30f;
				s_qaBkFlickPtsSum = 0;
				s_qaBkDMeanPk = 0.f;
				s_qaGridN = 0;
				s_qaCopyPending = false;
				s_qaWantCopy = false;
				s_qaRawLuma = false;
				s_qaWarnedFmt = false;
				QaRemoveFile("ShadowLimitFix_QA.start");
				QaRemoveFile("ShadowLimitFix_QA.stop");
				s_qaLastAutoEnd = QaNow();  // AUTO loop: next run fires after
				                            // kQaAutoGap of idle
				SKSE::log::info("[SLF-QA] reset to idle (AUTO re-arms in {:.0f}s; drop ShadowLimitFix_QA.start to force an immediate run)",
					kQaAutoGap);
			}

			// Called from every Draw/DrawIndexed/DrawIndexedInstanced thunk.
			void QATickDraw(::ID3D11DeviceContext* a_ctx)
			{
				switch (s_qaPhase) {
				case QaPhase::Idle: {
					const double now = QaNow();
					if (now - s_qaLastPoll < 0.5)
						return;
					s_qaLastPoll = now;
					if (s_qaFirstTick == 0)
						s_qaFirstTick = now;
					// 2026-09-06: the scheduler (107137 hook, Scheduler.cpp)
					// publishes the engine's real caster list every frame;
					// loading/menu screens publish 0. Gate the auto trigger on
					// the FIRST publish of >=8 casters so the 90 s sample
					// covers an actual in-game scene (09:26 QA sampled the
					// loading screen and was void).
					if (s_qaSceneReady == 0 &&
						ShadowLimitFixNS::P1::g_scheduledShadowCount.load(std::memory_order_acquire) >= 8)
						s_qaSceneReady = now;
					// Manual trigger (.start file) always wins; otherwise run
					// ONE automatic 90 s sample per process 20 s after the
					// scene went live so every session produces QA data
					// without any file dance (2026-09-06: user ran twice,
					// never dropped the file -> no samples; auto mode fixes
					// the process gap).
					const char* how = nullptr;
					if (QaFileExists("ShadowLimitFix_QA.start"))
						how = "ShadowLimitFix_QA.start found";
					else if (s_qaSceneReady != 0 &&
						(s_qaLastAutoEnd == 0 ? now - s_qaSceneReady >= kQaAutoDelay
						                      : now - s_qaLastAutoEnd >= kQaAutoGap))
						how = (s_qaLastAutoEnd == 0)
							? "AUTO (first run, 20 s after scene live)"
							: "AUTO (repeat - full-session coverage)";
					if (!how)
						return;
					SKSE::log::info("[SLF-QA] ==== TRIGGER ==== ({}) - stay in the test scene ~{} s (walking OK, data is numeric)",
						how, int(kQaDuration));
					// Manual .start always re-triggers on later polls; AUTO
					// repeats forever (QaDumpAndReset stamps s_qaLastAutoEnd
					// when a run ends, the next fires kQaAutoGap later) so the
					// whole session is monitored, not just the first 90 s.
					if (!QaFileExists("ShadowLimitFix_QA.start")) {
						QaRemoveFile("ShadowLimitFix_QA.start");
						QaRemoveFile("ShadowLimitFix_QA.stop");
					}
					s_qaBestTexels = 0;
					s_qaPrevMean = -1.f;
					s_qaPhase = QaPhase::Armed;
					s_qaRunStart = now;
					break;
				}
				case QaPhase::Armed: {
					const double now = QaNow();
					if (now - s_qaRunStart < kQaArmWindow)
						return;
					if (!s_qaMainTex) {
						SKSE::log::warn("[SLF-QA] arming failed: no non-shadow RTV observed in {:.0f}s",
							kQaArmWindow);
						s_qaPhase = QaPhase::Idle;
						return;
					}
					QaStartSampling(a_ctx);
					break;
				}
				case QaPhase::Sampling: {
					const double now = QaNow();
					// auto-stop / early stop check (throttled by the cadence gate)
					if (now - s_qaLastSample < kQaCadence)
						return;
					if (now - s_qaRunStart >= kQaDuration ||
						QaFileExists("ShadowLimitFix_QA.stop")) {
						SKSE::log::info("[SLF-QA] stopping ({})", now - s_qaRunStart >= kQaDuration ? "90 s auto-stop" : "QA.stop file");
						QaDumpAndReset(a_ctx);
						return;
					}
					// fallback sampling for windows where only the main RTV binds
					if (s_qaWantCopy || s_qaCopyPending)
						QaTrySample(a_ctx);
					break;
				}
				case QaPhase::Done:
					break;
				}
			}

			// Called from the OMSetRenderTargets thunk (records the main RTV
			// while arming; samples when the OM state moves OFF the main RT).
			void QANoteOMSetRT(::ID3D11DeviceContext* a_ctx, UINT a_numViews,
				::ID3D11RenderTargetView* const* a_rtvs, ::ID3D11DepthStencilView* a_dsv)
			{
				if (s_qaPhase == QaPhase::Idle || s_qaPhase == QaPhase::Done)
					return;
				// is this OMSet binding the main RT itself?
				bool bindsMain = false;
				if (s_qaMainTex && a_numViews && a_rtvs && a_rtvs[0]) {
					::ID3D11Resource* res = nullptr;
					a_rtvs[0]->GetResource(&res);
					if (res) {
						::ID3D11Texture2D* t2 = nullptr;
						if (SUCCEEDED(res->QueryInterface(__uuidof(::ID3D11Texture2D), reinterpret_cast<void**>(&t2)))) {
							bindsMain = (t2 == s_qaMainTex);
							t2->Release();
						}
						res->Release();
					}
				}
				if (s_qaPhase == QaPhase::Armed) {
					// ignore shadow-array / tiny / MSAA binds; track the
					// largest plain RTV as "the picture"
					if (bindsMain)
						return;  // already tracked
					if (!a_numViews || !a_rtvs || !a_rtvs[0] || a_dsv)
						return;
					if (a_dsv && DSVIsShadowArray(a_dsv))
						return;
					::ID3D11Resource* res = nullptr;
					a_rtvs[0]->GetResource(&res);
					if (!res)
						return;
					::ID3D11Texture2D* tex = nullptr;
					if (FAILED(res->QueryInterface(__uuidof(::ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
						res->Release();
						return;
					}
					res->Release();
					::D3D11_TEXTURE2D_DESC td{};
					tex->GetDesc(&td);
					if (td.SampleDesc.Count > 1 || td.Width < 64 || td.Height < 64) {
						tex->Release();
						return;
					}
					const std::uint64_t texels = std::uint64_t(td.Width) * td.Height;
					if (texels > s_qaBestTexels) {
						s_qaBestTexels = texels;
						if (s_qaMainTex)
							s_qaMainTex->Release();
						s_qaMainTex = tex;
						s_qaMainTex->AddRef();
						s_qaMainDesc = td;
					}
					tex->Release();
					return;
				}
				// Sampling: sample only at safe moments (main RT not bound)
				if (bindsMain) {
					s_qaWantCopy = true;
					return;
				}
				QaTrySample(a_ctx);
			}
#endif  // SLF_QA_ENABLED
	}  // anonymous namespace

	void InstallShadowRenderTrace()
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer)
			return;
		auto* ctx = reinterpret_cast<::ID3D11DeviceContext*>(renderer->GetRuntimeData().context);
		if (!ctx)
			return;
		void** vtbl = *reinterpret_cast<void***>(ctx);
		if (!vtbl)
			return;

		if (!g_origOMSetRT && vtbl[33] != reinterpret_cast<void*>(&HookOMSetRenderTargets)) {
			g_origOMSetRT = reinterpret_cast<OMSetRenderTargetsFn>(vtbl[33]);
			DWORD oldProtect = 0;
			::VirtualProtect(&vtbl[33], sizeof(void*), PAGE_READWRITE, &oldProtect);
			vtbl[33] = reinterpret_cast<void*>(&HookOMSetRenderTargets);
			::VirtualProtect(&vtbl[33], sizeof(void*), oldProtect, &oldProtect);
		}
		if (!g_origClearDSV && vtbl[53] != reinterpret_cast<void*>(&HookClearDepthStencilView)) {
			g_origClearDSV = reinterpret_cast<ClearDepthStencilViewFn>(vtbl[53]);
			DWORD oldProtect = 0;
			::VirtualProtect(&vtbl[53], sizeof(void*), PAGE_READWRITE, &oldProtect);
			vtbl[53] = reinterpret_cast<void*>(&HookClearDepthStencilView);
			::VirtualProtect(&vtbl[53], sizeof(void*), oldProtect, &oldProtect);
		}
		if (!g_origDraw && vtbl[13] != reinterpret_cast<void*>(&HookDraw)) {
			g_origDraw = reinterpret_cast<DrawFn>(vtbl[13]);
			DWORD oldProtect = 0;
			::VirtualProtect(&vtbl[13], sizeof(void*), PAGE_READWRITE, &oldProtect);
			vtbl[13] = reinterpret_cast<void*>(&HookDraw);
			::VirtualProtect(&vtbl[13], sizeof(void*), oldProtect, &oldProtect);
		}
		if (!g_origDrawIndexed && vtbl[12] != reinterpret_cast<void*>(&HookDrawIndexed)) {
			// DrawIndexed is vtable[12] in ID3D11DeviceContext (d3d11.h
			// order: ...DrawIndexed(12), Draw(13), Map(14)...). Verified by
			// script against Windows SDK d3d11.h - do NOT guess these.
			g_origDrawIndexed = reinterpret_cast<DrawIndexedFn>(vtbl[12]);
			DWORD oldProtect = 0;
			::VirtualProtect(&vtbl[12], sizeof(void*), PAGE_READWRITE, &oldProtect);
			vtbl[12] = reinterpret_cast<void*>(&HookDrawIndexed);
			::VirtualProtect(&vtbl[12], sizeof(void*), oldProtect, &oldProtect);
		}
		if (!g_origDrawIndexedInstanced && vtbl[20] != reinterpret_cast<void*>(&HookDrawIndexedInstanced)) {
			// DrawIndexedInstanced is vtable[20] (d3d11.h: ...IASetIndexBuffer(19),
			// DrawIndexedInstanced(20), DrawInstanced(21)...).
			g_origDrawIndexedInstanced = reinterpret_cast<DrawIndexedInstancedFn>(vtbl[20]);
			DWORD oldProtect = 0;
			::VirtualProtect(&vtbl[20], sizeof(void*), PAGE_READWRITE, &oldProtect);
			vtbl[20] = reinterpret_cast<void*>(&HookDrawIndexedInstanced);
			::VirtualProtect(&vtbl[20], sizeof(void*), oldProtect, &oldProtect);
		}
		SKSE::log::info("[SLF] shadow render trace installed (OMSetRT/ClearDSV/Draw+DrawIndexed+DrawIndexedInstanced vs shadow array)");
	}

#if SLF_QA_ENABLED
	void InstallFrameQA()
	{
		// QA probe needs no vtable of its own - it rides the shadow-trace
		// context hooks (installed lazily by BeginTechniqueHook on the first
		// draw). Logging the contract here so the trigger is discoverable.
		SKSE::log::info("[SLF-QA] frame-quality probe LIVE (data-only verdicts - no eyeball reports needed)");
		SKSE::log::info("[SLF-QA] trigger: drop ShadowLimitFix_QA.start next to SkyrimSE.exe, stand still 90 s; auto-stop or ShadowLimitFix_QA.stop");
	}
#endif

	// ---------------------------------------------------------------------
	// v10-phase2-flicker probe (2026-09-03 22:3x). The canvas + slice-pin
	// side is verified correct (SC-A idx=[slot,slot], SelectDSB SAME=yes,
	// slices 8-27 get renders) yet the image flickers. The remaining unknown
	// is the MATERIAL PASS sampling side - four facts, taken at the exact
	// moment the engine binds the shadow-mask texture to PS slot t14 (the
	// CS/SLF-verified "this material pass samples shadows" signal):
	//   (1) t103 SRV view range the engine bound (ArraySize 8 vs 127?)
	//   (2) shadow-canvas slice content at that moment (MAT-pass readback)
	//   (3) engine shadow globals: accum slot / mask index / shadow mask
	//   (4) our scheduled lights' descriptor state at that moment
	// Read-only + throttled; never mutates D3D state or render output.
	// ---------------------------------------------------------------------
	using PSSetShaderResourcesProbeFn = void(STDMETHODCALLTYPE*)(
		::ID3D11DeviceContext*, UINT, UINT, ::ID3D11ShaderResourceView* const*);
	PSSetShaderResourcesProbeFn g_origPSSetSRVProbe = nullptr;

	// fix17d (2026-09-04, crash 01:46): SEH-guarded snapshot helpers for the
	// material-pass probe. The probe runs on the D3D context thread and can
	// fire AFTER the engine released a light that the scheduler still lists
	// (walking out a door = cell change = lights freed/rebuild between the
	// shadow pass and the later material pass). Dereferencing a dangling
	// BSShadowLight* faults (crash: ShadowLimitFix.dll+002823F, rax=0x5 read
	// through a freed object). These helpers catch the AV and report a code
	// instead of crashing the game:
	//   -2 = faulted/unreadable, -1 = no descriptors,
	//    0..127 = descriptor[0].shadowmapIndex, +0x10000 = directional.
	// They are __declspec(noinline) and free of C++ objects so __try compiles
	// under /EHsc (no unwinding required in these frames).
	static __declspec(noinline) std::int32_t SnapshotLightD0(void* a_light)
	{
		__try {
			auto* l = static_cast<RE::BSShadowLight*>(a_light);
			auto& rtd = l->GetRuntimeData();
			const auto& descs = rtd.shadowmapDescriptors;
			std::int32_t d0 = descs.empty() ? -1 : static_cast<std::int32_t>(descs[0].shadowmapIndex);
			if (l->GetIsDirectionalLight())
				d0 |= 0x10000;
			return d0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return -2;
		}
	}

	// Snapshot the engine's shadowLightsAccum roster (ShadowSceneNode
	// +0x230, BSTArray<BSShadowLight*>). Up to a_max entries into a_out.
	// Returns entry count, or -2 if the scene node itself faulted.
	static __declspec(noinline) std::int32_t SnapshotAccumRoster(
		const void* a_ssn, std::int32_t* a_out, std::uint32_t a_max)
	{
		__try {
			auto* ssn = static_cast<const RE::ShadowSceneNode*>(a_ssn);
			const auto& accum = ssn->GetRuntimeData().shadowLightsAccum;
			std::uint32_t n = accum.size();
			if (n > a_max)
				n = a_max;
			for (std::uint32_t i = 0; i < n; i++) {
				auto* l = accum[i];
				a_out[i] = l ? SnapshotLightD0(l) : -1;
			}
			return static_cast<std::int32_t>(n);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return -2;
		}
	}

	void STDMETHODCALLTYPE HookPSSetShaderResourcesProbe(
		::ID3D11DeviceContext* a_ctx, UINT a_startSlot, UINT a_numViews,
		::ID3D11ShaderResourceView* const* a_views)
	{
		g_origPSSetSRVProbe(a_ctx, a_startSlot, a_numViews, a_views);
		if (!(a_startSlot <= 14 && a_startSlot + a_numViews > 14))
			return;  // not a shadow-mask bind -> not a shadow-sampling pass

		// fix11 (23:5x): fix10's log showed 3303 t14-bind hits but EVERY
		// one was shadow-render noise - rendering a light's depth also binds
		// a PS SRV set covering t14 (alpha-tested casters sample their
		// diffuse/alpha maps, which span slots up to/past t14). At that
		// moment the t103 depth array is naturally NOT bound (we are
		// rasterizing shadow depth, not sampling existing shadows), so the
		// probe logged "NOT BOUND" 3303x and NEVER saw the real material
		// pass. Filter to the sampling signal only: skip binds issued while
		// (a) our dispatch is rendering a light, or (b) the last OMSetRT
		// still targets the shadow array. Remaining binds = main-scene
		// material pass consuming shadows (t14 mask + t103 array).
		if (s_renderingLight.load(std::memory_order_relaxed) != nullptr)
			return;
		if (s_arrayIsOMDSV.load(std::memory_order_acquire))
			return;

		static std::uint32_t s_hit = 0;
		if (((++s_hit) & 0x0Fu) != 0)
			return;  // every 16th material-pass shadow bind (post-filter)

		// (1a) fix11 (23:5x): log what THIS bind call put at t14 - the
		// actual shadow resource the material pass samples. t103 was a CS
		// slot convention; if the vanilla pass binds its shadow texture
		// elsewhere (or a 2D mask instead of a depth array), reading only
		// t103 would stay "NOT BOUND" forever and mislead. Print the t14
		// SRV desc directly from the bind args.
		{
			const UINT idx14 = 14 - a_startSlot;  // valid: startSlot<=14<=start+num-1
			::ID3D11ShaderResourceView* t14 = (idx14 < a_numViews && a_views) ? a_views[idx14] : nullptr;
			char d14[160];
			if (t14) {
				::D3D11_SHADER_RESOURCE_VIEW_DESC sd14{};
				t14->GetDesc(&sd14);
				if (sd14.ViewDimension == ::D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
					std::snprintf(d14, sizeof(d14), "bind#%u dim=TEX2DARRAY first=%u arraySize=%u",
						a_numViews, sd14.Texture2DArray.FirstArraySlice, sd14.Texture2DArray.ArraySize);
				} else if (sd14.ViewDimension == ::D3D11_SRV_DIMENSION_TEXTURE2D) {
					std::snprintf(d14, sizeof(d14), "bind#%u dim=TEX2D mips=%u (a 2D mask, NOT an array!)",
						a_numViews, sd14.Texture2D.MipLevels);
				} else {
					std::snprintf(d14, sizeof(d14), "bind#%u dim=%d (other)",
						a_numViews, static_cast<int>(sd14.ViewDimension));
				}
			} else {
				std::snprintf(d14, sizeof(d14), "bind#%u t14=NULL", a_numViews);
			}
			SKSE::log::info("[SLF][MP] t14-bind: PS t14 = {}", d14);
		}

		// (1b) The t103 depth-array view bound for this pass.
		::ID3D11ShaderResourceView* t103 = nullptr;
		a_ctx->PSGetShaderResources(103, 1, &t103);
		{
			char vdesc[128];
			if (t103) {
				::D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
				t103->GetDesc(&sd);
				if (sd.ViewDimension == ::D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
					std::snprintf(vdesc, sizeof(vdesc), "dim=TEX2DARRAY first=%u arraySize=%u",
						sd.Texture2DArray.FirstArraySlice, sd.Texture2DArray.ArraySize);
				} else {
					std::snprintf(vdesc, sizeof(vdesc), "dim=%d (NOT an array!)",
						static_cast<int>(sd.ViewDimension));
				}
			} else {
				std::snprintf(vdesc, sizeof(vdesc), "NOT BOUND");
			}
			SKSE::log::info("[SLF][MP] t14-bind: PS t103 view = {}", vdesc);
			if (t103)
				t103->Release();
		}

		// (2) Canvas content at the sampling moment (own 128-throttle inside).
		DebugReadbackShadowArrayAtMaterialPass(a_ctx);

		// (3) Engine shadow globals (CS names: accum slot / mask index /
		// shadow mask; SLF logs *uid528091 as "engine slot counter").
		static REL::RelocationID uidSlot(528091, 415036);
		static REL::RelocationID uidMask(528093, 415038);
		SKSE::log::info("[SLF][MP] engine globals: accumSlot={} maskIndex={} shadowMask=0x{:08x}",
			*reinterpret_cast<std::uint32_t*>(uidSlot.address()),
			*reinterpret_cast<std::uint32_t*>(uidSlot.address() + 4),
			*reinterpret_cast<std::uint32_t*>(uidMask.address()));

		// (3b) fix17 (2026-09-04): engine accumulator roster at the sampling
		// moment. shadowLightsAccum (ShadowSceneNode +0x230) is what the
		// engine's CalculateActiveShadowCasters filled THIS frame; the first
		// entries are the lights the engine's own shadow-mask pass bakes
		// into t14 channels (maskIndex counts up to 4). Answer: does the
		// roster rotate while the scene stands still (= engine re-picks its
		// t14 lights every frame = the vanilla flicker source) or is it
		// stable and the pop comes from elsewhere?
		{
			static REL::RelocationID uidSSN(513211, 390951);
			auto* ssn = *reinterpret_cast<RE::ShadowSceneNode**>(uidSSN.address());
			if (ssn) {
				// fix17d: read through the SEH snapshot - the lights the
				// engine accumulated this frame may already be freed when
				// this material-pass probe fires (cell change).
				std::int32_t slots[16];
				const std::int32_t nAcc = SnapshotAccumRoster(ssn, slots, 16);
				if (nAcc >= 0) {
					std::string acc;
					char ab[80];
					for (std::int32_t i = 0; i < nAcc; i++) {
						const std::uint32_t idx = static_cast<std::uint32_t>(i);
						const std::int32_t raw = slots[i];
						if (raw == -1) {
							std::snprintf(ab, sizeof(ab), "%s-", idx == 0 ? "" : " ");
						} else if (raw == -2) {
							std::snprintf(ab, sizeof(ab), "%s?", idx == 0 ? "" : " ");
						} else {
							const std::int32_t d0 = raw & 0xFFFF;
							const bool dir = (raw & 0x10000) != 0;
							std::snprintf(ab, sizeof(ab), "%s%u@%d%s", idx == 0 ? "" : " ", idx, d0,
								dir ? "d" : "");
						}
						acc += ab;
					}
					SKSE::log::info("[SLF][MP] engine accum[{}]: {}", nAcc, acc);
				} else {
					SKSE::log::info("[SLF][MP] engine accum: unreadable (scene node faulted)");
				}
			}
		}

		// (4) Our scheduled lights at this moment (slot@descs0.shadowmapIndex).
		// fix17d: per-light deref is SEH-guarded - the probe can fire after
		// the scheduler's lights were freed (cell change) and the scheduler
		// may be overwriting this array concurrently.
		const std::uint32_t n = g_scheduledShadowCount.load(std::memory_order_acquire);
		std::string line;
		char buf[64];
		for (std::uint32_t i = 0; i < n; i++) {
			auto& e = g_scheduledShadowLights[i];
			if (!e.light) {
				std::snprintf(buf, sizeof(buf), "%s-", i == 0 ? "" : " ");
				line += buf;
				continue;
			}
			const std::int32_t raw = SnapshotLightD0(e.light);
			if (raw == -2) {
				std::snprintf(buf, sizeof(buf), "%s?@%u", i == 0 ? "" : " ", e.slot);
			} else if (raw == -1) {
				std::snprintf(buf, sizeof(buf), "%s%u@-", i == 0 ? "" : " ", e.slot);
			} else {
				const std::int32_t d0 = raw & 0xFFFF;
				std::snprintf(buf, sizeof(buf), "%s%u@%d", i == 0 ? "" : " ", e.slot, d0);
			}
			line += buf;
		}
		SKSE::log::info("[SLF][MP] scheduled {} lights (slot@idx0): {}", n, line);
	}

	void InstallMaterialPassProbe()
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			SKSE::log::warn("[SLF][MP] probe install aborted: Renderer singleton null (kDataLoaded too early?)");
			return;
		}
		auto* ctx = reinterpret_cast<::ID3D11DeviceContext*>(renderer->GetRuntimeData().context);
		if (!ctx) {
			SKSE::log::warn("[SLF][MP] probe install aborted: Renderer context null (kDataLoaded too early?)");
			return;
		}
		void** vtbl = *reinterpret_cast<void***>(ctx);
		if (!vtbl) {
			SKSE::log::warn("[SLF][MP] probe install aborted: context vtable null");
			return;
		}
		// fix10 (23:4x): slot REVERTED 16 -> 8. fix8 was a WRONG fix: it
		// assumed "0-6 IUnknown/DeviceChild, 7-14 VS*, 15+ PS*" and moved the
		// hook from slot 8 to 16. Verified against the Windows SDK
		// d3d11.h (10.0.26100.0) ID3D11DeviceContext declaration - the VS/PS
		// methods are INTERLEAVED (historic d3d11 vtable quirk):
		//   7 VSSetConstantBuffers
		//   8 PSSetShaderResources   <- PIXEL-stage SRV bind lives HERE
		//   9 PSSetShader
		//   16 PSSetConstantBuffers  <- what fix8 actually hooked
		// So the ORIGINAL slot 8 was correct all along; the pre-fix8 probe
		// never fired only because it was installed at PostLoad when the
		// D3D context was still null (never installed at all). fix9 moved
		// install to kDataLoaded (probe now installs, 23:44:25 confirmed)
		// but fix8's slot 16 = PSSetConstantBuffers: cbuffer binds (slot
		// 0-3) go through it, parsed as SRVs -> t14 condition never true ->
		// installed but silent. fix10 restores slot 8.
		if (g_origPSSetSRVProbe || vtbl[8] == reinterpret_cast<void*>(&HookPSSetShaderResourcesProbe))
			return;
		g_origPSSetSRVProbe = reinterpret_cast<PSSetShaderResourcesProbeFn>(vtbl[8]);
		DWORD oldProtect = 0;
		::VirtualProtect(&vtbl[8], sizeof(void*), PAGE_READWRITE, &oldProtect);
		vtbl[8] = reinterpret_cast<void*>(&HookPSSetShaderResourcesProbe);
		::VirtualProtect(&vtbl[8], sizeof(void*), oldProtect, &oldProtect);
		SKSE::log::info("[SLF] material-pass probe installed (PSSetShaderResources vtable[8], t14-bind)");
	}
}
