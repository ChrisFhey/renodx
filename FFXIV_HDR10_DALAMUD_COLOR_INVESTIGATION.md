# FFXIV HDR10: Dalamud overlay color and composition investigation

Date: 2026-09-26  
Branch and source revision: `feature/hdr10`, `f75a147de9c4488a8774c0a08e4dbddc74060797`  
Scope: investigation and implementation recommendation only. No fix, source edit, configuration edit, build, deployment, or commit was performed.

## 1. Conclusion and confidence

**Dalamud mode 3 fixes the draw ordering, not the color pipeline.** Its ordinary ImGui shader writes SDR RGB values into the already encoded RGB10A2/PQ backbuffer. Those values are interpreted as absolute-luminance PQ values with BT.2020 primaries. Ordinary D3D11 alpha blending then mixes these incompatible values with the existing PQ image. This explains visible but overbright, incorrectly colored UI.

**A linear FP16 composition stage is the technically correct solution.** Encoding UI colors to PQ in a replacement pixel shader would improve opaque colors, but would leave transparency mathematically wrong. Redirecting unmodified Dalamud into FFXIV's existing FP16 proxy would avoid PQ blending, but that proxy contains gamma-encoded RGB, not linear light; it also changes the position of the overlay relative to ReShade effects.

The recommended implementation is a narrowly scoped, late, linear FP16 composition stage around Dalamud's mode-3 callback, preserving the existing HDR10 scene presentation path. Initialize its working image from the post-effect PQ backbuffer, render color-converted Dalamud into it, then encode the result back to PQ before ReShade renders its own GUI and before OptiScaler's downstream presentation work.

This can reside entirely in the FFXIV addon and optional shared RenoDX infrastructure **without rebuilding or editing Dalamud**, but it requires new native interception/integration. Existing RenoDX shader replacement and resource-cloning callbacks alone cannot intercept Dalamud's peeled native D3D11 device/context. There is no existing one-line setting that supplies this compositor.

### Evidence limits

- The local configuration now selects `ReShadeHandlingModeV2 = 3`, `SwapChainHookMode = 0`.
- The inspected Dalamud source is the installed release, **15.0.3.5**.
- The local ReShade source is submodule commit `4a50d1eddace85734871d91792ff214f13f66c01`. The earlier runtime log identified ReShade `6.8.0.2155`; exact binary/source correspondence was not established.
- The saved OptiScaler log identifies `10.0.0-dev (f45ccf3a)` and reports `UI Mode: PQ`. That commit was not available through the queried upstream GitHub endpoint. The inspected, pinned upstream source is `44cfee4d436857742a9bf71bbe81396ec9989715`. Its explicit PQ compatibility implementation is consistent with the reported darker overlay, but is not proof of the installed DLL's exact instructions.
- No failing-frame GPU capture, actual pixel readback, or new runtime experiment was performed. Statements about the exact loaded OptiScaler shader, display luminance, and callback identity must therefore remain qualified.
- The meanings of “architecture A” and “architecture B” were not supplied in the available conversation. Clarification was requested. This report explicitly defines its comparison below; the labels must not be mistaken for confirmed definitions from an earlier discussion.

## 2. Presentation history and the visibility regression

| Revision | Presentation behavior | Dalamud implication |
|---|---|---|
| Original `9b212eda` | Game renders into real FP16 backbuffer; FFXIV `OnPresent` copies it and converts the copy to scRGB | Direct Dalamud writes before Present are included |
| Stage 1 `a9fc687b` | Shared `SwapchainProxyPass`, compatibility mode enabled, real and proxy formats both FP16 | Still copies the real buffer before final conversion |
| Stage 2 `f75a147d` | RGB10A2/ST2084 output; compatibility mode disabled; intercepted game rendering redirected into an FP16 clone | Final proxy draw overwrites direct writes to the real backbuffer |

The original disappearance was caused by **resource separation and ordering**, not by PQ encoding alone. In the previously selected mode 4, Dalamud's detour rendered before calling the original ReShade `on_present`. Its renderer peeled the ReShade swapchain and used the underlying native device, context, and backbuffer. The UI bypassed RenoDX's clone redirection and was overwritten when RenoDX presented the clone.

Mode 3 moves Dalamud to `reshade_overlay`, after RenoDX's Present conversion and ReShade effects. The pixels survive, exposing the separate color problem.

Relevant source:

