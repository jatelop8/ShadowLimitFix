// Lighting_SLF.hlsl v2 - faithful re-implementation of the vanilla engine
// Lighting PS, disassembled from shader_dump/_asm/PS0000000282088570_0.asm.
//
// v1 bug (grayscale): input semantics were GUESSED and mismatched the engine
// VS output. DX11 matches registers by SEMANTIC NAME, so every input after
// v1 landed in the wrong slot (worldPos -> tangent, fog -> worldPos, etc),
// killing all lighting -> flat gray. v2 maps each register EXACTLY:
//   v0 SV_POSITION, v1 uv, v2 worldPos, v3-5 TBN, v6 normalWS,
//   v7-9 TBN2 (o2 encoding), v10 worldPos2, v11 prevWorldPos,
//   v12 vertexColor, v13 fog.
//
// Output: o0 = color, o1 = motion vector (vanilla RT0/RT1). We deliberately
// skip the vanilla o2 normal buffer (RT2, engine SSAO): ENB SSAO rebuilds
// normals from depth, so it does not depend on RT2 - and outputting fewer
// targets than bound RTs is legal in D3D11 (extra RTs keep old content),
// which keeps us safe across passes that bind only 1-2 RTs.

cbuffer CB0 : register(b0)
{
	float4 cb0[3];  // [0]=fog params (w=dist factor), [1]=tonemap (x/z), [2]=screen UV xform
};

cbuffer CB1 : register(b1)
{
	float4 cb1[9];  // [2].x/y=env/reflect strength, [4].w=spec power, [4].xyz=spec color, [8]=extra light
};

cbuffer CB2 : register(b2)
{
	float4 cb2[30]; // [0]=sun dir, [1]=sun color, [2]=per-light shadow channel, [3].x/y/z=env/spec/alpha,
					// [4].yzw=ambient const, [7]=o2 params, [11..13]=SH env, [15+i]=light pos/radius,
					// [22+i]=light color, [29].x=total lights, [29].y=shadow lights
};

cbuffer CB12 : register(b12)
{
	float4 cb12[45];  // [12..15]=viewproj, [16..19]=prev viewproj, [42].y/z=tonemap,
					  // [43].xy=screen size, [44].xy=1/res, [44].z=width
};

Texture2D<float4> TexColor : register(t0);
Texture2D<float4> TexNormal : register(t1);
TextureCube<float4> TexEnv : register(t4);
Texture2D<float4> TexDetail : register(t5);
Texture2D<float4> TexShadowMask : register(t14);

SamplerState SampColor : register(s0);
SamplerState SampNormal : register(s1);
SamplerState SampEnv : register(s4);     // vanilla: cubemap t4 sampled with s4
SamplerState SampDetail : register(s5);  // vanilla: detail t5 sampled with s5
SamplerState SampShadow : register(s14);

// ---- SLF N-light shadow sampling (v3) ----
// Depth array of the engine's shadow maps (kSHADOWMAPS, 127 slices) - the
// SRV lives at Renderer::GetDepthStencilData().depthStencils[4].depthSRV and
// is bound by the plugin at BeginTechnique. Slices >= 8 live in our extended
// arrays (P1b). We sample it directly, bypassing the vanilla t14 4-channel
// screen-space mask, so N > 4 shadow lights become visible.
Texture2DArray<float> ShadowDepthArray : register(t103);

// Our own per-frame data (plugin-fillable): 8 lights x 7 float4.
//   slfLight[i][0..3] = light view-proj (row-major, v' = v * vp)
//   slfLight[i][4]    = (pos.x, pos.y, pos.z, radius)
//   slfLight[i][5]    = (shadowMapIndex, flags, 0, 0)   flags 1 = valid
//   slfLight[i][6]    = pad
//   slfHeader.x       = light count
//   slfHeader.y       = shadow map texel size (e.g. 2048)
cbuffer SLFShadowData : register(b13)
{
	float4 slfLight[8][7];
	float4 slfHeader;
};

