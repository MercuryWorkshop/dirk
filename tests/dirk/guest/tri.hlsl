// The guest test's triangle: a position per vertex, a colour from a
// constant buffer the test writes through Map(WRITE_DISCARD).
cbuffer Colour : register(b0) { float4 colour; };

float4 vs_main(float2 pos : POSITION) : SV_Position {
  return float4(pos, 0.0, 1.0);
}

float4 ps_main(float4 pos : SV_Position) : SV_Target {
  return colour;
}
