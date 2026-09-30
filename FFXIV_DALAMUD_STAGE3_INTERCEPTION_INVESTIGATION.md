# FFXIV Dalamud Stage 3 interception investigation

Date: 2026-09-27. Branch: `feature/hdr10`; HEAD: `3856f13e`, with the existing
uncommitted Stage 3 implementation. Investigation only: no source edits, build,
deployment, configuration change, commit, or push. This report is the only new
file created by this investigation.

## Diagnosis

**The current game-directory log proves that Stage 3 reaches the ordinary-ImGui
recognition function and passes its shader checks. The observed draw is rejected
by the texture-SRV whitelist, before blend validation or FP16 initialization.**

The decisive line is in `H:/Games/FFXIV/game/ReShade.log:1223`:

```text
02:22:28:976 [36796] | WARN | [RenoDX] [FFXIV Dalamud] IMGUI_FALLBACK reason=unsupported ImGui texture SRV (requires SDR UNORM Texture2D) frame=492 converted_draws_discarded=0 original_native_path=untouched
```

The exact failing expression is `if (!sdr_texture)` in
`src/games/ffxiv/dalamud_overlay.hpp:502`. The current diagnostic does not print
the SRV pointer, its format, or its view dimension. Consequently, the particular
descriptor condition cannot be determined statically or from this log.

Do not replace the shader-identification mechanism or loosen unrelated gates
on the basis of this failure. Add a small descriptor diagnostic at this already
identified rejection point first.

## Logs and binary provenance

The two logs currently available do not match the marker counts described in the
request. I inspected both rather than assuming the repository-root copy was fresh.

| Evidence | Repository root `ReShade.log` | `H:/Games/FFXIV/game/ReShade.log` |
|---|---|---|
| Last write | 2026-09-26 15:38:04 | 2026-09-27 02:24:07 |
| Size | 130,546 bytes | 183,256 bytes |
| Startup marker | `ARMED instrumentation-only` | `ARMED stage=3 ordinary-ImGui-linear-composition` |
| `ROUNDTRIP_*` lines | 0 | 0 |
| `IMGUI_SHADER_VERIFIED` lines | 0 | 4: two VS, two PS |
| `IMGUI_LINEAR_DRAW` lines | 0 | 0 |
| `IMGUI_COMMIT` lines | 0 | 0 |
| `IMGUI_FALLBACK` lines | 0 | 1: unsupported texture SRV |
| Main `NATIVE_DRAW` lines | 1 | 1 |

SHA256:

```text
Repository-root log:
EFAB9A5F352C11371C167E86C794391B5147BCA24F65DC827276C4588A02771C

Game-directory log:
408F240582AC817BC3AEE820A041DE55128A506D111EA87E3002EF1C354757D9

build/Debug/renodx-ffxiv.addon64 AND H:/Games/FFXIV/game/renodx-ffxiv.addon64:
128CE641D4324F3CF613EC1C8A126EB01C83119B22863ADBDD23B811EB4AC539
```

Both on-disk addons are 4,499,456 bytes, timestamped 02:15:22, preceding this
run. The game log records loading that game-directory addon at line 46 and its
Stage 3 startup marker at line 58. This is strong evidence against a stale
deployed addon for this run; no process-memory binary dump was performed.

The user's earlier successful Stage 2 test remains valid historical evidence.
It does not imply that the retained decode/encode functions execute in every
Stage 3 callback. In the current source they execute only after ordinary-draw
validation. A separate log containing successful round-trip execution but no
shader-verification markers would need separate provenance analysis. It is not
either of the two logs inspected here.

## Trace from the working native DrawIndexed hook

Source references below refer to the current working tree, not HEAD's Stage 2.

1. `OnInitRuntime` installs the native detours and computes `native_supported`
   (`dalamud_overlay.hpp:606-670`). Game log line 124 reports
   `native_supported=1`, context `0000022AD898DFF8`, and runtime
   `0000022B8BE990A0`. The additional Stage 3 hook installation and native-context
   identity checks therefore did not disable this runtime.
2. The ownership adapter finds Dalamud 15.0.3.5's callback (log line 1195).
   `OverlayCallback` establishes `active` and the main-HDR10 composition candidate
   (`dalamud_overlay.hpp:798-835`). The later rejection message can only be
   emitted while a candidate is active; the outer candidate checks passed.
