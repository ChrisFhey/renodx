# Dalamud Stage 3: direct PQ feasibility

2026-09-27; `feature/hdr10`, working tree based on `3856f13e`.
Investigation only: this report is the only change. No source/configuration
changes, build, deployment, or new runtime experiment.

**Recommendation: test direct PQ as a simpler compatibility path for verified
ordinary SDR ImGui draws.** It should fix their extreme brightness and incorrect
primaries, and removes the full-frame reconstruction/writeback involved in the
reported flicker. It sacrifices correct transparency; acceptable font and panel
appearance must be demonstrated. Do not yet replace Stage 3 unconditionally or
claim that changing the shader resolves the outstanding draw-coverage question.

## ReShade is a practical precedent

The inspected ReShade submodule is
`4a50d1eddace85734871d91792ff214f13f66c01`; exact correspondence to the installed
6.8.0.2155 binary remains unproven. Its
[HDR helper](external/reshade/res/shaders/imgui_hdr.hlsl) converts vertex RGB
using gamma 2.2 decoding, a BT.709-to-BT.2020 matrix, brightness/10000 scaling,
then PQ encoding. Its [pixel shader](external/reshade/res/shaders/imgui_ps_4_0.hlsl)
subsequently multiplies that encoded vertex color by the sampled texture.
[GUI setup](external/reshade/source/runtime_gui.cpp), lines 4762-4770 and
4982-4995, supplies brightness/color space and ordinary RGB
`SRC_ALPHA / INV_SRC_ALPHA / ADD` blending. There is no linear composition
intermediate in this GUI path. ReShade therefore demonstrates this design,
not mathematically correct HDR transparency.

Copy the overall approach, **not its texture-modulation order**. Converting
vertex RGB first works well for white-RGB font coverage textures, but multiplying
PQ values by colored SDR image samples misrepresents icons. Dalamud's existing
[replacement shader](src/games/ffxiv/0xE95DEEC4.ps_5_0.hlsl) already provides the
better starting contract: form the original vertex-times-texture result, then
decode its RGB once. Retain exact sRGB decoding rather than introducing gamma
2.2 merely to match ReShade.

## Proposed signal and scope

For the currently supported SDR UNORM source contract:

```text
ui = interpolated vertex color * sampled texture
RGB = PQ_encode(BT709_to_BT2020(sRGB_decode(ui.rgb)) * UI_white_nits / 10000)
A   = ui.a
```

Here PQ_encode accepts linear values normalized to 10,000 nits. With RenoDX's
scale-parameter helper, use the equivalent nits scale exactly once. Use existing
`toneMapUINits` (default 203), not display peak brightness or ReShade's independent
overlay-brightness setting. White then outputs PQ approximately 0.580689 at
203 nits, rather than raw 1.0, which signals 10,000 nits. BT.709 primaries are
correctly represented in BT.2020. This is SDR UI encoding, not scene inverse
tonemapping; the game's existing proxy/output path stays unchanged.

Alpha remains straight coverage: never sRGB/PQ-decode it or premultiply RGB
before the existing source-alpha blend. The likely B4G4R4A4 font atlas samples
as white RGB plus coverage and fits this contract; its four-bit source alpha
precision remains unchanged. RGB10A2's two stored alpha bits do not reduce the
pixel shader's source-alpha blend factor to four levels.

Keep verified shader, HDR10 target, source-domain and blend checks. UNORM alone
does not prove a texture is SDR: a copied PQ background, custom blur, HDR image,
sRGB-decoding view or detached SDR viewport needs separate treatment. A global
shader substitution based only on the PS fingerprint would be unsafe.

## Expected visual tradeoffs

RGB10A2_UNORM does not decode PQ for blending. Hardware mixes encoded numbers:
`PQ_result = alpha * PQ_source + (1-alpha) * PQ_background`, unlike Stage 3's
linear-light source-over followed by encoding. The special automatic linear
blending rule applies to sRGB render targets, not PQ presentation metadata.
[Microsoft output-merger documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage).

Calculated using ST2084, before display mapping/quantization, at alpha 0.5:

| Neutral source / background | Direct PQ result | Linear Stage 3 result |
|---|---:|---:|
| 203-nit white / black | 8.87 nits | 101.50 nits |
| 203-nit white / 20-nit gray | 67.27 nits | 111.50 nits |
| Black panel / 203-nit background | 8.87 nits | 101.50 nits |
| 203-nit white / 1000-nit highlight | 454.31 nits | 601.50 nits |

- **Opaque fills/icons:** sane luminance and colors under the supported input
  contract; essentially the same intended output as successful linear Stage 3.
- **Fonts/AA:** opaque glyph interiors are correct; fractional coverage becomes
  much darker against black. Small text can look thinner, harsher or uneven.
  This is not simply familiar SDR blending: gamma-domain sRGB blending of
  half-white over black would give about 43.45 nits at 203-nit white. No single
  alpha adjustment fixes all backgrounds and colors.
- **Panels, fades and shadows:** dark translucent panels obscure HDR scenery
  more strongly; fades and overlapping layers have altered brightness curves.
  Errors grow with source/background contrast. Per-channel PQ interpolation
  can also shift hue/saturation in transparent colored edges.
- **Backgrounds:** pixels outside submitted UI geometry stay untouched. Scene
  highlights behind translucent UI are not composited correctly; copied/blurred
  scene textures cannot be treated as ordinary SDR icons. The shader cannot
  correctly blend against an unknown destination without access to that image.

Thus ordinary predominantly opaque UI may be quite usable, as ReShade's design
suggests, but readability and transparency quality are not established by source
inspection. Stable inputs do not inherently flicker because of PQ blending;
moving backgrounds can change edge appearance.

## Simplification versus current Stage 3

Current [overlay code](src/games/ffxiv/dalamud_overlay.hpp), `DrawLinearImGui`
and `OverlayCallback`, uses
[BackgroundRoundTrip](src/games/ffxiv/dalamud_roundtrip.hpp): snapshot/decode the
post-effect presentation, duplicate validated draws into linear BT.2020 FP16,
also execute all original native PQ draws, then conditionally replace full-frame
RGB at End. Correct linear blending requires that reconstruction to be complete.

A direct path would substitute the PQ PS/constants **around the single original
native draw**, restore those bindings, and leave its RTV/blend/geometry intact.
It needs no snapshot, FP16 working texture, duplicate UI submission, fullscreen
decode/encode, context-state swap or End commit. It is not achieved by merely
adding PQ encoding to the existing FP16 shader: that would encode twice. Nor
can it retain an extra uncorrected draw afterward, which would overwrite or
double-blend the correction. Native interception remains necessary because
Dalamud bypasses ReShade's tracked context.

This removes stale/incomplete full-frame writeback and whole-callback
commit/fallback switching. It does **not** correct missed draws or prevent later
native writers. Direct per-draw correction also loses transactional rollback:
a later unsupported command cannot undo an earlier corrected draw. Copying the
current sticky rejection policy unchanged could leave mixed corrected/native UI.
Blur/custom-work policy needs explicit review before general adoption.

The [End investigation](FFXIV_DALAMUD_STAGE3_END_WRITEBACK_INVESTIGATION.md)
records modes 2/4/5 stable and mode 3 flickering; this supports testing removal
of writeback, not proof of a PQ-math bug. The later
[draw investigation](FFXIV_DALAMUD_STAGE3_DRAW_INVESTIGATION.md) found two
one-draw commits and 118 empty callbacks, not demonstrated repeated rejection.
The new bounded `STAGE3_DRAW_SCOPE` instrumentation is implemented, but no new
capture result was supplied here. A direct path with identical interception
coverage might still correct only that one incidental draw.

## Smallest next step

After authorization, add an opt-in direct-PQ comparison for verified ordinary
draws while retaining existing modes and scope counts. First test a continuously
visible plain window, blur off: opaque white/colors, colored icons, small fonts,
50%-transparent panels and fades over dark and bright scenes, then FG off/on.
Confirm relevant draws are covered and forwarded exactly once; compare against
mode 1 and current mode 3, also outside the logging window. Accept the simpler
path only if brightness, readability and stability are adequate. Keep linear
composition as the correctness option if the transparency compromise fails.

Reviewed root/game instructions, resource-analysis guidance, current source/diff,
Stage 3 notes, color/B4G4R4A4/render-path/End/draw reports and diagnostic docs.
Calculations above are analytical, not measured game/display results.