float ShadowTest(uint lightIdx, float3 worldPos)
{
	float4x4 vp = float4x4(
		slfLight[lightIdx][0], slfLight[lightIdx][1],
		slfLight[lightIdx][2], slfLight[lightIdx][3]);
	float4 clip = mul(float4(worldPos, 1.0), vp);
	if (clip.w <= 0.0)
		return 1.0;
	float2 uv = clip.xy / clip.w * 0.5 + 0.5;
	float depth = clip.z / clip.w;
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
		return 1.0;
	uint slice = (uint)slfLight[lightIdx][5].x;
	float texel = slfHeader.y > 1.0 ? slfHeader.y : 2048.0;
	uint2 texcoord = uint2(uv * texel);
	float shadowDepth = ShadowDepthArray.Load(int4(texcoord, slice, 0)).r;
	// Bias against shadow acne (vanilla uses ~0.002 in clip space).
	return (depth < shadowDepth + 0.002) ? 1.0 : 0.0;
}

struct PSInput
{
	float4 pos : SV_POSITION;        // v0
	float2 uv : TEXCOORD0;           // v1
	float3 worldPos : TEXCOORD1;     // v2
	float3 tangent : TEXCOORD2;      // v3
	float3 binormal : TEXCOORD3;     // v4
	float3 normalTS : TEXCOORD4;     // v5
	float3 normalWS : TEXCOORD5;     // v6
	float3 tangent2 : TEXCOORD6;     // v7
	float3 binormal2 : TEXCOORD7;    // v8
	float3 normal2 : TEXCOORD8;      // v9
	float4 worldPos2 : TEXCOORD9;    // v10
	float4 prevWorldPos : TEXCOORD10; // v11
	float4 color : COLOR0;           // v12
	float4 fog : TEXCOORD11;         // v13
};

struct PSOutput
{
	float4 color : SV_Target0;
	float4 motion : SV_Target1;
};

