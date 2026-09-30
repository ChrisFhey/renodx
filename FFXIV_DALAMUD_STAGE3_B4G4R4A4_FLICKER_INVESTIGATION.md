# FFXIV Dalamud Stage 3: B4G4R4A4 and flicker investigation

Date: 2026-09-27. Branch: `feature/hdr10`; HEAD: `3856f13e`.
Investigation only. This report is the only file written by this investigation;
no source, configuration, build, deployment, commit, or push was performed.

## Conclusions

- **B4G4R4A4 is an expected Dalamud font format.** The pinned 15.0.3.5 managed
  atlas uploader deliberately selects it for alpha-only glyph data. A 512x512
  texture in this format is very plausibly a managed font atlas, including its
  white texel for ordinary ImGui shapes. The exact resource is not identified
  conclusively by its dimensions and format alone.
- **The existing SDR-to-linear shader can handle this font texture unchanged.**
  Sampling produces ordinary normalized RGBA; white RGB modulates vertex color,
  and alpha remains glyph coverage. Add only the exact UNORM format to the
  Texture2D whitelist. No swizzle, texture upgrade, alpha transfer conversion,
  shader-fingerprint change, or compositor redesign is needed.
- **The supplied zero-conversion rejection does not execute the compositor.**
  It returns before initialization, state substitution, duplicate draws, or PQ
  writeback. A direct color-conversion explanation for that run's flicker is
  inconsistent with this control flow. Hook/diagnostic timing and interception
  interactions remain plausible, but no specific flicker root cause is proven.
- **The current checkout and game log have advanced beyond the supplied
  rejection.** The exact whitelist case already exists, and the current log
  shows successful format-115 draws and 64 commits. Do not implement the same
  change again or describe the current on-disk run as having zero conversions.
  The next justified work is a small, controlled flicker isolation experiment,
  not another rendering change based solely on this format.

## Evidence and provenance

Read the root and game `AGENTS.md`, shared-utility guidance, the
`swapchain-resource-analysis` and `setup-game-mod-dev` skills, all four retained
root investigation/Stage 3 documents, the complete tracked Stage 3 diff, the
untracked ImGui shader, cached renderer sources, and current runtime evidence.
Upstream font and callback sources were read at the installed release tag
`15.0.3.5`, rather than inferred from a generic ImGui backend.

Setup: existing `src/games/ffxiv` mod; D3D11 x64; game folder
`H:/Games/FFXIV/game`. The local Dalamud configuration still has
`ReShadeHandlingModeV2 = 3`, `SwapChainHookMode = 0`. No settings were changed.

| Evidence | What it establishes |
|---|---|
| User-supplied diagnostic | A rejected t0 Texture2D was 512x512, format 115; dimension passed, format failed, `converted_draws=0`, no linear-draw/commit markers; new visible flicker was observed. This remains evidence for that earlier build/run. |
| Root `ReShade.log` | Historical Stage 1 instrumentation capture, last written September 26. It does not describe the diagnostic build. |
| `FFXIV_DALAMUD_STAGE3_INTERCEPTION_INVESTIGATION.md` | Earlier September 27 run rejected an unsupported SRV before its descriptor was logged. Its recommendation to collect the descriptor was appropriate then. |
| Current `dalamud_overlay.hpp:499` | Already contains `case DXGI_FORMAT_B4G4R4A4_UNORM:` alongside the previously supported formats. |
| Current `H:/Games/FFXIV/game/ReShade.log` | September 27 03:35-03:37 run: `ARMED stage=3`, verified shaders, successful format-115 draws and commits; no `IMGUI_SRV_REJECT` or `IMGUI_FALLBACK` markers. |

The latest log is 131,224 bytes, last written at 03:37:26 local time. Both the
build-directory and game-directory addons are 4,531,200 bytes, timestamped
03:34:21. Their matching on-disk hashes support consistent deployment provenance;
this is not a process-memory verification or proof of visual correctness.

```text
Root ReShade.log SHA256:
EFAB9A5F352C11371C167E86C794391B5147BCA24F65DC827276C4588A02771C
Current game-directory ReShade.log SHA256:
C9CB42B4EF405D13066B66DFE006D959645D336A6FF494566293962B40BC31DF
build/Debug/renodx-ffxiv.addon64 and game/renodx-ffxiv.addon64 SHA256:
8EF0082A0ADB13AAB2981BE8522EF2790E445CE5F666AA214C1B83C08BD32299
```

Decisive current-log locations:

