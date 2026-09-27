// Standalone Dalamud-boundary writeback; no UI grading or primary conversion.
#include "../../shaders/color/pq.hlsl"

Texture2D<float4> linear_working : register(t0);

float4 main(float4 position : SV_POSITION) : SV_TARGET {
  float4 composited_working = linear_working.Load(int3(uint2(position.xy), 0));
  return float4(renodx::color::pq::EncodeSafe(composited_working.rgb, 80.f), composited_working.a);
}
