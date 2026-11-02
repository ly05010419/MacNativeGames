// Shader model 2: colour outputs are clamped; parameters with semantics
// instead of structs; #pragma pack_matrix.
#pragma pack_matrix(row_major)
float4x4 mvp;
float4 materialColor;
sampler2D tex : register(s0);

void vs(float4 pos : POSITION, float2 uv : TEXCOORD0, out float4 opos : POSITION,
        out float4 color : COLOR0, out float2 ouv : TEXCOORD0) {
  opos = mul(pos, mvp);
  color = materialColor * 2;
  ouv = uv;
}

float4 ps(float4 color : COLOR0, float2 uv : TEXCOORD0) : COLOR {
  return tex2D(tex, uv) * color;
}
