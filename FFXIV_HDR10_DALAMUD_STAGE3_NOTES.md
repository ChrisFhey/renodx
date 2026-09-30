# FFXIV Dalamud Stage 3: ordinary ImGui composition

Scope: native integration for Dalamud 15.0.3.5 mode 3 on the already validated
ReShade binary and main, single-buffer HDR10 runtime. No deployment, commit, or
configuration changes. Runtime validation of this build remains pending.

## Evidence and interception

The root `ReShade.log` available during implementation is still the Stage 1
`ARMED instrumentation-only` capture. It confirms callback ownership, ordering,
and the 3440x1440 native target matching presentation/buffer0/native-buffer0 but
not RenoDX's proxy. Stage 2's successful runtime validation is supplied by the
user's latest test results, rather than independently visible in that file.

### Latest interception handoff (2026-09-29)

Runtime evidence supplied in the handoff reports that pinned Dalamud 15.0.3.5's
live context had slot-9 `PSSetShader = d3d11 + 0x159760`, while RenoDX sampled
and detoured `d3d11 + 0x146770`. These values are not present in the repository
logs. The handoff further reports that D3D11 dynamically rewrites this embedded
dispatch-table slot, so an initialization-time address detour is unstable.

The proposed interception design is an owned forwarding `ID3D11DeviceContext`
proxy returned only to Dalamud from its `GetImmediateContext` acquisition. The
unresolved prerequisite is reliably identifying the pinned
`Dx11Win32Backend` constructor's acquisition across CLR JIT tiering and
recompilation, without fixed JIT addresses or callback/thread heuristics.

### Shared-FP16 final presentation boundary (2026-09-29)

Evidence: the installed `H:/Games/FFXIV/game/OptiScaler.log` is a September 27
capture reporting `10.0.0-dev (f45ccf3a)`, not the September 29 ReShade session.
`D:/Development/OptiScaler` has matching HEAD
`f45ccf3a761df91450959d325ac0166cba5364c9`; its presentation/FG files are clean.
Two unrelated input/feature files have existing local changes. This establishes
revision correspondence, not a reproducible binary/source match. Installed
`winmm.dll` SHA256: `42B3B7C7600D19E3EF30DBE745370BF2C1434271FB2A84ED57746E7BE4498778`.
The September 29 INI selects `dlss_12`, FG input `Upscaler`, output `DLSSG`, NR on.
`RunBeforeSR`, `FinishedPicture`, `DeferredDLSS`, and `HUDFix` are `auto`; this
revision defaults all four to false (`OptiScaler/Config.h`). The retained log
also explicitly reports NR after SR and the FG path without HUD-fix capture.

The latest suitable boundary is **after ReShade's complete `on_present` work,
before forwarding to OptiScaler's `Dx11wDx12SC::Present`**. Local ReShade source
orders addon `present`, effects, `reshade_overlay` (mode-3 Dalamud), ReShade GUI,
final resolve/copy, then `reshade_present`, before `_orig->Present`.
`reshade_present` is the late addon candidate, but does not guarantee being the
last listener. A strict bridge boundary must follow all those listeners.
`finish_present` is too late; `present`/`reshade_finish_effects` are too early.
Sources: [ReShade forwarding](external/reshade/source/dxgi/dxgi_swapchain.cpp),
[runtime](external/reshade/source/runtime.cpp),
[GUI](external/reshade/source/runtime_gui.cpp). Installed ReShade is 6.8.0.2155;
this investigation did not establish its exact source/binary correspondence.

The matching OptiScaler source and retained log distinguish these consumers:

| Stage | Actual resource/order |
| --- | --- |
| SR + NR | Earlier NGX evaluation bridges game color/depth/MV/output resources to D3D12; NR runs after SR, before returning the output to the game. This is not final-backbuffer capture. |
| FG guide inputs | `UpscalerInputsDx11wDx12` reuses cached depth/MV and tags those resources before final composition. Its UI command-list name does not establish a UI texture. |
| HUD/UI color | Separate optional HUD-less/UI tags are distinct from both depth/MV and the completed frame. HUD-fix is off in this configuration; the retained log has depth/velocity tags and no `HudlessColor`/`UIColor` tags. Enabling HUD-fix permits earlier color capture, including upscaler output. |
| Real-frame capture | `Dx11wDx12SC::Present` copies `_real->GetBuffer(index)` to a same-format D3D11 shared shadow, signals its fence, waits on the D3D12 copy queue, copies to the current D3D12 FG backbuffer, then orders the presentation queue after that copy. |
| Finished-picture NR | Optional `ApplyToFinishedPicture` runs on that D3D12 backbuffer after the interop wait, before FG presentation. It is off here. |
| Generated presentation | `_fgSwapChain->Present` enters `FGHooks::FGPresent`, `DLSSG_Dx12::Present`/dispatch and downstream display presents. OptiScaler's own menu is drawn downstream. Those presents are not another FFXIV/ReShade composition cycle. |

Source anchors in the matching checkout:
`OptiScaler/with_dx12/dx11_with_dx12_sc.cpp:359,1005,1065,1161,1248`,
`OptiScaler/inputs/FG/Upscaler_Inputs_Dx11wDx12.cpp:76,306`,
`OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp:821`, and
`OptiScaler/hooks/FG_Hooks.cpp:1133`. The log's 15:05:34.891-15:05:34.896 sequence
corroborates guide tagging -> both frame copies -> DLSSG dispatch -> display
presents; it is not a new runtime capture of the proposed architecture.

**Conclusion:** one FP16-to-RGB10A2/PQ conversion per real frame can preserve
this setup's existing OptiScaler input contract, provided it writes the actual
RGB10A2 buffer the bridge copies, on the ordered D3D11 immediate context, before
that copy. Keep upstream SR/NR resources, depth/MV, dimensions, alpha behavior,
HDR10 signaling and interop synchronization intact. Generated frames then remain
downstream HDR10 frames; they need no additional RenoDX FP16 conversion. This
does not guarantee interpolation-free overlays or cover enabled HUD-less/UI
capture, which would require its own color-domain audit.

This requires an authoritative app/ReShade-facing FP16 surface plus a distinct
OptiScaler-facing RGB10A2 output. The current RenoDX redirected clone is not that
shared surface, and OptiScaler's `CopyResource` calls perform no color conversion.
Moving today's callback or changing the whole downstream swapchain to FP16 is
insufficient. Stage 3's conditional reconstruction/writeback flicker (also with
FG off) does not prove an unconditional authoritative final encode is invalid.
Feasibility is established at the ordering/contract level; shared-FP16 operation
with DLSS NR + DLSSG remains unimplemented and unvalidated. No build or runtime
changes were made for this investigation.

### Historical Stage 3 draw-interception implementation

The pinned [renderer source](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/Dalamud/Interface/ImGuiBackend/Renderers/Dx11Renderer.cs)
confirms ordinary commands bind a texture and call `DrawIndexed`. Blur uses
`CopyResource` and non-indexed fullscreen draws. The existing native indexed-draw
hook therefore permits replacing the RTV and pixel shader for an ordinary draw.

Recognition requires all of the following:

- The existing ownership-validated mode-3 callback, native device/context, main
  swapchain identity, RGB10A2/HDR10 contract, and distinct RenoDX proxy.
- Exact SHA256 matches for both original shaders, observed at native creation:
  PS: `B6E7F2FEB00CC787D3F039DFD8DD02720B59F7497ED936B4FD4B85D16E39834A` (740 bytes);
  VS: `B62A4AAEEE2B9F9EA9F1837AA60E7585A0B2D6F1889662FEDF0C122E69D6572E` (996 bytes).
- The main presentation RTV, no other render/depth targets, expected source-over
  blend state, no extra graphics shader stages, predicate, or OM UAV bindings.
- An ordinary SDR UNORM Texture2D SRV: RGBA8, BGRA8, BGRX8, BC1/2/3/7.

Fingerprints are stored as private scalar tags on the original shader objects;
they neither replace creation results nor retain shader pointers globally.
Shaders created before interception remain unverified and cause fallback.
Start a fresh game process for this test.

## Composition and fallback

The first verified draw lazily starts the existing Stage 2 background decode.
Its PQ snapshot and FP16 working target are reused until resource recreation.
The replacement shader samples the same texture/sampler and computes:

