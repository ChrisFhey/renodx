// Standalone Dalamud-boundary background pass; not a game shader replacement.
#include "../../shaders/color/pq.hlsl"

Texture2D<float4> pq_snapshot : register(t0);

float4 main(float4 position : SV_POSITION) : SV_TARGET {
  float4 background_pq = pq_snapshot.Load(int3(uint2(position.xy), 0));
  return float4(renodx::color::pq::Decode(background_pq.rgb, 80.f), background_pq.a);
}
