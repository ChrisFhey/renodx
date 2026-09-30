# FFXIV Dalamud Stage 3: End/PQ writeback investigation

Date: 2026-09-27. Branch `feature/hdr10`, HEAD `3856f13e`.
Investigation only: this report is the only repository change made in this turn.
No source/configuration edit, build, deployment, commit, push, or new game test.

## Conclusion and next action

**End is necessary to expose the reported flicker, but the isolation does not
prove that its PQ shader is defective.** Modes 4 and 5 leave the original native
image visible. Mode 3 conditionally replaces that image with a full-frame
reconstruction. That difference is the leading concrete failure mechanism:

- A callback that commits displays corrected UI; a callback that falls back
  displays the substantially brighter native PQ-space UI. Changing eligibility
  produces flicker even if every GPU operation is correct.
- If the reconstructed working image omits native UI pixels, End overwrites
  valid native pixels with background or other working-image contents. This
  explains disappearing UI even with `committed=1` throughout.

**These are hypotheses, not a proven choice between two observed traces.** The
current log contains only mode 5. There is no retained mode 3 outcome sequence
or pixel capture from the flickering comparison. Inspection found no definite
wrong source binding, destination alias, PQ scale, blend equation, or missing
GPU wait in End. A shader rewrite or resource-rotation fix would be premature.

The smallest immediate diagnostic requires **no implementation**: capture the
already available bounded mode 3 summaries during the visible transition and
check whether commits alternate with fallback. If a new implementation is
needed, add **one post-native identity-roundtrip control**: on the same eligible
callbacks, replace the working image with a fresh decode of presentation after
all original Dalamud calls, then execute the unchanged End. This exercises real
RGB10A2 writeback at the failing boundary while preserving the native UI image
approximately. It is more informative about this result than simply skipping
End again. Details and interpretation are below; nothing is implemented here.

## Evidence reviewed

Read root/game repository guidance, the resource-analysis workflow, the current
tracked diff, current diagnostic implementation/documentation, round-trip code,
source shaders, existing compiled encode bytecode, ReShade ordering code, pinned
Dalamud renderer/control-flow source, and retained reports:

- [Stage 3 notes](FFXIV_HDR10_DALAMUD_STAGE3_NOTES.md).
- [Interception](FFXIV_DALAMUD_STAGE3_INTERCEPTION_INVESTIGATION.md).
- [B4G4R4A4 investigation](FFXIV_DALAMUD_STAGE3_B4G4R4A4_FLICKER_INVESTIGATION.md).
- [Rendering-path investigation](FFXIV_DALAMUD_STAGE3_RENDER_PATH_FLICKER_INVESTIGATION.md).
- [Color investigation](FFXIV_HDR10_DALAMUD_COLOR_INVESTIGATION.md) and
  [initial integration investigation](FFXIV_HDR10_DALAMUD_INVESTIGATION.md).
- [Current diagnostic instructions](src/games/ffxiv/DALAMUD_STAGE3_DIAGNOSTIC.md).

The supplied new results are decisive experimental evidence:

| Diagnostic mode | Work added beyond native rendering | Reported result |
|---|---|---|
| 2 | Validation | No flicker |
| 4 | Validation + Begin/background copy/decode | No flicker |
| 5 | Above + corrected draws and restoration, without End | No flicker |
| 3 | Above + End/full-frame PQ writeback | Flicker, FG on and off |

This lowers pre-End native-state corruption as the leading explanation. It does
not validate the invisible FP16 pixels in mode 5. It also rules out a failure
that requires generated frames, while leaving any common downstream interop
path in scope if the output is correct immediately after End.

Current on-disk evidence is a different, specifically mode-5 capture:

- `H:/Games/FFXIV/game/ReShade.log`: 163,865 bytes, last written
  2026-09-27 18:11:23 local; SHA256
  `9EB3BB950A1A7DD12AB7F85AB68F40244F9A035690208DB70E2EC59CA9391F49`.