- [FFXIV addon](src/games/ffxiv/addon.cpp), `DllMain`, around line 305.
- [Shared swapchain module](src/mods/swapchain_v2.hpp), `UsingSwapchainCompatibilityMode`, `SetUseHDR10`, `OnInitSwapchain`, `OnPresent`.
- [Proxy rendering](src/utils/draw.hpp), `SwapchainProxyPass::Render`.
- [Dalamud mode-4 detour](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/Dalamud/Interface/Internal/InterfaceManager.AsHook.cs#L77).
- [Dalamud mode-3 callback](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/Dalamud/Interface/Internal/InterfaceManager.AsReShadeAddon.cs#L49).

## 3. Why mode 3 produces incorrect colors

### 3.1 The shader has no HDR output conversion

Dalamud's ordinary fragment shader computes:

```hlsl
float4 out_col = input.col * FontTexture.Sample(FontSampler, input.uv);
return out_col;
```

It has no SDR-reference-white parameter, BT.709-to-BT.2020 conversion, or PQ encoder. Changing the callback mode does not change this shader.

This is not inferred solely from a generic ImGui example: the release renderer's embedded `imgui-frag.hlsl.bytes` and the release testbed's corresponding shader bytecode were fetched and compared in memory. Both were 740 bytes and had SHA256:

```text
B6E7F2FEB00CC787D3F039DFD8DD02720B59F7497ED936B4FD4B85D16E39834A
```

Sources: [shader source](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/imgui/StandaloneImGuiTestbed/Shaders/HLSL/imgui-frag.hlsl), [renderer shader creation](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/Dalamud/Interface/ImGuiBackend/Renderers/Dx11Renderer.cs#L869).

### 3.2 RGB10A2 does not perform PQ conversion automatically

An `R10G10B10A2_UNORM` RTV stores normalized RGB numbers. Setting the DXGI color space tells the presentation/display path how to interpret them; it does not make the D3D11 render target automatically encode SDR shader output as PQ.

For an opaque neutral pixel, writing SDR white `1.0` therefore writes PQ code value `1.0`, which denotes 10,000 nits. It does not denote `toneMapUINits`.

The following values were calculated using the ST2084 constants also present in RenoDX's [PQ helper](src/shaders/color/pq.hlsl):

| Raw value written to the PQ buffer | Nominal luminance represented by PQ |
|---|---:|
| 0.25 | 5.15 nits |
| 0.50 | 92.25 nits |
| 0.75 | 983.38 nits |
| 1.00 | 10,000 nits |

Correct PQ code values for neutral white are approximately **0.580689 at 203 nits** and **0.602559 at 250 nits**. Actual monitor output can differ because of display mapping and clipping; these are signal values, not measurements of the monitor.

The mismatch is nonlinear. It can make bright text and highlights excessive while dark controls remain dark. A single multiplier applied to already encoded PQ values cannot fix the entire UI ramp.

### 3.3 The primaries are also wrong

Dalamud's ordinary SDR colors are interpreted using BT.2020 primaries when written directly into the HDR10 target. To preserve their intended BT.709/sRGB colors, conversion must happen in linear light:

```text
SDR RGB -> SDR transfer decode -> linear BT.709
       -> reference-white scale -> linear BT.2020 -> PQ encode
```

The display sees RGB numbers, not their originating renderer. There is no automatic indication that one rectangle in a PQ backbuffer contains unconverted SDR UI.

### 3.4 Its alpha blending operates on PQ code values

Dalamud creates an ordinary blend state with RGB factors `SRC_ALPHA` and `INV_SRC_ALPHA`. Its alpha factors are `INV_DEST_ALPHA` and `ONE`, which produce the usual union alpha:

```text
Aout = As * (1 - Ad) + Ad = As + Ad * (1 - As)
```

The RGB result on the current target is:

```text
Pout = As * SDR_UI_RGB + (1 - As) * PQ_background
```

The two RGB terms are not even in the same transfer function or primaries. This is more than an incorrect brightness constant.

Even a shader that correctly converts the UI to PQ would still produce:

```text
Pout = As * PQ(UI_linear) + (1 - As) * PQ(background_linear)
```

That is not linear-light compositing. For example, 50%-alpha 203-nit white over black should produce 101.5 nits. Averaging its PQ code value with black produces only about **8.87 nits**. This is a concrete reason not to consider a PQ-only shader replacement a complete fix.

Source: [Dalamud blend-state creation](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/Dalamud/Interface/ImGuiBackend/Renderers/Dx11Renderer.cs#L932).

## 4. Why ReShade and OptiScaler behave differently

### 4.1 ReShade: explicit HDR vertex-color conversion, still nonlinear blending

ReShade has its own ImGui shaders and constants. Its [ImGui pixel shader](external/reshade/res/shaders/imgui_ps_4_0.hlsl) selects a color conversion based on `color_space`. Its [HDR helper](external/reshade/res/shaders/imgui_hdr.hlsl) performs the PQ path as:

```text
vertex RGB ^ 2.2
-> BT.709-to-BT.2020 matrix
-> multiply by hdr_overlay_brightness / 10000
-> PQ inverse EOTF
```

The local `ReShade.ini` contains `HdrOverlayBrightness=250.000000` and `HdrOverlayOverwriteColorSpaceTo=0` (automatic). This is a separate brightness control from RenoDX's `toneMapUINits`.

ReShade's runtime receives color-space updates through RenoDX's `utils::swapchain::ChangeColorSpace()` and `OnInitEffectRuntime()`. Its GUI pushes the selected runtime/override color space and brightness into its shader. The loaded constants should still be checked in a capture; the configuration alone does not prove the live value.

Two limitations explain why its UI is not a correctness reference:

1. ReShade still enables standard source-alpha blending on the PQ render target. Transparent windows, shadows, and antialiased glyph edges therefore blend nonlinearly and can look dark.
2. In the inspected shader it converts **vertex RGB before multiplying by sampled texture RGB**. White-RGB font textures primarily modulate alpha and fit this approach better than arbitrary colored image textures. Multiplying an encoded PQ color by an SDR texture sample is not general color-correct texture modulation.

Thus ReShade's ordinary white UI colors receive intentional luminance mapping, unlike Dalamud's raw `1.0`, but its transparency is not fully linear-light correct.

Sources: [GUI pipeline and constants](external/reshade/source/runtime_gui.cpp), around lines 4762 and 4982; [runtime color-space propagation](src/utils/swapchain.hpp), around lines 484 and 628; [runtime update](external/reshade/source/runtime_api.cpp), `runtime::set_color_space`.

### 4.2 OptiScaler: deliberate PQ compatibility compression

The saved log reports `MenuHdrCheck ... UI Mode: PQ` and a D3D12 presentation overlay path. In the inspected upstream implementation, `getUiTargetMode()` classifies the output transfer function and primaries. `MenuHdrCheck()` saves the SDR ImGui style and applies `toneMapColor()` to its style colors.

Crucially, that source's PQ branch does **not** call its available `linearToPQ()` function. It deliberately calls `legacyHdrToneMap()` because standard ImGui alpha blending on a PQ target is problematic. With its constants, the operation is:

```text
peak = max(r, g, b)
RGBout = RGBin / (1 + peak)
alpha unchanged
```

White `1.0` becomes `0.5`, corresponding to roughly 92 nits if presented as neutral PQ. That naturally looks much darker than Dalamud's unmodified white `1.0` and is not a calibrated 203-nit or 250-nit output. Style-color compression also does not establish correct handling of every image texture or custom ImGui color.

This is a compatibility approximation, not a linear HDR compositor. The source explicitly anticipates a future linear intermediate/composite solution. Its output differs from both Dalamud's unconverted SDR and ReShade's explicit HDR vertex-color transform.

Source: [OptiScaler menu color handling](https://github.com/optiscaler/OptiScaler/blob/44cfee4d436857742a9bf71bbe81396ec9989715/OptiScaler/menu/menu_common.cpp#L966), including `legacyHdrToneMap`, `toneMapColor`, and `MenuHdrCheck`.

**Binary qualification:** the exact logged `f45ccf3a` build could not be matched to retrievable upstream source. The above is the exact inspected source behavior and a strong explanation consistent with the log and symptom, not a claim that the installed binary was disassembled or proven identical.

### 4.3 Different drawing positions also matter

The normal mode-3 sequence is:

```text
FFXIV scene/native UI in RenoDX gamma-space FP16 proxy
-> RenoDX Present: encode proxy into RGB10A2/PQ
-> ReShade effects
-> Dalamud mode-3 callback, currently unconverted SDR drawing
-> ReShade's own GUI, using its own HDR shader
-> underlying Present / OptiScaler interop and FG path
-> OptiScaler's overlay at its presentation boundary
```

ReShade's `reshade_overlay` event runs before its own ImGui draw submission; it is not the same event as `reshade_present`, which occurs later. OptiScaler's exact nesting depends on FG and wrapper configuration, but its overlay can be drawn in the downstream D3D12 presentation path. A RenoDX compositor placed around Dalamud must finish before that handoff.

Sources: [ReShade GUI ordering](external/reshade/source/runtime_gui.cpp), around lines 1431 and 1500; [ReShade runtime](external/reshade/source/runtime.cpp); [OptiScaler DX11/DX12 presentation](https://github.com/optiscaler/OptiScaler/blob/44cfee4d436857742a9bf71bbe81396ec9989715/OptiScaler/with_dx12/dx11_with_dx12_sc.cpp#L358).

## 5. Architecture A versus architecture B

The labels below are explicit assumptions because their original definitions were unavailable. Both avenues were mentioned descriptively in the previous investigation.

### Architecture A: route Dalamud into the existing pre-PQ FP16 proxy

This means rendering Dalamud before the existing final conversion and making its native render-target writes reach RenoDX's game proxy.

Advantages:

- Can reproduce the old FFXIV gamma-space overlay composition relatively closely.
- Uses the existing final brightness scaling and output shader.
- Potentially avoids an additional full-image PQ decode/encode pair.

Problems:

- **FP16 is a storage format, not a transfer function.** FFXIV's current proxy is gamma-encoded. Rendering unmodified ImGui into it still blends in gamma space.
- Mode 3 currently happens after the proxy was encoded and after ReShade effects. Redirecting it into that already-consumed proxy would restore the invisibility bug unless scheduling also changes.
- Delaying the original proxy conversion until after mode 3 would leave ReShade effects with the wrong image or overwrite their results. A second conversion of the old proxy can erase effects and overlays.
- Moving Dalamud earlier changes whether ReShade effects process it. This changes established mode-3 semantics and background blur inputs.
- Native interception remains necessary because Dalamud peels ReShade's wrappers.
- Making the entire existing game proxy linear is a much broader shader/blending migration, with risks to native FFXIV UI and every game pass that assumes the current gamma domain.

**Assessment:** reasonable as an explicitly legacy-look compatibility mode, but not the smallest fully correct solution for linear transparency while preserving the present mode-3 effect ordering. Adding a local linear composition island to A can make its blending correct, but then it is no longer simple unmodified drawing into the existing proxy.

### Architecture B: a separate late linear FP16 composition stage

This keeps the existing game/PQ/ReShade-effect path and gives Dalamud a separate linear working surface at its current mode-3 boundary.

Two variants are possible:

- **Transparent UI layer:** render correctly decoded UI into a transparent linear FP16 target, then combine that premultiplied layer with the decoded background and encode PQ once.
- **Background-seeded working image:** decode the current post-effect PQ background into linear FP16, render the UI directly over it, then encode the completed image back to PQ.

The second variant is recommended for this renderer because Dalamud 15.0.3.5 includes background-dependent Kawase blur. A transparent layer alone does not give those passes the real background they expect. The transparent-layer variant remains useful for renderers whose drawing is independent of the destination image and for a future dedicated FG UI layer.

**Assessment:** B is the cleaner color/composition architecture. It creates an explicit linear-light boundary, preserves mode-3 ordering, isolates the change from native FFXIV rendering, and keeps the external HDR10 swapchain unchanged. The required native integration is substantial and must be tested; B is not an existing turnkey RenoDX feature.

| Criterion | A: existing game proxy | B: late linear composition |
|---|---|---|
| Correct PQ output for opaque UI | Yes, after suitable SDR interpretation | Yes |
| Correct linear transparency as described | No: current proxy is gamma-space | Yes |
| Preserve current ReShade-effect ordering | Requires additional scheduling work | Yes |
| Preserve game/native-UI domain | Yes for legacy blending; no if whole proxy becomes linear | Yes |
| Background blur | Sees a different stage of the image | Can retain post-effect background; needs linear blur adaptation |
| Extra image passes | Potentially fewer | Decode and encode around active overlay rendering |
| No Dalamud source/binary rebuild | Possible with native integration | Possible with native integration |
| Stock ReShade addon callbacks alone | Insufficient | Insufficient |

If A was intended to mean “replace only Dalamud's pixel shader with a PQ encoder,” that alternative is even smaller but is **not a complete fix**: the alpha-blending failure in section 3.4 still applies.

## 6. Feasibility entirely within RenoDX

### What existing infrastructure cannot do

Dalamud's `Dx11Win32Backend` peels ReShade, obtains the underlying native context, and creates/binds its RTV directly. Its shader creation and draw calls need not trigger RenoDX's ordinary ReShade addon events.

Consequently:

- Adding a Dalamud shader hash to FFXIV's `custom_shaders` list is not sufficient.
- `swapchain_proxy_revert_state` restores bindings, not image contents or transfer functions.
- Setting swapchain compatibility mode true does not help when real RGB10A2 and proxy FP16 formats differ.
- An SRGB RTV cannot make an RGB10A2/PQ resource perform hardware PQ-aware blending; there is no equivalent PQ RTV interpretation.
- Switching the DXGI color-space tag for the duration of an overlay draw does not change D3D11 blend arithmetic.
- A correction pass over the already blended final image cannot generally recover UI RGB and coverage. The source/background separation has been lost.

### What an addon-only implementation would require

Both A and B are technically possible in RenoDX code if that code adds native D3D11 interception and a reliable render interval. No change to Dalamud's on-disk assembly or configuration is necessary for the already selected mode 3. However, this is a new runtime integration with a version-sensitive renderer, not something supplied by the ordinary shared proxy API.

For B, the implementation needs:

1. A reliable boundary around the identified Dalamud `reshade_overlay` callback, including native state preservation. Observe its native ReShade registration rather than guessing when another callback has finished. Wrapping registration/forwarding must preserve callback ownership, ABI, order, and unregister/unload behavior. Do not assume that registering two RenoDX callbacks automatically places Dalamud between them.
2. Native device/context interception to recognize the supported renderer and substitute the main backbuffer RTV with the linear working RTV during that interval.
3. Shader substitution for the ordinary SDR ImGui output and all supported background-effect paths, with explicit texture-domain handling.
4. Consistent handling of copies and auxiliary textures used by background blur; RTV substitution alone is insufficient.
5. A completed linear-to-PQ writeback before returning from the wrapped callback, so ReShade's own GUI draws over the corrected image.

Dalamud registers managed delegate thunks through `ReShadeRegisterEvent`. Correct identification cannot rely only on a thunk address belonging to a normal DLL text section: Dalamud itself has module-resolution handling for these delegates. A native registration adapter must validate ownership and preserve registration lifetimes. It should not inspect arbitrary CLR internals or patch managed render methods.

Existing RenoDX [vtable](src/utils/vtable.hpp) and [detour](src/utils/detour.hpp) utilities are relevant building blocks, not evidence that this integration already exists. Vtable slots may be shared across contexts/devices; filter every interception by the validated native object, active callback interval, and resource identity. Never redirect all ImGui-looking draws globally.

Source: [Dalamud addon registration](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/Dalamud/Interface/Internal/ReShadeHandling/ReShadeAddonInterface.cs#L31), [native backend](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/Dalamud/Interface/ImGuiBackend/Dx11Win32Backend.cs#L41).

## 7. Smallest technically correct implementation recommended

Implement **B, with a background-seeded linear FP16 working image**, only for the validated main FFXIV swapchain and the supported Dalamud mode-3 renderer. Keep the existing game final shader and HDR10 swapchain policy.

The per-frame operation is:

```text
Existing RenoDX conversion and ReShade effects complete normally.

Enter the identified Dalamud overlay callback:
  Preserve actual native D3D11 state.
  Lazily prepare working resources when a supported main-viewport draw is needed.
  Copy the real PQ buffer to a shader-readable snapshot if required.
  Decode snapshot: PQ BT.2020 -> linear BT.2020 in units of 80 nits.
  Redirect supported Dalamud main-viewport rendering into FP16 working image.
  Decode/convert ordinary UI RGB to those same linear units before blending.
  Execute supported blur and other destination-dependent passes in that domain.
  Encode the finished working image back to the original RGB10A2/PQ buffer.
  Restore the native state and leave the interception scope.

ReShade's own GUI and downstream OptiScaler presentation proceed normally.
```

Use actual buffer identity and index, dimensions, sample count, and device. Do not assume every `GetBuffer(0)` resource is the current physical scanout buffer when OptiScaler interop is active. ReShade's known presentation resource and the resource obtained by Dalamud must be reconciled during validation.

Use GPU copies and render passes, not CPU readbacks, in the implementation. Avoid per-frame allocation, `wait_idle`, or unconditional flushes. Use recursion guards so the compositor's native calls do not intercept themselves. Account for all RTV/SRV hazards by unbinding resources before reading or writing them.

### Scope and the blur requirement

The ordinary shader plus FP16 target is a valid first prototype, but it is not complete support for this Dalamud release. Its blur path queries the original target format, allocates intermediates, copies the target, and draws several passes. If only the RTV is redirected, those copies still read the original PQ resource, or encounter incompatible FP16/RGB10A2 copy formats.

A complete implementation must map the relevant blur resources/views/copies to compatible FP16 resources, retain the current composited background, and adapt tint/noise/luminosity math to the chosen linear domain. Do not double-decode a sampled linear blur texture. Validate its nonlinear artistic operations separately rather than assuming the ordinary ImGui replacement fixes them.

For unsupported shader paths or versions, explicitly leave the compatibility feature inactive for that renderer/frame and report the reason; do not silently apply a partial transform or advertise full support. Preserve an untouched background until the compositor can safely commit its result. An ordinary-UI-only prototype must be labeled as such and tested with blur disabled by the tester, not silently disabled in user configuration.

Source: [Dalamud blur handling](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/Dalamud/Interface/ImGuiBackend/Renderers/Dx11Renderer.cs#L411).

## 8. Reference white and alpha contracts

### SDR reference white

Use `W = shader_injection.toneMapUINits` as the default Dalamud SDR white in nits. This is already the FFXIV UI-brightness control, defaulting to 203 nits. Do not use `toneMapPeakNits`, which describes scene highlight range, or `toneMapGameNits`, which scales scene brightness.

For a linear BT.2020 working buffer normalized so that `1.0 = 80 nits`:

```text
UI_working = BT709_to_BT2020(SDRDecode(UI_rgb)) * W / 80
Background_working = PQDecode(PQ_background) in units of 80 nits
Final_PQ = PQEncode(composited_working) using 80-nit units
```

RenoDX's helper API makes the units explicit:

```hlsl
background_working = renodx::color::pq::Decode(background_pq, 80.f);
output_pq = renodx::color::pq::EncodeSafe(composited_working, 80.f);
```

Do not apply `toneMapUINits` to the decoded background. The background already contains the correct absolute luminance from the game conversion and ReShade effects. Scaling it again would change the entire scene when adjusting UI white.

Do not rerun FFXIV's existing final shader on this linear working image: that shader expects gamma-space BT.709 and applies UI scaling. It would decode and scale twice. Use a dedicated linear-to-PQ writeback shader built from the existing color helpers.

For standard sRGB UI content, use the piecewise sRGB transfer decode. The legacy FFXIV presentation used gamma 2.2; an explicitly named gamma-2.2 compatibility policy could preserve that appearance more closely, but it is a different documented choice. Avoid changing the game's existing gamma convention while fixing the overlay.

Ordinary UNORM ImGui vertex/texture products need an explicit SDR-domain contract. To preserve the established opaque SDR appearance, decode the RGB result of the ordinary shader's modulation once, then convert primaries and scale. Inspect SRV formats and texture semantics: an SRGB SRV already decodes sampled RGB, and linear/HDR image textures must not receive another SDR decode. Font coverage and alpha remain coverage values.

An independent Dalamud brightness control is optional future UI, not required for the smallest implementation. ReShade's 250-nit setting and OptiScaler's compatibility mapping should not be silently rewritten to match RenoDX.

### Alpha and transparency

For straight-alpha UI on a linear working image:

```text
Cout = As * Cs + (1 - As) * Cd
Aout = As + (1 - As) * Ad
```

Keep `As` unchanged by transfer functions, matrices, and nits scaling. The shader supplies unassociated linear RGB; the RGB blend factors remain `SRC_ALPHA` and `INV_SRC_ALPHA` on the FP16 target.

For a separate transparent UI layer, clear to `(0,0,0,0)`. Straight-alpha source-over accumulation yields premultiplied accumulated RGB. Composite that layer with:

```text
Cout = UI_premultiplied + (1 - UI_alpha) * background_linear
```

Do not multiply accumulated premultiplied RGB by alpha again. Do not decode gamma-premultiplied RGB as if it were straight color. Correct per-fragment conversion before blending avoids that problem and handles overlapping windows properly.

Do not use the RGB10A2 destination alpha as a high-quality UI mask; it has only two bits. Keep any working coverage in FP16 alpha, and preserve or deliberately define the final backbuffer alpha according to the presenter/FG contract. Ordinary opaque desktop presentation does not imply that every downstream FG consumer ignores alpha.

Do not saturate the HDR background to `[0,1]` in the 80-nit-normalized working image. That would clip all values above 80 nits. Preserve finite HDR range and clamp only at a justified output-domain boundary.

## 9. Files and functions involved in a future implementation

These are proposed changes only. None were made for this report.

| File / proposed component | Required responsibility |
|---|---|
| [src/games/ffxiv/addon.cpp](src/games/ffxiv/addon.cpp) | Opt-in integration setup/teardown in `DllMain`; attach device/swapchain/runtime lifecycle callbacks; supply `toneMapUINits`; diagnostics for supported mode/version/resource identity. Keep existing game shader registrations and copy tracking intact. |
| New game-local `src/games/ffxiv/dalamud_overlay.hpp` | Own the FFXIV/Dalamud-specific registration adapter, renderer recognition, native interception scope, resource mapping, and `BeginOverlay`/`EndOverlay` lifecycle. Names are proposed, not existing APIs. |
| New game-local overlay shader assets | PQ-to-linear background decode; SDR ImGui-to-linear pixel shader; linear-to-PQ writeback; required blur shader adaptations. Assign filenames according to repository shader conventions when implementation is authorized. Reuse the existing fullscreen vertex shader where its interface fits. |
| [src/games/ffxiv/shared.h](src/games/ffxiv/shared.h) | No required change if the existing injection data is reused. If a dedicated overlay cbuffer is preferable, define its layout in the new component rather than needlessly changing all game shader interfaces. |
| [src/games/ffxiv/final_pixel_shader.ps_5_0.hlsl](src/games/ffxiv/final_pixel_shader.ps_5_0.hlsl) | No required change for recommended B. Continue producing the original game PQ image. |
| [src/utils/render.hpp](src/utils/render.hpp) | Reuse `RenderPass` for explicit conversion targets; only extend if a concrete native-state or resource-lifetime gap is proven. Its tracked ReShade state alone is not evidence of the current state after raw native overlay calls. |
| [src/utils/vtable.hpp](src/utils/vtable.hpp), [src/utils/detour.hpp](src/utils/detour.hpp) | Reuse interception primitives. Do not change global behavior merely for this game. |
| [src/mods/swapchain_v2.hpp](src/mods/swapchain_v2.hpp), [src/utils/draw.hpp](src/utils/draw.hpp) | No required semantic change for B. If a reusable composition API is later extracted, make it opt-in; do not move proxy conversion for every game. |
| [src/shaders/color/pq.hlsl](src/shaders/color/pq.hlsl), [src/shaders/color/srgb.hlsl](src/shaders/color/srgb.hlsl), [src/shaders/color/convert.hlsl](src/shaders/color/convert.hlsl) | Reuse existing transfer/matrix functions; no planned math-library changes. |

A general native overlay module could later live under `src/utils/`, but the first implementation should remain game-local until another consumer establishes a stable shared API. No vendored ReShade, Dalamud, OptiScaler, global CMake preset, or CI modification is required by the proposed design.

## 10. Implications for the rest of the pipeline

| Component | Implications and required checks |
|---|---|
| FFXIV native UI | Already included in the game proxy and initial PQ output. B must preserve it; do not apply a second UI-white scale to the background. Existing native UI gamma-space blending is outside this task. |
| HDR10 | Keep RGB10A2, BT.2020/PQ tagging, and the existing final presentation format. Linear scratch is internal only. Avoid transient color-space retagging. |
| ReShade effects | B consumes their current PQ output and composites after them, preserving mode-3 semantics. It does not make arbitrary SDR ReShade effects HDR-correct. Test with effects off and with known HDR-aware effects. |
| ReShade GUI | Must render after the Dalamud compositor has restored PQ output. Otherwise its already PQ-converted colors could be decoded/scaled twice or overwritten. Its own nonlinear-alpha limitation is not fixed by this scoped change. |
| OptiScaler overlay | Leave its downstream renderer and style mapping alone. Never apply the Dalamud shader replacement by generic ImGui signature alone outside the identified callback/device/resource scope. |
| XeFG / DLSSG | Keep the external HDR10 resource contract. Complete the writeback before the D3D11-to-D3D12 copy/fence/Present handoff. Validate whether UI is included in generated frames, ghosted, repeated, or handled as a dedicated HUD layer. Do not assume that a correct displayed real frame proves correct generated frames. |
| DLSS Neural Rendering | Do not change upscaler/NR input formats, exposure, motion vectors, HDR flags, or color-space tags. A later presentation integration may still consume the backbuffer; verify the actual boundary rather than assuming all NR processing precedes overlays. |
| Interop synchronization | Issue work on the same immediate context in correct order so the existing handoff fence covers it. Avoid new device-wide waits or changing buffer rotation. Native hooks must forward untouched calls outside the scope. |
| Detached Dalamud viewports | They can have independent swapchains and color-space/composition rules. Initially restrict the correction to the validated main game target. Detect and report unsupported detached targets; do not indiscriminately apply main-window PQ assumptions. |
| Performance | Expect additional fullscreen decode/encode work while the overlay actually draws. Reuse allocations, skip empty callbacks, and measure GPU bandwidth/time at native resolution and with FG. Extra quantization from a PQ round trip must be measured. |

## 11. Concrete implementation plan

1. **Establish the supported contract.** Record exact Dalamud, ReShade, and OptiScaler binaries; confirm mode 3; capture native resource/context identities and actual callback ordering. Confirm the ordinary shader fingerprint and enumerate all additional shaders used by blur, images, and custom draw callbacks.

2. **Prototype the interception boundary before color changes.** Observe native callback registration and trace entry/exit without changing outputs. Verify ownership, unload/reload, resize, multiple runtimes, and secondary viewports. Prove the wrapper encompasses Dalamud only and returns before ReShade GUI drawing.

3. **Implement a no-UI PQ round trip.** Decode the captured post-effect background into linear FP16 and encode it back. Validate color preservation before redirecting draws. Keep an untouched source and skip unsupported formats/resources. This is transfer decoding of a known HDR signal, not inverse tonemapping of an SDR image.

4. **Support ordinary UI draws.** Intercept the native device/context operations needed for the validated renderer, substitute the main RTV and ordinary ImGui shader, supply the white level once, and restore actual native state. Validate straight alpha, glyph coverage, textures, overlapping commands, and absence of accidental recursion.

5. **Complete background-dependent paths.** Add compatible FP16 shadow resources/view mappings and copy handling for blur. Adapt its shader constants and color math. Test custom callbacks; unknown paths must have an explicit unsupported behavior instead of receiving a guessed transform.

6. **Integrate lifecycle and failure handling.** Correctly handle resource release, ResizeBuffers, device removal, addon unloading, and callback unregistering. Account for shared vtables and other hooks; do not tear down another component's replacement. Leave the original path operational when the compatibility feature is inactive.

7. **Build and validate only when implementation is authorized.** Use the existing Clang preset and smallest target, normally `cmake --build --preset clang-x64-debug --target ffxiv`, with the game closed if it has the addon loaded. Inspect the generated shader embeds and `renodx-ffxiv.addon64`. No build was run for this report.

## 12. Runtime validation and acceptance criteria

### Ordering and resource proof

- Capture the original control, Stage 1 scRGB, Stage 2 mode 4, Stage 2 mode 3, and the proposed compositor in separate runs with matching logs and binary identifiers.
- At native D3D11 level, inspect the real PQ resource, RenoDX game proxy, Dalamud RTV, overlay linear working image, and final real backbuffer.
- Verify the ordinary UI draws occur into linear FP16, followed by exactly the intended writeback before ReShade GUI and before OptiScaler's presentation copy.
- A ReShade/DevKit-only trace can miss peeled native calls; use a native capture or dedicated native instrumentation for proof.

### Numerical color checks

- Display opaque neutral patches at SDR values 0, 0.25, 0.5, 0.75, and 1.0, plus saturated BT.709 red/green/blue and image textures. Compare readbacks with the selected SDR transfer function, matrix, and reference-white formula.
- At `toneMapUINits = 203`, opaque neutral white must encode near PQ 0.580689, within the agreed RGB10 quantization tolerance. Repeat at 80, 100, 250, and 400 nits.
- Change `toneMapUINits` while holding the scene and other settings fixed. Dalamud white should track it; the compositor must not rescale the already-produced background. Account separately for the existing game UI brightness behavior.
- Compare background pixels outside the overlay before and after composition. Differences should be within a measured round-trip precision bound; require bit preservation for skipped/empty overlay frames. Investigate a target bound of at most one RGB10 code where representable, rather than declaring it guaranteed before measurement.

### Transparency and difficult content

- Test alpha 0, 0.25, 0.5, 0.75, and 1 against black, 203-nit gray/white, and bright HDR backgrounds.
- Confirm the 50%-alpha 203-nit white-over-black case is near 101.5 nits, not the roughly 8.87-nit PQ-code average.
- Test overlapping windows, rounded edges, shadows, font antialiasing, colored text, premultiplied image assets, UNORM versus SRGB SRVs, and transparent images.
- Exercise Kawase blur with different radii, tint, luminosity, noise, and overlapping windows. Verify copies read the current linear composition and all intermediate formats are compatible.
- Exercise custom ImGui callbacks and confirm unsupported operations are detected without affecting other renderers.

### Integration matrix

| Axis | Required cases |
|---|---|
| Frame generation | Off; XeFG; DLSSG and supported multipliers |
| Neural rendering | Off/on for the actual DLSS NR configuration |
| ReShade | Effects off; known HDR-aware effects; GUI closed/open |
| OptiScaler | Menu closed/open; confirm its appearance is unaffected by the Dalamud-only interception |
| Game state | Login/title screen, gameplay, loading transitions, native menus, cutscenes |
| Window lifecycle | Resize, minimize/restore, alt-tab, supported fullscreen/borderless transitions, monitor changes |
| Dalamud content | Plain windows, tooltips, text input, images, blur, detached viewports where supported |
| White level | 80/100/203/250/400 nits with scene peak setting held fixed |

Check generated frames for missing/flickering UI, motion ghosting, repeated overlays, and timing changes. Check GPU frame time and latency. Confirm no new color-space changes, invalid resource copies, D3D11 RTV/SRV hazards, or resource leaks occur.

## 13. Repository integrity

At task start, the repository already had one untracked file from the preceding request:

```text
?? FFXIV_HDR10_DALAMUD_INVESTIGATION.md
```

That file was left untouched. Its pre-task SHA256 was:

```text
C196C04E225A824FB04985AFA27E598A5624E55E83A338E036DF232A6295FF00
```

The only permitted and intended new repository change for this task is this report, `FFXIV_HDR10_DALAMUD_COLOR_INVESTIGATION.md`. Full `git status` will therefore also show the pre-existing report; removing or staging it merely to make status appear cleaner would violate the requested scope.
