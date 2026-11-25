// SkinFlow gravity pass.
// Replays the game's skinned skin draw: the game's Lighting vertex shader (SKINNED) runs as
// usual, this geometry shader moves each triangle to its UV position and the pixel shader writes
//   xy = downhill direction in UV space (length = sine of the slope, 0 on flat ground)
//   z  = outward normal .z (+1 facing up, -1 facing down: where drops fall off)
//   w  = 1 where the mesh covers the texel (0 where the normal map was missing)
// and adds each covered texel's world units per UV unit (fixed point) and a count to the
// surface's metrics buffer (u1), from which SkinFlow.dll reads the surface's mean scale.
//
// The surface normal is the one the skin is lit with: the game's normal map (t1, still bound
// from the material setup) turned to world space by the VS's frame, exactly as Lighting.hlsl
// does for SKINNED (worldNormal = mul(tbn, map * 2 - 1), the map read as .xzy when it is model
// space: SkinFlow.dll compiles PSMain with MODELSPACENORMALS for those). Skin uses model-space maps, and
// for those the VS frame is only the bone rotation (no vertex normal exists), so the map is the
// one source of a smooth normal. It is read at the mip that matches the surface size.
// The UV direction of that slope comes from the triangle's own UV-to-world mapping.
//
// The input struct is the start of Community Shaders' Lighting.hlsl VS_OUTPUT for SKINNED
// (non-EYE): same elements in the same order up to WorldPosition. The rest of the VS output is
// left unread, which D3D11 allows for trailing elements.

struct VS_OUTPUT
{
	float4 Position : SV_POSITION0;
	float2 TexCoord0 : TEXCOORD0;
	float3 TBN0 : TEXCOORD1;
	float3 TBN1 : TEXCOORD2;
	float3 TBN2 : TEXCOORD3;
	float4 WorldPosition : POSITION1;
};

struct GS_OUTPUT
{
	float4 Position : SV_POSITION;
	float2 TexCoord : TEXCOORD0;
	float3 TBN0 : TEXCOORD1;
	float3 TBN1 : TEXCOORD2;
	float3 TBN2 : TEXCOORD3;
	// World-space gradients of u and v on this triangle: a world displacement d moves the
	// texture coordinate by (dot(d, GradU), dot(d, GradV)).
	nointerpolation float3 GradU : TEXCOORD4;
	nointerpolation float3 GradV : TEXCOORD5;
	nointerpolation float UvScale : TEXCOORD6;  // world units per UV unit (square root of the area ratio)
};

#ifndef UV_SCALE_FIXED
#	define UV_SCALE_FIXED 4  // set by SkinFlow.dll (GravityPass::kUvScaleFixed)
#endif

RWByteAddressBuffer Metrics : register(u1);  // uint sum of UvScale * UV_SCALE_FIXED, uint texel count

Texture2D<float4> TexNormal : register(t1);
SamplerState SampNormal : register(s0);  // Lighting.hlsl samples the normal map with s0 too

[maxvertexcount(3)] void GSMain(triangle VS_OUTPUT input[3], inout TriangleStream<GS_OUTPUT> output)
{
	const float3 p0 = input[0].WorldPosition.xyz;
	const float3 e1 = input[1].WorldPosition.xyz - p0;
	const float3 e2 = input[2].WorldPosition.xyz - p0;
	const float2 t0 = input[0].TexCoord0;
	const float2 d1 = input[1].TexCoord0 - t0;
	const float2 d2 = input[2].TexCoord0 - t0;
	const float uvDet = d1.x * d2.y - d1.y * d2.x;
	if (abs(uvDet) < 1e-12)
		return;

	// World-space change per unit of u and of v, then their dual (gradient) vectors.
	const float3 dPdu = (e1 * d2.y - e2 * d1.y) / uvDet;
	const float3 dPdv = (e2 * d1.x - e1 * d2.x) / uvDet;
	const float uu = dot(dPdu, dPdu), uv = dot(dPdu, dPdv), vv = dot(dPdv, dPdv);
	const float gram = uu * vv - uv * uv;
	if (gram < 1e-20)
		return;

	GS_OUTPUT o;
	o.GradU = (vv * dPdu - uv * dPdv) / gram;
	o.GradV = (uu * dPdv - uv * dPdu) / gram;
	o.UvScale = sqrt(length(cross(dPdu, dPdv)));
	[unroll] for (int i = 0; i < 3; ++i)
	{
		const float2 t = input[i].TexCoord0;
		o.Position = float4(t.x * 2 - 1, 1 - t.y * 2, 0, 1);
		o.TexCoord = t;
		o.TBN0 = input[i].TBN0;
		o.TBN1 = input[i].TBN1;
		o.TBN2 = input[i].TBN2;
		output.Append(o);
	}
}

float4 PSMain(GS_OUTPUT input) : SV_Target
{
	// Drawn in UV space, so the hardware picks the mip that matches the surface size.
	const float3 texel = TexNormal.Sample(SampNormal, input.TexCoord).xyz;
#ifdef MODELSPACENORMALS
	const float3 mapped = texel.xzy * 2 - 1;  // model-space maps store x, z, y (as Lighting.hlsl reads them)
#else
	const float3 mapped = texel * 2 - 1;
#endif
	if (dot(mapped, mapped) < 0.25)
		return 0;  // no normal map bound: leave the texel uncovered rather than guess

	Metrics.InterlockedAdd(0, uint(input.UvScale * UV_SCALE_FIXED + 0.5));
	Metrics.InterlockedAdd(4, 1u);

	const float3x3 tbn = float3x3(input.TBN0, input.TBN1, input.TBN2);
	const float3 N = normalize(mul(tbn, mapped));
	const float3 g = float3(0, 0, -1);
	const float3 gSurface = g - N * dot(g, N);  // length = sine of the slope

	float2 dir = float2(dot(gSurface, input.GradU), dot(gSurface, input.GradV));
	const float len = length(dir);
	dir = len > 1e-8 ? dir / len * length(gSurface) : 0;
	return float4(dir, N.z, 1);
}