3. `DrawIndexedHook` calls `ObserveTarget`, then `DrawLinearImGui`, inside the
   same validated active-context branch (`dalamud_overlay.hpp:577-585`). It sets
   `observing=true` to prevent recursive interception. This does **not** prevent
   `DrawLinearImGui` or `RejectComposition` from executing: neither tests that
   guard. The original native draw is still forwarded afterward.
4. Log line 1222 identifies frame 492's native RTV/resource at 3440x1440,
   format 24 (`R10G10B10A2_UNORM`), with presentation/buffer0/native-buffer0
   matches and no proxy match. This proves interception and resource identity.
5. `DrawLinearImGui`'s early candidate/count check and VS/PS tag checks are at
   lines 451-460. Its main RTV/no-other-target checks are at lines 463-482.
   Execution reaching the SRV rejection proves these earlier gates passed
   for the logged draw. This is stronger evidence than shader creation logs alone.
6. Lines 483-505 query PS slot t0, obtain its view descriptor, and reject the
   draw unless the format and dimension match the whitelist below. This is
   the observed failure.
7. Blend validation at lines 506-520 and extra-stage/predicate/UAV validation
   at lines 521-544 are **not reached for this draw**.
8. `round_trip.Begin` at lines 546-554 is also not reached. Thus no snapshot,
   FP16 target creation, or PQ decode is submitted for this draw. There is no
   independent Stage 2 round trip before the callback in this working tree.
9. `RejectComposition` sets `composition_candidate=false` (lines 127-135).
   Further draws in that callback immediately return from `DrawLinearImGui`.
   The post-callback condition at line 837 then prevents `round_trip.End` and
   `IMGUI_COMMIT`. The original native PQ rendering remains visible and bright.

`RejectComposition` suppresses repeated identical reasons and limits new reason
logs to 16 per runtime. One failure line therefore does not mean one failed frame.
The candidate resets each callback, but the last logged reason does not. Nor does
one logged reason establish that every rejected draw used the same descriptor.

The trace also contains `effects_finished_this_frame=0` while effects compile.
That value is diagnostic only: the current candidate condition does not require
`effects_frame == frame`. It is not the observed blocker.

## Shader fingerprinting findings

Both original shader fingerprints are verified at creation in the game log:

| Log line | Time | Stage | Shader pointer |
|---|---|---|---|
| 1201 | 02:22:28:688 | VS | `0000022EB7E25138` |
| 1202 | 02:22:28:689 | PS | `0000022EB7E25C38` |
| 1216 | 02:22:28:821 | VS | `0000022EB6C68438` |
| 1217 | 02:22:28:821 | PS | `0000022EB6C653B8` |

All four report device `0000022AD8798B90` and `sha256=matched`. Runtime hooks were
installed at 02:22:17, before those creations. `TagImGuiShader` only prints success
after the byte count, supported-device pointer, full SHA256, and `SetPrivateData`
checks succeed (`dalamud_overlay.hpp:138-157`).

The cached release bytecode also matches the arrays currently in the source:

- `build/dalamud-imgui-ps.bytes`: 740 bytes;
  `B6E7F2FEB00CC787D3F039DFD8DD02720B59F7497ED936B4FD4B85D16E39834A`.
- `build/dalamud-imgui-vs.bytes`: 996 bytes;
  `B62A4AAEEE2B9F9EA9F1837AA60E7585A0B2D6F1889662FEDF0C122E69D6572E`.

Shader creation before hook installation, changed bytecode, non-null class
linkage, different device pointers, or private-data failure remain possible
limitations in other runs. They do not explain the observed frame-492 SRV
rejection: both bound shader tags and the zero-class-instance checks had passed.

## Texture and blend predicates

The whitelist accepts only these **view** formats, each with
`D3D11_SRV_DIMENSION_TEXTURE2D`:

```text
R8G8B8A8_UNORM
B8G8R8A8_UNORM
B8G8R8X8_UNORM
BC1_UNORM / BC2_UNORM / BC3_UNORM / BC7_UNORM
```

The observed rejection means at least one of these conditions holds:

- `PSGetShaderResources(0, 1, ...)` returned a null SRV; the zero-initialized
  descriptor then remains UNKNOWN.
- The actual SRV format is absent from the whitelist.
- The format is accepted, but the view dimension is not Texture2D.

The warning currently combines these cases. It cannot establish SRGB, FP16,
single-channel, packed-format, or array-view usage by itself. The log's
`NATIVE_DRAW ... format=24` describes the **output render target**, not the
sampled t0 texture. It is not evidence of t0's format.

The cached pinned renderer source provides useful counterevidence against
guessing that ordinary fonts intrinsically require another format:

- `build/Dx11Renderer.cs:737-777`, `CreateFontsTexture`, obtains RGBA32 atlas
  data and requests `R8G8B8A8_UNORM`.
- Its `CreateTexture2D` creates a Texture2D SRV (`:239-244`). An unchanged
  font atlas from this path should pass the current whitelist.
- Ordinary commands instead bind whichever SRV is in `cmd.TextureId`
  (`:394-399`); the first intercepted draw is not proven to be that font atlas.
  Images, alternate atlas providers, or a null binding cannot be ruled out.

The game log contains FP16 resource upgrades, but their shown dimensions are
3440x1440. There is no captured t0 resource pointer linking those upgrades to the
rejected draw. Treating the failure as an upgraded font texture is unproven.

The exact blend predicate is stricter than accepting all mathematically
equivalent source-over states. For example, its alpha factors require
`INV_DEST_ALPHA, ONE`; `ONE, INV_SRC_ALPHA` would give the same coverage union
but be rejected. However, the inspected ordinary Dalamud renderer explicitly
creates the required factors (`build/Dx11Renderer.cs:936-954`), and its setup
uses sample mask `0xffffffff` (`:800`). No blend defect is established here:
the failing draw returns before querying that state. Blur's different blend
state is intentionally outside Stage 3 support.

## Smallest next diagnostic change — proposed only

Keep all validation and rendering behavior unchanged. Extend the existing
unsupported-SRV rejection in `DrawLinearImGui` with a bounded diagnostic named,
for example, `[FFXIV Dalamud] IMGUI_SRV_REJECT`.

Record the following for the first rejected descriptor, optionally up to eight
distinct descriptor signatures per runtime to distinguish transient startup
bindings from the persistent failure:

- Runtime/frame/context and draw index count; `composition_candidate` and
  `converted_draws` before rejection.
- t0 SRV pointer, explicit `srv_present`, `srv_desc.Format`, and
  `srv_desc.ViewDimension`, with separate `format_allowed` and
  `dimension_allowed` booleans.
- If present, the underlying resource pointer/type; for Texture2D, format,
  width/height, mip count, array size, sample count, and bind flags.
- Optional bounded debug names from the SRV/resource. Dalamud's font path sets
  names such as `Texture:Font#0:SRV`, which could immediately identify the input.

Use stack storage and descriptor/private-data queries only. No shader dumping,
new interception mechanism, GPU readback, GPU wait, or whitelist relaxation is
needed for this diagnostic. Preserve original calls and existing fallback.

Expected decisions from the next capture:

- Null SRV: investigate the actual command/binding and whether it is a transient
  no-op; do not guess a replacement texture.
- Another ordinary SDR UNORM view: validate its existing sampling/channel
  semantics before adding that exact case.
- SRGB view: sampled RGB is already hardware-decoded; the current combined
  product decode cannot simply be reused unchanged.
- Float/HDR or background-derived texture: establish its color/luminance
  contract; do not assume every float image is SDR or enable it indiscriminately.
- Unexpected view dimension: verify the texture/view construction before
  changing the Texture2D shader contract.

The immediate blocking gate is established; this next diagnostic is needed to
choose a justified fix for the rejected input, not to rediscover callback or
shader ownership. A global gate tracer or new shader fingerprint scheme would
be unnecessary for the run inspected here.

## Repository preservation

Read the root and game instructions, relevant workflow skills, Stage 3 notes,
current diff, retained color investigation, cached pinned renderer sources,
and current addon/log metadata. The existing Stage 3 source changes, original
investigation reports, root log, and game-directory files were left untouched.
Only `FFXIV_DALAMUD_STAGE3_INTERCEPTION_INVESTIGATION.md` was added.
