// PostLightPass.hlsl v1 (2026-09-06) - POST-LIGHT MULTI-LAMP PASS.
// Runs AFTER the engine's main forward scene, BEFORE its image-space post.
// Consumer for the extended lamps SLF renders into the 127-slice shadow
// array but the engine never lights with (engine cb2 budget <= 7 diffuse
// with <= 4 shadowed).
//
// Pipeline: full-screen triangle over kMAIN (color t0 + depth t1).
//   depth -> world pos;  normal = screen-space difference of world pos;
//   for each EXTENDED light (b9, engine's own 4 excluded on the CPU):
//     diffuse += color * saturate(NdotL) * engine-style attenuation
//   out = in + albedoApprox * diffuseAcc * kLight  (albedoApprox = in,
//   engine forward pass already divided color by its own lighting so the
//   remaining signal is albedo-scaled - approximation, see fix20 notes).
// Engine PS untouched -> zero material regression. ENB untouched.
//
// b9 layout (CPU mirrors exactly):
//   pl[i*7+0] = (pos.xyz, radius)
//   pl[i*7+1] = (color.rgb, intensity)
//   pl[i*7+2] = (slice, lightType, flags, farDist)
//   pl[i*7+3..6] = proj: world->light-space AFFINE (row-major, v' = v * M)
//                  paraboloid keeps distance: |light-space pos| == dist to light
//   plHead.x  = light count
//   plRes     = (screenW, screenH, dbgMode, 0)   dbgMode 2 = shadow-only
//   plVpInv   = inverse(view * proj), row-major (v' = v * M)
//
// Step 2 shadow test (2026-09-07): CS LLF GetOmnidirectionalShadow math,
// ported to our row-major convention (mul(v, M)). depth =
// saturate(length(lightSpace) / farDist); slice is the kSHADOWMAPS array
// slice this light rendered into (t2). lit = mapDepth > receiverDepth.

cbuffer PostLight : register(b9)
{
	float4 pl[224];     // up to 32 extended lights x 7 float4
	float4 plHead;      // x = extended light count
	float4 plRes;       // x = screenW, y = screenH, z = dbgMode
	float4x4 plVpInv;   // inverse view-proj (row-major)
};

Texture2D<float4> ColorIn : register(t0);
Texture2D<float>  DepthIn : register(t1);
// kSHADOWMAPS depth array (engine 127-slice shadow maps). Step 2.
Texture2DArray<float> ShadowDepthArray : register(t2);
SamplerState SampPoint : register(s0);

struct VSOut
{
	float4 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
	VSOut o;
	// Full-screen triangle: 0=( -1,-1) 1=(3,-1) 2=(-1,3)
	// fix22 (2026-09-06): vid=1 previously became (3,3) not (3,-1) - the
	// triangle degenerated onto the y>x half of clip space (hypotenuse on
	// the y=x diagonal through screen center), so rasterization painted
	// only ONE diagonal half of the temp RT and CopyResource overwrote the
	// other half with clear black. That is the persistent POST 128/256
	// diagonal split / on-screen black wedge (2222.jpg), present in every
	// single pass run. Correct triangle covers the whole clip square.
	float2 c = float2((vid == 1) ? 3.0 : -1.0,
	                  (vid == 2) ? 3.0 : -1.0);
	o.pos = float4(c, 0.0, 1.0);
	o.uv  = c * 0.5 + 0.5;
	return o;
}

float3 WorldFromDepth(float2 px)
{
	float d = DepthIn.Load(int3(int2(px), 0)).r;
	if (d <= 0.0 || d >= 1.0)
		return float3(0, 0, 0);  // cleared / far - caller tests length
	float2 ndc = px / plRes.xy * 2.0 - 1.0;
	float4 clip = float4(ndc, d, 1.0);
	float4 w = mul(clip, plVpInv);
	// fix20b: guard the divide. A wrong/zero inverse VP makes w.w ~ 0 ->
	// a huge/NaN world coordinate that then NaNs every light's attenuation
	// and corrupts the whole temp RT (black/diagonal garbage after the
	// CopyResource overwrites kMAIN). Return a zero marker instead so the
	// caller falls back to the untouched engine color.
	float ww = max(abs(w.w), 1e-6);
	float3 p = w.xyz / ww;
	if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z))
		return float3(0, 0, 0);
	return p;
}