- All 107 summaries have `mode=5`: 106 `empty`, one `converted-no-commit`,
  with no rejection reason. Line 903, frame 648, records one native draw,
  one presentation draw, one validated/corrected draw, `composition_started=1`,
  `committed=0`, `effects_finished_this_frame=1`, and no round-trip error.
- These are short capture windows, not full-session draw statistics. The earlier
  clean mode-1 log also had sparse observed draws. Empty summaries alone do not
  demonstrate missing interception, failed commits, or missing UI.
- Both build/game addons are 4,473,856 bytes, timestamped 18:04:49, SHA256
  `5AEEAC34015D9ABEE768D5A8A65A2D26C1E481DE9963466A16EDBC1F665DE8A7`.
  This verifies matching current files, not the loaded binary for every prior
  user test. The root log is still historical Stage 1 evidence.

Older reports' zero-conversion rejection and 64 successful commits describe
earlier runs. Neither establishes the outcome pattern during this flicker.

## End: exact source, destination and execution order

Source references below describe the current working tree:
[overlay](src/games/ffxiv/dalamud_overlay.hpp),
[round trip](src/games/ffxiv/dalamud_roundtrip.hpp).

`OverlayCallback` calls the original callback at `overlay:800`. Only after it
returns does the mode-3 gate at `:803` require a still-valid candidate, a
successful Begin, and nonzero corrected-draw count. It sets `observing=true`,
calls End, restores observation, and marks `committed=true`. No End runs for
modes 4/5, rejected callbacks, or empty callbacks.

End calls `Render(nullptr)` (`roundtrip:198`). That selects the encode branch,
not preparation. Its resource chain is:

```text
Before first supported draw:
  presentation RGB10A2/PQ -> snapshot RGB10A2/PQ -> working FP16 linear BT.2020

During original callback, for each supported command:
  corrected duplicate -> working FP16
  original native draw -> presentation RGB10A2/PQ

After original callback returns, only on commit:
  working SRV -> encode PS -> cached presentation RTV, RGB overwrite
```

| Object | Creation/content | End use |
|---|---|---|
| `snapshot`/SRV | Separate full-size RGB10A2 texture, copied from presentation before the first corrected/native draw pair | Not sampled by End |
| `working`/SRV/RTV | Separate full-size R16G16B16A16_FLOAT image, mip 0, one sample, one array slice; decoded background plus corrected UI | PS t0 input |
| `presentation_rtv` | Created from the validated presentation texture on initialization; retained until identity/lifecycle reset | Sole color output |
| RenoDX game proxy | Distinct resource checked at callback eligibility | Not End's input or destination |

End does not re-copy current presentation or merge native RGB. Its cached RTV is
created from the same resource used to seed Begin; it does not select whichever
RTV Dalamud happened to leave bound. This is intentional but makes working-image
completeness essential. A different current OM target after the callback is not
itself proof that the cached target is wrong: the renderer restores prior state.

The ordering is serial on the native immediate context. There is no normal
"native draw races End" within this sequence: original indexed calls return
before the wrapper submits End, and dependent GPU commands use the same context.
The code does not Present between Begin and End. Concurrent external context use
or an interop handoff would need separate evidence; thread-local observation
flags provide recursion filtering, not synchronization.

In inspected ReShade source, effects precede `draw_gui`; `reshade_overlay` runs
at `runtime_gui.cpp:1431`, before ReShade's own ImGui submission at `:1506/1514`.
End finishes submitting within that event callback. Restoring ReShade's app
state later (`runtime.cpp:826`) restores bindings, not pre-End pixels.
The normal RGB10A2, single-sample D3D11 path does not enter ReShade's separate
resolve/format-conversion image path (`runtime.cpp:273`), so that later copyback
is not an identified explanation here. Other addon callbacks or downstream
consumers may still write afterward; no failing-frame capture establishes one.

## Shader and pass audit