- Lines 862-868: two verified VS/PS pairs.
- Lines 877-882, frame 1156: resource creation, decode, linear draw with
  `srv_format=115 ui_nits=120`, writeback, commit, one native indexed draw.
- Lines 894-901: second and third successful transactions.
- Lines 914-918, frame 3732: successful transaction, execution 41.
- Lines 934-935, frame 4116: resource reuse and commit at execution 64.
- Line 940: teardown records 64 executions.

The full descriptor diagnostic from the supplied earlier run is not retained in
either current ReShade log. Its exact debug name, pointers, and resource flags
cannot be reconstructed from the later successful-draw marker. The two runs
must not be merged into a single narrative.

## What the texture likely represents

Microsoft identifies format 115 as a 16-bit unsigned-normalized format with
four bits each for B, G, R and A. It is a typed color format, not a palette,
integer shader input, HDR encoding, or special font sampling mode.
[DXGI format definition](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format)

The important missing source in the earlier investigation was Dalamud's managed
font system. `FontAtlasFactory.BuildToolkit.UploadTextures`, lines 690-763,
checks B4G4R4A4 support. For `TexPixelsAlpha8`, it packs each pixel with
`(alphaByte << 8) | 0x0FFF`, giving white RGB and four-bit coverage alpha, then
creates a B4G4R4A4 texture and assigns its handle to the atlas. RGBA32 atlas
data uses a separate upload branch. Thus the renderer's older
`CreateFontsTexture` RGBA8 path does **not** establish that every Dalamud font
texture must be RGBA8.
[Pinned managed atlas uploader](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/Dalamud/Interface/ManagedFontAtlas/Internals/FontAtlasFactory.BuildToolkit.cs#L690)

There is a second legitimate font origin: `FontAtlasFactory.GetChannelTexture`
extracts a selected channel from the game's prebaked font textures into white
RGB plus coverage alpha. It also prefers B4G4R4A4 when supported.
[Pinned game-font texture extraction](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/Dalamud/Interface/ManagedFontAtlas/Internals/FontAtlasFactory.cs#L248)

**Inference:** a managed glyph atlas is the leading explanation; an extracted
game-font page is another concrete possibility. A plugin image or game UI asset
can also have these dimensions and format. The evidence does not identify the
font family, plugin owner, or actual texels. Ordinary ImGui binds the command's
texture ID, so the first intercepted command is not necessarily a font draw.
[Pinned renderer, ordinary draw binding](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/Dalamud/Interface/ImGuiBackend/Renderers/Dx11Renderer.cs#L394)

If exact identity becomes necessary, correlate the resource with Dalamud texture
tracking or inspect its pixels in a separate authorized capture. Atlas naming
arguments include `FontAtlasBuiltData[...]` and `FontAtlasFactory[...]`. Their
absence from a D3D debug-name query would not disprove a font origin: Dalamud's
texture blame name is separately tracked and tracking is configurable.
[Pinned texture name tracking](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/Dalamud/Interface/Textures/Internal/TextureManager.BlameTracker.cs#L98)

## Composition safety and resource chain

For the font upload described above, the sampled value is conceptually
`(1, 1, 1, coverage)`. The typed SRV supplies logical RGBA to `Texture2D<float4>`;
the BGR storage spelling does not require swapping shader channels. UNORM does
not automatically apply sRGB transfer decoding. The existing shader therefore
has the correct contract:

```text
ui = vertex_color * sampled_texture
linear_UI_RGB = BT709_to_BT2020(sRGB_decode(ui.rgb)) * ui_nits / 80
UI_alpha = ui.a
```

The source's four-bit alpha precision is an existing asset choice. Preserve it
and the existing sampler/filtering behavior. Do not decode alpha, multiply RGB
by coverage before the existing straight-alpha blend, or try to recover more
coverage precision by changing the resource format. Arbitrary plugin textures
still require the ordinary SDR/straight-alpha semantic contract; UNORM alone
does not prove color space or alpha convention.

The current resource chain remains:

```text
Game gamma-space FP16 proxy -> existing final conversion -> RGB10A2/PQ
  -> ReShade effects -> Dalamud mode-3 callback
      -> snapshot current PQ background -> linear BT.2020 FP16 working image
      -> supported ImGui samples unchanged t0/s0, converts RGB, blends in FP16
      -> encode working RGB to original PQ target after successful callback
  -> ReShade GUI -> downstream presentation/OptiScaler handoff
```

The working image uses 80-nit units. The background is already HDR/PQ, and its
decode is a transfer conversion, not reconstruction of HDR from final SDR.
The existing game proxy is not this linear working image. No new resource
upgrade or output color-space change is justified by a 16-bit font SRV.

Relevant local implementation:
[ImGui shader](src/games/ffxiv/0xE95DEEC4.ps_5_0.hlsl),
[validation and duplicate draw](src/games/ffxiv/dalamud_overlay.hpp),
[background decode/writeback](src/games/ffxiv/dalamud_roundtrip.hpp).
The original native draw still runs for fallback and presentation alpha;
successful writeback replaces RGB only. Keep all other existing gates, including
exact shader identity, main target, blend state, Texture2D dimension, and the
rejection of SRGB, float/HDR, blur, and unsupported auxiliary operations.

## Flicker analysis: what executes with zero converted draws

For the supplied rejection, the path in the current implementation, before its
format-115 allowance, is:

1. `DrawIndexedHook` increments `draws`, calls `ObserveTarget`, and calls
   `DrawLinearImGui` (`dalamud_overlay.hpp:639`).
2. Shader, RTV and SRV queries succeed up to the format check
   (`:455-510`). Diagnostic queries obtain descriptors and optional private
   debug names (`:511-563`).
3. `RejectComposition` clears only the candidate flag and conditionally logs
   (`:132`). No render target or shader is rebound on this branch.
4. Control returns to the hook, which calls the original `DrawIndexed` with
   the original context/count/start/base (`:660`). Rejection does not skip it.
5. The post-callback commit condition fails (`:898`).

Consequently, this branch cannot run `round_trip.Begin`, its copy/decode,
`SwapDeviceContextState`, the replacement pixel shader, duplicate drawing,
constant-buffer substitution, `ClearState`, or `round_trip.End`. The first
rendering mutation is below all validation gates (`:607-626`). Citing one of
those operations as the cause of the **earlier zero-conversion run** would
require evidence that a different path actually reached it.

`converted_draws=0` is a per-callback value. A single rejection record alone
does not establish zero conversions for an entire session. Likewise, the
log text `original_native_path=untouched` describes intended forwarding, not a
measured guarantee of identical timing, hook behavior, or final pixels.

### Remaining mechanisms, ranked by direct evidence

| Mechanism | What the code supports | What is not established |
|---|---|---|
| Diagnostic latency | `log::w/i` formats strings and calls ReShade synchronously inside draw/callback paths. The local ReShade logger opens a write-through file and calls `WriteFile`; diagnostic bursts can stall the render thread. | A stall is not itself proof of disappearing UI. Persistent flicker needs correlation with frame timing or downstream FG/interop. Exact loaded logger instructions were not disassembled. |
| Repeated native state inspection | The candidate resets each callback. The first rejected draw repeats shader tags and RTV/SRV queries each callback; `ObserveTarget` queries RTV/resource identity on every intercepted draw even after rejection. COM references are acquired/released normally. | These getters do not intentionally alter bindings. No unbalanced COM lifetime or direct pixel mutation was found in this rejection path. |
| Expanded native detours | Stage 3 increases the native hook set from one draw hook to 29 entries, including shader creation and auxiliary operations. They remain installed even when no conversion succeeds. This increases exposure to other native hooks and timing interactions. | No misforwarded argument, wrong slot, double native draw on rejection, or conflicting third-party trampoline has been proven. All auxiliary wrappers visibly forward their calls. |
| Thread/lifecycle assumptions | `TagImGuiShader` reads mutable `runtimes` without synchronization; creation logs show worker-thread and render-thread calls. `active`/`observing` are thread-local, not general cross-thread protection. Install transactions enlist only the current thread in the shared utility. | A concurrent runtime reset, install race, or teardown race was not captured. These are audit concerns, not demonstrated causes of steady-state flicker. |
| Removed Stage 2 work on rejected frames | HEAD performed a background round trip before Dalamud. Stage 3 starts it lazily; rejected/empty callbacks now perform none. GPU work and incidental timing differ even at zero conversions. | Restoring unused fullscreen work or adding a flush/wait would mask, not diagnose, a possible ordering/synchronization issue. |

Logging sources: [RenoDX logger](src/utils/log.hpp),
[local ReShade logger](external/reshade/source/dll_log.cpp), especially its
write-through open at line 40 and `WriteFile` at line 105. Hook sources:
`AuxiliaryOperation` (`dalamud_overlay.hpp:190`), native installation (`:667`),
and [shared detour transaction](src/utils/detour.hpp).

The descriptor diagnostic is bounded to eight distinct
`(present, format, dimension)` signatures for the runtime lifetime. It does not
retain the SRV/texture. Rejection-reason messages are also bounded and repeated
identical reasons are suppressed. The regular trace is limited to three frames
at initialization, first observed draw, reset, or ReShade-menu rearm. **There is
no demonstrated per-frame SRV log flood.** Debug-name buffers are zero-initialized
and reserve terminator space; no specific overflow or state-changing name query
was found. A brief startup/rearm hitch is more directly supported than endless
flicker from that diagnostic alone.

After conversions are enabled, another distinct risk exists: successful
callbacks can appear corrected while unsupported callbacks retain the original
overbright PQ rendering. Alternating commit/fallback decisions can cause visible
brightness changes. That mechanism needs some successful transactions and does
not explain a run with none. The current retained game log has no fallback
marker proving such alternation.

### Useful current-runtime clue, with limits

The new log records one native indexed draw at frame 1156 and zero at 1157/1158;
similarly, frames 3730/3731 have zero and 3732 has one. Zero here counts draws
before Stage 3 recognition and rejection, so the whitelist does not cause that
counter to become zero. If a continuously visible window was expected, this
would direct investigation toward missing submissions, context/callback
coverage, or a later presentation overwrite. The log does not record what UI
was open; intermittent notifications or tooltips could also explain it.
Historical Stage 1 traces already contain adjacent zero-draw callbacks, so
this pattern alone cannot be attributed to new Stage 3 code.

The matching Dalamud log also records startup and plugin hitches, but no
identified rendering exception ties them to the rejected SRV or flicker.
There is no synchronized frame capture proving whether the reported flicker
is missing geometry, alternating brightness, frame pacing, or generated-frame
behavior. The flicker cause remains unresolved rather than disproven.

## Smallest justified next step

**For the supplied rejected-format build:** add exactly
`DXGI_FORMAT_B4G4R4A4_UNORM` to the existing SDR Texture2D whitelist, keeping
the shader and all other validation unchanged. The pinned font implementation
justifies that change; it is not necessary to identify the exact font family
first. Treat this as a format-blocker fix, not a flicker fix.

**For the actual current checkout:** that change is already present and the
latest run demonstrates execution through the compositor. Do not broaden the
whitelist further or rewrite fingerprinting. The smallest next implementation,
if requested, is a temporary game-local diagnostic control for a controlled
comparison, with rendering and logging effects separated:

1. Provide a native-draw pass-through diagnostic mode that retains the callback
   wrapper and original calls but skips `ObserveTarget` and `DrawLinearImGui`.
   Disable in-draw diagnostic output in that control. Keep simple draw counters
   and record a bounded callback summary after the callback returns.
2. Compare with validation active and composition deliberately disabled before
   `Begin`, then with current composition enabled. This distinguishes inspection
   overhead from conversion work. Record callback frame/context, indexed draw
   count, rejection reason, converted draws, and commit/abort/empty outcome in
   a small bounded capture; never add an unbounded per-draw log.
3. If flicker remains in pass-through mode, compare a control with the added
   Stage 3 native detours absent, or the known Stage 2 baseline. Merely setting
   `composition_candidate=false` does not remove the installed detours or
   `ObserveTarget` overhead and cannot exonerate them.

These are proposed experiments, not changes made here. Start with one fixed
plain Dalamud window, blur off and FG off; preserve other settings between
comparisons. Then repeat the decisive comparison with the affected FG mode.
Capture actual frame appearance alongside the log so empty callbacks and
brightness transitions can be correlated. Restore the user's settings after
testing. If native draws are present but disappear later, inspect the native
presentation image immediately after the callback and at downstream handoff.

Acceptance for the existing format fix: verified shaders; format-115 linear
draw; successful commit; stable glyphs and translucent windows; expected UI
white response; no new native-state errors; correct original fallback when an
unsupported command occurs. A commit log alone does not establish these visual
or pixel-level properties.

Any future authorized build should use the existing `clang-x64-debug` preset
and `ffxiv` target with the game closed, producing
`build/Debug/renodx-ffxiv.addon64`. No DevKit launch, live shader change, build,
link adjustment, or deployment is needed to complete this report, and none was
performed.

## Repository preservation

The existing three modified C++ files, untracked shader, four investigation
documents, root log, build artifacts, and game-folder files were preserved.
Only this report was added. Source references and line numbers describe the
working tree inspected on September 27, not the committed Stage 2 baseline.