```text
original = vertex_color * sampled_texture
working_RGB = BT709_to_BT2020(sRGB_decode(original.rgb)) * toneMapUINits / 80
coverage = original.a
```

The existing blend state performs straight source-over on linear FP16 RGB.
Its alpha equation is equivalent to coverage union. No UI mask is reconstructed
from presentation alpha. PQ writeback preserves the alpha produced by the
original native presentation path; it only replaces RGB.

Each supported command also executes its original native draw into presentation.
This deliberately retains a complete fallback if a later command is unsupported.
Only after the callback returns successfully is the corrected FP16 image encoded
over presentation RGB. This adds indexed draw submissions, but no second image
copy, command recording/replay, GPU wait, or per-frame resource creation.

Unknown shaders, unsupported views/blends, detached targets, nested callbacks,
and intercepted auxiliary draws/copies/clears/dispatches/queries cancel the
entire correction transaction. D3D11.1 copy/update/clear/discard entry points
are also guarded. Original native calls still execute with their original
arguments. A rejected frame receives no corrected writeback, preserving its
existing overbright appearance instead of a partial correction.

The replacement draw restores its RTV, PS, and PS constant-buffer binding/range.
Decode/writeback retain Stage 2's full native context-state isolation. Writeback
occurs after the original Dalamud callback and before the wrapper's `EXIT`, hence
before ReShade GUI and the existing downstream presentation handoff. Existing
resource teardown and callback unregister behavior remain in place; all native
detours are removed on addon unload.

## Unsupported and remaining concerns

Blur, custom auxiliary rendering, detached viewports, SRGB SRVs, linear/HDR image
textures, and nonstandard blend states are unsupported. Any detected occurrence
rejects correction for that callback, including ordinary draws in the same frame.
Arbitrary plugin renderers, other devices/threads, and other native API extensions
are not a supported rendering contract. UNORM content is explicitly interpreted
as ordinary SDR; its semantic color space cannot be inferred from its format alone.

Native hook installation, shader creation timing, runtime color accuracy, resize,
and the added draw overhead still require testing. Hook installation is atomic;
unsupported context identity or installation failure leaves correction inactive.

## Build and runtime verification

- Built `ffxiv` with `clang-x64-debug` in Visual Studio's x64 environment.
  Output: `build/Debug/renodx-ffxiv.addon64`.
- Final build succeeds with 31 existing C++ deprecation warnings and no new
  shader warnings. An initial link attempt used inherited x86 runtime libraries;
  rerunning with the x64 environment resolved it without preset changes.
- `git diff --check` passes. SDK vtable slots were checked against installed
  headers. Compiled ImGui shader inspection confirms b0/t0/s0, unchanged alpha,
  piecewise sRGB decode, the BT.709-to-BT.2020 matrix, and the 1/80 multiplier.

Test in a fresh process with blur disabled by the tester, mode 3 unchanged:

1. Confirm `ARMED stage=3`, `IMGUI_SHADER_VERIFIED stage=VS` and `stage=PS`,
   `ROUNDTRIP_CREATED`, `ROUNDTRIP_PQ_DECODE_SUBMITTED`, `IMGUI_LINEAR_DRAW`,
   `ROUNDTRIP_PQ_WRITEBACK_SUBMITTED ... after_Dalamud=1`, and `IMGUI_COMMIT`.
   After 64 successful commits, expect `ROUNDTRIP_REUSED` with stable resources.
2. Check text, colored icons, overlapping/translucent windows and antialiased
   edges. Vary UI white between 80, 203, and 400 nits. White should track the
   setting; this compositor must not independently rescale background pixels.
   The pre-existing game shader also uses the same setting, so account for that.
3. Check ReShade GUI/effects, OptiScaler menu, DLSS NR, FG off/DLSSG/XeFG, movement,
   resize, minimize/restore, and teardown. Compare GPU time and generated frames.
4. Separately enable blur to verify `IMGUI_FALLBACK ... CopyResource/blur capture`
   and unchanged native rendering, without crashes or partial correction.

Send the complete new ReShade log and visual observations, including any
`IMGUI_FALLBACK`, `NATIVE_INACTIVE`, or `ROUNDTRIP_CREATE_FAILED` marker. Opening
or closing ReShade's GUI rearms the existing short diagnostic trace.
