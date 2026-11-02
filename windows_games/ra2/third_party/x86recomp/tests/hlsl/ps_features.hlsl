// Pixel shader features: sampler kinds (typed, untyped, arrays), sampling
// variants, clip/discard, VPOS/VFACE, multiple render targets and depth
// through out parameters, bool registers, intrinsics with D3D9 semantics.

sampler2D diffuse : register(s0);
samplerCUBE envMap : register(s1);
sampler3D volume;
sampler untyped;  // 2D from its use
sampler2D lightmaps[2] : register(s4);
float4 tint;
float4x4 texMat;
bool alphaTest : register(b3);
float alphaRef;

struct PsIn {
  float2 uv : TEXCOORD0;
  float3 normal : TEXCOORD1;
  float4 proj : TEXCOORD2;
  float4 color : COLOR0;
  float fog : FOG;
  float2 vpos : VPOS;
  float face : VFACE;
};

void main(PsIn i, out float4 c0 : COLOR0, out float4 c1 : COLOR1, out float depth : DEPTH) {
  float4 base = tex2D(diffuse, i.uv) * tint;
  float4 env = texCUBE(envMap, reflect(normalize(i.normal), float3(0, 0, 1)));
  float4 vol = tex3D(volume, float3(i.uv, 0.5));
  float4 u = tex2D(untyped, i.uv.yx);
  float4 lm = tex2Dproj(lightmaps[1], i.proj) + tex2Dbias(lightmaps[0], float4(i.uv, 0, -1));
  float4 g = tex2D(diffuse, i.uv, ddx(i.uv), ddy(i.uv));
  float4 tc = mul(float4(i.uv, 0, 1), texMat);
  clip(base.a - 0.01);
  if (alphaTest && base.a < alphaRef)
    discard;
  float s, co;
  sincos(i.fog, s, co);
  float4 l = lit(dot(i.normal, float3(0, 1, 0)), 0.5, 16);
  float3 m = fmod(i.normal, 0.3) + i.normal % 0.25;
  float p = pow(base.r, 2.2) + log(base.g) + sqrt(base.b) + rsqrt(base.a + 1);
  c0 = lerp(base, env, 0.25) * vol * u + lm + g * tc + l * (s + co) + float4(m, p);
  c0.rgb *= saturate(i.color.rgb) * (i.face > 0 ? 1 : 0.5);
  c1 = float4(i.vpos / 1024, isfinite(p) ? 1 : 0, 1);
  depth = frac(i.proj.z);
}
