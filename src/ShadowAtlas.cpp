// ShadowAtlas.cpp - SLF self-owned shadow atlas (step 1: texture + tiles).
//
// A single 2D depth texture holds variable-free fixed-grid tiles, one per
// extended point light. Format mirrors the engine's kSHADOWMAPS (R16_TYPELESS
// resource, D16_UNORM DSV, R16_UNORM SRV) so a later shader patch can sample it
// with the same depth compare semantics the engine uses for t103.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <Windows.h>
#include <d3d11.h>

#include "ShadowAtlas.h"

namespace ShadowLimitFixNS
{
	namespace
	{
		constexpr std::uint32_t kAtlasDim = 4096;      // atlas texture width/height
		constexpr std::uint32_t kTileSize = 1024;      // fixed tile width/height
		constexpr std::uint32_t kGrid = kAtlasDim / kTileSize;  // 4 -> 16 tiles

		struct AtlasState
		{
			::ID3D11Texture2D* texture = nullptr;
			::ID3D11DepthStencilView* dsv = nullptr;
			::ID3D11ShaderResourceView* srv = nullptr;
			bool ready = false;
		};

		AtlasState g_atlas;

		::ID3D11Device* GetDevice()
		{
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			if (!renderer)
				return nullptr;
			return reinterpret_cast<::ID3D11Device*>(renderer->GetRuntimeData().forwarder);
		}
	}

	void InstallShadowAtlas()
	{
		if (g_atlas.ready)
			return;

		auto* dev = GetDevice();
		if (!dev) {
			SKSE::log::warn("[SLF][Atlas] InstallShadowAtlas: D3D11 device unavailable (defer)");
			return;
		}

		// Depth formats pair as (typeless resource, DSV, SRV) - R16 mirrors the
		// engine shadow map (fmt53 R16_TYPELESS) so depth compare matches t103.
		const DXGI_FORMAT texFmt = DXGI_FORMAT_R16_TYPELESS;
		const DXGI_FORMAT dsvFmt = DXGI_FORMAT_D16_UNORM;
		const DXGI_FORMAT srvFmt = DXGI_FORMAT_R16_UNORM;

		::D3D11_TEXTURE2D_DESC desc{};
		desc.Width = desc.Height = kAtlasDim;
		desc.MipLevels = desc.ArraySize = 1;
		desc.Format = texFmt;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(dev->CreateTexture2D(&desc, nullptr, &g_atlas.texture))) {
			SKSE::log::warn("[SLF][Atlas] CreateTexture2D failed ({0}x{0} {1})", kAtlasDim, static_cast<int>(texFmt));
			return;
		}

		::D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		dsvDesc.Format = dsvFmt;
		dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		if (FAILED(dev->CreateDepthStencilView(g_atlas.texture, &dsvDesc, &g_atlas.dsv))) {
			SKSE::log::warn("[SLF][Atlas] CreateDepthStencilView failed");
			return;
		}

		::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = srvFmt;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		if (FAILED(dev->CreateShaderResourceView(g_atlas.texture, &srvDesc, &g_atlas.srv))) {
			SKSE::log::warn("[SLF][Atlas] CreateShaderResourceView failed");
			return;
		}

		g_atlas.ready = true;
		SKSE::log::info("[SLF][Atlas] self-owned shadow atlas ready: {0}x{0}, {1}x{1} grid, tile {2}",
			kAtlasDim, kGrid, kTileSize);
	}

	void* ShadowAtlasSRV()
	{
		return g_atlas.ready ? g_atlas.srv : nullptr;
	}

	bool ShadowAtlasTile(std::uint32_t a_slot, AtlasTileRect& a_out)
	{
		if (!g_atlas.ready || a_slot >= kGrid * kGrid)
			return false;
		a_out.size = kTileSize;
		a_out.x = (a_slot % kGrid) * kTileSize;
		a_out.y = (a_slot / kGrid) * kTileSize;
		return true;
	}

	std::uint32_t ShadowAtlasDim()
	{
		return kAtlasDim;
	}

	std::uint32_t ShadowAtlasTileSize()
	{
		return kTileSize;
	}
}
