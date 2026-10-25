// dirk's host cube: a textured, vertex-coloured cube.
cbuffer Frame : register(b0) {
  float4x4 mvp;
  float4   tint;
};

struct VsIn {
  float3 pos   : POSITION;
  float3 color : COLOR;
  float2 uv    : TEXCOORD;
};

struct VsOut {
  float4 pos   : SV_POSITION;
  float3 color : COLOR;
  float2 uv    : TEXCOORD;
};

VsOut vs_main(VsIn v) {
  VsOut o;
  o.pos = mul(mvp, float4(v.pos, 1.0));
  o.color = v.color;
  o.uv = v.uv;
  return o;
}

Texture2D    tex  : register(t0);
SamplerState samp : register(s0);

float4 ps_main(VsOut v) : SV_TARGET {
  return float4(v.color * tex.Sample(samp, v.uv).rgb, 1.0) * tint;
}
