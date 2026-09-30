// Standalone Dalamud direct-PQ PS; not an original shader fingerprint.
// ID is CRC32("FFXIV Dalamud diagnostic direct PQ").
#include "../../shaders/color/srgb.hlsl"
#include "../../shaders/color/rgb.hlsl"
#include "../../shaders/color/pq.hlsl"

Texture2D<float4> ui_texture : register(t0);
SamplerState ui_sampler : register(s0);
cbuffer UIWhite : register(b0) {
  float ui_nits;
};

float4 main(float4 position : SV_POSITION, float4 color : COLOR0, float2 uv : TEXCOORD0) : SV_TARGET {
  // Match the supported SDR modulation contract. Keep straight coverage alpha;
  // the original blend state intentionally blends the encoded RGB in PQ space.
  float4 ui = color * ui_texture.Sample(ui_sampler, uv);
  ui.rgb = renodx::color::bt2020::from::BT709(renodx::color::srgb::DecodeSafe(ui.rgb));
  ui.rgb = renodx::color::pq::EncodeSafe(ui.rgb, ui_nits);
  return ui;
}
