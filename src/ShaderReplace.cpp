// ShaderReplace.cpp - P1c-3: material PS replacement pipeline
// Compiles CS Lighting.hlsl (engine macros + PSHADER) for LANDSCAPE pixel
// shaders, caches them, and swaps them in at BeginTechnique time
// (CS ShaderCache mechanism, ported from DynamicWetness Wetness.cpp).
//
// P1c-3 step 2 goal: replacement pipeline works, screen identical to
// vanilla (CS Lighting.hlsl without WETNESS_EFFECTS = vanilla lighting).
// Step 3 (later): add N-light shadow sampling to the compiled shader.
#include <RE/Skyrim.h>
#include <RE/B/BSShaderRenderTargets.h>  // RE::BSGraphics::RENDER_TARGET / _DEPTHSTENCIL
#include <SKSE/SKSE.h>

#include <d3d11.h>          // native ID3D11DeviceContext (OMGetDepthStencil)
#include <d3dcompiler.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "HookUtil.h"
#include "SlfBytecodePatch.h"   // SLF-B: engine-PS shadow-unit bytecode surgery

namespace ShadowLimitFixNS::P1
{
	// ---------------------------------------------------------------------
	// Vanilla PS bytecode capture (ID3D11Device::CreatePixelShader vtable
	// slot 15). Needed to study how the ORIGINAL engine shader samples
	// shadows (the "from vanilla semantics" path - independent of CS/ENB).
	// Record bytecode per shader object at creation; dumped to disk from the
	// LoadShaders hook for Lighting techniques.
	// ---------------------------------------------------------------------
	std::mutex g_psBcMutex;
	std::unordered_map<::ID3D11PixelShader*, std::vector<std::uint8_t>> g_psBytecode;
	std::uint32_t g_psBcTotal = 0;

	using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(
		::ID3D11Device*, const void*, SIZE_T, const ::ID3D11ClassLinkage*, ::ID3D11PixelShader**);
	CreatePixelShaderFn g_origCreatePS = nullptr;

	// SLF-B statistics + per-variant dedup for the shadow-unit patch
	std::atomic<std::uint32_t> g_shadowPatched{0};
	std::atomic<std::uint32_t> g_shadowNoUnit{0};
	std::atomic<std::uint32_t> g_shadowRejected{0};
	std::mutex g_patchSeenMutex;
	std::unordered_set<std::uint64_t> g_patchSeen;   // FNV-1a of raw bytecode (patched)
	std::unordered_set<std::uint64_t> g_rejectSeen;  // FNV-1a of raw bytecode (guard-rejected)
	// patched PS *objects* (not bytecode) - so the material-pass probe can
	// tell whether the engine actually DRAWS the patched shaders it created.
	std::mutex g_patchObjMutex;
	std::unordered_set<::ID3D11PixelShader*> g_patchedObj;
	// Zero-default binding for the patch payload. D3D11: reading an SRV slot
	// that was never bound is UNDEFINED (drivers return NaN garbage, which
	// defeats the payload's enabled-gate and produces full-black + flicker).
	// Until B4 fills real light data we bind a 1-element zero LightRec here,
	// so the payload reads enabled=0 -> gate short-circuits -> factor 1.0.
	// NOTE: out-of-bounds StructuredBuffer reads are DEFINED as 0 in D3D11,
	// so one zero element covers every slot index the engine loop may use.
	std::mutex g_zeroMutex;
	::ID3D11Buffer* g_zeroLRBuf = nullptr;
	::ID3D11ShaderResourceView* g_zeroLRSRV = nullptr;

	// ---- SLF-B B4 resource set (2026-09-05, bind-pipeline step) ----
	// The patched payload consumes three slots:
	//   t102  LightRec structured buffer (96B stride), indexed by the engine
	//         light-loop counter. Until B4 fills real per-light records this
	//         stays the 32-elem zero buffer (every enabled gate reads 0 ->
	//         shadow factor 1.0 -> screen identical to the dim-0.35 build).
	//   t103  kSHADOWMAPS depth-array SRV (full 127-slice view) - the
	//         payload's SampleLevel target once enabled opens the gate.
	//   s15   the sampler the payload's sample_l instructions reference
	//         (probe HLSL bound it as register(s15); the engine never binds
	//         s15, so WE must bind it at draw time like t102).
	// Binds are INERT while every LightRec stays disabled - the payload
	// short-circuits before touching t103/s15. This build is therefore a
	// pure "wire the pipeline, prove it binds, screen must not change" step.
	std::mutex g_b2bMutex;
	::ID3D11ShaderResourceView* g_b2bDepthSRV = nullptr;   // t103
	::ID3D11SamplerState* g_b2bSampler = nullptr;          // s15

	// Full-array (127-slice) view over the kSHADOWMAPS texture. The engine's
	// own depthSRV may only cover the vanilla 8 slices; the payload needs
	// whatever slice each light's shadowMapIndex says, so build a full view
	// when the engine view is too small (same logic the dormant SLF_PS path
	// used, now lifted into the shared B2b path).
	void EnsureB2bDepthSRV(::ID3D11Device* a_dev)
	{
		std::lock_guard<std::mutex> lock(g_b2bMutex);
		if (g_b2bDepthSRV)
			return;
		if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
			auto& dsd = renderer->GetDepthStencilData();
			auto* srv = dsd.depthStencils[4].depthSRV;
			if (!srv) {
				SKSE::log::warn("[SLF-B] EnsureB2bDepthSRV: kSHADOWMAPS depthSRV NULL");
				return;
			}
			auto* native = reinterpret_cast<::ID3D11ShaderResourceView*>(srv);
			::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			native->GetDesc(&srvDesc);
			const int arraySize =
				srvDesc.ViewDimension == ::D3D11_SRV_DIMENSION_TEXTURE2DARRAY ?
				static_cast<int>(srvDesc.Texture2DArray.ArraySize) : -1;
			if (arraySize >= 127) {
				g_b2bDepthSRV = native;
				g_b2bDepthSRV->AddRef();
				SKSE::log::info("[SLF-B] t103 SRV: engine view already covers {} slices (reused)", arraySize);
			} else {
				::ID3D11Resource* tex = nullptr;
				native->GetResource(&tex);
				if (tex) {
					::D3D11_TEXTURE2D_DESC td{};
					reinterpret_cast<::ID3D11Texture2D*>(tex)->GetDesc(&td);
					::D3D11_SHADER_RESOURCE_VIEW_DESC full{};
					full.Format = srvDesc.Format;
					full.ViewDimension = ::D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
					full.Texture2DArray.MipLevels = 1;
					full.Texture2DArray.FirstArraySlice = 0;
					full.Texture2DArray.ArraySize = 127;
					if (SUCCEEDED(a_dev->CreateShaderResourceView(tex, &full, &g_b2bDepthSRV)))
						SKSE::log::info("[SLF-B] t103 SRV: created full 127-slice view (engine view was {}), tex {}x{}",
							arraySize, td.Width, td.Height);
					else
						SKSE::log::error("[SLF-B] t103 full 127-slice SRV create FAILED");
					tex->Release();
				}
			}
		}
	}

	// s15 sampler for the payload's SampleLevel calls. The probe HLSL used a
	// plain SamplerState (default = linear, no comparison); SampleLevel is a
	// hard single tap (no PCF) so an ordinary filter is correct. CLAMP keeps
	// NDC-overflow UVs from wrapping across the map (the payload's own
	// saturate(1 - dot(p.xy,p.xy)) falloff then zeroes the out-of-cone edge).
	void EnsureB2bSampler(::ID3D11Device* a_dev)
	{
		std::lock_guard<std::mutex> lock(g_b2bMutex);
		if (g_b2bSampler)
			return;
		::D3D11_SAMPLER_DESC sd{};
		sd.Filter = ::D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = ::D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressV = ::D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressW = ::D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = FLT_MAX;
		if (FAILED(a_dev->CreateSamplerState(&sd, &g_b2bSampler)))
			SKSE::log::error("[SLF-B] s15 sampler create FAILED");
		else
			SKSE::log::info("[SLF-B] s15 sampler ready (linear+clamp)");
	}

	void EnsureZeroBindResources(::ID3D11Device* a_dev)
	{
		std::lock_guard<std::mutex> lock(g_zeroMutex);
		if (g_zeroLRSRV)
			return;
		// LightRec array, 32 elements x 96B. The payload indexes by the
		// ENGINE's light-loop counter (0..n, n can reach 7+ with SLF's extra
		// lights), so a 1-element buffer was a trap: slot 0 short-circuited
		// fine, but slots >= 1 read OUT OF BOUNDS, and drivers are NOT
		// required to return 0 for that (many return NaN garbage) -> the
		// payload's enabled-gate failed for those lights -> NaN shadow
		// factors -> black + flicker. 32 elements cover every slot the
		// engine loop may produce (and the future 32-slot B5 layout).
		// B4a (2026-09-05): the buffer is DYNAMIC now - UpdateB2bLightRec
		// refills it every frame from g_shadowLights (real per-light data).
		// Before the first fill it reads as all-zero (enabled=0 -> every
		// gate short-circuits -> vanilla behaviour), which is the exact
		// zero-data safety state the 9daf0362/32-elem fixes relied on.
		static constexpr std::uint32_t kZeroElems = 32;
		static constexpr std::uint32_t kZeroBytes = kZeroElems * 96;
		::D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = kZeroBytes;
		bd.Usage = ::D3D11_USAGE_DYNAMIC;
		bd.BindFlags = ::D3D11_BIND_SHADER_RESOURCE;
		bd.CPUAccessFlags = ::D3D11_CPU_ACCESS_WRITE;
		bd.MiscFlags = ::D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = 96;
		::ID3D11Buffer* buf = nullptr;
		if (FAILED(a_dev->CreateBuffer(&bd, nullptr, &buf)) || !buf) {
			SKSE::log::warn("[SLF-B] dynamic LightRec buffer create FAILED");
			return;
		}
		::D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = ::DXGI_FORMAT_UNKNOWN;
		srv.ViewDimension = ::D3D11_SRV_DIMENSION_BUFFEREX;
		srv.BufferEx.FirstElement = 0;
		srv.BufferEx.NumElements = kZeroElems;
		::ID3D11ShaderResourceView* view = nullptr;
		if (FAILED(a_dev->CreateShaderResourceView(buf, &srv, &view)) || !view) {
			buf->Release();
			SKSE::log::warn("[SLF-B] dynamic LightRec SRV create FAILED");
			return;
		}
		g_zeroLRBuf = buf;
		g_zeroLRSRV = view;
		SKSE::log::info("[SLF-B] dynamic LightRec t102 resource ready ({} elems x 96B, B4a)", kZeroElems);
	}

	// B4a (2026-09-05): refill the LightRec SRV with THIS frame's real light
	// data (g_shadowLights, written by the scheduler from the engine's
	// accumulator - same order the engine's material light-loop walks, so
	// t102[idx] lines up with the light the loop is processing).
	//
	// LightRec 96B layout (authoritative - b2_template_v2.py + golden asm):
	//   float4 proj[4]  @0..63   row-major world->light projection (the
	//                            4 ld_structured offsets 0/16/32/48)
	//   float  pos.xyz   @64..75  (unused by the payload; kept for layout)
	//   float4 @76: x=radius y=type z=slice w=enabled  -- ld_structured
	//                             offset 76 swizzle xywz yields
	//                             (radius, type, enabled, slice) in r19
	//                             (payload gates on r19.w = raw[84])
	//   float  bias      @92..95  (ld_structured offset 92)
	// Payload semantics (golden asm 95-140): enabled>0.5 -> shadow-sample
	// this light (type<=0.5 frustum path, else paraboloid path), depth-test
	// t103[(u,v),slice] vs proj'd depth - bias; enabled<=0.5 -> gate else ->
	// mask pinned to 1.0 (light contributes fully, no shadow).
	void UpdateB2bLightRec(::ID3D11DeviceContext* a_ctx)
	{
		{
			::ID3D11Device* dev = nullptr;
			a_ctx->GetDevice(&dev);
			if (dev) {
				if (!g_zeroLRSRV)
					EnsureZeroBindResources(dev);
				dev->Release();
			}
		}
		if (!g_zeroLRBuf)
			return;
		const std::uint32_t count = std::min<std::uint32_t>(
			ShadowLimitFixNS::P1::g_shadowLightCount.load(std::memory_order_acquire), 32u);
		::D3D11_MAPPED_SUBRESOURCE ms{};
		if (FAILED(a_ctx->Map(g_zeroLRBuf, 0, ::D3D11_MAP_WRITE_DISCARD, 0, &ms)))
			return;
		auto* dst = static_cast<std::uint8_t*>(ms.pData);
		std::memset(dst, 0, 32 * 96);
		constexpr float kBias = 0.00025f;  // CS LLF default (shadowBiasScale x 0.00025)
		float fType = -1.0f, fEn = -1.0f, fSlice = -1.0f;
		for (std::uint32_t i = 0; i < count; i++) {
			const auto& ld = ShadowLimitFixNS::P1::g_shadowLights[i];
			auto* d = reinterpret_cast<float*>(dst + i * 96);
			for (int k = 0; k < 16; k++)
				d[k] = ld.proj[k];
			d[16] = ld.pos[0];
			d[17] = ld.pos[1];
			d[18] = ld.pos[2];
			d[19] = ld.radius;
			// type: engine light class, set by the data channel
			// (PublishShadowLightDataChannel): 0 = frustum (sun), 1 = hemi
			// (cone/spot single paraboloid), 2 = omni (dual paraboloid).
			// Previously guessed from proj[11] - WRONG: a perspective
			// frustum proj also has [11]=1, so every frustum light was
			// mislabelled omni; and the paraboloid proj is now affine
			// (row3col3 = 0) which would have read as frustum.
			d[20] = ld.lightType;
			d[21] = (ld.flags > 0.5f) ? 1.0f : 0.0f;      // enabled (valid matrix)
			d[22] = ld.shadowMapIndex;                    // array slice
			d[23] = kBias;
			if (i == 0) {
				fType = d[20];
				fEn = d[21];
				fSlice = d[22];
			}
		}
		a_ctx->Unmap(g_zeroLRBuf, 0);
		static std::uint32_t lc = 0;
		if ((lc++ & 0xFFu) == 0)
			SKSE::log::info("[SLF-B][b4a] LightRec filled: {} lights (first: type={} en={} slice={})",
				count, static_cast<int>(fType), static_cast<int>(fEn), static_cast<int>(fSlice));
	}

	static std::uint64_t Fnv1a(const std::uint8_t* p, size_t n)
	{
		std::uint64_t h = 1469598103934665603ULL;
		for (size_t i = 0; i < n; i++) {
			h ^= p[i];
			h *= 1099511628211ULL;
		}
		return h;
	}

	// Try to splice the SLF shadow-unit payload into the bytecode and create
	// the patched PS. Returns true (and sets *a_ps) if the driver accepted it.
	// Semantic note: until B4 binds t102/t103 data, the patched shader's
	// enabled-gate reads an empty structured buffer (returns 0) -> shadow
	// factor pinned to 1.0 -> lights render unshadowed but STABLE. That is
	// exactly the observable "engine accepted the surgery" signal we want
	// before wiring real shadow data.
	bool TryCreatePatchedPS(::ID3D11Device* a_dev, const void* a_bytecode,
		SIZE_T a_len, const ::ID3D11ClassLinkage* a_linkage,
		::ID3D11PixelShader** a_ps)
	{
		const auto* bc = static_cast<const std::uint8_t*>(a_bytecode);
		std::vector<std::uint8_t> buf(bc, bc + a_len);
		uint32_t rej = slf_bc::REJ_NONE;
		auto patched = slf_bc::PatchContainer(buf, &rej);
		if (!patched) {
			if (rej == slf_bc::REJ_NOUNIT) {
				g_shadowNoUnit++;
				return false;
			}
			// a shadow unit WAS found but a guard rejected it (shape / temps
			// ceiling) -- log per-unique-variant to measure the runtime shape
			// spread vs the offline-validated corpus
			const std::uint64_t f = Fnv1a(bc, a_len);
			std::lock_guard<std::mutex> lock(g_patchSeenMutex);
			if (g_rejectSeen.insert(f).second) {
				const char* why = rej == slf_bc::REJ_SHAPE    ? "shape-mismatch"
					: rej == slf_bc::REJ_TEMPS                ? "temps>17"
					: rej == slf_bc::REJ_PARSE                ? "parse"
															   : "other";
				SKSE::log::info("[SLF-B] guard rejected variant {} ({}, raw {}B)",
					static_cast<const void*>(bc), why, a_len);
			}
			return false;
		}
		::ID3D11PixelShader* ps = nullptr;
		const HRESULT hr = g_origCreatePS(a_dev, patched->data(),
			patched->size(), a_linkage, &ps);
		if (FAILED(hr) || !ps) {
			g_shadowRejected++;
			SKSE::log::warn("[SLF-B] driver REJECTED patched PS (hr={:#x}); "
				"falling back to vanilla bytecode", static_cast<unsigned>(hr));
			return false;
		}
		*a_ps = ps;
		g_shadowPatched++;
		EnsureZeroBindResources(a_dev);
		{
			std::lock_guard<std::mutex> lock(g_patchObjMutex);
			g_patchedObj.insert(ps);
		}
		{
			// record patched bytecode so DumpBoundPixelShader can capture the
			// actual spliced product for offline comparison
			std::lock_guard<std::mutex> lock(g_psBcMutex);
			auto& v = g_psBytecode[ps];
			v.assign(patched->begin(), patched->end());
		}
		{
			const std::uint64_t f = Fnv1a(bc, a_len);
			std::lock_guard<std::mutex> lock(g_patchSeenMutex);
			if (g_patchSeen.insert(f).second) {
				SKSE::log::info("[SLF-B] shadow-unit patched variant {} (raw {}B -> "
					"patched {}B, total patched {})", static_cast<void*>(ps),
					a_len, patched->size(), g_shadowPatched.load());
			}
		}
		return true;
	}

	HRESULT STDMETHODCALLTYPE HookCreatePixelShader(
		::ID3D11Device* a_dev, const void* a_bytecode, SIZE_T a_len,
		const ::ID3D11ClassLinkage* a_linkage, ::ID3D11PixelShader** a_ps)
	{
#if SLF_B2B_ENABLED
		if (a_bytecode && a_len > 8 && a_len < (1 << 20)) {
			if (TryCreatePatchedPS(a_dev, a_bytecode, a_len, a_linkage, a_ps))
				return S_OK;
		}
#else
		(void)a_bytecode;
		(void)a_len;
		(void)a_linkage;
#endif
		HRESULT hr = g_origCreatePS(a_dev, a_bytecode, a_len, a_linkage, a_ps);
		if (SUCCEEDED(hr) && *a_ps && a_bytecode && a_len > 0 && a_len < (1 << 20)) {
			std::lock_guard<std::mutex> lock(g_psBcMutex);
			auto& v = g_psBytecode[*a_ps];
			v.assign(static_cast<const std::uint8_t*>(a_bytecode),
				static_cast<const std::uint8_t*>(a_bytecode) + a_len);
			g_psBcTotal++;
		}
		return hr;
	}

	void InstallBytecodeCapture()
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer)
			return;
		auto* dev = reinterpret_cast<::ID3D11Device*>(renderer->GetRuntimeData().forwarder);
		if (!dev)
			return;
		void** vtbl = *reinterpret_cast<void***>(dev);
		if (!vtbl || vtbl[15] == reinterpret_cast<void*>(&HookCreatePixelShader))
			return;
		g_origCreatePS = reinterpret_cast<CreatePixelShaderFn>(vtbl[15]);
		DWORD oldProtect = 0;
		::VirtualProtect(&vtbl[15], sizeof(void*), PAGE_READWRITE, &oldProtect);
		vtbl[15] = reinterpret_cast<void*>(&HookCreatePixelShader);
		::VirtualProtect(&vtbl[15], sizeof(void*), oldProtect, &oldProtect);
		SKSE::log::info("[SLF] bytecode capture installed (CreatePixelShader vtable[15])");