End's common setup (`roundtrip:163-195`) swaps to its private D3D11.1 state,
selects triangle-list topology and the fullscreen SV_VertexID shader, sets a
full-resource viewport, no culling, disabled depth and no scissor clipping.
It binds `working_srv` at PS t0, then `presentation_rtv`, an RGB-only write-mask
blend state with blending disabled, and the encode shader. It submits `Draw(3,0)`.

The [encode source](src/games/ffxiv/0xE67E155C.ps_5_0.hlsl) performs:

```text
pixel = working.Load(integer SV_POSITION.xy, mip 0)
output.rgb = PQ_Encode(max(pixel.rgb, 0), scale = 80 nits)
output.a = pixel.a                 // excluded by the RTV alpha write mask
```

I disassembled the existing 720-byte `build/ffxiv.include/embed/0xE67E155C.cso`
read-only with `fxc /dumpbin`; no compilation or output-file generation occurred.
It declares Texture2D t0, integer coordinate conversion and load, the nonnegative
clamp, multiplier `0.008` (80/10000), and the expected ST2084 constants/powers.
It has no sampler, constant buffer, UI-white read, temporal input, or blend logic.
Its SHA256 is
`51BC2EDE4B262AC01575A83045E33C3FF23183244D6D4CF3E59D377125F7895B`.
This supports the local cached shader's behavior, not live PS identity at End.

The [decode](src/games/ffxiv/0x4453AD2C.ps_5_0.hlsl) uses the matching 80-nit
units. The [corrected UI shader](src/games/ffxiv/0xE95DEEC4.ps_5_0.hlsl) produces
linear BT.2020 in those units. End does not apply the game's gamma-space proxy
conversion, repeat sRGB decoding, repeat gamut conversion, or multiply UI white
again. RGB10A2_UNORM stores the encoded numbers; it is not an automatic second
PQ encode. Quantization/FP16 rounding can perturb values, but no frame-dependent
operation in this shader explains large alternating brightness for fixed input.

Because blending is disabled, successful writeback does **not** alpha-blend the
corrected UI over the already native-composited RGB. The originals are overwritten.
Keeping native presentation alpha through the RGB-only mask is deliberate; End
does not use two-bit alpha as a UI mask. Changing alpha handling is not justified.

### Binding hazard and state restoration

There is a plausible-looking hazard worth checking precisely: End binds
`working_srv` before its destination RTV. D3D11 would null t0 if the working
resource were still bound as output. However, the code swaps into private state
first; Begin clears that private state before restoring the caller, and each
corrected draw restores presentation RTV. Under the intended path, working is
not an active output when End binds t0. The removed Stage-2 explicit unbind is
therefore not by itself a demonstrated regression. Binding an SRV before a
distinct output is legal. [Microsoft SRV hazard contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-pssetshaderresources).

At End exit, `ClearState` clears private bindings, then the saved previous state
is swapped back. The previous-state `ComPtr` survives through restoration.
These operations do not undo the pixels End wrote. The state API is designed to
restore the previous/default context state; it does not isolate active async
queries or concurrent use. End's saved state is the state after Dalamud's own
backup restoration, which differs from Begin's entry state. A failure here could
affect later ReShade/interop rendering, but modes 4/5 do not test that exact exit
boundary. [Microsoft state-swap contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-swapdevicecontextstate).

`Draw` and the setters return no HRESULT. `End` increments executions and the
wrapper sets committed after submitting commands; neither verifies t0, actual
pixel coverage, GPU completion, or the eventual displayed image.

## Resource identity: a correction to the earlier single-buffer wording

ReShade's D3D11 implementation hardcodes `get_back_buffer_count()` to 1 and
`get_current_back_buffer_index()` to 0 in
`external/reshade/source/d3d11/d3d11_impl_swapchain.hpp`.
Its `get_back_buffer(0)` calls native `GetBuffer(0)`.
Consequently the Stage 3 count check proves one **ReShade-exposed logical
buffer**, not one physical swapchain allocation.

