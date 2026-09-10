#pragma once

// ---------------------------------------------------------------------------
// ShadowAtlas.h - SLF self-owned shadow atlas (CS-style, independent of the
// engine's kSHADOWMAPS 8-slice array).
//
// The engine's fixed 8-slot shadow state machine cannot walk an expanded
// slice array (crash 14CC1A2, verified fix107a), so >8-light shadows need a
// separate depth texture SLF renders point lights into, then samples in a
// patched shader. This module owns the texture + per-light tile layout.
//
// Step 1 (this file): create the atlas texture + DSV + SRV, fixed-grid tiles.
// Step 2 (later): render point lights into tiles (viewport + DSV).
// Step 3 (later): patch the material shader to sample the atlas (t104 + UV).
// ---------------------------------------------------------------------------

namespace ShadowLimitFixNS
{
	// Creates the atlas depth texture (D3D11), its DSV and SRV, and the fixed
	// tile grid. Safe to call once at Init; idempotent. Logs the result.
	void InstallShadowAtlas();

	// Returns the atlas SRV (nullptr before InstallShadowAtlas / on failure).
	// The shader patch (step 3) binds this to t104.
	void* ShadowAtlasSRV();

	// Fixed-grid tile rect (texel space) for a point-light slot. Returns false
	// if the atlas is not ready or slot >= capacity.
	struct AtlasTileRect
	{
		std::uint32_t x;
		std::uint32_t y;
		std::uint32_t size;
	};
	bool ShadowAtlasTile(std::uint32_t a_slot, AtlasTileRect& a_out);

	// Atlas dimensions / tile size (for logging and shader-UV math).
	std::uint32_t ShadowAtlasDim();
	std::uint32_t ShadowAtlasTileSize();
}
