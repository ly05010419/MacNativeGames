// Vertex shader features: matrix packing and mul, uniform arrays indexed
// at run time, register bindings (fixed and placed), struct inputs and
// outputs, overloads, out/inout parameters, conversions, loops.
#define BONES 4

float4x4 worldViewProj : register(c0);
float4x3 bones[BONES] : register(c4);  // column_major: 3 registers each
row_major float3x4 rowMat;             // row_major: 3 registers
float4 lightDir;
float3 lightColors[3];
bool useFog;
int lightCount : register(i0);
float fogScale = 1.0;
float4 unusedButBound : register(c20);  // not read: its registers stay free
static const float weights[4] = { 0.1, 0.2, 0.3, 0.4 };
static float2 scale = float2(2, 3);
sampler2D heightMap : register(s0);

struct VsIn {
  float3 pos : POSITION;
  float3 normal : NORMAL;
  float4 weights : BLENDWEIGHT;
  float4 indices : BLENDINDICES;
  float2 uv : TEXCOORD0;
  float4 color : COLOR0;
};

struct VsOut {
  float4 pos : POSITION;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
  float3 normal : TEXCOORD1;
  float fog : FOG;
  float size : PSIZE;
};

float3 Skin(float4 p, float4 w, float4 idx) {
  float3 r = 0;
  [unroll] for (int i = 0; i < BONES; i++)
    r += mul(p, bones[idx[i]]) * w[i];
  return r;
}

half3 Shade(half3 n) { return n * 0.5h; }
float3 Shade(float3 n) { return n * 2; }

void Light(float3 n, out float3 diffuse, inout float count) {
  diffuse = 0;
  for (int k = 0; k < lightCount; ++k) {
    diffuse += saturate(dot(n, -lightDir.xyz)) * lightColors[k];
    count++;
  }
}

VsOut main(VsIn i) {
  VsOut o;
  float4 p = float4(i.pos, 1);
  float3 skinned = Skin(p, i.weights, i.indices);
  o.pos = mul(float4(skinned, 1), worldViewProj);
  float3x3 rot = (float3x3)worldViewProj;
  float3 n = normalize(mul(i.normal, rot));
  n = mul(rot, n);
  float4x4 m2 = mul(worldViewProj, transpose(worldViewProj));
  o.pos.z += m2._43 * 0.0 + m2._m32 * 0.0 + rowMat[1].x * 0.0 + rowMat._12_21.y;
  float3 d;
  float cnt = 0;
  Light(n, d, cnt);
  half3 h = Shade((half3)n);
  float3 f = Shade(n);
  o.color = i.color * float4(d + h + f, 1) * weights[lightCount % 4];
  o.uv = i.uv * scale + tex2Dlod(heightMap, float4(i.uv, 0, 0)).xy;
  o.normal = n.zyx;
  float fogv = useFog ? dot(p.xyz, lightDir.xyz) : 0;
  o.fog = fogv * fogScale;
  float4 v = { 1, 2, 3, 4 };
  int k = 3;
  k *= 1.5;
  v.xy = v.yx;
  v[k % 4] = 0.5;
  o.color += (v > 2) ? v : -v;
  o.size = lightColors[lightCount].x + length(v) + cnt;
  o.color.a = all(v) && any(v.xy) ? 1 : 0;
  float4 wide = 2;
  o.color.rgb *= wide;  // truncated to the left side's float3
  return o;
}