#if SLF_B2B_ENABLED
		SKSE::log::info("[SLF-B] B2B splice ENABLED");
#else
		SKSE::log::info("[SLF-B] B2B splice DISABLED (A/B isolation control) - engine PS bytecode passes through untouched");
#endif
	}

	void DumpLightingBytecode(RE::BSShader* a_shader)
	{
		// Dump the vanilla bytecode of Lighting PS to an ABSOLUTE path
		// (relative "Data/..." lands in the process CWD which is not the game
		// dir under MO2 - 64 files "dumped" but nowhere to be found at 10:56).
		// Written next to the crash log, verified writable.
		// NOTE (11:03): LoadShaders-time dump yielded 41 files with NO
		// Lighting shaders (no 0x48-prefixed technique IDs) - Lighting PS are
		// loaded lazily / not present in the early pixelShaders list. The
		// BeginTechnique hook now dumps the ACTUAL bound PS instead.
		static std::uint32_t dumped = 0;
		const std::uint32_t maxDump = 64;
		std::filesystem::path dir = std::filesystem::path("C:/Users/Administrator/Documents/My Games/Skyrim Special Edition/SKSE/shader_dump");
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);

		(void)a_shader;
		// no-op - see BeginTechniqueHook for the actual dump path.
		(void)dumped;
		(void)maxDump;
		(void)dir;
		(void)ec;
	}

	// Dump the vanilla bytecode of the PS currently bound by the engine
	// (after func() ran in BeginTechnique) - guarantees we get real Lighting
	// PS regardless of when/how the engine loaded them.
	void DumpBoundPixelShader(RE::BSShader* a_shader, ID3D11DeviceContext* a_ctx)
	{
		if (!a_shader || a_shader->shaderType.get() != RE::BSShader::Type::Lighting)
			return;
		static std::uint32_t dumped = 0;
		const std::uint32_t maxDump = 64;
		if (dumped >= maxDump)
			return;
		auto& rtd = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
		auto* curPS = rtd.currentPixelShader;
		if (!curPS)
			return;
		auto* obj = reinterpret_cast<::ID3D11PixelShader*>(curPS->shader);
		std::lock_guard<std::mutex> lock(g_psBcMutex);
		auto it = g_psBytecode.find(obj);
		if (it == g_psBytecode.end())
			return;
		// Deduplicate: skip if we already dumped this exact object.
		static std::unordered_set<::ID3D11PixelShader*> s_dumpedObjs;
		if (!s_dumpedObjs.insert(obj).second)
			return;
		std::filesystem::path dir = std::filesystem::path("C:/Users/Administrator/Documents/My Games/Skyrim Special Edition/SKSE/shader_dump");
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		char name[64]{};
		std::snprintf(name, sizeof(name), "PS%016llX.fxcb",
			reinterpret_cast<std::uint64_t>(obj));
		std::ofstream ofs(dir / name, std::ios::binary);
		if (ofs) {
			ofs.write(reinterpret_cast<const char*>(it->second.data()),
				static_cast<std::streamsize>(it->second.size()));
			dumped++;
			SKSE::log::info("[SLF] dumped bound Lighting PS #{} ({} bytes, obj {:016X})",
				dumped, it->second.size(), reinterpret_cast<std::uint64_t>(obj));
		}
		(void)a_ctx;
	}

	// SLF-B material-pass probe: (a) is the PS the engine is about to draw a
	// PATCHED one or vanilla? (b) what do the payload's resource slots see at
	// this moment - t102 (LightRec structured buffer), t103 (shadow depth
	// array) and cbuffer b13? 1/256 throttle, read-only (GetDesc, no Map).
	// Answers two open questions with one run:
	//   - are patched PS ever actually drawn (or created-but-unused)?
	//   - are t102/t103/b13 bound by the engine, and to WHAT (if the enabled
	//     gate reads garbage > 0.5 instead of the assumed 0, the payload runs
	//     its sampling path against stale/foreign data -> black/flicker)?
	void ProbeMaterialPassBindings(void* a_ctx)
	{
		static std::uint32_t s_n = 0;
		if (((s_n++) & 0xFFu) != 0)
			return;
		auto& rtd = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
		auto* curPS = rtd.currentPixelShader;
		if (!curPS)
			return;
		auto* pobj = reinterpret_cast<::ID3D11PixelShader*>(curPS->shader);
		bool isPatched = false;
		{
			std::lock_guard<std::mutex> lock(g_patchObjMutex);
			isPatched = g_patchedObj.count(pobj) != 0;
		}
		auto* ctx = reinterpret_cast<::ID3D11DeviceContext*>(a_ctx);
		::ID3D11ShaderResourceView* s102 = nullptr;
		::ID3D11ShaderResourceView* s103 = nullptr;
		::ID3D11Buffer* b13 = nullptr;
		::ID3D11SamplerState* sm15 = nullptr;
		ctx->PSGetShaderResources(102, 1, &s102);
		ctx->PSGetShaderResources(103, 1, &s103);
		ctx->PSGetConstantBuffers(13, 1, &b13);
		ctx->PSGetSamplers(15, 1, &sm15);
		char d102[80] = "UNBOUND";
		char d103[80] = "UNBOUND";
		char d13[80] = "UNBOUND";
		char d15[80] = "UNBOUND";
		if (s102) {
			::D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
			s102->GetDesc(&sd);
			{
				if (sd.ViewDimension == ::D3D11_SRV_DIMENSION_BUFFEREX)
					std::snprintf(d102, sizeof(d102), "BUFFEREX fmt=%u els=%u",
						static_cast<unsigned>(sd.Format), static_cast<unsigned>(sd.BufferEx.NumElements));
				else if (sd.ViewDimension == ::D3D11_SRV_DIMENSION_BUFFER)
					std::snprintf(d102, sizeof(d102), "BUFFER fmt=%u els=%u",
						static_cast<unsigned>(sd.Format), static_cast<unsigned>(sd.Buffer.NumElements));
				else
					std::snprintf(d102, sizeof(d102), "dim=%u fmt=%u",
						static_cast<unsigned>(sd.ViewDimension), static_cast<unsigned>(sd.Format));
			}
			s102->Release();
		}
		if (s103) {
			::D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
			s103->GetDesc(&sd);
			{
				if (sd.ViewDimension == ::D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
					std::snprintf(d103, sizeof(d103), "TEX2DARRAY fmt=%u arr=%u",
						static_cast<unsigned>(sd.Format), static_cast<unsigned>(sd.Texture2DArray.ArraySize));
				else
					std::snprintf(d103, sizeof(d103), "dim=%u fmt=%u",
						static_cast<unsigned>(sd.ViewDimension), static_cast<unsigned>(sd.Format));
			}
			s103->Release();
		}
		if (b13) {
			::D3D11_BUFFER_DESC bd{};
			b13->GetDesc(&bd);
			std::snprintf(d13, sizeof(d13), "CB size=%u", static_cast<unsigned>(bd.ByteWidth));
			b13->Release();
		}
		if (sm15) {
			::D3D11_SAMPLER_DESC sd{};
			sm15->GetDesc(&sd);
			std::snprintf(d15, sizeof(d15), "SAMPLER filter=%u addr=%u/%u/%u",
				static_cast<unsigned>(sd.Filter),
				static_cast<unsigned>(sd.AddressU),
				static_cast<unsigned>(sd.AddressV),
				static_cast<unsigned>(sd.AddressW));
			sm15->Release();
		}
		SKSE::log::info("[SLF-B][probe] PS={} patched={} t102:[{}] t103:[{}] b13:[{}] s15:[{}]",
			static_cast<void*>(pobj), isPatched ? 1 : 0, d102, d103, d13, d15);
	}

	// B4c-v3 (2026-09-05): DRAW-time cb2 readback - the decisive alignment
	// probe. ReadbackPerSurfaceLightCounts samples cb2 at BeginTechnique,
	// which can precede the engine's per-surface cb2 fill (stale content of
	// an earlier surface/draw). This one runs from EnsureZeroBindForCurrentPS
	// at the last moment before a PATCHED draw: cb2 slot 2 at this instant is
	// exactly the batch the payload consumes this draw. Verdict:
	//   cb2[15].xyz == sched[0].xyz  -> engine batch == our prefix (aligned)
	//   cb2[29].x  ~  16             -> our count survived the engine fill
	//   else                         -> the cb2 fill is independent of our
	//      lights[] output -> the PS light loop is NOT running our prefix
	//      -> the B4b mismatch (cb2 batch vs t102 order) is still open.
	void DrawTimePerSurfaceCB2(::ID3D11DeviceContext* a_ctx)
	{
		static std::uint32_t n = 0;
		if ((n++ & 0xFFFu) != 0)  // every 4096th draw (lighting-gated below)
			return;
		::ID3D11Buffer* cb = nullptr;
		a_ctx->PSGetConstantBuffers(2, 1, &cb);
		if (!cb)
			return;
		::D3D11_BUFFER_DESC bd{};
		cb->GetDesc(&bd);
		::ID3D11Device* dev = nullptr;
		a_ctx->GetDevice(&dev);
		if (!dev || bd.ByteWidth < 480) {  // need >= cb2[30]
			if (dev)
				dev->Release();
			cb->Release();
			return;
		}
		::D3D11_BUFFER_DESC sd = bd;
		sd.Usage = ::D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = 0;
		::ID3D11Buffer* staging = nullptr;
		if (FAILED(dev->CreateBuffer(&sd, nullptr, &staging))) {
			dev->Release();
			cb->Release();
			return;
		}
		a_ctx->CopyResource(staging, cb);
		::D3D11_MAPPED_SUBRESOURCE ms{};
		if (FAILED(a_ctx->Map(staging, 0, ::D3D11_MAP_READ, 0, &ms))) {
			staging->Release();
			dev->Release();
			cb->Release();
			return;
		}
		const float* f = static_cast<const float*>(ms.pData);
		const float* c29 = f + 29 * 4;  // cb2[29] = (lights, shadow) counts
		const float* c2 = f + 2 * 4;    // cb2[2] = per-light t14 channel map
		const float* l0 = f + 15 * 4;   // batch light 0 pos.xyz + range.w
		const float* l1 = f + 16 * 4;
		const float* l2 = f + 17 * 4;
		// Lighting-cb2 validity gate (2026-09-06): this probe now runs from
		// the generic Draw hooks (was: B4d thunk, dead while B4d disabled),
		// which fire for EVERY pass incl. shadow-map/depth/UI draws whose
		// cb2 slot 2 is not the lighting batch -> stale or garbage counts.
		// A real lighting batch has cb2[29]=(lights 1..7, shadow 0..4) and a
		// nonzero range on light 0. Anything else is not a material pass the
		// PS light loop consumes -> skip (no log, no match work).
		if (c29[0] < 1.f || c29[0] > 8.f || c29[1] < 0.f || c29[1] > 4.f ||
			l0[3] <= 0.5f || l0[3] > 5000.f) {
			a_ctx->Unmap(staging, 0);
			staging->Release();
			dev->Release();
			cb->Release();
			return;
		}
		const auto nSch = ShadowLimitFixNS::P1::g_scheduledShadowCount.load(std::memory_order_acquire);
		const float* g0 = ShadowLimitFixNS::P1::g_shadowLights[0].pos;
		const float* g1 = ShadowLimitFixNS::P1::g_shadowLights[1].pos;
		const float* g2 = ShadowLimitFixNS::P1::g_shadowLights[2].pos;
		SKSE::log::info("[SLF][B4c][draw] nSch={} cb2[29]=({:.0f},{:.0f}) chmap=({:.0f},{:.0f},{:.0f},{:.0f}) "
			"cb2[0..2]=({:.1f},{:.1f},{:.1f})r{:.1f}|({:.1f},{:.1f},{:.1f})r{:.1f}|({:.1f},{:.1f},{:.1f})r{:.1f} "
			"sched[0..2]=({:.1f},{:.1f},{:.1f})|({:.1f},{:.1f},{:.1f})|({:.1f},{:.1f},{:.1f})",
			nSch, c29[0], c29[1], c2[0], c2[1], c2[2], c2[3],
			l0[0], l0[1], l0[2], l0[3], l1[0], l1[1], l1[2], l1[3], l2[0], l2[1], l2[2], l2[3],
			g0[0], g0[1], g0[2], g1[0], g1[1], g1[2], g2[0], g2[1], g2[2]);
#if SLF_B5_MATCH_PROBE
		// B5 pre-flight v3 (2026-09-06): DUAL-MODE position-match probe.
		// History: v1 (09-05) matched cb2[15+i] against g_shadowLights[].pos
		// directly - 48191/48191 matched=0 at 162-418u. v2 assumed cb2 was
		// VIEW space (from a misread of the z range) and transformed the
		// schedule set to view - still 0/18 matched with every slot
		// collapsing onto one schedule record (constant-offset fingerprint
		// of a wrong-space transform). v3 runs BOTH hypotheses on the same
		// sample (see the match block below) so the coordinate space is
		// settled by data, not by assumption.
		{
			const auto lrCount = std::min<std::uint32_t>(nSch, 32u);
			const int nShadow = (c29[1] < 8.f) ? static_cast<int>(c29[1]) : 4;
			const int nAll = static_cast<int>(c29[0] < 8.f ? c29[0] : 7.f);

			// world-camera view matrix (row-major 16 floats). Same source
			// the engine fills cb2 from; the lighting pass runs on the
			// render thread reading the same matrix, so this is race-safe
			// at the moment of the draw.
			float vm[16]{};
			bool haveCam = false;
			{
				static REL::RelocationID uid(528087, 415032);
				auto* sg = *reinterpret_cast<RE::BSSceneGraph**>(uid.address());
				RE::NiCamera* cam = sg ? sg->GetRuntimeData().camera.get() : nullptr;
				if (cam) {
					const auto& w2c = cam->GetRuntimeData().worldToCam;
					for (int r = 0; r < 16; r++)
						vm[r] = (&w2c[0][0])[r];
					haveCam = true;
				}
			}

			// v3 (2026-09-06): DUAL-MODE match - one cb2 sample, two space
			// hypotheses, verdict by data:
			//   mode W: cb2 slot pos treated as WORLD  vs g_shadowLights[].pos
			//   mode V: cb2 slot pos treated as VIEW   vs g_shadowLights[].pos
			//           transformed by worldToCam
			// Only shadow slots (i < cb2[29].y) count - diffuse-only trailing
			// slots are not expected in the schedule set. Motivation: v2's
			// view-only transform still matched 0/18 rows with EVERY slot
			// collapsing onto one schedule record (lr#11, 400-800u) = the
			// constant-offset fingerprint of a wrong-space transform; the
			// engine disasm `add r9, -v2, cb2[r6.w+15]` (light - pixel =
			// direction; v2 likely interpolated WORLD pixel pos) and the
			// ReadbackPerSurfaceLightCounts print (world-magnitude coords,
			// e.g. (-1285,-539,350) r=1181) instead suggest cb2[15+i] is
			// WORLD space. mW>0 -> world; mV>0 -> view; both 0 -> the cb2
			// light set genuinely does not overlap the schedule set.
			int mW = 0, mV = 0;
			float avgW = 0.f, avgV = 0.f;
			int bKW = -1, bKV = -1;
			float dW0 = -1.f, dV0 = -1.f;  // slot-0 best distance per mode
			const int nShdI = std::min(nAll, nShadow);
			for (int i = 0; i < nShdI; i++) {
				const float* p = f + (15 + i) * 4;  // cb2[15+i].xyz
				float bestW = 1e30f, bestV = 1e30f;
				int kkW = -1, kkV = -1;
				for (std::uint32_t k = 0; k < lrCount; k++) {
					const float* lp = ShadowLimitFixNS::P1::g_shadowLights[k].pos;
					const float dx = p[0] - lp[0], dy = p[1] - lp[1], dz = p[2] - lp[2];
					const float dw = dx * dx + dy * dy + dz * dz;
					if (dw < bestW) {
						bestW = dw;
						kkW = static_cast<int>(k);
					}
					if (haveCam) {
						// world -> view (row-vector convention, v' = v*M)
						const float lx = lp[0] * vm[0] + lp[1] * vm[4] + lp[2] * vm[8] + vm[12];
						const float ly = lp[0] * vm[1] + lp[1] * vm[5] + lp[2] * vm[9] + vm[13];
						const float lz = lp[0] * vm[2] + lp[1] * vm[6] + lp[2] * vm[10] + vm[14];
						const float vdx = p[0] - lx, vdy = p[1] - ly, vdz = p[2] - lz;
						const float dv = vdx * vdx + vdy * vdy + vdz * vdz;
						if (dv < bestV) {
							bestV = dv;
							kkV = static_cast<int>(k);
						}
					}
				}
				const float bdW = std::sqrt(bestW);
				const float bdV = haveCam ? std::sqrt(bestV) : -1.f;
				avgW += bdW;
				if (bdV >= 0.f)
					avgV += bdV;
				if (bdW < 8.f)
					mW++;
				if (bdV >= 0.f && bdV < 8.f)
					mV++;
				if (i == 0) {
					dW0 = bdW;
					dV0 = bdV;
					bKW = kkW;
					bKV = kkV;
				}
			}
			if (nShdI > 0) {
				avgW /= nShdI;
				avgV /= nShdI;
			}
			const float* c0 = f + 15 * 4;
			SKSE::log::info("[SLF][B5][dual] cam={} nShd={}/{} c0=({:.1f},{:.1f},{:.1f})r{:.1f} "
				"W:m{} av{:.1f}u s0:lr#{}d{:.1f} | V:m{} av{:.1f}u s0:lr#{}d{:.1f} lr={}",
				haveCam ? 1 : 0, nShadow, nAll,
				c0[0], c0[1], c0[2], c0[3],
				mW, avgW, bKW, dW0, mV, avgV, bKV, dV0, lrCount);
		}
#endif  // SLF_B5_MATCH_PROBE
		a_ctx->Unmap(staging, 0);
		staging->Release();
		dev->Release();
		cb->Release();
	}

	// SLF-B: called from the engine Draw hooks (P1_hooks.cpp) at the very
	// last moment before every draw. If the bound pixel shader is one of our
	// patched variants, bind the SLF-B resource set so the payload's slots
	// are all defined:
	//   t102  zero LightRec (enabled=0 -> gate short-circuits -> factor 1.0)
	//   t103  kSHADOWMAPS full depth-array SRV (inert while gated)
	//   s15   payload sampler (inert while gated)
	// Binding at Draw time cannot be overwritten by the engine's own
	// resource setup, which is why the earlier bind inside
	// ProbeMaterialPassBindings failed (probe kept showing t102 UNBOUND on
	// every patched draw -> engine reset the slot between passes). This is
	// also the seam where B4 will swap t102 for the real per-light buffer.
	void EnsureZeroBindForCurrentPS(::ID3D11DeviceContext* a_ctx)
	{
		::ID3D11PixelShader* ps = nullptr;
		// Note: this Windows SDK's d3d11.h declares the 3-arg overload
		// (ppPixelShader, ppClassInstances, pNumClassInstances).
		a_ctx->PSGetShader(&ps, nullptr, nullptr);
		if (!ps)
			return;
		bool isPatched = false;
		{
			std::lock_guard<std::mutex> lock(g_patchObjMutex);
			isPatched = g_patchedObj.count(ps) != 0;
		}
		ps->Release();
		if (!isPatched)
			return;
		// Ensure the SRV/sampler exist before binding them (lazy create on
		// first patched draw; device calls are thread-safe).
		{
			::ID3D11Device* dev = nullptr;
			a_ctx->GetDevice(&dev);
			if (dev) {
				if (!g_b2bDepthSRV)
					EnsureB2bDepthSRV(dev);
				if (!g_b2bSampler)
					EnsureB2bSampler(dev);
				dev->Release();
			}
		}
		::ID3D11ShaderResourceView* zero = nullptr;
		{
			std::lock_guard<std::mutex> lock(g_zeroMutex);
			zero = g_zeroLRSRV;
			if (zero)
				zero->AddRef();
		}
		if (!zero)
			return;  // EnsureZeroBindResources not created yet (shouldn't happen)
		a_ctx->PSSetShaderResources(102, 1, &zero);
		zero->Release();
		if (g_b2bDepthSRV) {
			a_ctx->PSSetShaderResources(103, 1, &g_b2bDepthSRV);
			if (g_b2bSampler)
				a_ctx->PSSetSamplers(15, 1, &g_b2bSampler);
		}
		static std::uint32_t zc = 0;
		if ((zc++ & 0xFFu) == 0)
			SKSE::log::info("[SLF-B][zerobind] PS={} t102 zero + t103 SRV + s15 sampler bound @draw",
				static_cast<void*>(ps));
		// B4c-v3: cb2 content at the pre-draw seam = the exact batch this
		// draw's payload consumes. Decides the alignment question.
		DrawTimePerSurfaceCB2(a_ctx);
	}

	// Read the engine's per-surface light counts (cb2[29].x = total lights
	// clamped to 7, cb2[29].y = shadow lights clamped to 4 - the vanilla
	// limit, seen in the disassembly: `min r3.xy, cb2[29].yxyy, l(4,7)`).
	// Confirms whether the CPU-side fill is what caps us at 4.
	//
	// B4b (2026-09-05): the old body Map'd the engine's DEFAULT-usage cbuffer
	// directly -> Map(D3D11_MAP_READ) always fails -> silent return -> cb2
	// was NEVER observed (0 hits across 150k+ lighting passes). Fixed with a
	// staging copy. This cb2 is the per-surface light list the engine's
	// lighting loop walks (pos @ cb2[i+15].xyz, range @ .w, color @
	// cb2[i+22], count @ cb2[29].x/y). Whether its batch (loop runs
	// min(count,7) times) lines up with our LightRec order (accumulator
	// order) is THE open question behind residual shadow flicker - this
	// readback is the first direct observation of that structure.
	void ReadbackPerSurfaceLightCounts(ID3D11DeviceContext* a_ctx)
	{
		static std::uint32_t n = 0;
		if ((n++ & 0x3Fu) != 0)  // every 64th pass (short test runs must see it)
			return;
		::ID3D11Buffer* cb = nullptr;
		a_ctx->PSGetConstantBuffers(2, 1, &cb);
		if (!cb) {
			SKSE::log::info("[SLF] cb2 not bound at BeginTechnique (may bind later per draw)");
			return;
		}
		::D3D11_BUFFER_DESC bd{};
		cb->GetDesc(&bd);
		::ID3D11Device* dev = nullptr;
		a_ctx->GetDevice(&dev);
		if (!dev || bd.ByteWidth < 480) {  // cb2[30] float4 regs = 480B minimum
			if (dev)
				dev->Release();
			cb->Release();
			return;
		}
		::D3D11_BUFFER_DESC sd = bd;
		sd.Usage = ::D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = 0;
		::ID3D11Buffer* staging = nullptr;
		if (FAILED(dev->CreateBuffer(&sd, nullptr, &staging))) {
			dev->Release();
			cb->Release();
			return;
		}
		a_ctx->CopyResource(staging, cb);
		::D3D11_MAPPED_SUBRESOURCE ms{};
		if (FAILED(a_ctx->Map(staging, 0, ::D3D11_MAP_READ, 0, &ms))) {
			staging->Release();
			dev->Release();
			cb->Release();
			return;
		}
		const float* f = static_cast<const float*>(ms.pData);
		// cb2[29] -> float offset 29*4 float4 regs -> byte 29*16 = 464
		const float* c29 = f + 29 * 4;
		const float* c2 = f + 2 * 4;    // cb2[2].xyzw = per-light t14 channel map
		const float* l0 = f + 15 * 4;   // light 0 pos.xyz + range.w
		const float* l1 = f + 16 * 4;
		const float* l2 = f + 17 * 4;
		// Our LightRec first entries (accumulator order) for comparison.
		const float* g0 = ShadowLimitFixNS::P1::g_shadowLights[0].pos;
		const float* g1 = ShadowLimitFixNS::P1::g_shadowLights[1].pos;
		const float* g2 = ShadowLimitFixNS::P1::g_shadowLights[2].pos;
		const float* gs0 = ShadowLimitFixNS::P1::g_shadowLights[0].proj;
		SKSE::log::info("[SLF] cb2 per-surface: lights={:.0f} shadow={:.0f} chmap=({:.0f},{:.0f},{:.0f},{:.0f})",
			c29[0], c29[1], c2[0], c2[1], c2[2], c2[3]);
		SKSE::log::info("[SLF] cb2 light[0..2] pos=({:.1f},{:.1f},{:.1f}) r={:.1f} | ({:.1f},{:.1f},{:.1f}) r={:.1f} | ({:.1f},{:.1f},{:.1f}) r={:.1f}",
			l0[0], l0[1], l0[2], l0[3], l1[0], l1[1], l1[2], l1[3], l2[0], l2[1], l2[2], l2[3]);
		SKSE::log::info("[SLF] LightRec[0..2] pos=({:.1f},{:.1f},{:.1f}) | ({:.1f},{:.1f},{:.1f}) | ({:.1f},{:.1f},{:.1f}) sl0={:.0f} proj0=({:.3f},{:.3f},{:.3f},{:.3f})",
			g0[0], g0[1], g0[2], g1[0], g1[1], g1[2], g2[0], g2[1], g2[2],
			ShadowLimitFixNS::P1::g_shadowLights[0].shadowMapIndex,
			gs0[0], gs0[1], gs0[2], gs0[3]);
		a_ctx->Unmap(staging, 0);
		staging->Release();
		dev->Release();
		cb->Release();
	}

	namespace
	{
		const char* ShaderTypeName(RE::BSShader* a_shader)
		{
			if (!a_shader)
				return "null";
			switch (a_shader->shaderType.get()) {
				case RE::BSShader::Type::Grass:
					return "Grass";
				case RE::BSShader::Type::Sky:
					return "Sky";
				case RE::BSShader::Type::Water:
					return "Water";
				case RE::BSShader::Type::Lighting:
					return "Lighting";
				case RE::BSShader::Type::Effect:
					return "Effect";
				case RE::BSShader::Type::DistantTree:
					return "DistantTree";
				case RE::BSShader::Type::Particle:
					return "Particle";
				default:
					return "?";
			}
		}

		// Multi-base include handler: Lighting.hlsl includes Common/*.hlsli
		// (open-shaders package/Shaders) - second base not needed for now.
		class SimpleIncludeHandler : public ID3DInclude
		{
		public:
			explicit SimpleIncludeHandler(std::vector<std::filesystem::path> a_bases) :
				baseDirs(std::move(a_bases)) {}

			HRESULT __stdcall Open(D3D_INCLUDE_TYPE /*a_type*/, LPCSTR a_fileName, LPCVOID /*a_parent*/,
				LPCVOID* a_data, UINT* a_bytes) override
			{
				try {
					for (const auto& base : baseDirs) {
						const auto p = base / a_fileName;
						if (!std::filesystem::exists(p))
							continue;
						std::ifstream f(p, std::ios::binary);
						if (!f)
							continue;
						std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
						auto* buf = new char[content.size()];
						std::memcpy(buf, content.data(), content.size());
						*a_data = buf;
						*a_bytes = static_cast<UINT>(content.size());
						return S_OK;
					}
				} catch (...) {}
				return E_FAIL;
			}

			HRESULT __stdcall Close(LPCVOID a_data) override
			{
				delete[] static_cast<const char*>(a_data);
				return S_OK;
			}

		private:
			std::vector<std::filesystem::path> baseDirs;
		};

		ID3D11Device* GetD3DDevice()
		{
			static REL::Relocation<ID3D11Device**> g_device{ RELOCATION_ID(524729, 411348) };
			return (g_device.address() && *g_device) ? *g_device : nullptr;
		}

		// Compile Lighting.hlsl PS with engine defines + extra macros.
		ID3D11PixelShader* CompileLightingPS(uint32_t a_descriptor)
		{
			auto* device = GetD3DDevice();
			if (!device)
				return nullptr;

			static REL::Relocation<void(uint32_t, D3D_SHADER_MACRO*)> VanillaGetLightingShaderDefines(
				RELOCATION_ID(101631, 108698));
			if (!VanillaGetLightingShaderDefines.address()) {
				SKSE::log::error("[SLF] VanillaGetLightingShaderDefines not in Address Library");
				return nullptr;
			}

			D3D_SHADER_MACRO macros[128]{};
			VanillaGetLightingShaderDefines(a_descriptor, macros);
			int last = 0;
			while (last < 127 && macros[last].Name)
				last++;
			macros[last++] = { "PSHADER", nullptr };
			macros[last] = { nullptr, nullptr };

			SimpleIncludeHandler incHandler({ L"D:/Modding/open-shaders/package/Shaders" });
			ID3DBlob* blob = nullptr;
			ID3DBlob* errBlob = nullptr;
			const HRESULT hr = D3DCompileFromFile(L"D:/Modding/open-shaders/package/Shaders/Lighting.hlsl", macros, &incHandler,
				"main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errBlob);
			if (FAILED(hr) || !blob) {
				std::string errText;
				if (errBlob && errBlob->GetBufferPointer() && errBlob->GetBufferSize() > 0) {
					errText.assign(static_cast<const char*>(errBlob->GetBufferPointer()), errBlob->GetBufferSize());
					while (!errText.empty() && (errText.back() == '\0' || errText.back() == '\n' || errText.back() == '\r'))
						errText.pop_back();
				}
				SKSE::log::error("[SLF] Lighting.hlsl PS compile FAILED tech={:08X} hr={:08X}\n{}",
					a_descriptor, static_cast<unsigned>(hr), errText);
				if (errBlob)
					errBlob->Release();
				return nullptr;
			}
			ID3D11PixelShader* ps = nullptr;
			const HRESULT hr2 = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps);
			blob->Release();
			if (errBlob)
				errBlob->Release();
			return FAILED(hr2) ? nullptr : ps;
		}

		// Compile Lighting.hlsl VS (VSHADER macro, vs_5_0). CS VS+PS are a
		// matched pair (PS_INPUT = VS_OUTPUT); replacing only the PS while
		// keeping the engine VS mismatches the input layout -> broken/black
		// terrain or crash (DynamicWetness v24 root cause).
		ID3D11VertexShader* CompileLightingVS(uint32_t a_descriptor)
		{
			auto* device = GetD3DDevice();
			if (!device)
				return nullptr;

			static REL::Relocation<void(uint32_t, D3D_SHADER_MACRO*)> VanillaGetLightingShaderDefines(
				RELOCATION_ID(101631, 108698));
			if (!VanillaGetLightingShaderDefines.address())
				return nullptr;

			D3D_SHADER_MACRO macros[128]{};
			VanillaGetLightingShaderDefines(a_descriptor, macros);
			int last = 0;
			while (last < 127 && macros[last].Name)
				last++;
			macros[last++] = { "VSHADER", nullptr };
			macros[last] = { nullptr, nullptr };

			SimpleIncludeHandler incHandler({ L"D:/Modding/open-shaders/package/Shaders" });
			ID3DBlob* blob = nullptr;
			ID3DBlob* errBlob = nullptr;
			const HRESULT hr = D3DCompileFromFile(L"D:/Modding/open-shaders/package/Shaders/Lighting.hlsl", macros, &incHandler,
				"main", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errBlob);
			if (FAILED(hr) || !blob) {
				SKSE::log::error("[SLF] Lighting.hlsl VS compile FAILED tech={:08X} hr={:08X}",
					a_descriptor, static_cast<unsigned>(hr));
				if (errBlob)
					errBlob->Release();
				return nullptr;
			}
			ID3D11VertexShader* vs = nullptr;
			const HRESULT hr2 = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs);
			blob->Release();
			if (errBlob)
				errBlob->Release();
			return FAILED(hr2) ? nullptr : vs;
		}

		// Compiled PS cache (per pixel descriptor) for BeginTechnique.
		std::map<std::uint32_t, ID3D11PixelShader*> g_psCache;
		// Compiled VS cache (per vertex descriptor).
		std::map<std::uint32_t, ID3D11VertexShader*> g_vsCache;

		bool MacroHas(const D3D_SHADER_MACRO* a_macros, const char* a_name)
		{
			for (int i = 0; i < 127 && a_macros[i].Name; i++) {
				if (std::strcmp(a_macros[i].Name, a_name) == 0)
					return true;
			}
			return false;
		}

		// Engine depth-target index global (type at addr; 4 = shadow maps).
		static int32_t GetDepthTargetTypeNow()
		{
			static REL::RelocationID uid(524780, 388826);
			return *reinterpret_cast<int32_t*>(uid.address());
		}

		// Visible shadow count - THE definitive check: read back the engine's
		// shadow depth array (kSHADOWMAPS) and count how many slices actually
		// contain geometry. slice N has content = the N-th shadow map was
		// rendered = the N-th light casts a visible shadow in our SLF PS.
		// CHEAP version (fixed the 0.4s/268MB stall): copy only a 256x256
		// center block per slice (~1MB total) and throttle to every 8192nd
		// pass (~6s) - imperceptible.
		void CheckShadowArrayContent(::ID3D11DeviceContext* a_ctx, ::ID3D11Device* a_dev)
		{
			static std::uint32_t s_frame = 0;
			if ((s_frame++ & 0x1FFFu) != 0)
				return;
			if (!a_ctx || !a_dev)
				return;
			if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
				auto& dsd = renderer->GetDepthStencilData();
				auto* srv = dsd.depthStencils[4].depthSRV;
				if (!srv)
					return;
				::ID3D11Resource* res = nullptr;
				reinterpret_cast<::ID3D11ShaderResourceView*>(srv)->GetResource(&res);
				if (!res)
					return;
				::ID3D11Texture2D* tex = nullptr;
				res->QueryInterface(__uuidof(::ID3D11Texture2D), reinterpret_cast<void**>(&tex));
				if (!tex) {
					res->Release();
					return;
				}
				::D3D11_TEXTURE2D_DESC tdesc{};
				tex->GetDesc(&tdesc);
				SKSE::log::info("[SLF] kSHADOWMAPS SRV-resource: format={} {}x{} slices={}", 
					static_cast<int>(tdesc.Format), tdesc.Width, tdesc.Height, tdesc.ArraySize);
				// Compare the DSV (what the engine actually renders into) with
				// the SRV (what our t103 binds). If they are DIFFERENT
				// textures, t103 shows cleared content -> 0% shadows.
				if (auto* dsv0 = dsd.depthStencils[4].views[0]) {
					::ID3D11Resource* dres = nullptr;
					reinterpret_cast<::ID3D11DepthStencilView*>(dsv0)->GetResource(&dres);
					if (dres) {
						::D3D11_TEXTURE2D_DESC dtd{};
						reinterpret_cast<::ID3D11Texture2D*>(dres)->GetDesc(&dtd);
						SKSE::log::info("[SLF] kSHADOWMAPS DSV0-resource: format={} {}x{} slices={} (vs SRV tex={})",
							static_cast<int>(dtd.Format), dtd.Width, dtd.Height, dtd.ArraySize,
							reinterpret_cast<void*>(tex) == reinterpret_cast<void*>(dres) ? "SAME" : "DIFFERENT");
						dres->Release();
					}
				}
				static ::ID3D11Texture2D* s_staging = nullptr;
				if (!s_staging) {
					::D3D11_TEXTURE2D_DESC sd = tdesc;
					sd.Width = 256;
					sd.Height = 256;
					sd.Usage = ::D3D11_USAGE_STAGING;
					sd.BindFlags = 0;
					sd.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
					sd.MiscFlags = 0;
					if (FAILED(a_dev->CreateTexture2D(&sd, nullptr, &s_staging))) {
						tex->Release();
						res->Release();
						return;
					}
				}
				// Copy a 256x256 center block of each slice (not the whole
				// 4096x4096 map - that was a 268MB blocking readback).
				constexpr UINT kBlock = 256;
				const UINT ox = tdesc.Width > kBlock ? (tdesc.Width - kBlock) / 2 : 0;
				const UINT oy = tdesc.Height > kBlock ? (tdesc.Height - kBlock) / 2 : 0;
				::D3D11_BOX box{};
				box.left = ox;
				box.top = oy;
				box.right = ox + kBlock;
				box.bottom = oy + kBlock;
				box.front = 0;
				box.back = 1;
				const std::uint32_t nSlices = std::min<std::uint32_t>(tdesc.ArraySize, 8u);
				std::string line;
				for (std::uint32_t slice = 0; slice < nSlices; slice++) {
					a_ctx->CopySubresourceRegion(s_staging, slice, 0, 0, 0, tex, slice, &box);
					::D3D11_MAPPED_SUBRESOURCE ms{};
					if (FAILED(a_ctx->Map(s_staging, slice, ::D3D11_MAP_READ, 0, &ms)))
						continue;
					const std::uint16_t* p = static_cast<const std::uint16_t*>(ms.pData);
					std::uint32_t content = 0, samples = 0;
					for (std::uint32_t y = 0; y < kBlock; y += 8) {
						const std::uint16_t* row = p + static_cast<std::size_t>(y) * ms.RowPitch / 2;
						for (std::uint32_t x = 0; x < kBlock; x += 8) {
							const float df = static_cast<float>(row[x]) / 65535.0f;
							// D16: content = depth not at either extreme (cleared value).
							if (df > 0.001f && df < 0.999f)
								content++;
							samples++;
						}
					}
					a_ctx->Unmap(s_staging, slice);
					char buf[32];
					std::snprintf(buf, sizeof(buf), "%s%u=%.0f%%", slice == 0 ? "" : " ", slice,
						samples > 0 ? content * 100.0f / static_cast<float>(samples) : 0.0f);
					line += buf;
				}
				SKSE::log::info("[SLF] shadow array content (center {}x{}): {}", kBlock, kBlock, line);
				tex->Release();
				res->Release();
			}
		}

		// output merger IS one of the engine's shadow-map depth stencils
		// (kSHADOWMAPS = depthStencils[4].views/readOnlyViews[0..7]).
		// More robust than GetDepthTargetTypeNow() which reads a global that
		// is not yet updated at BeginTechnique time (this is why the shadow
		// pass got our normal-render shaders and crashed at
		// SkyrimSE.exe+14DFC01, crash-2026-09-02-02-11-37).
		static bool IsShadowPass(::ID3D11DeviceContext* a_ctx)
		{
			// d3d11.h has no OMGetDepthStencil; OMGetRenderTargets returns the
			// bound DSV as its last param. NumViews=0 + null RTV array fetches
			// just the depth-stencil view.
			::ID3D11DepthStencilView* dsv = nullptr;
			a_ctx->OMGetRenderTargets(0, nullptr, &dsv);
			if (!dsv)
				return false;

			bool isShadow = false;
			if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
				auto& dsd = renderer->GetDepthStencilData();
				// Engine slots 0..7 (kSHADOWMAPS views, synced from our arrays).
				for (int i = 0; i < 8; i++) {
					if (dsd.depthStencils[4].views[i] == dsv ||
						dsd.depthStencils[4].readOnlyViews[i] == dsv) {
						isShadow = true;
						break;
					}
				}
				// Extended slots 8..127 (our arrays; the 127-slice patch lets
				// the engine render slices beyond 7 and SelectDepthBuffer
				// redirects those DSVs here). MISSING THIS = shadow pass
				// misdetected as normal render -> swap -> crash at
				// SkyrimSE.exe+14DFC01 (crash-2026-09-02-02-21-54/02-27-48).
				if (!isShadow) {
					for (int i = 8; i < 128; i++) {
						if (ShadowLimitFixNS::P1::g_normalDepthBuffer[i] == dsv ||
							ShadowLimitFixNS::P1::g_readOnlyDepthBuffer[i] == dsv) {
							isShadow = true;
							break;
						}
					}
				}
			}
			dsv->Release();
			return isShadow;
		}

		// True when the current pass has the shadow-mask texture (t14) bound -
		// i.e. this material actually samples shadows (DefShadow technique).
		// We swap in our SLF PS only for these passes; all other Lighting
		// materials keep the vanilla PS so their features stay intact.
		static bool IsShadowMaskBound(::ID3D11DeviceContext* a_ctx)
		{
			::ID3D11ShaderResourceView* srv = nullptr;
			a_ctx->PSGetShaderResources(14, 1, &srv);
			if (srv)
				srv->Release();
			return srv != nullptr;
		}

		void ReplaceLightingShaders(RE::BSShader* a_shader)
		{
			static REL::Relocation<void(uint32_t, D3D_SHADER_MACRO*)> VanillaGetLightingShaderDefines(
				RELOCATION_ID(101631, 108698));
			if (!VanillaGetLightingShaderDefines.address())
				return;

			// STRATEGY (crash-2026-09-02-02-36-58): CS Lighting.hlsl terrain
			// (LANDSCAPE) replacement crashes the engine's shadow-heightmap
			// pass (000ShadowHL at SkyrimSE.exe+14DFC01, 5 reproductions).
			// Same root cause as DynamicWetness ("terrain never worked").
			// Outdoor terrain only has the sun - no multi-light shadows needed.
			// => SKIP LANDSCAPE entirely; replace NON-terrain object materials
			// only (indoor multi-light scenes - the actual use case).
			// Cap the compile count so load time stays sane.
			static constexpr int kMaxCompile = 256;

			int vsReplaced = 0, vsFailed = 0;
			for (auto* vsh : a_shader->vertexShaders) {
				if (!vsh || vsReplaced >= kMaxCompile)
					continue;
				D3D_SHADER_MACRO macros[128]{};
				VanillaGetLightingShaderDefines(vsh->id, macros);
				if (MacroHas(macros, "LANDSCAPE"))
					continue;
				auto* newVS = CompileLightingVS(vsh->id);
				if (!newVS) {
					vsFailed++;
					continue;
				}
				g_vsCache[vsh->id] = newVS;
				vsReplaced++;
			}

			int replaced = 0, failed = 0;
			for (auto* ps : a_shader->pixelShaders) {
				if (!ps || replaced >= kMaxCompile)
					continue;
				D3D_SHADER_MACRO macros[128]{};
				VanillaGetLightingShaderDefines(ps->id, macros);
				if (MacroHas(macros, "LANDSCAPE"))
					continue;

				auto* newPS = CompileLightingPS(ps->id);
				if (!newPS) {
					failed++;
					continue;
				}
				g_psCache[ps->id] = newPS;
				replaced++;
			}
			SKSE::log::info("[SLF] OBJECT VS cached: {} failed: {} | PS cached: {} failed: {} (LANDSCAPE skipped)",
				vsReplaced, vsFailed, replaced, failed);
		}

		// ---- BSShader::LoadShaders hook ----
		struct LoadShadersHook
		{
			static void thunk(RE::BSShader* a_shader, std::uintptr_t a_stream)
			{
				// Lazy-install bytecode capture here too: LoadShaders runs
				// BEFORE the first BeginTechnique, and lighting PS are loaded
				// in this same call - capturing from here records them.
				// (10:51 run: capture installed at first BeginTechnique, but
				// Lighting shaders had already loaded -> dump came up empty.)
				static std::once_flag s_once;
				std::call_once(s_once, [] { InstallBytecodeCapture(); });

				func(a_shader, a_stream);

				if (!a_shader)
					return;
				// Vanilla PS bytecode dump (from-vanilla-semantics study).
				DumpLightingBytecode(a_shader);
#if P1C3_ENABLED
				const auto typeName = ShaderTypeName(a_shader);
				if (std::strcmp(typeName, "Lighting") == 0)
					ReplaceLightingShaders(a_shader);
#endif
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		// ---------------------------------------------------------------------
		// CS shader cbuffer data (b4 Permutation + b12 FrameBuffer).
		// CS Lighting.hlsl reads these; the engine does NOT fill them, so we
		// bind our own. b5/b6 (SharedData/FeatureData) read as null = 0, which
		// matches the "all zero -> branches disabled" state that is safe.
		// Ported from DynamicWetness v39 (verified in-game 09-01).
		// ---------------------------------------------------------------------
		struct alignas(16) FrameBufferCB
		{
			float CameraView[16];
			float CameraProj[16];
			float CameraViewProj[16];
			float CameraViewProjUnjittered[16];
			float CameraPreviousViewProjUnjittered[16];
			float CameraProjUnjittered[16];
			float CameraProjUnjitteredInverse[16];
			float CameraViewInverse[16];
			float CameraViewProjInverse[16];
			float CameraProjInverse[16];
			float CameraPosAdjust[4];
			float CameraPreviousPosAdjust[4];
			float FrameParams[4];
			float DynamicResolutionParams1[4];
			float DynamicResolutionParams2[4];
		};
		static_assert(sizeof(FrameBufferCB) == 720);

		ID3D11Buffer* g_permutationBuf = nullptr;
		ID3D11Buffer* g_frameBuf = nullptr;
		ID3D11Buffer* g_frameStaging = nullptr;

		void EnsurePermutationBuffer(ID3D11Device* a_device)
		{
			if (g_permutationBuf || !a_device)
				return;
			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = 32;  // 8 uint32
			bd.Usage = D3D11_USAGE_DYNAMIC;
			bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			std::uint32_t permInit[8] = { 0, 0, 1, 0, 0, 0, 0, 0 };  // InWorld=1
			D3D11_SUBRESOURCE_DATA psd{ permInit, 0, 0 };
			a_device->CreateBuffer(&bd, &psd, &g_permutationBuf);
		}

		void EnsureFrameBuffer(ID3D11Device* a_device)
		{
			if (g_frameBuf || !a_device)
				return;
			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = sizeof(FrameBufferCB);
			bd.Usage = D3D11_USAGE_DYNAMIC;
			bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			D3D11_SUBRESOURCE_DATA fsd{ nullptr, 0, 0 };
			a_device->CreateBuffer(&bd, &fsd, &g_frameBuf);
		}

		// Write camera matrices into b12 each frame (from engine ViewData).
		void WriteFrameBuffer(ID3D11DeviceContext* a_ctx)
		{
			if (!g_frameBuf)
				return;
			FrameBufferCB fb{};
			bool gotData = false;
			if (auto* ss = RE::BSGraphics::RendererShadowState::GetSingleton()) {
				const auto vd = ss->GetRuntimeData().cameraData.getEye(0);
				const float chk = vd.viewMat.m[0][0] + vd.viewMat.m[1][1] + vd.viewMat.m[2][2];
				if (std::abs(chk) > 1e-6f) {
					std::memcpy(fb.CameraView, &vd.viewMat, 64);
					std::memcpy(fb.CameraProj, &vd.projMat, 64);
					std::memcpy(fb.CameraViewProj, &vd.viewProjMat, 64);
					std::memcpy(fb.CameraViewProjUnjittered, &vd.viewProjMatrixUnjittered, 64);
					std::memcpy(fb.CameraPreviousViewProjUnjittered, &vd.previousViewProjMatrixUnjittered, 64);
					std::memcpy(fb.CameraProjUnjittered, &vd.projMatrixUnjittered, 64);
					const auto invProjU = vd.projMatrixUnjittered.Invert();
					const auto invView = vd.viewMat.Invert();
					const auto invViewProj = vd.viewProjMat.Invert();
					const auto invProj = vd.projMat.Invert();
					std::memcpy(fb.CameraProjUnjitteredInverse, &invProjU, 64);
					std::memcpy(fb.CameraViewInverse, &invView, 64);
					std::memcpy(fb.CameraViewProjInverse, &invViewProj, 64);
					std::memcpy(fb.CameraProjInverse, &invProj, 64);
					const auto pa = ss->GetRuntimeData().posAdjust.getEye();
					fb.CameraPosAdjust[0] = pa.x;
					fb.CameraPosAdjust[1] = pa.y;
					fb.CameraPosAdjust[2] = pa.z;
					fb.CameraPosAdjust[3] = 1.0f;
					gotData = true;
				}
			}
			if (!gotData)
				return;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(a_ctx->Map(g_frameBuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				std::memcpy(mapped.pData, &fb, sizeof(fb));
				a_ctx->Unmap(g_frameBuf, 0);
			}
		}

		void BindCSConstantBuffers(ID3D11DeviceContext* a_ctx, ID3D11Device* a_dev)
		{
			EnsurePermutationBuffer(a_dev);
			EnsureFrameBuffer(a_dev);
			if (!g_permutationBuf || !g_frameBuf)
				return;
			WriteFrameBuffer(a_ctx);
			ID3D11Buffer* buffers[1] = { g_permutationBuf };
			a_ctx->PSSetConstantBuffers(4, 1, buffers);
			a_ctx->PSSetConstantBuffers(12, 1, &g_frameBuf);
		}

		// ---------------------------------------------------------------------
		// Shadow-mask readback diagnostics (t14 = TexShadowMaskSampler).
		// The engine's material shaders sample this screen-space RGBA mask for
		// shadowing; each channel holds one shadow light's screen-space result.
		// Counting non-empty channels = how many shadow lights are actually
		// visible on screen. Pure read, throttled - zero behavior change.
		// ---------------------------------------------------------------------
		static void ReadbackShadowMask(::ID3D11DeviceContext* a_ctx)
		{
			static ::ID3D11Texture2D* s_staging = nullptr;
			static std::uint32_t s_w = 0, s_h = 0;
			static std::uint32_t s_t14passes = 0;

			::ID3D11ShaderResourceView* srv = nullptr;
			reinterpret_cast<::ID3D11DeviceContext*>(a_ctx)->PSGetShaderResources(14, 1, &srv);
			if (!srv) {
				return;  // not a shadow-receiving pass - silently skip
			}
			// t14 IS bound: throttle on t14-pass count (every 64th) instead of
			// total BeginTechnique count (non-t14 passes drowned the old
			// throttle so the readback never fired). 15MB blocking readback
			// per shot - every 4th was too aggressive (stutter).
			if ((s_t14passes++ & 0x3Fu) != 0) {
				srv->Release();
				return;
			}
			::ID3D11Resource* res = nullptr;
			srv->GetResource(&res);
			srv->Release();
			if (!res) {
				return;
			}
			::ID3D11Texture2D* tex = nullptr;
			if (FAILED(res->QueryInterface(__uuidof(::ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
				res->Release();
				return;
			}
			res->Release();

			::D3D11_TEXTURE2D_DESC td{};
			tex->GetDesc(&td);
			if (td.Width != s_w || td.Height != s_h || !s_staging) {
				if (s_staging)
					s_staging->Release();
				s_w = td.Width;
				s_h = td.Height;
				::D3D11_TEXTURE2D_DESC sd{};
				sd.Width = td.Width;
				sd.Height = td.Height;
				sd.MipLevels = 1;
				sd.ArraySize = 1;
				sd.Format = td.Format;
				sd.SampleDesc = { 1, 0 };
				sd.Usage = ::D3D11_USAGE_STAGING;
				sd.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
				::ID3D11Device* dev = nullptr;
				a_ctx->GetDevice(&dev);
				if (dev) {
					dev->CreateTexture2D(&sd, nullptr, &s_staging);
					dev->Release();
				}
			}
			if (!s_staging) {
				tex->Release();
				return;
			}

			a_ctx->CopyResource(s_staging, tex);
			tex->Release();

			::D3D11_MAPPED_SUBRESOURCE ms{};
			if (FAILED(a_ctx->Map(s_staging, 0, ::D3D11_MAP_READ, 0, &ms))) {
				return;
			}
			// Thinned scan: every 8th pixel, every 8th row.
			std::uint32_t samples = 0;
			double sum[4] = { 0, 0, 0, 0 };
			std::uint32_t nonZero[4] = { 0, 0, 0, 0 };  // pixels with value > 0.01
			const auto* row = static_cast<const std::uint8_t*>(ms.pData);
			for (std::uint32_t y = 0; y < s_h; y += 8) {
				const auto* px = row + static_cast<std::size_t>(y) * ms.RowPitch;
				for (std::uint32_t x = 0; x < s_w; x += 8) {
					const float v[4] = {
						px[x * 4 + 0] / 255.0f, px[x * 4 + 1] / 255.0f,
						px[x * 4 + 2] / 255.0f, px[x * 4 + 3] / 255.0f
					};
					for (int c = 0; c < 4; c++) {
						sum[c] += v[c];
						if (v[c] > 0.01f)
							nonZero[c]++;
					}
					samples++;
				}
			}
			a_ctx->Unmap(s_staging, 0);

			const float total = samples > 0 ? static_cast<float>(samples) : 1.0f;
			// A channel "has shadow content" if >5% of pixels are non-zero.
			char active[16] = { 0 };
			int n = 0;
			const char* names[4] = { "R", "G", "B", "A" };
			for (int c = 0; c < 4; c++) {
				if (nonZero[c] / total > 0.05f)
					n += std::snprintf(active + n, sizeof(active) - static_cast<std::size_t>(n), "%s ", names[c]);
			}
			SKSE::log::info("[SLF] t14 {}x{}: content [{}] mean {:.2f}/{:.2f}/{:.2f}/{:.2f} nz% {:.0f}/{:.0f}/{:.0f}/{:.0f}",
				s_w, s_h, n > 0 ? active : "none",
				sum[0] / total, sum[1] / total, sum[2] / total, sum[3] / total,
				nonZero[0] / total * 100.0f, nonZero[1] / total * 100.0f,
				nonZero[2] / total * 100.0f, nonZero[3] / total * 100.0f);
		}

		// ---------------------------------------------------------------------
	// SLF PS: our own engine-semantics Lighting PS (Shaders/Lighting_SLF.hlsl).
	// Compiled WITHOUT CS macros (it uses only engine cbuffers b0/b1/b2/b12 +
	// engine textures t0/t1/t4/t5/t14). Replaces the vanilla PS by binding to
	// the D3D device at BeginTechnique - engine state keeps the vanilla shader.
	// ---------------------------------------------------------------------
#if SLF_PS_ENABLED
	inline ID3D11PixelShader* g_slfPS = nullptr;
	inline std::uint32_t g_slfPSCompileFail = 0;

	ID3D11PixelShader* CompileSLFPS(ID3D11Device* a_dev)
	{
		if (g_slfPS)
			return g_slfPS;
		static const wchar_t* src = L"D:/Modding/ShadowLimitFix/Shaders/Lighting_SLF.hlsl";
		ID3DBlob* blob = nullptr;
		ID3DBlob* err = nullptr;
		HRESULT hr = D3DCompileFromFile(src, nullptr, nullptr, "main", "ps_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
		if (FAILED(hr)) {
			g_slfPSCompileFail++;
			if (err) {
				SKSE::log::error("[SLF] SLF PS compile FAILED: {}", static_cast<const char*>(err->GetBufferPointer()));
				err->Release();
			} else {
				SKSE::log::error("[SLF] SLF PS compile FAILED hr={:#x}", hr);
			}
			return nullptr;
		}
		HRESULT hr2 = a_dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_slfPS);
		blob->Release();
		if (FAILED(hr2)) {
			SKSE::log::error("[SLF] SLF PS CreatePixelShader FAILED hr={:#x}", hr2);
			return nullptr;
		}
		SKSE::log::info("[SLF] SLF PS compiled + created (own engine-semantics shader)");
		return g_slfPS;
	}

	// ---- v3 N-light shadow data: b13 cbuffer + t103 depth-array SRV ----
	// slfLight[i][0..3] = view-proj (row-major, v' = v * vp)
	// slfLight[i][4]    = (pos.xyz, radius)
	// slfLight[i][5]    = (shadowMapIndex, flags, 0, 0)
	// slfLight[i][6]    = pad
	// slfHeader         = (count, texSize, 0, 0)
	inline ::ID3D11Buffer* g_slfShadowCB = nullptr;
	inline ::ID3D11ShaderResourceView* g_slfDepthSRV = nullptr;  // t103 view (engine or self-made)
	inline std::uint32_t g_slfShadowTexSize = 2048;

	::ID3D11Buffer* EnsureSLFShadowBuffer(::ID3D11Device* a_dev)
	{
		if (g_slfShadowCB)
			return g_slfShadowCB;
		::D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = (8 * 7 + 1) * 16;  // 57 float4 = 912 B
		bd.Usage = ::D3D11_USAGE_DYNAMIC;
		bd.BindFlags = ::D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = ::D3D11_CPU_ACCESS_WRITE;
		if (FAILED(a_dev->CreateBuffer(&bd, nullptr, &g_slfShadowCB))) {
			SKSE::log::error("[SLF] SLF shadow cbuffer create FAILED");
			return nullptr;
		}
		// Diagnose the engine's shadow depth-array SRV (slice count matters:
		// our 127-slice patch only helps if the SRV covers all slices).
		if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
			auto& dsd = renderer->GetDepthStencilData();
			auto* srv = dsd.depthStencils[4].depthSRV;
			if (srv) {
				auto* native = reinterpret_cast<::ID3D11ShaderResourceView*>(srv);
				::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				native->GetDesc(&srvDesc);
				const int arraySize =
					srvDesc.ViewDimension == ::D3D11_SRV_DIMENSION_TEXTURE2DARRAY ?
					static_cast<int>(srvDesc.Texture2DArray.ArraySize) : -1;
				SKSE::log::info("[SLF] kSHADOWMAPS depthSRV: format={} dim={} arraySize={}",
					static_cast<int>(srvDesc.Format),
					static_cast<int>(srvDesc.ViewDimension), arraySize);
				// The engine created this SRV for the VANILLA slice count
				// (8). Our 127-slice patch extends the texture but not the
				// view - so build our own full-array view over the same
				// resource when the engine view is too small.
				if (arraySize >= 127) {
					g_slfDepthSRV = native;
					g_slfDepthSRV->AddRef();
				} else {
					::ID3D11Resource* tex = nullptr;
					native->GetResource(&tex);
					if (tex) {
						::D3D11_SHADER_RESOURCE_VIEW_DESC full{};
						full.Format = srvDesc.Format;
						full.ViewDimension = ::D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
						full.Texture2DArray.MipLevels = 1;
						full.Texture2DArray.FirstArraySlice = 0;
						full.Texture2DArray.ArraySize = 127;
						if (SUCCEEDED(a_dev->CreateShaderResourceView(tex, &full, &g_slfDepthSRV))) {
							SKSE::log::info("[SLF] created full 127-slice depth SRV (engine view was {})", arraySize);
						} else {
							SKSE::log::error("[SLF] full 127-slice SRV create FAILED - falling back to engine view");
							g_slfDepthSRV = native;
							g_slfDepthSRV->AddRef();
						}
						tex->Release();
					} else {
						g_slfDepthSRV = native;
						g_slfDepthSRV->AddRef();
					}
				}
				if (arraySize > 512)
					g_slfShadowTexSize = 4096;
				else
					g_slfShadowTexSize = 2048;
				// Prefer the REAL texture width (the arraySize heuristic was
				// wrong: the map is 4096x4096 with 127 slices, guessed 1024 ->
				// HLSL sampled only the top-left quarter of each shadow map).
				::ID3D11Resource* texRes = nullptr;
				native->GetResource(&texRes);
				if (texRes) {
					::D3D11_TEXTURE2D_DESC td{};
					reinterpret_cast<::ID3D11Texture2D*>(texRes)->GetDesc(&td);
					g_slfShadowTexSize = td.Width;
					texRes->Release();
				}
			} else {
				SKSE::log::error("[SLF] kSHADOWMAPS depthSRV is NULL!");
			}
		}
		SKSE::log::info("[SLF] SLF shadow cbuffer created (912B), tex size {}", g_slfShadowTexSize);
		return g_slfShadowCB;
	}

	// Copy our per-frame light data (g_shadowLights, written by the scheduler)
	// into the b13 cbuffer. Throttled to once per frame via frame counter.
	void UpdateSLFShadowData(::ID3D11DeviceContext* a_ctx)
	{
		if (!g_slfShadowCB)
			return;
		static std::uint32_t s_frame = 0;
		const std::uint32_t frame = s_frame++;
		const std::uint32_t count = std::min<std::uint32_t>(
			ShadowLimitFixNS::P1::g_shadowLightCount.load(std::memory_order_acquire), 8u);
		if (frame == 0)
			return;  // first call: ensure cbuffer exists before updating

		::D3D11_MAPPED_SUBRESOURCE ms{};
		if (FAILED(a_ctx->Map(g_slfShadowCB, 0, ::D3D11_MAP_WRITE_DISCARD, 0, &ms)))
			return;
		auto* dst = static_cast<float*>(ms.pData);
		for (std::uint32_t i = 0; i < 8; i++) {
			const auto& ld = ShadowLimitFixNS::P1::g_shadowLights[i];
			float* d = dst + i * 28;  // 7 float4 per light
			for (int k = 0; k < 16; k++)
				d[k] = ld.proj[k];
			d[16] = ld.pos[0];
			d[17] = ld.pos[1];
			d[18] = ld.pos[2];
			d[19] = ld.radius;
			d[20] = ld.shadowMapIndex;
			d[21] = ld.flags;
			d[22] = d[23] = 0.0f;
			d[24] = d[25] = d[26] = d[27] = 0.0f;  // pad
		}
		// header: (count, texSize, 0, 0)
		dst[224] = static_cast<float>(count);
		dst[225] = static_cast<float>(g_slfShadowTexSize);
		dst[226] = dst[227] = 0.0f;
		a_ctx->Unmap(g_slfShadowCB, 0);
	}

	// Bind b13 (our shadow data) + t103 (kSHADOWMAPS depth-array SRV).
	void BindSLFResources(::ID3D11DeviceContext* a_ctx)
	{
		if (!g_slfShadowCB)
			return;
		a_ctx->PSSetConstantBuffers(13, 1, &g_slfShadowCB);
		if (g_slfDepthSRV)
			a_ctx->PSSetShaderResources(103, 1, &g_slfDepthSRV);
	}

	// ---------------------------------------------------------------------
	// t14-bind swap point (13:20 diagnosis):
	// BeginTechnique fires BEFORE the engine binds this material's SRVs, so
	// IsShadowMaskBound(t14) there only sees the PREVIOUS draw's leftover
	// binding - usually NULL => the swap NEVER fired since v2.1 ("materials
	// recovered" was really "swap went 0% and everything fell back to
	// vanilla"). The engine binds the shadow-mask texture (t14) during draw
	// setup via PSSetShaderResources. Swap the SLF PS at THAT call instead:
	//   engine binds slot 14  <=>  this pass really samples the shadow mask
	// which is the accurate DefShadow filter the old check tried to be.
	// ---------------------------------------------------------------------
	inline std::atomic<bool> g_slfArmSwap{ false };
	std::uint32_t g_slfSwapCount = 0;
	std::uint32_t g_slfSwapLogged = 0;

	// Route B gate (fix12 2026-09-03): engage the SLF PS ONLY when more than
	// 4 shadow lights are scheduled this frame. Vanilla's shadow consumer is
	// a 4-channel 2D mask (t14) - the engine physically cannot express >4
	// shadow lights per frame and its slot rotation between them IS the
	// flicker. When the scene fits in 4 the vanilla PS is 100% faithful, so
	// we skip the swap entirely (no PS change, no arm, no b13/t103 binds) -
	// the fix11-era global-replacement material degradation cannot recur in
	// <=4-light scenes. Count is published by the scheduler every frame
	// (g_shadowLightCount, engine-accum prefix + SLF extension).
	inline bool SLFShouldSwap()
	{
		return ShadowLimitFixNS::P1::g_shadowLightCount.load(std::memory_order_acquire) > 4;
	}

	using PSSetShaderResourcesFn = HRESULT(STDMETHODCALLTYPE*)(
		::ID3D11DeviceContext*, UINT, UINT, ::ID3D11ShaderResourceView* const*);
	PSSetShaderResourcesFn g_origPSSetSRV = nullptr;

	// Swap the vanilla Lighting PS for our SLF PS at the exact moment t14 is
	// bound. b13/t103 refresh first, then the PS, then the MAT-pass readback
	// (throttled) proving whether shadow geometry reached the t103 array.
	void DoSLFShadowSwap(::ID3D11DeviceContext* a_ctx)
	{
		if (!g_slfPS)
			return;
		// Belt & braces: the arm is cleared on every state reset, but a swap
		// armed in a >4-light scene must not fire after the scene dropped to
		// <=4 (engine PS is already bound for those draws - leave it).
		if (!SLFShouldSwap())
			return;
		UpdateSLFShadowData(a_ctx);
		BindSLFResources(a_ctx);
		a_ctx->PSSetShader(g_slfPS, nullptr, 0);
		g_slfSwapCount++;
		SLFRecordBT(0, 0, nullptr, false, false, true);
		DebugReadbackShadowArrayAtMaterialPass(a_ctx);
		// fix14-diag: throttle 32 -> 8 so a low swap rate is visible in a
		// short test run; also print the gate value at execution time.
		if (g_slfSwapCount - g_slfSwapLogged >= 8) {
			g_slfSwapLogged = g_slfSwapCount;
			SKSE::log::info("[SLF] SLF PS swap-on-t14-bind: total swaps = {} (gate open, {} scheduled)",
				g_slfSwapCount,
				ShadowLimitFixNS::P1::g_shadowLightCount.load(std::memory_order_acquire));
		}
	}

	HRESULT STDMETHODCALLTYPE HookPSSetShaderResources(
		::ID3D11DeviceContext* a_ctx, UINT a_startSlot, UINT a_numViews,
		::ID3D11ShaderResourceView* const* a_views)
	{
		const HRESULT hr = g_origPSSetSRV(a_ctx, a_startSlot, a_numViews, a_views);
		if (g_slfArmSwap.load(std::memory_order_acquire) &&
			a_startSlot <= 14 && a_startSlot + a_numViews > 14) {
			// The engine just bound the shadow-mask texture to slot 14 -
			// this pass samples shadows. Consume the arm and swap the PS.
			g_slfArmSwap.store(false, std::memory_order_release);
			DoSLFShadowSwap(a_ctx);
		}
		return hr;
	}

	void InstallSLFContextHooks()
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer)
			return;
		auto* ctx = reinterpret_cast<::ID3D11DeviceContext*>(renderer->GetRuntimeData().context);
		if (!ctx || g_origPSSetSRV)
			return;
		void** vtbl = *reinterpret_cast<void***>(ctx);
		if (!vtbl || vtbl[8] == reinterpret_cast<void*>(&HookPSSetShaderResources))
			return;
		g_origPSSetSRV = reinterpret_cast<PSSetShaderResourcesFn>(vtbl[8]);
		DWORD oldProtect = 0;
		::VirtualProtect(&vtbl[8], sizeof(void*), PAGE_READWRITE, &oldProtect);
		vtbl[8] = reinterpret_cast<void*>(&HookPSSetShaderResources);
		::VirtualProtect(&vtbl[8], sizeof(void*), oldProtect, &oldProtect);
		SKSE::log::info("[SLF] t14-bind swap hook installed (PSSetShaderResources vtable[8])");
	}
#endif  // SLF_PS_ENABLED

#if SLF_POSTLIGHT_ENABLED
	// ---------------------------------------------------------------------
	// fix20 (2026-09-06): POST-LIGHT MULTI-LAMP PASS.
	//
	// Consumer for the extended lamps SLF renders into the 127-slice shadow
	// array (scheduled index >= engine count) that the engine forward pass
	// never lights. Fired once per scene frame at the first ImageSpace
	// BeginTechnique (main scene draws done, engine image-space post still
	// ahead -> whatever we copy into kMAIN is what the post chain sees).
	// Full-screen triangle over kMAIN color+depth, re-lights with extended
	// lamps, writes a temp RT, copies back into the kMAIN texture.
	// Engine PS untouched; ENB untouched (runs before its hooks see it).
	namespace {
		inline ::ID3D11PixelShader* g_postPS = nullptr;
		inline ::ID3D11VertexShader* g_postVS = nullptr;
		inline ::ID3D11Buffer* g_postCB = nullptr;
		inline ::ID3D11Texture2D* g_postTempTex = nullptr;
		inline ::ID3D11RenderTargetView* g_postTempRTV = nullptr;
		inline ::ID3D11SamplerState* g_postSamp = nullptr;
		// fix20b (2026-09-06, black-screen report): the full-screen triangle
		// previously inherited whatever rasterizer/blend/depth-stencil state
		// the engine left bound (it may have scissor rects clipped to a local
		// draw, an additive/alpha blend, or depth write on). With the temp RT
		// cleared to black and CopyResource unconditionally overwriting kMAIN,
		// any partial coverage / bad blend = the black screen + diagonal band
		// the user saw. Own explicit states so the triangle ALWAYS covers the
		// whole target with opaque overwrite and no depth interaction.
		inline ::ID3D11RasterizerState* g_postRS = nullptr;   // cull none, scissor OFF
		inline ::ID3D11BlendState* g_postBS = nullptr;        // opaque (blend OFF)
		inline ::ID3D11DepthStencilState* g_postDSS = nullptr;  // depth OFF
		inline ::ID3D11Texture2D* g_postDiagStaging = nullptr;  // kMAIN content readback
		inline std::uint32_t g_postRun = 0;
		inline std::uint32_t g_postSkipNoGeom = 0;
		inline std::uint32_t g_postCopyFail = 0;

		inline RE::NiCamera* PostGetWorldCamera()
		{
			static REL::RelocationID uid(528087, 415032);
			auto* sg = *reinterpret_cast<RE::BSSceneGraph**>(uid.address());
			return sg ? sg->GetRuntimeData().camera.get() : nullptr;
		}

		// Row-major 4x4 inverse (v' = v * M convention; o = M^-1).
		inline bool PostInvert4(const float* m, float* o)
		{
			float inv[16];
			inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
			         m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
			inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
			         m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
			inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
			         m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
			inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
			          m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
			inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
			         m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
			inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
			         m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
			inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
			         m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
			inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
			          m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
			inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
			         m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
			inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
			         m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
			inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
			          m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
			inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
			          m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
			inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
			         m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
			inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
			         m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
			inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
			          m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
			inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
			          m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
			float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
			if (det == 0.0f)
				return false;
			det = 1.0f / det;
			for (int i = 0; i < 16; i++)
				o[i] = inv[i] * det;
			return true;
		}

		// Camera view-proj (row-major v' = v * vp), same math as the
		// scheduler's ShadowmapDescriptor rebuild (verified by 23-lamp
		// shadow rendering). Main camera frustum, perspective path.
		inline bool PostCameraViewProj(RE::NiCamera* a_cam, float* o_vp)
		{
			const float* view = &a_cam->GetRuntimeData().worldToCam[0][0];
			const auto& fr = a_cam->GetRuntimeData2().viewFrustum;
			if (fr.bOrtho)
				return false;  // main view is perspective; skip ortho path
			const float l = fr.fLeft, r = fr.fRight, t = fr.fTop, b = fr.fBottom;
			const float fn = fr.fNear, ff = fr.fFar;
			if (ff <= fn || r <= l || t <= b)
				return false;
			float p[16]{};
			p[0] = 2.0f * fn / (r - l);
			p[5] = 2.0f * fn / (t - b);
			p[8] = (r + l) / (r - l);
			p[9] = (t + b) / (t - b);
			p[10] = ff / (ff - fn);
			p[11] = 1.0f;
			p[14] = -fn * ff / (ff - fn);
			// vp = view * proj (row-major multiply, same as scheduler)
			for (int r2 = 0; r2 < 4; r2++)
				for (int c = 0; c < 4; c++) {
					float s = 0.0f;
					for (int k = 0; k < 4; k++)
						s += view[r2 * 4 + k] * p[k * 4 + c];
					o_vp[r2 * 4 + c] = s;
				}
			return true;
		}

		// fix20b diag: read back a 16x16 center block of the kMAIN color
		// texture (R11G11B10_FLOAT = 32 bpp) to learn (a) whether the engine
		// had ALREADY rendered the scene into kMAIN at our fire moment (PRE =
		// mostly non-zero) or fired too early into a black target, and (b)
		// what the CopyResource left in kMAIN (POST; == temp content, black
		// if our pass painted nothing). Non-zero pixel ratio + center pixel
		// raw hex (0x00000000 = black). GPU stall is fine at the throttled
		// cadence this runs at (~every 256 passes).
		inline void PostReadbackCenter(::ID3D11Device* a_dev, ::ID3D11DeviceContext* a_ctx,
			::ID3D11Texture2D* a_src, const char* a_tag)
		{
			if (!a_src)
				return;
			::D3D11_TEXTURE2D_DESC srcDesc{};
			a_src->GetDesc(&srcDesc);
			// fix22 (2026-09-06): full-frame staging + 16x16 grid ASCII map.
			// The old helper copied only the CENTER 16x16 block, so the
			// persistent POST "nonzero=128/256" was really "half the center
			// block dark" - consistent with the diagonal VS-triangle bug,
			// but invisible as a shape. Now every diag paints the whole
			// frame's lit/dark layout ('.' dark, '#' lit) so coverage /
			// shape errors are read at a glance, not inferred from one count.
			if (g_postDiagStaging) {
				::D3D11_TEXTURE2D_DESC td{};
				g_postDiagStaging->GetDesc(&td);
				if (td.Width != srcDesc.Width || td.Height != srcDesc.Height || td.Format != srcDesc.Format) {
					g_postDiagStaging->Release();
					g_postDiagStaging = nullptr;
				}
			}
			if (!g_postDiagStaging) {
				::D3D11_TEXTURE2D_DESC sd{};
				sd.Width = srcDesc.Width;
				sd.Height = srcDesc.Height;
				sd.MipLevels = 1;
				sd.ArraySize = 1;
				sd.Format = srcDesc.Format;  // CopyResource demands exact match
				sd.SampleDesc.Count = 1;
				sd.Usage = ::D3D11_USAGE_STAGING;
				sd.BindFlags = 0;
				sd.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
				if (FAILED(a_dev->CreateTexture2D(&sd, nullptr, &g_postDiagStaging))) {
					SKSE::log::error("[SLF][POST] diag staging create FAILED");
					return;
				}
			}
			a_ctx->CopyResource(g_postDiagStaging, a_src);
			::D3D11_MAPPED_SUBRESOURCE ms{};
			if (FAILED(a_ctx->Map(g_postDiagStaging, 0, ::D3D11_MAP_READ, 0, &ms))) {
				SKSE::log::error("[SLF][POST] diag map FAILED ({})", a_tag);
				return;
			}
			const std::uint32_t W = srcDesc.Width > 0 ? srcDesc.Width : 1;
			const std::uint32_t H = srcDesc.Height > 0 ? srcDesc.Height : 1;
			const std::uint32_t cw = W / 16;
			const std::uint32_t ch = H / 16;
			auto sampleCell = [&](std::uint32_t gx, std::uint32_t gy) -> std::uint32_t {
				const std::uint32_t px = std::min(W - 1u, gx * cw + cw / 2);
				const std::uint32_t py = std::min(H - 1u, gy * ch + ch / 2);
				const auto* p = reinterpret_cast<const std::uint32_t*>(
					reinterpret_cast<const std::uint8_t*>(ms.pData) + static_cast<std::size_t>(py) * ms.RowPitch);
				return p[px];
			};
			std::uint32_t nz = 0;
			char map[16][17];
			for (std::uint32_t gy = 0; gy < 16; gy++) {
				for (std::uint32_t gx = 0; gx < 16; gx++) {
					const bool lit = sampleCell(gx, gy) != 0;
					map[gy][gx] = lit ? '#' : '.';
					if (lit)
						nz++;
				}
				map[gy][16] = '\0';
			}
			const std::uint32_t tl = sampleCell(0, 0);
			const std::uint32_t tr = sampleCell(15, 0);
			const std::uint32_t cc = sampleCell(8, 8);
			const std::uint32_t bl = sampleCell(0, 15);
			const std::uint32_t br = sampleCell(15, 15);
			a_ctx->Unmap(g_postDiagStaging, 0);
			SKSE::log::info("[SLF][POST] diag {}: nz={}/256 TL=0x{:08X} TR=0x{:08X} C=0x{:08X} BL=0x{:08X} BR=0x{:08X} ({}x{} fmt={})",
				a_tag, nz, tl, tr, cc, bl, br, W, H, static_cast<int>(srcDesc.Format));
			for (std::uint32_t gy = 0; gy < 16; gy++)
				SKSE::log::info("[SLF][POST] diag {} grid: {}", a_tag, map[gy]);
		}
	}

	// Step 2 diag (fix26c, 2026-09-07): read back one kSHADOWMAPS slice at
	// the post-pass moment (the texture t2 samples) and print a 16x16 depth
	// grid. Answers "does the extended light's slice actually carry depth?",
	// which decides whether an all-lit shadow test is empty-map or math.
	// kSHADOWMAPS is D16 (P1_hooks reads uint16 / 65535); staging reuses the
	// same format so depth = u16 / 65535.
	void PostReadbackShadowSlice(::ID3D11Device* a_dev, ::ID3D11DeviceContext* a_ctx,
		::ID3D11Texture2D* a_arrTex, std::uint32_t a_slice, const char* a_tag)
	{
		if (!a_arrTex)
			return;
		::D3D11_TEXTURE2D_DESC td{};
		a_arrTex->GetDesc(&td);
		if (a_slice >= td.ArraySize)
			return;
		static ::ID3D11Texture2D* s_stg = nullptr;
		static ::D3D11_TEXTURE2D_DESC s_stgDesc{};
		if (!s_stg || s_stgDesc.Width != td.Width || s_stgDesc.Height != td.Height ||
			s_stgDesc.Format != td.Format) {
			if (s_stg) {
				s_stg->Release();
				s_stg = nullptr;
			}
			::D3D11_TEXTURE2D_DESC sd = td;
			sd.ArraySize = 1;
			sd.Usage = ::D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = ::D3D11_CPU_ACCESS_READ;
			sd.MiscFlags = 0;
			if (FAILED(a_dev->CreateTexture2D(&sd, nullptr, &s_stg))) {
				SKSE::log::error("[SLF][POST] shadow-slice staging create FAILED ({})", a_tag);
				return;
			}
			s_stgDesc = sd;
		}
		::D3D11_BOX box{};
		box.right = td.Width;
		box.bottom = td.Height;
		box.back = 1;
		a_ctx->CopySubresourceRegion(s_stg, 0, 0, 0, 0, a_arrTex, a_slice, &box);
		::D3D11_MAPPED_SUBRESOURCE ms{};
		if (FAILED(a_ctx->Map(s_stg, 0, ::D3D11_MAP_READ, 0, &ms))) {
			SKSE::log::error("[SLF][POST] shadow-slice map FAILED ({})", a_tag);
			return;
		}
		const std::uint32_t W = td.Width > 0 ? td.Width : 1;
		const std::uint32_t H = td.Height > 0 ? td.Height : 1;
		const std::uint32_t cw = W / 16;
		const std::uint32_t ch = H / 16;
		auto depthAt = [&](std::uint32_t gx, std::uint32_t gy) -> float {
			const std::uint32_t px = std::min(W - 1u, gx * cw + cw / 2);
			const std::uint32_t py = std::min(H - 1u, gy * ch + ch / 2);
			const auto* p = reinterpret_cast<const std::uint8_t*>(ms.pData) +
				static_cast<std::size_t>(py) * ms.RowPitch;
			return static_cast<float>(*reinterpret_cast<const std::uint16_t*>(
				p + static_cast<std::size_t>(px) * 2u)) / 65535.0f;
		};
		std::uint32_t content = 0, nearC = 0, midC = 0, farC = 0;
		char map[16][17];
		for (std::uint32_t gy = 0; gy < 16; gy++) {
			for (std::uint32_t gx = 0; gx < 16; gx++) {
				const float d = depthAt(gx, gy);
				char c = '.';
				if (d > 0.001f && d < 0.999f) {
					c = '#';
					content++;
					if (d < 0.4f)
						nearC++;
					else if (d < 0.8f)
						midC++;
					else
						farC++;
				}
				map[gy][gx] = c;
			}
			map[gy][16] = '\0';
		}
		a_ctx->Unmap(s_stg, 0);
		SKSE::log::info("[SLF][POST] shadow-slice {}: slice={} {}x{} content={}/256 (near<0.4:{} mid:{} far:{})",
			a_tag, a_slice, W, H, content, nearC, midC, farC);
		for (std::uint32_t gy = 0; gy < 16; gy++)
			SKSE::log::info("[SLF][POST] shadow-slice {} grid: {}", a_tag, map[gy]);
	}

	void RunPostLightPass(::ID3D11DeviceContext* a_ctx)
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer)
			return;
		auto* dev = reinterpret_cast<::ID3D11Device*>(renderer->GetRuntimeData().forwarder);
		if (!dev)
			return;
		const std::uint32_t nL = ShadowLimitFixNS::P1::g_postLightCount.load(std::memory_order_acquire);
		if (nL == 0)
			return;
		// fix23 perf probe: full post-pass CPU cost (CB fill + clear + draw
		// + copy-back). Logged every 64th run, skipped on doDiag frames so
		// the 256-frame readback never pollutes the sample.
		using pclk = std::chrono::steady_clock;
		const auto pp0 = pclk::now();

		// ---- Lazy resource creation ----
		auto& mainRT = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMAIN];
		auto& mainDS = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN];
		if (!mainRT.texture || !mainRT.SRV || !mainDS.texture || !mainDS.depthSRV)
			return;

		if (!g_postPS) {
			static const wchar_t* src = L"D:/Modding/ShadowLimitFix/Shaders/PostLightPass.hlsl";
			ID3DBlob* vsB = nullptr;
			ID3DBlob* psB = nullptr;
			ID3DBlob* err = nullptr;
			HRESULT hr = D3DCompileFromFile(src, nullptr, nullptr, "VSMain", "vs_5_0",
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsB, &err);
			if (FAILED(hr)) {
				SKSE::log::error("[SLF][POST] VS compile FAILED: {}",
					err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
				if (err)
					err->Release();
				return;
			}
			hr = D3DCompileFromFile(src, nullptr, nullptr, "PSMain", "ps_5_0",
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psB, &err);
			if (FAILED(hr)) {
				SKSE::log::error("[SLF][POST] PS compile FAILED: {}",
					err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
				if (err)
					err->Release();
				vsB->Release();
				return;
			}
			if (FAILED(dev->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &g_postVS)) ||
				FAILED(dev->CreatePixelShader(psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &g_postPS))) {
				SKSE::log::error("[SLF][POST] shader create FAILED");
				vsB->Release();
				psB->Release();
				return;
			}
			vsB->Release();
			psB->Release();

			::D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = (32 * 7 + 6) * 16;  // 32 lights x 7 float4 + head + res + vpInv
			bd.Usage = ::D3D11_USAGE_DYNAMIC;
			bd.BindFlags = ::D3D11_BIND_CONSTANT_BUFFER;
			bd.CPUAccessFlags = ::D3D11_CPU_ACCESS_WRITE;
			if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_postCB))) {
				SKSE::log::error("[SLF][POST] cbuffer create FAILED");
				return;
			}
			::D3D11_SAMPLER_DESC sd{};
			sd.Filter = ::D3D11_FILTER_MIN_MAG_MIP_POINT;
			sd.AddressU = sd.AddressV = sd.AddressW = ::D3D11_TEXTURE_ADDRESS_CLAMP;
			if (FAILED(dev->CreateSamplerState(&sd, &g_postSamp))) {
				SKSE::log::error("[SLF][POST] sampler create FAILED");
				return;
			}
			// Own rasterizer: cull none + scissor OFF (engine may leave a
			// clipped scissor rect bound; with scissor enable inherited the
			// triangle only painted part of the temp RT -> black band).
			::D3D11_RASTERIZER_DESC rd{};
			rd.FillMode = ::D3D11_FILL_SOLID;
			rd.CullMode = ::D3D11_CULL_NONE;
			rd.FrontCounterClockwise = FALSE;
			rd.DepthBias = 0;
			rd.DepthBiasClamp = 0.0f;
			rd.SlopeScaledDepthBias = 0.0f;
			rd.DepthClipEnable = FALSE;
			rd.ScissorEnable = FALSE;
			rd.MultisampleEnable = FALSE;
			rd.AntialiasedLineEnable = FALSE;
			if (FAILED(dev->CreateRasterizerState(&rd, &g_postRS))) {
				SKSE::log::error("[SLF][POST] rasterizer create FAILED");
				return;
			}
			// Own opaque blend: no engine alpha/additive leftover.
			::D3D11_BLEND_DESC bd2{};
			bd2.AlphaToCoverageEnable = FALSE;
			bd2.IndependentBlendEnable = FALSE;
			for (UINT bi = 0; bi < 8; bi++) {
				bd2.RenderTarget[bi].BlendEnable = FALSE;
				bd2.RenderTarget[bi].RenderTargetWriteMask = ::D3D11_COLOR_WRITE_ENABLE_ALL;
			}
			if (FAILED(dev->CreateBlendState(&bd2, &g_postBS))) {
				SKSE::log::error("[SLF][POST] blend create FAILED");
				return;
			}
			// Own depth-stencil: fully off (we bind no DSV; do not let an
			// inherited depth state reject pixels).
			::D3D11_DEPTH_STENCIL_DESC dsd{};
			dsd.DepthEnable = FALSE;
			dsd.DepthWriteMask = ::D3D11_DEPTH_WRITE_MASK_ZERO;
			dsd.DepthFunc = ::D3D11_COMPARISON_ALWAYS;
			dsd.StencilEnable = FALSE;
			if (FAILED(dev->CreateDepthStencilState(&dsd, &g_postDSS))) {
				SKSE::log::error("[SLF][POST] depth-stencil create FAILED");
				return;
			}
			SKSE::log::info("[SLF][POST] pass resources created (VS/PS/CB/sampler/RS/BS/DSS)");
		}

		// ---- Temp RT sized like kMAIN (create once; recreate if resized) ----
		::D3D11_TEXTURE2D_DESC mainDesc{};
		mainRT.texture->GetDesc(&mainDesc);
		bool needCreate = true;
		if (g_postTempTex) {
			::D3D11_TEXTURE2D_DESC td{};
			g_postTempTex->GetDesc(&td);
			needCreate = (td.Width != mainDesc.Width || td.Height != mainDesc.Height ||
				td.Format != mainDesc.Format);
		}
		if (needCreate) {
			if (g_postTempRTV) {
				g_postTempRTV->Release();
				g_postTempRTV = nullptr;
			}
			if (g_postTempTex) {
				g_postTempTex->Release();
				g_postTempTex = nullptr;
			}
			::D3D11_TEXTURE2D_DESC td2 = mainDesc;
			td2.BindFlags = ::D3D11_BIND_RENDER_TARGET;
			td2.MipLevels = 1;
			td2.Usage = ::D3D11_USAGE_DEFAULT;
			td2.CPUAccessFlags = 0;
			td2.MiscFlags = 0;
			if (FAILED(dev->CreateTexture2D(&td2, nullptr, &g_postTempTex))) {
				SKSE::log::error("[SLF][POST] temp RT create FAILED");
				return;
			}
			if (FAILED(dev->CreateRenderTargetView(g_postTempTex, nullptr, &g_postTempRTV))) {
				SKSE::log::error("[SLF][POST] temp RTV create FAILED");
				return;
			}
		}

		// ---- Fill b9 ----
		::D3D11_MAPPED_SUBRESOURCE ms{};
		if (FAILED(a_ctx->Map(g_postCB, 0, ::D3D11_MAP_WRITE_DISCARD, 0, &ms)))
			return;
		auto* f = static_cast<float*>(ms.pData);
		const std::uint32_t nUp = nL < 32 ? nL : 32;
		// b9 layout (HLSL mirrors): pl[i*7+0]=(pos,radius)
		//   [i*7+1]=(color,intensity) [i*7+2]=(slice,type,flags,farDist)
		//   [i*7+3..6]=proj affine world->light-space (row-major, v'=v*M)
		for (std::uint32_t i = 0; i < nUp; i++) {
			const auto& pl = ShadowLimitFixNS::P1::g_postLight[i];
			float* d = f + i * 7 * 4;
			d[0] = pl.pos[0];
			d[1] = pl.pos[1];
			d[2] = pl.pos[2];
			d[3] = pl.radius;
			d[4] = pl.color[0];
			d[5] = pl.color[1];
			d[6] = pl.color[2];
			d[7] = pl.intensity;
			d[8] = pl.slice;
			d[9] = pl.lightType;
			d[10] = pl.flags;
			d[11] = pl.farDist;
			for (int k = 0; k < 16; k++)
				d[12 + k] = pl.proj[k];
		}
		float* head = f + 224 * 4;  // pl[224] (32 lights x 7)
		head[0] = static_cast<float>(nUp);
		head[1] = 0.0f;
		head[2] = 0.0f;
		head[3] = 0.0f;
		float* res = head + 4;
		res[0] = static_cast<float>(mainDesc.Width);
		res[1] = static_cast<float>(mainDesc.Height);
		res[2] = 0.0f;  // dbgMode: 0 = composite, 1 = extended-only, 2 = shadow-only
		res[3] = 0.0f;
		// vp inverse
		auto* cam = PostGetWorldCamera();
		float vp[16]{};
		float vpInv[16]{};
		if (cam && PostCameraViewProj(cam, vp) && PostInvert4(vp, vpInv)) {
			std::memcpy(f + 226 * 4, vpInv, sizeof(vpInv));
		} else {
			// No valid camera matrix: zero out so WorldFromDepth returns
			// garbage-safe (still runs, extended lights will misplace).
			std::memset(f + 226 * 4, 0, sizeof(vpInv));
			g_postSkipNoGeom++;
		}
		a_ctx->Unmap(g_postCB, 0);

		// ---- Draw ----
		// fix21 (2026-09-06): BIND ORDER. Previously PSSetShaderResources
		// bound kMAIN's SRV to t0/t1 while the engine's OM still had kMAIN
		// bound as an RTV (we had not switched yet). D3D11 forbids the same
		// resource as RTV and SRV at once -> the runtime silently unbinds
		// our SRV -> ColorIn.Load/DepthIn.Load read 0 -> the pass painted
		// black over the real scene (PRE bright, POST black). Switch OM to
		// OUR temp target FIRST (this detaches the engine's kMAIN RTV and
		// mainDS DSV), THEN bind kMAIN/mainDS as SRVs - legal again.
		// ---- fix25: save EVERY engine state this pass hijacks ----
		// PPT root cause (user A/B: fix24 pass-off = smooth): we swapped
		// IA/VS/PS/CB9/SRV/sampler/OM/RS/VP to our own mid-frame and returned
		// without restoring. The engine caches its current state on the CPU
		// (Renderer current* pointers) and skips redundant D3D11 sets, so
		// with the device left in OUR state its batch cache is invalidated
		// every frame -> per-frame lighting-pass count exploded (49 vs 13)
		// -> engine CPU-bound. Restoring exactly what we found keeps engine
		// cache == device. Get* AddRefs; Release after restore.
		::ID3D11InputLayout* s_ial = nullptr;
		::D3D11_PRIMITIVE_TOPOLOGY s_topo = ::D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		::ID3D11VertexShader* s_vs = nullptr;
		::ID3D11PixelShader* s_ps = nullptr;
		::ID3D11Buffer* s_vscb9 = nullptr;
		::ID3D11Buffer* s_pscb9 = nullptr;
		::ID3D11ShaderResourceView* s_srv[3] = { nullptr, nullptr, nullptr };
		::ID3D11SamplerState* s_samp0 = nullptr;
		::ID3D11RenderTargetView* s_rtv0 = nullptr;
		::ID3D11DepthStencilView* s_dsv = nullptr;
		::ID3D11BlendState* s_bs = nullptr;
		float s_bf[4] = { 0.f, 0.f, 0.f, 0.f };
		UINT s_sm = 0xFFFFFFFFu;
		::ID3D11DepthStencilState* s_dss = nullptr;
		UINT s_stref = 0;
		::ID3D11RasterizerState* s_rs = nullptr;
		::D3D11_VIEWPORT s_vp[1] = {};
		UINT s_nvp = 1;
		::D3D11_RECT s_sr[1] = {};
		UINT s_nsr = 1;
		a_ctx->IAGetInputLayout(&s_ial);
		a_ctx->IAGetPrimitiveTopology(&s_topo);
		a_ctx->VSGetShader(&s_vs, nullptr, nullptr);
		a_ctx->PSGetShader(&s_ps, nullptr, nullptr);
		a_ctx->VSGetConstantBuffers(9, 1, &s_vscb9);
		a_ctx->PSGetConstantBuffers(9, 1, &s_pscb9);
		a_ctx->PSGetShaderResources(0, 3, s_srv);
		a_ctx->PSGetSamplers(0, 1, &s_samp0);
		a_ctx->OMGetRenderTargets(1, &s_rtv0, &s_dsv);
		a_ctx->OMGetBlendState(&s_bs, s_bf, &s_sm);
		a_ctx->OMGetDepthStencilState(&s_dss, &s_stref);
		a_ctx->RSGetState(&s_rs);
		a_ctx->RSGetViewports(&s_nvp, s_vp);
		a_ctx->RSGetScissorRects(&s_nsr, s_sr);

		a_ctx->IASetInputLayout(nullptr);
		a_ctx->IASetPrimitiveTopology(::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		a_ctx->VSSetShader(g_postVS, nullptr, 0);
		a_ctx->PSSetShader(g_postPS, nullptr, 0);
		a_ctx->VSSetConstantBuffers(9, 1, &g_postCB);
		a_ctx->PSSetConstantBuffers(9, 1, &g_postCB);
		// fix20b: own states so the triangle always covers the whole target
		// with an opaque overwrite, independent of whatever the engine left
		// bound (scissor rect clipped to a local draw / alpha blend / depth).
		a_ctx->RSSetState(g_postRS);
		a_ctx->OMSetBlendState(g_postBS, nullptr, 0xFFFFFFFFu);
		a_ctx->OMSetDepthStencilState(g_postDSS, 0);
		::D3D11_RECT fullScr{ 0, 0, static_cast<LONG>(mainDesc.Width), static_cast<LONG>(mainDesc.Height) };
		a_ctx->RSSetScissorRects(1, &fullScr);  // belt + suspenders (RS has scissor OFF)
		::ID3D11RenderTargetView* rtv = g_postTempRTV;
		a_ctx->OMSetRenderTargets(1, &rtv, nullptr);
		const float clearC[4] = { 0.f, 0.f, 0.f, 1.f };
		a_ctx->ClearRenderTargetView(g_postTempRTV, clearC);
		// bind kMAIN/mainDS as SRVs only AFTER the engine's RTV/DSV bindings
		// are gone (fix21 order). Same-resource RTV+SRV is invalid in D3D11.
		// Step 2 (2026-09-07): t2 = kSHADOWMAPS depth array SRV (index 4,
		// same as P1_hooks / the engine t103) so the pass can shadow-test
		// each extended light against its rendered slice.
		auto& shadowArr = renderer->GetDepthStencilData().depthStencils[4];
		::ID3D11ShaderResourceView* srvs[3] = { mainRT.SRV, mainDS.depthSRV,
			shadowArr.depthSRV };
		a_ctx->PSSetShaderResources(0, 3, srvs);
		a_ctx->PSSetSamplers(0, 1, &g_postSamp);
		// fix20b diag: PRE = kMAIN content at our fire moment. If the engine
		// had not painted the scene into kMAIN yet, PRE reads ~all-zero and
		// the CopyResource below replaces the real scene with black - that is
		// the black-screen mechanism to look for in the log.
		static std::uint32_t s_log = 0;
		const bool doDiag = (g_postRun == 0) || ((s_log++ & 0xFFu) == 0);
		if (doDiag)
			PostReadbackCenter(dev, a_ctx, mainRT.texture, "PRE-copy kMAIN");
		::D3D11_VIEWPORT vp2{};
		vp2.Width = static_cast<float>(mainDesc.Width);
		vp2.Height = static_cast<float>(mainDesc.Height);
		vp2.MinDepth = 0.0f;
		vp2.MaxDepth = 1.0f;
		a_ctx->RSSetViewports(1, &vp2);
		a_ctx->Draw(3, 0);

		// ---- Copy result back into kMAIN texture ----
		// fix20b: CopyResource returns void (D3D11, errors surface only via
		// the debug layer) and demands EXACT resource match (format, size,
		// array/mip count). kMAIN carrying mips > 1 would silently no-op -
		// the POST-copy readback below is the observable check for that.
		a_ctx->CopyResource(mainRT.texture, g_postTempTex);

		// ---- fix25: restore every engine state we hijacked ----
		a_ctx->IASetInputLayout(s_ial);
		a_ctx->IASetPrimitiveTopology(s_topo == ::D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED ?
			::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST : s_topo);
		a_ctx->VSSetShader(s_vs, nullptr, 0);
		a_ctx->PSSetShader(s_ps, nullptr, 0);
		a_ctx->VSSetConstantBuffers(9, 1, &s_vscb9);
		a_ctx->PSSetConstantBuffers(9, 1, &s_pscb9);
		a_ctx->PSSetShaderResources(0, 3, s_srv);
		a_ctx->PSSetSamplers(0, 1, &s_samp0);
		::ID3D11RenderTargetView* rtvR = s_rtv0;
		a_ctx->OMSetRenderTargets(1, &rtvR, s_dsv);
		a_ctx->OMSetBlendState(s_bs, s_bf, s_sm);
		a_ctx->OMSetDepthStencilState(s_dss, s_stref);
		a_ctx->RSSetState(s_rs);
		if (s_nvp)
			a_ctx->RSSetViewports(s_nvp, s_vp);
		if (s_nsr)
			a_ctx->RSSetScissorRects(s_nsr, s_sr);
		if (s_ial)
			s_ial->Release();
		if (s_vs)
			s_vs->Release();
		if (s_ps)
			s_ps->Release();
		if (s_vscb9)
			s_vscb9->Release();
		if (s_pscb9)
			s_pscb9->Release();
		if (s_srv[0])
			s_srv[0]->Release();
		if (s_srv[1])
			s_srv[1]->Release();
		if (s_srv[2])
			s_srv[2]->Release();
		if (s_samp0)
			s_samp0->Release();
		if (s_rtv0)
			s_rtv0->Release();
		if (s_dsv)
			s_dsv->Release();
		if (s_bs)
			s_bs->Release();
		if (s_dss)
			s_dss->Release();
		if (s_rs)
			s_rs->Release();

		g_postRun++;
		if (doDiag) {
			// POST = what CopyResource left in kMAIN (== our temp content).
			// Black here while PRE had scene = our pass painted nothing into
			// the temp RT (depth/state issue); black in both = wrong fire
			// timing (engine scene not in kMAIN yet).
			PostReadbackCenter(dev, a_ctx, mainRT.texture, "POST-copy kMAIN");
			// fix21 diag: vpInv health + camera + first extended lights, so
			// one session tells us whether lamp data is world-space sane
			// (positions near the player, radius > 0) and the inverse
			// view-proj actually computed (skip counter stays 0).
			if (cam) {
				const auto& wt = cam->world.translate;
				const float* v0 = vpInv;  // row 0 of the inverse
				SKSE::log::info("[SLF][POST] vp: skipNoGeom={} cam=({:.1f},{:.1f},{:.1f}) vpInv[0..3]=({:.4f},{:.4f},{:.4f},{:.4f})",
					g_postSkipNoGeom, wt.x, wt.y, wt.z, v0[0], v0[1], v0[2], v0[3]);
				const std::uint32_t nd = nUp < 3 ? nUp : 3;
				for (std::uint32_t i = 0; i < nd; i++) {
					const auto& pl = ShadowLimitFixNS::P1::g_postLight[i];
					SKSE::log::info("[SLF][POST] ext[{}]: pos=({:.1f},{:.1f},{:.1f}) r={:.1f} col=({:.2f},{:.2f},{:.2f}) slice={:.0f} type={:.0f} far={:.1f}",
						i, pl.pos[0], pl.pos[1], pl.pos[2], pl.radius,
						pl.color[0], pl.color[1], pl.color[2], pl.slice, pl.lightType, pl.farDist);
				}
				// fix26c: is the t2 shadow array actually carrying depth for
				// the extended lights' slices? Empty slices -> every shadow
				// test returns lit -> dbgMode2 renders black.
				if (auto* srv4 = renderer->GetDepthStencilData().depthStencils[4].depthSRV) {
					::ID3D11Resource* sres = nullptr;
					reinterpret_cast<::ID3D11ShaderResourceView*>(srv4)->GetResource(&sres);
					if (sres) {
						auto* arrTex = reinterpret_cast<::ID3D11Texture2D*>(sres);
						for (std::uint32_t i = 0; i < nd; i++) {
							const auto& pl = ShadowLimitFixNS::P1::g_postLight[i];
							PostReadbackShadowSlice(dev, a_ctx, arrTex,
								static_cast<std::uint32_t>(pl.slice + 0.5f), "ext");
						}
						sres->Release();
					}
				}
			}
			SKSE::log::info("[SLF][POST] pass ran: ext={} run={} (kMAIN {}x{} fmt={} mips={} arr={})",
				nUp, g_postRun, mainDesc.Width, mainDesc.Height, static_cast<int>(mainDesc.Format),
				mainDesc.MipLevels, mainDesc.ArraySize);
		} else {
			// fix23 perf probe: throttle to every 64th non-diag run so a
			// slow pass shows up even between the 256-frame diag points.
			static std::uint32_t s_pp = 0;
			if (((s_pp++) & 0x3Fu) == 0) {
				const auto pp1 = pclk::now();
				const double ms = std::chrono::duration<double, std::milli>(pp1 - pp0).count();
				SKSE::log::info("[SLF][PERF] postpass cpu={:.2f}ms ext={} run={}", ms, nUp, g_postRun);
			}
		}
	}