PSOutput main(PSInput input)
{
	PSOutput o;

	// ---- Albedo + normal map (sample t0/t1, unpack normal) ----
	float4 albedo = TexColor.Sample(SampColor, input.uv);
	float4 nm = TexNormal.Sample(SampNormal, input.uv);
	float3 normalT = nm.xyz * 2.0 - 1.0;

	// ---- TBN transform to world normal (asm: dp3 v3/v4/v5 with normalT) ----
	float3 N = normalize(
		normalT.x * input.tangent +
		normalT.y * input.binormal +
		normalT.z * input.normalTS);

	// ---- viewDir = normalized world normal (vanilla convention) ----
	float3 viewDir = normalize(input.normalWS);

	// ---- Light counts (asm: min(cb2[29].y,4) shadow, min(cb2[29].x,7) total) ----
	int shadowLightCount = (int)min(cb2[29].y, 4.0);
	int totalLightCount = (int)min(cb2[29].x, 7.0);

	// ---- Shadow mask from t14 (vanilla 4-channel screen-space mask) ----
	float4 shadowMask = float4(1, 1, 1, 1);
	if (shadowLightCount > 0) {
		float2 suv = input.pos.xy * cb12[44].xy;
		suv = suv * cb0[2].xy + cb0[2].zw;
		suv = suv * cb12[43].xy;
		suv = max(suv, 0);
		suv.x = min(suv.x, cb12[44].z);
		suv.y = min(suv.y, cb12[43].y);
		shadowMask = TexShadowMask.Sample(SampShadow, suv);
	}

	// ---- Directional light (cb2[0]=dir, cb2[1]=color) ----
	float ndl = saturate(dot(N, cb2[0].xyz));
	float3 dirDiffuse = ndl * cb2[1].xyz;
	float3 halfVec = normalize(viewDir + cb2[0].xyz);
	float ndh = saturate(dot(halfVec, N));
	float dirSpec = pow(ndh, cb1[4].w);
	float3 dirSpecular = dirSpec * cb2[1].xyz;

	// ---- Point lights (loop over cb2[29].x clamped to 7) ----
	float3 pointDiffuse = 0;
	float3 pointSpecular = 0;
	for (int i = 0; i < totalLightCount; i++) {
		// SLF N-light shadow: direct depth-array sampling when our data is
		// valid (flags=1); falls back to the vanilla t14 channel extraction
		// for engine-baked lights. fix12 (route B): the OLD outer
		// "i < shadowLightCount" (<=4) gate meant material lights beyond the
		// vanilla t14 budget NEVER got a shadow test at all - the extended
		// slices 8+ were rendered but unread. Now ANY material light with a
		// valid SLF entry (scheduled slice + camera matrix) is depth-tested
		// regardless of the vanilla 4-channel cap. Vanilla output is bit-
		// identical when no SLF data is present (slfHeader.x == 0 -> the
		// else-if below reproduces the exact channel extraction).
		float lightShadow = 1.0;
		if ((uint)i < (uint)slfHeader.x && slfLight[i][5].y > 0.5) {
			lightShadow = ShadowTest((uint)i, input.worldPos);
		} else if (i < shadowLightCount) {
			// Vanilla: channel extraction of t14 (asm: dp4 r5.xyzw,
			// icb[cb2[2][i]] -> R/G/B/A channel).
			int channel = (int)cb2[2][i];
			lightShadow = shadowMask[channel];
		}
		// Light data: cb2[15+i] = pos + radius, cb2[22+i] = color
		float3 lpos = cb2[15 + i].xyz;
		float lradius = cb2[15 + i].w;
		float3 ldir = lpos - input.worldPos;
		float dist = length(ldir);
		ldir = dist > 0.000001 ? ldir / dist : float3(0, 0, 1);
		float atten = saturate(dist / lradius);
		atten = 1.0 - atten * atten;
		float3 lcolor = cb2[22 + i].xyz * lightShadow * atten;
		float lndl = saturate(dot(N, ldir));
		pointDiffuse += lndl * lcolor;
		float3 lhalf = normalize(viewDir + ldir);
		float lndh = saturate(dot(lhalf, N));
		float lspec = pow(lndh, cb1[4].w);
		pointSpecular += lspec * lcolor;
	}

	// ---- Env reflection ----
	// reflectFactor = cb1[2].y * (detail.r - nm.w) + nm.w; then * cb1[2].x * cb2[3].x
	float detailR = TexDetail.Sample(SampDetail, input.uv).r;
	float reflectFactor = cb1[2].y * (detailR - nm.w) + nm.w;
	reflectFactor *= cb1[2].x * cb2[3].x;
	float3 R = reflect(-viewDir, N);
	float3 envColor = TexEnv.Sample(SampEnv, R).xyz * reflectFactor;

	// ---- Total light: SH ambient (cb2[11..13] . (N,1)) + const ambient + diffuse + extra ----
	float4 N1 = float4(N, 1.0);
	float3 shAmbient = float3(
		dot(cb2[11], N1),
		dot(cb2[12], N1),
		dot(cb2[13], N1));
	float3 totalLight = shAmbient + cb2[4].yzw + dirDiffuse + pointDiffuse + cb1[8].yzw * cb1[8].x;

	// ---- Combine (vanilla order) ----
	float3 diffuseColor = albedo.xyz * totalLight;
	envColor *= totalLight;
	float3 specColor = nm.a * (dirSpecular + pointSpecular) * cb2[3].y;
	float3 col = diffuseColor * input.color.xyz + envColor;

	// ---- Fog + tonemap pass 1 (asm: min(col, fogged*gamma + cb0[1].x)) ----
	float3 f = lerp(col, input.fog.xyz, input.fog.w);
	f = lerp(col, f, cb0[0].w);
	col = min(col, f * cb12[42].y + cb0[1].x);

	// ---- Specular added AFTER pass-1 clamp (asm: mad r0, r3, cb1[4], r0) ----
	col += specColor * cb1[4].xyz;

	// ---- Fog + tonemap pass 2 (asm order) ----
	f = lerp(col, input.fog.xyz, input.fog.w);
	f = lerp(col, f, cb0[0].w);
	float3 fg = f * cb12[42].y;
	col = min(col, f * cb12[42].y + cb0[1].z);

	// ---- Output ----
	o.color.rgb = col - fg * cb12[42].z;
	o.color.a = albedo.a * cb2[3].z * input.color.w;

	// ---- Motion vector (o1): current clip pos - previous clip pos ----
	float2 cur = float2(dot(cb12[12], input.worldPos2), dot(cb12[13], input.worldPos2));
	float curW = dot(cb12[15], input.worldPos2);
	float2 prev = float2(dot(cb12[16], input.prevWorldPos), dot(cb12[17], input.prevWorldPos));
	float prevW = dot(cb12[19], input.prevWorldPos);
	float2 mv = cur / max(abs(curW), 1e-6) - prev / max(abs(prevW), 1e-6);
	mv *= float2(-0.5, 0.5);
	o.motion.xy = (cb2[7].z > 1e-5) ? float2(1, 0) : mv;
	o.motion.zw = float2(0, 1);

	return o;
}