// ---- Step 2 (2026-09-07): paraboloid shadow test (CS GetOmnidirectional-
// Shadow math, row-major port). Inputs are the light's packed b9 fields.
// Returns 1.0 = lit, 0.0 = shadowed. No PCF yet (hard 1-tap; PCF 8x later).
float ShadowTestParaboloid(float3 world, float4 meta, float4x4 projM)
{
	const float sliceF = meta.x;
	const float lightType = meta.y;   // 1 = hemi, 2 = omni
	const float farD = max(meta.w, 0.001);

	float4 positionLS = mul(float4(world, 1.0), projM);
	const bool lowerHalf = positionLS.z < 0.0;
	// Hemi renders only the +Z paraboloid; behind the light has no shadow
	// data -> fully lit (CS: attenuation handles the falloff there).
	if (lightType < 1.5 && lowerHalf)
		return 1.0;

	positionLS.xyz /= max(abs(positionLS.w), 1e-6);

	float3 posOffset = lowerHalf ? float3(0, 0, -1) : float3(0, 0, 1);
	float3 lightDirection = normalize(normalize(positionLS.xyz) + posOffset);
	float2 sampleUV = lightDirection.xy / max(abs(lightDirection.z), 1e-6) * 0.5 + 0.5;

	// Omni packs front/back paraboloids stacked in one slice: upper half in
	// y in [0, 0.5], lower half in y in [0.5, 1]. Hemi fills the whole slice.
	if (lightType > 1.5)
		sampleUV.y = lowerHalf ? 1.0 - 0.5 * sampleUV.y : 0.5 * sampleUV.y;

	if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0)
		return 1.0;  // outside the paraboloid footprint -> no shadow data

	float depth = saturate(length(positionLS.xyz) / farD);
	depth -= 0.002;  // acne bias (vanilla ~0.002 clip-space equivalent)

	const uint slice = (uint)sliceF;
	if (slice >= 127)
		return 1.0;
	// Load (not Sample): Sample inside a varying [loop] forces an fxc
	// gradient-instruction unroll (X3570) - 32 unrolled lamps. Load has no
	// implicit gradient, keeps the loop rolled. Point sampling matches the
	// pass's point sampler anyway.
	uint tw, th, ts;
	ShadowDepthArray.GetDimensions(tw, th, ts);
	float2 texel = sampleUV * float2((float)tw, (float)th);
	uint2 tc = uint2(min((uint)texel.x, tw - 1u), min((uint)texel.y, th - 1u));
	float mapDepth = ShadowDepthArray.Load(int4(tc, slice, 0)).r;
	if (mapDepth <= 0.0 || mapDepth >= 1.0)
		return 1.0;  // cleared region -> assume lit
	return (mapDepth > depth) ? 1.0 : 0.0;
}