The current log records a two-buffer creation request and RenoDX's transition
to flip-discard (`present mode 0x4`), as well as an OptiDx11SwapchainHost and
D3D12 initialization. The current mode result does not independently establish
the physical/interoperability mapping for every frame.

This is a validation limitation, **not a finding that caching buffer 0's RTV is
wrong**. Microsoft's D3D10/11 guidance permits keeping the buffer-0 reference
across presentation; D3D12's explicit index model differs. End explicitly
rebinds its RTV before drawing, as required after flip presentation. Do not
introduce D3D12-style index cycling into this D3D11 path on the strength of the
count alone. [Microsoft DXGI overview](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi).

If target identity remains suspect, compare the End RTV's resource identity
against a fresh runtime/native buffer-0 identity **at End**, plus the identity
of the original draw target used by Begin. Entry's cached identity matches are
not a measurement at commit time. A mismatch would justify reacquiring the
correct target or rejecting that transaction; none is observed yet.

Persistent `ComPtr` resources and existing resize/reset teardown provide normal
lifetime ownership. Each new successful Begin refreshes contents, so an aborted
previous scratch transaction is not intentionally reused as current content.
There is no demonstrated use-after-free or reason to add Flush, a GPU wait, or
extra rotating scratch allocations.

## Why conditional writeback can flicker

The most concrete code-level issue is the **visible switch between two images**,
not an arithmetic instability:

| Callback | What presentation contains after original calls | What End does | Visible result |
|---|---|---|---|
| Supported | Complete native PQ-space UI | Replace RGB with corrected working image | Corrected appearance |
| Later unsupported command | Complete native UI including that command | Nothing; whole transaction rejected | Native appearance |
| Supported, but duplicate produced incomplete pixels | Valid native UI | Replace it with incomplete working image | UI pixels disappear/change |
| Empty | No matching indexed draws recorded in this callback | Nothing | No new Stage 3 correction; cannot infer UI origin |

`RejectComposition` is sticky within a callback and candidate eligibility resets
on the next callback (`overlay:127`, `:737-742`). Blur/custom work, changed shader
or texture, detached targets, other-context operations, or nested callbacks can
change eligibility. They were not established in this user's failing run.
If the outcomes alternate, mode 5 is naturally stable: it keeps native RGB on
both eligible and rejected callbacks.

Recognized rejection after some corrected draws does **not** commit a supported
prefix: it prevents all writeback. But a submitted duplicate can draw zero or
incomplete pixels without a CPU error, and unmodeled work can escape the
transaction. End checks nonzero converted submissions, not equality of all
native and reconstructed pixels. Thus consistent `committed=1` does not exclude
partial-image replacement.