#endif  // SLF_POSTLIGHT_ENABLED

	// Renderer::ResetState hook: rebind after every engine state reset.
		struct RendererResetStateHook
		{
			static void thunk(void* a_this)
			{
				func(a_this);
				if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
					auto& rt = renderer->GetRuntimeData();
					if (auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(rt.context)) {
						auto* dev = reinterpret_cast<ID3D11Device*>(rt.forwarder);
						if (ctx && dev)
							BindCSConstantBuffers(ctx, dev);
#if SLF_PS_ENABLED
						// v3: rebind our b13 + t103 after every engine state
						// reset (the engine clears SRV/cbuffer bindings).
						if (g_slfShadowCB)
							BindSLFResources(reinterpret_cast<::ID3D11DeviceContext*>(ctx));
						// A state reset clears every binding, so a pending
						// arm from a previous technique must not fire later
						// against a different (non-shadow) material.
						g_slfArmSwap.store(false, std::memory_order_release);
#endif
					}
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

	// ---- BeginTechnique hook: swap in cached VS+PS (matched pair) ----
	struct BeginTechniqueHook
	{
		static bool thunk(RE::BSShader* a_shader, std::uint32_t a_vertexDescriptor,
			std::uint32_t a_pixelDescriptor, bool a_skipPixelShader)
		{
			// Lazy-install the bytecode capture on the FIRST BeginTechnique:
			// at PostLoad the D3D11 device may not exist yet (forwarder null,
			// InstallBytecodeCapture silently no-oped at 10:45). By first draw
			// the device is guaranteed present.
			static std::once_flag s_once;
			std::call_once(s_once, [] { InstallBytecodeCapture(); });
			// fix19c (2026-09-04): shadow-render trace moved OUT of the
			// SLF_PS_ENABLED gate. fix19b (SLF_PS_ENABLED=0, gate closed)
			// logged "shadow render trace installed" count = 0 and
			// s_arrayIsOMDSV stayed false, so the log had NO evidence
			// whether ClearDepthStencilView / OMSetRenderTargets / Draw
			// ever target the t103 array while every fullscan (54408
			// samples, 8 slices) read back all-0.000 minD. The trace is
			// pure observation (counter + orig call, zero engine-state
			// change) and needs only the D3D context, guaranteed present
			// by first BeginTechnique - safe to arm unconditionally.
			static std::once_flag s_traceOnce;
			std::call_once(s_traceOnce, [] { ShadowLimitFixNS::P1::InstallShadowRenderTrace(); });
#if SLF_PS_ENABLED
			// Lazy-compile our own engine-semantics PS on first draw.
			static std::once_flag s_slfOnce;
			std::call_once(s_slfOnce, [] {
				if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
					if (auto* dev = reinterpret_cast<ID3D11Device*>(renderer->GetRuntimeData().forwarder)) {
						CompileSLFPS(dev);
						EnsureSLFShadowBuffer(dev);
						InstallSLFContextHooks();
					}
				}
			});
#endif

			// fix14-diag (2026-09-04): proof-of-life for the BeginTechnique
			// hook itself - fix13 log showed the ResetState hook rebinding
			// t103 (13399x) yet ZERO cb2[29] / swap activity, i.e. the
			// Lighting branch below never visibly ran. Log the first thunk
			// invocation (any shader type) to separate "hook never called"
			// from "hook called but never in a Lighting normal pass".
			static std::once_flag s_btHit;
			std::call_once(s_btHit, [] {
				SKSE::log::info("[SLF] BT diag: BeginTechnique hook first invocation");
			});

#if SLF_POSTLIGHT_ENABLED
			// fix20 trigger: the FIRST ImageSpace BeginTechnique of a scene
			// frame = all main-scene draws are done (opaque/transparent
			// forward passes write kMAIN; image-space post comes next and
			// reads kMAIN). Fire our extended-light pass right before the
			// engine starts its post chain - whatever we CopyResource into
			// the kMAIN texture is what the engine post / ENB will see.
			// The latch resets on any Lighting BT (next scene frame).
			if (a_shader) {
				static bool s_postFired = false;
				const auto stype = a_shader->shaderType.get();
				if (stype == RE::BSShader::Type::Lighting) {
					s_postFired = false;
				} else if (stype == RE::BSShader::Type::ImageSpace && !s_postFired) {
					s_postFired = true;
					if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
						if (auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context)) {
							RunPostLightPass(ctx);
						}
					}
				}
			}
#endif  // SLF_POSTLIGHT_ENABLED

#if SLF_ALWAYS_LIT
			// fix39 (2026-09-07): the engine re-zeros lodDimmer AFTER the
			// scheduler thunk (UpdateCamera runs again on shadow-render /
			// later paths), so the fix38 scheduler restore loses. Restore at
			// the LAST moment the engine reads lamp data - the Lighting
			// BeginTechnique right before func() fills cb2 - so every draw
			// sees dimmer=1 (lamps never distance-faded, no walk-up pop).
			if (a_shader && a_shader->shaderType.get() == RE::BSShader::Type::Lighting)
				ShadowLimitFixNS::P1::ForceLampDimmersOne();
#endif

			const bool result = func(a_shader, a_vertexDescriptor, a_pixelDescriptor, a_skipPixelShader);

				// SKIP shadow-map passes: binding our normal-render VS/PS
				// during the shadow pass corrupted engine state (crash:
				// movups [r8], xmm1 r8=1 at SkyrimSE.exe+14DFC01, TESObjectLAND
				// shadow pass). Check the OM-bound DSV directly instead of the
				// global depth-target type (stale at BeginTechnique time).
				if (a_shader && a_shader->shaderType.get() == RE::BSShader::Type::Lighting) {
					if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
						if (auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context)) {
							// Ultimate guard: shadow-map / Z-prepass renders are
							// depth-only (no RTV bound). Normal render passes
							// always have an RTV. This catches shadow passes
							// even if their DSV is not in our known arrays.
							::ID3D11RenderTargetView* rtvs[1] = { nullptr };
							::ID3D11DepthStencilView* dsv = nullptr;
							reinterpret_cast<::ID3D11DeviceContext*>(ctx)->OMGetRenderTargets(1, rtvs, &dsv);
							const bool depthOnly = (rtvs[0] == nullptr);
							const bool isShadow = IsShadowPass(reinterpret_cast<::ID3D11DeviceContext*>(ctx));
							if (dsv)
								dsv->Release();
							if (rtvs[0])
								rtvs[0]->Release();

							if (isShadow || depthOnly) {
								// Shadow-map / Z-prepass: never swap here.
#if SLF_PS_ENABLED
								g_slfArmSwap.store(false, std::memory_order_release);
#endif
								SLFRecordBT(a_vertexDescriptor, a_pixelDescriptor, dsv, isShadow, a_skipPixelShader, false);
								return result;
							}

							// fix14-diag: Lighting NORMAL pass reached - log
							// the swap-gate state every 64th pass. If this
							// line never appears while [MP] t14-binds flood
							// the log, the engine is NOT routing its shadow-
							// receiving draws through this hook.
							{
								static std::uint32_t s_gateLog = 0;
								if (((s_gateLog++) & 0x3Fu) == 0) {
									const auto lc = ShadowLimitFixNS::P1::g_shadowLightCount.load(std::memory_order_acquire);
									const auto sc = ShadowLimitFixNS::P1::g_scheduledShadowCount.load(std::memory_order_acquire);
									SKSE::log::info("[SLF] BT diag lighting pass: skipPS={} lightCount={} scheduled={} shouldSwap={}",
										a_skipPixelShader ? 1 : 0, lc, sc, (lc > 4) ? 1 : 0);
								}
							}

							// fix19b: canvas readback at the material-pass
							// sampling moment. The MP PSSetSRV probe is filtered
							// into silence while the shadow array stays the OM
							// DSV after our manual dispatch, so this Lighting
							// normal pass is the reliable host. Own 128-throttle
							// inside; read-only.
							DebugReadbackShadowArrayAtMaterialPass(ctx);

							// Shadow-mask readback diagnostics (throttled):
							// counts how many shadow lights are actually visible
							// on screen via the t14 mask channels. t14 is only
							// bound for shadow-receiving techniques;
							// (t14 readback detection disabled - noise + timing
							// can't catch the engine's internal shadow stage)
							// Dump the vanilla Lighting PS bytecode actually
							// bound by the engine (deduplicated).
							DumpBoundPixelShader(a_shader, reinterpret_cast<ID3D11DeviceContext*>(ctx));
							// SLF-B probe: patched-vs-vanilla of the bound PS
							// + t102/t103/b13 resource bindings at this pass.
							ProbeMaterialPassBindings(ctx);
							// Read the engine's per-surface light counts
							// (cb2[29]) to confirm the 4-shadow-light cap.
							ReadbackPerSurfaceLightCounts(reinterpret_cast<ID3D11DeviceContext*>(ctx));
#if P1C3_ENABLED
							const auto itVS = g_vsCache.find(a_vertexDescriptor);
							const auto itPS = g_psCache.find(a_pixelDescriptor);
							if (itVS != g_vsCache.end() || itPS != g_psCache.end()) {
								// CRITICAL (crash-2026-09-02-02-41-41, 6 reproductions):
								// func() already set currentVertexShader/currentPixelShader
								// to the VANILLA shaders and bound them. We must ONLY
								// override the D3D device binding with our compiled
								// shaders - do NOT touch rtd.currentVertexShader /
								// currentPixelShader and do NOT set DIRTY_* flags
								// (a dirty flag would make the engine rebind the
								// vanilla shader next frame, and writing our shader
								// into the engine state corrupted it - crashes in
								// BSLightingShader::SetupGeometry at SkyrimSE.exe+14DFC01
								// across terrain / NPC body / UI passes).
								// This matches CS: their SetVertexShader/SetPixelShader
								// thunk writes the VANILLA pointer into the engine
								// state and only the D3D call uses the custom shader.
								if (itVS != g_vsCache.end())
									ctx->VSSetShader(itVS->second, nullptr, 0);
								if (itPS != g_psCache.end() && !a_skipPixelShader)
									ctx->PSSetShader(itPS->second, nullptr, 0);
								SLFRecordBT(a_vertexDescriptor, a_pixelDescriptor, dsv, false, a_skipPixelShader, true);
							} else {
								SLFRecordBT(a_vertexDescriptor, a_pixelDescriptor, dsv, false, a_skipPixelShader, false);
							}
#elif SLF_PS_ENABLED
							// Our own engine-semantics PS: swap in ONLY for
							// passes that actually sample shadows (t14 shadow
							// mask bound). Every other Lighting material keeps
							// the vanilla PS so its features (parallax, detail,
							// envmap variants) stay intact - a single generic
							// PS replacing all techniques lost them (user:
							// "material features missing").
							//
							// 13:20: BeginTechnique runs BEFORE the engine
							// binds this material's SRVs, so a t14 check here
							// only sees the PREVIOUS draw's leftover state and
							// usually misses (swap never fired -> "shadows
							// never appear"). Two paths now:
							//   fast - t14 already bound (leftover from an
							//          earlier shadow pass draw): swap now.
							//   slow - arm the swap; the PSSetShaderResources
							//          hook fires it when the engine actually
							//          binds slot 14 during draw setup.
							// fix12 (route B): both paths only exist while the
							// gate is open (>4 scheduled shadow lights). With
							// the gate closed this block is skipped entirely:
							// no arm, no swap, no b13/t103 traffic - the
							// engine PS renders the scene 100% vanilla.
							if (!a_skipPixelShader && g_slfPS && SLFShouldSwap()) {
								auto* nctx = reinterpret_cast<::ID3D11DeviceContext*>(ctx);
								UpdateSLFShadowData(nctx);
								if (IsShadowMaskBound(nctx)) {
									g_slfArmSwap.store(false, std::memory_order_release);
									// fix14-diag: fast-path swap (t14 leftover
									// already bound at BeginTechnique time).
									{
										static std::uint32_t s_fastLog = 0;
										if (((s_fastLog++) & 0x3Fu) == 0)
											SKSE::log::info("[SLF] BT diag: FAST path - t14 bound at BeginTechnique, swapping now");
									}
									DoSLFShadowSwap(nctx);
									SLFRecordBT(a_vertexDescriptor, a_pixelDescriptor, dsv, false, a_skipPixelShader, true);
								} else {
									// fix14-diag: slow path - armed, waiting for
									// the real t14 bind through the PSSetSRV hook.
									{
										static std::uint32_t s_armLog = 0;
										if (((s_armLog++) & 0x3Fu) == 0)
											SKSE::log::info("[SLF] BT diag: SLOW path - swap armed, awaiting t14 bind");
									}
									g_slfArmSwap.store(true, std::memory_order_release);
									SLFRecordBT(a_vertexDescriptor, a_pixelDescriptor, dsv, false, a_skipPixelShader, false);
								}
							} else {
								g_slfArmSwap.store(false, std::memory_order_release);
								SLFRecordBT(a_vertexDescriptor, a_pixelDescriptor, dsv, false, a_skipPixelShader, false);
							}
#endif  // P1C3_ENABLED / SLF_PS_ENABLED
						}
					}
				}
				return result;
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallShaderHooks()
	{
		SKSE::log::info("[SLF] P1c-3 installing LoadShaders + BeginTechnique + ResetState hooks...");
		InstallBytecodeCapture();
		stl::detour_thunk<LoadShadersHook>(REL::RelocationID(101339, 108326));
		stl::detour_thunk<BeginTechniqueHook>(REL::RelocationID(101341, 108328));
		stl::detour_thunk<RendererResetStateHook>(REL::RelocationID(75570, 77371));
		SKSE::log::info("[SLF] P1c-3 shader hooks installed");
	}
}