float4 PSMain(VSOut i) : SV_Target
{
	const int count = (int)plHead.x;
	const float2 px = i.pos.xy;

	// ---- Reconstruct world pos + normal from depth ----
	float3 world = WorldFromDepth(px);
	float4 outCol = ColorIn.Load(int3(int2(px), 0));
	if (length(world) < 1e-4 || count <= 0)
		return outCol;  // no geometry or no extended lights

	float3 pL = WorldFromDepth(px - float2(1, 0));
	float3 pR = WorldFromDepth(px + float2(1, 0));
	float3 pU = WorldFromDepth(px - float2(0, 1));
	float3 pD = WorldFromDepth(px + float2(0, 1));
	float3 N = float3(0, 0, 1);
	float lenL = length(pL), lenR = length(pR), lenU = length(pU), lenD = length(pD);
	if (lenL > 1e-4 && lenR > 1e-4 && lenU > 1e-4 && lenD > 1e-4) {
		float3 dx = pR - pL;
		float3 dy = pD - pU;  // v grows downward on screen
		N = normalize(cross(dy, dx));  // handedness: flip below if lighting looks inverted
		if (N.z < 0.0)                 // keep the normal pointing toward the camera
			N = -N;
	}

	// ---- Extended-light diffuse accumulation (+ Step 2 shadow test) ----
	float3 acc = 0;
	float3 shdOnly = 0;  // dbgMode 2: pure shadow visualization
	int hits = 0;
	// fix33 (2026-09-07): "lamps lit at any distance, no pop when walking
	// into their radius". The engine lamp radius is a hard edge - walking
	// across it the fill goes 0 -> full, which gain only made more obvious
	// (kLight 1.0 experiment, reverted). Instead EXTEND the fill range and
	// keep the engine-style curve smooth: sample attenuation out to
	// kFillReach * radius so coverage is continuous across the room and the
	// boundary is soft. No engine data touched (pure consumer-side).
	const float kFillReach = 1.7f;  // fill extends 70% beyond the lamp radius
	for (int i = 0; i < count; i++) {
		float4 pr = pl[i * 7 + 0];
		float4 pm = pl[i * 7 + 2];
		float3 lpos = pr.xyz;
		float radius = max(pr.w, 0.001);
		float3 ldir = lpos - world;
		float dist = length(ldir);
		float fillR = radius * kFillReach;
		if (dist > fillR || dist < 1e-4)
			continue;
		float3 ldirN = ldir / dist;
		float t = dist / fillR;
		float atten = saturate(1.0 - t * t);   // engine-style curve on the extended range
		if (atten <= 0.001)
			continue;
		float3 col = pl[i * 7 + 1].rgb;
		float ndl = saturate(dot(N, ldirN));
		if (ndl <= 0.001)
			continue;

		// Step 2: real shadow against the t103 slice this light rendered
		// into (flags=1 means the CPU carried a valid transform).
		float shadow = 1.0;
		if (pm.z > 0.5) {
			float4x4 projM = float4x4(pl[i * 7 + 3], pl[i * 7 + 4],
				pl[i * 7 + 5], pl[i * 7 + 6]);
			shadow = ShadowTestParaboloid(world, pm, projM);
		}

		acc += col * (ndl * atten) * shadow;
		if (shadow < 0.5)
			shdOnly += col * ndl * atten;  // how much light the shadow killed
		hits++;
	}

	// ---- Compose ----
	// fix35 (2026-09-07): pop-in masking experiment. Data: engine diffuse
	// activation (activeLights budget ~6, lamps activate when approached) is
	// the pop source; CS kills it by replacing the engine PS, we cannot. The
	// only mask available to a post pass = make OUR fill strong enough that
	// the engine's up-close activation add has small relative headroom.
	// fix33 already softens OUR radius boundary (kFillReach 1.7). Now raise
	// gain toward engine-level. Overbright double-lit zones expected - tune
	// down if unacceptable.
	const float kLight = 0.9f;  // tuning: extended lamps relight the albedo approx
	// fix20b: never let a NaN/inf accumulation corrupt the frame.
	if (!isfinite(acc.x) || !isfinite(acc.y) || !isfinite(acc.z) || any(acc < 0.0))
		acc = 0.0;
	if (plRes.z > 0.5f && plRes.z < 1.5f) {
		// dbgMode 1: pure extended-light contribution (shape/range check)
		if (hits > 0)
			return float4(saturate(acc * 3.0), 1.0);
		return float4(0, 0, 0, 1);
	}
	if (plRes.z > 1.5f) {
		// dbgMode 2: pure shadow visualization - red where the extended
		// lamps' shadows fell (the light the shadow test killed). Black =
		// fully lit (no occlusion); validates shape/alignment per light.
		if (hits > 0)
			return float4(saturate(shdOnly * 3.0), 1.0);
		return float4(0, 0, 0, 1);
	}
	outCol.rgb += outCol.rgb * acc * kLight;
	return outCol;
}
