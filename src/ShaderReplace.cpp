// ShaderReplace.cpp - engine render-path observation + shadow-engine
// stabilizer hooks:
//   - CreatePixelShader vtable[15] bytecode capture (vanilla-semantics study)
//   - BSShader::LoadShaders / BeginTechnique hooks (shadow-pass detection,
//     per-material diagnostics, lamp-dimmer restore)
//   - SLF-B data-channel resource set (per-light payload cbuffers, cb2
//     readbacks for the 4-shadow-light cap evidence)
// Standalone plugin: engine-behavior references are REL-ID facts used for
// verification only; no runtime dependency on any other mod.
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
		constexpr float kBias = 0.00025f;  // upstream default (shadowBiasScale x 0.00025)
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
		(void)a_bytecode;
		(void)a_len;
		(void)a_linkage;
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
		SKSE::log::info("[SLF-B] B2B splice DISABLED (A/B isolation control) - engine PS bytecode passes through untouched");
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
		std::error_code ec;
		std::filesystem::path dir;
		if (auto ld = SKSE::log::log_directory(); ld)
			dir = *ld / "shader_dump";
		else
			dir = std::filesystem::path("shader_dump");  // CWD fallback (MO2: virtual)
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
		// (an upstream package's shader directory) - second base not needed for now.
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

		// Compiled PS cache (per pixel descriptor) for BeginTechnique.

		// Engine depth-target index global (type at addr; 4 = shadow maps).

		// Visible shadow count - THE definitive check: read back the engine's
		// shadow depth array (kSHADOWMAPS) and count how many slices actually
		// contain geometry. slice N has content = the N-th shadow map was
		// rendered = the N-th light casts a visible shadow in our SLF PS.
		// CHEAP version (fixed the 0.4s/268MB stall): copy only a 256x256
		// center block per slice (~1MB total) and throttle to every 8192nd
		// pass (~6s) - imperceptible.

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