The pinned ordinary renderer binds texture/scissor and then calls DrawIndexed;
its control path renders the backend and post-render work within the callback.
That supports the intended command order, not proof that every plugin command
or all visible UI is covered in this runtime.
[Pinned renderer](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/Dalamud/Interface/ImGuiBackend/Renderers/Dx11Renderer.cs#L380),
[pinned callback draw control](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/Dalamud/Interface/Internal/InterfaceManager.cs#L656).

For brightness alternation, commit/fallback is the first hypothesis to test.
For missing/reappearing geometry with steady commits, incomplete working-image
replacement is stronger. End-specific binding/restoration or later overwrite
remains the next branch if the pre-End working image is correct. The supplied
word "flicker" and mode-only evidence cannot select among these more precisely.

## Smallest next diagnostic and conditional fixes

### First: use the mode 3 summary already implemented

With one continuously visible plain window and FG off, rearm the existing
60-callback capture during flicker and preserve the log before another run.
Correlate the visual transition with runtime/frame, native/presentation counts,
validated/converted counts, `committed`, `reason`, and effects-finished flag.
Observe outside the logging window too. This is a proposed manual test, not a
test performed by this investigation.

If bright frames align with rejected callbacks and dim frames with commits,
the smallest fix is to address the **observed eligibility transition** within
its proven supported contract. A known unsupported feature may need a stable
fallback policy, but broadening validation or forcing End on rejected frames
would hide the diagnostic and risk publishing incomplete content. There is no
justified encode-shader fix for this result.

### If implementation is requested: one fresh-native-source control

Propose `Stage3DiagnosticMode=6`, preserving existing modes/default. This value
is **not currently implemented**; the present build treats it as out of range
and selects mode 3.

The new control should:

1. Execute validation, Begin, corrected duplicates and all original calls just
   as mode 3 does. Keep the same candidate/start/nonzero-conversion eligibility.
2. After the original callback returns, at the existing End boundary, obtain
   the current validated presentation texture. Verify that it matches the
   cached End target; report mismatch and skip rather than use an unrelated
   resource. Scope any added native work under the existing observation guard.
3. **Re-run the existing Begin on the post-native presentation image**, using
   the same identity and UI-white argument. This refreshes snapshot and decodes
   the complete native image into working, intentionally replacing the earlier
   corrected reconstruction. Reuse existing resources and shaders.
4. If that preparation succeeds, execute the existing End unchanged. Preserve
   all original calls and existing fallback; no shader, whitelist, hook,
   presentation scheduling, blend, alpha, or shared-infrastructure changes.

The intended output is approximately
`PQ_encode(FP16(PQ_decode(current_native_PQ)))`: native appearance, with the
ordinary round-trip precision limits. This is **not** an HDR correction fix.
It executes actual PQ writeback using an image known to include the native UI,
without inventing a new encode shader, scratch destination, or replay system.

Continue one bounded summary after the callback. Add only enough scalar fields
to disambiguate the experiment: `end_source=post-native`, reseed success,
writeback submitted, and target-match result. Use a distinct outcome such as
`native-roundtrip`; do not label discarded corrected draws as a corrected UI
commit. Retain existing draw/rejection counters and capture limits. A clean run
where the control never reaches writeback is inconclusive.

| Comparison with mode 3 | Interpretation |
|---|---|
| Fresh-native control is stable and demonstrably writes back | End can publish the native-content round trip at that boundary. Focus on corrected/native alternation, reconstructed content, or work missing between the original snapshot and commit. |
| Fresh-native control still flickers | Investigate actual End t0/RTV, current target identity, full-screen coverage, End state restoration and downstream handoff; UI reconstruction alone is insufficient. |
| Eligibility/outcome patterns differ or End is skipped | Not a controlled comparison; inspect reasons before drawing a conclusion. |

This control adds one copy/decode and its state swap, so it changes workload and
timing. A clean result is strong direction, not proof that End is universally
correct. Keep its scope diagnostic and record the actual submissions.

Do not substitute the **old pre-UI snapshot** directly at End: that would erase
native UI by design and manufacture a misleading disappearance. Do not sample
the current presentation while writing it; the existing snapshot and working
textures provide the necessary distinct source. Do not remove original draws
or move corrected writeback before the callback as a guessed fix.

### If pixels or End state still need proof

A separately authorized capture should inspect, for the same bad frame:
working after correction, presentation immediately before End, presentation
immediately after End, and final presentation. That locates the first loss:

- Bad working image, faithful encode: fix the missing/incorrect reconstruction.
- Good working image, bad immediate End output: inspect live PS t0, RTV identity,
  dimensions/viewport, blend/write mask and actual draw coverage before changing
  PQ math.
- Good post-End image, bad final image: locate the later writer/handoff.

If capture is unavailable and the fresh-native control still fails, the smallest
further state split is End's existing bind/setup/restore with only its fullscreen
Draw suppressed. It distinguishes End boundary/state side effects from pixel
publication; it cannot validate encode pixels. Do not add a suite of speculative
render changes before these observations select the failing boundary.

## Preservation

Only this report was added. The existing modified C++ files, untracked shader,
diagnostic instructions, reports and logs were preserved. Source/documentation
hashes were checked before and after this investigation. Cached bytecode was
read/disassembled only; no build or new runtime capture was performed.
