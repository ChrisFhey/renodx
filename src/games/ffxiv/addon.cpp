/*
 * Copyright (C) 2024 Ersh
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#define DEBUG_LEVEL_0

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include <embed/shaders.h>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/settings.hpp"
#include "./dalamud_overlay.hpp"
#include "./shared.h"

ShaderInjectData shader_injection;

static bool track_next_copy = false;

bool saturate_on_replace(reshade::api::command_list* cmd_list) {
  track_next_copy = true;
  return true;
}

bool copy_on_replace(reshade::api::command_list* cmd_list) {
  if (track_next_copy) {
    ++shader_injection.copyTracker;
  }
  return true;
}

namespace {

renodx::mods::shader::CustomShaders custom_shaders = {
    CustomShaderEntry(0x27EBC404),
    CustomShaderEntry(0x1F264D17), //new LUT permutation, added with 7.2
    CustomShaderEntryCallback(0xF8F57F0A, &saturate_on_replace),
    CustomShaderEntry(0x85E777EF),
    CustomShaderEntry(0xFE5B6B3E),  // BloomPass1 vs
    // CustomShaderEntry(0xBF06786C), // BloomPass1 ps
    CustomShaderEntry(0x9B242D09),  // BloomPass2 vs
    // CustomShaderEntry(0x5E42F039), // BloomPass2 ps
    CustomShaderEntry(0xCDC56365),                            // Vignette
    CustomShaderEntryCallback(0x6CFFD968, &copy_on_replace),  // Copy
    CustomShaderEntryCallback(0xB0CE42B9, &copy_on_replace),  // Copy
    CustomShaderEntry(0xF6E81A1B)};                           // FullscreenGammaCorrection

renodx::utils::settings::Settings settings = {
    new renodx::utils::settings::Setting{
        .key = "toneMapType",
        .binding = &shader_injection.toneMapType,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 2.f,
        .can_reset = true,
        .label = "Tone Mapper",
        .section = "Tone Mapping",
        .tooltip = "Sets the tone mapper type",
        .labels = {"Vanilla", "None", "DICE"},
        .parse = [](float value) { return std::fmin(std::fmax(value, 0.f), 2.f); },
    },
    new renodx::utils::settings::Setting{
        .key = "toneMapPeakNits",
        .binding = &shader_injection.toneMapPeakNits,
        .default_value = 1000.f,
        .can_reset = false,
        .label = "Peak Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of peak white in nits",
        .min = 48.f,
        .max = 4000.f,
        .is_enabled = []() { return shader_injection.toneMapType > 0; },
    },
    new renodx::utils::settings::Setting{
        .key = "toneMapGameNits",
        .binding = &shader_injection.toneMapGameNits,
        .default_value = 203.f,
        .label = "Game Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of 100% white in nits",
        .min = 48.f,
        .max = 500.f,
    },
    new renodx::utils::settings::Setting{
        .key = "toneMapUINits",
        .binding = &shader_injection.toneMapUINits,
        .default_value = 203.f,
        .label = "UI Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the brightness of UI and HUD elements in nits",
        .min = 48.f,
        .max = 500.f,
    },
    new renodx::utils::settings::Setting{
        .key = "diceShoulderStart",
        .binding = &shader_injection.diceShoulderStart,
        .default_value = 0.33f,
        .label = "DICE Shoulder Start",
        .section = "Tone Mapping",
        .tooltip = "Determines where the highlights curve (shoulder) starts in the DICE tonemapper.",
        .max = 1.f,
        .format = "%.2f",
        .is_enabled = []() { return shader_injection.toneMapType == 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "colorGradeExposure",
        .binding = &shader_injection.colorGradeExposure,
        .default_value = 1.f,
        .label = "Exposure",
        .section = "Color Grading",
        .max = 10.f,
        .format = "%.2f",
        .is_enabled = []() { return shader_injection.toneMapType > 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "colorGradeHighlights",
        .binding = &shader_injection.colorGradeHighlights,
        .default_value = 55.f,
        .label = "Highlights",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.toneMapType > 1; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "colorGradeShadows",
        .binding = &shader_injection.colorGradeShadows,
        .default_value = 50.f,
        .label = "Shadows",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.toneMapType > 1; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "colorGradeContrast",
        .binding = &shader_injection.colorGradeContrast,
        .default_value = 55.f,
        .label = "Contrast",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.toneMapType > 1; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "colorGradeSaturation",
        .binding = &shader_injection.colorGradeSaturation,
        .default_value = 55.f,
        .label = "Saturation",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.toneMapType > 1; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "colorGradeBlowout",
        .binding = &shader_injection.colorGradeBlowout,
        .default_value = 55.f,
        .label = "Blowout",
        .section = "Color Grading",
        .tooltip = "Controls highlight desaturation due to overexposure.",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.toneMapType > 1; },
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "hueCorrectionStrength",
        .binding = &shader_injection.hueCorrectionStrength,
        .default_value = 50.f,
        .label = "Hue Correction",
        .section = "Color Grading",
        .tooltip = "Controls the strength of the hue correction.",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.toneMapType > 1; },
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "gamutExpansion",
        .binding = &shader_injection.gamutExpansion,
        .default_value = 20.f,
        .label = "Gamut Expansion",
        .section = "Color Grading",
        .tooltip = "Generates HDR colors (BT.2020) from bright saturated SDR (BT.709) ones.",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.toneMapType > 1; },
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "blackFloorOffset",
        .binding = &shader_injection.blackFloorOffset,
        .default_value = -0.02f,
        .label = "Black Floor Offset",
        .section = "Miscellaneous",
        .tooltip = "An offset to apply to the black floor value in the LUT shader. \nThe final value will not go negative so you can set this to the minimum value to ensure the black floor is never raised.\nUnfortunately, the parameter is often used to raise the overall brightness in darker scenes, so you might end up darkening the game too much at times.\nThe game often uses around 0.02 to slightly raise the brightness, so something like -0.02 is a rather safe conservative value to use.",
        .min = -0.1f,
        .max = 0.f,
        .format = "%.3f",
    },
    new renodx::utils::settings::Setting{
        .key = "bloomRadiusMult",
        .binding = &shader_injection.bloomRadiusMult,
        .default_value = 1.f,
        .label = "Bloom Radius Multiplier",
        .section = "Miscellaneous",
        .tooltip = "Controls the radius of the game's bloom.",
        .max = 1.f,
        .format = "%.2f",
    },
    new renodx::utils::settings::Setting{
        .key = "vignetteStrength",
        .binding = &shader_injection.vignetteStrength,
        .default_value = 100.f,
        .label = "Vignette Strength",
        .section = "Miscellaneous",
        .tooltip = "Controls the vignette strength, if enabled in the game options.",
        .max = 100.f,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::TEXT,
        .label = "RenoDX by ShortFuse. FFXIV mod by Ersh with a lot of help from Pumbo.",
        .section = "About",
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "HDR Den Discord",
        .section = "About",
        .group = "button-line-1",
        .tint = 0x5865F2,
        .on_change = []() {
          renodx::utils::platform::LaunchURL("https://discord.gg/Z7kXxw5VDR");
        },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Ersh's Ko-Fi",
        .section = "About",
        .group = "button-line-1",
        .tint = 0xFF5F5F,
        .on_change = []() {
          renodx::utils::platform::LaunchURL("https://ko-fi.com/ershin");
        },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Pumbo's Ko-Fi",
        .section = "About",
        .group = "button-line-1",
        .tint = 0xFF5F5F,
        .on_change = []() {
          renodx::utils::platform::LaunchURL("https://buymeacoffee.com/realfiloppi");
        },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "ShortFuse's Ko-Fi",
        .section = "About",
        .group = "button-line-1",
        .tint = 0xFF5F5F,
        .on_change = []() {
          renodx::utils::platform::LaunchURL("https://ko-fi.com/shortfuse");
        },
    },
};

void OnPresetOff() {
  renodx::utils::settings::UpdateSetting("toneMapType", 0.f);
  renodx::utils::settings::UpdateSetting("toneMapPeakNits", 203.f);
  renodx::utils::settings::UpdateSetting("toneMapGameNits", 203.f);
  renodx::utils::settings::UpdateSetting("toneMapUINits", 203.f);
  renodx::utils::settings::UpdateSetting("colorGradeExposure", 1.f);
  renodx::utils::settings::UpdateSetting("colorGradeHighlights", 50.f);
  renodx::utils::settings::UpdateSetting("colorGradeShadows", 50.f);
  renodx::utils::settings::UpdateSetting("colorGradeContrast", 50.f);
  renodx::utils::settings::UpdateSetting("colorGradeSaturation", 50.f);
  renodx::utils::settings::UpdateSetting("colorGradeBlowout", 0.f);
  renodx::utils::settings::UpdateSetting("hueCorrectionStrength", 0.f);
  renodx::utils::settings::UpdateSetting("gamutExpansion", 0.f);
  renodx::utils::settings::UpdateSetting("blackFloorOffset", 0.f);
  renodx::utils::settings::UpdateSetting("vignetteStrength", 100.f);
  renodx::utils::settings::UpdateSetting("bloomRadiusMult", 1.f);
}

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for Final Fantasy XIV Online";

void OnPresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain, const reshade::api::rect* source_rect, const reshade::api::rect* dest_rect, uint32_t dirty_rect_count, const reshade::api::rect* dirty_rects) {
  // Registered after the shared proxy pass so this resets tracking for the next frame.
  track_next_copy = false;
  shader_injection.copyTracker = 0;
  renodx::games::ffxiv::dalamud_overlay::OnPresent(swapchain);
}

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH: {
      if (!reshade::register_addon(h_module)) {
        return FALSE;
      }

      // Keep game rendering in FP16 and encode only the final presentation as HDR10.
      renodx::mods::swapchain::SetUseHDR10();
      renodx::mods::swapchain::use_resource_cloning = true;
      renodx::mods::swapchain::swapchain_proxy_compatibility_mode = false;
      renodx::mods::swapchain::swap_chain_proxy_format = reshade::api::format::r16g16b16a16_float;
      renodx::mods::swapchain::swap_chain_proxy_vertex_shader = __final_vertex_shader;
      renodx::mods::swapchain::swap_chain_proxy_pixel_shader = __final_pixel_shader;
      renodx::mods::swapchain::expected_constant_buffer_index = 13;

      renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
      renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
      renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

      renodx::mods::swapchain::swap_chain_upgrade_targets.push_back({
          .old_format = reshade::api::format::r8g8b8a8_unorm,
          .new_format = reshade::api::format::r16g16b16a16_float,
      });

      //Only needed if Gamma Correction is active?????? (GammaCorrection != 50)
      renodx::mods::swapchain::swap_chain_upgrade_targets.push_back({
          .old_format = reshade::api::format::b8g8r8a8_unorm,
          .new_format = reshade::api::format::r16g16b16a16_float,
      });

      reshade::register_event<reshade::addon_event::present>(OnPresent);
      renodx::games::ffxiv::dalamud_overlay::Use(fdw_reason);

      break;
    }
    case DLL_PROCESS_DETACH:
      renodx::games::ffxiv::dalamud_overlay::Use(fdw_reason);
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);

      renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);
      renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
      renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);

      reshade::unregister_addon(h_module);
      break;
    default:
      break;
  }

  return TRUE;
}
