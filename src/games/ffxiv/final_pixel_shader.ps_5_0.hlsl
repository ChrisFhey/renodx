#include "./shared.h"

SamplerState sourceSampler_s : register(s0);
Texture2D<float4> sourceTexture : register(t0);

void main(
        float4 vpos : SV_Position,
        float2 texcoord : TEXCOORD,
    out float4 output : SV_Target0)
{
    float4 color = sourceTexture.Sample(sourceSampler_s, texcoord.xy);

    if (injectedData.toneMapType == 0) {
        color = saturate(color);
    }

    // Preserve the existing gamma and UI-white scaling, changing only the output encoding.
    renodx::draw::Config config = renodx::draw::BuildConfig();
    config.swap_chain_output_preset = renodx::draw::SWAP_CHAIN_OUTPUT_PRESET_NONE;
    config.swap_chain_decoding = renodx::draw::ENCODING_GAMMA_2_2;
    config.swap_chain_decoding_color_space = renodx::color::convert::COLOR_SPACE_BT709;
    config.swap_chain_gamma_correction = renodx::draw::GAMMA_CORRECTION_NONE;
    config.swap_chain_scaling_nits = injectedData.toneMapUINits;
    config.swap_chain_clamp_color_space = renodx::color::convert::COLOR_SPACE_NONE;
    config.swap_chain_compress_color_space = renodx::color::convert::COLOR_SPACE_NONE;
    // Disable the peak limiter for the entire finite FP16 input range after decoding/scaling.
    config.swap_chain_clamp_nits = 1e20f;
    config.swap_chain_encoding_color_space = renodx::color::convert::COLOR_SPACE_BT2020;
    config.swap_chain_encoding = renodx::draw::ENCODING_PQ;
    color.rgb = renodx::draw::SwapChainPass(color.rgb, vpos.xy, config);
    color.a = saturate(color.a);

    output.rgba = color;
}
