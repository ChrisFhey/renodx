# FFXIV Dalamud Stage 3: rendering-path flicker investigation

Date: 2026-09-27. Branch: `feature/hdr10`; HEAD: `3856f13e`.
Investigation only. This report is the only file added or changed by this
investigation. No source/configuration edits, build, deployment, commit, push,
or new game experiment was performed.

## Conclusions

The supplied comparison isolates the regression to the work after mode 2's
return before `Begin`: resource initialization/background preparation, the
corrected ImGui submissions and their restoration, or final writeback. Installed
hooks, the callback wrapper, ordinary validation, and FG are not sufficient to
produce the reported flicker in these tests. FG is not required for the failure.

There is no proven single faulty API call in the inspected implementation.
The leading explanations are:

1. **Alternating corrected commits and native fallback.** This directly produces
   brightness flicker without corrupting resources: every callback starts a new
   candidate transaction, and any later unsupported operation cancels correction
   for that entire callback. Mode 2 always displays the native result, so the
   same eligibility changes would be invisible there.
2. **State restoration or an incomplete working image exposed by writeback.**
   Stage 3 now interrupts a live ImGui draw with a context-state swap, duplicates
   draws into FP16, and writes back after Dalamud restores its own state. Losing
   IA/VS/viewport/scissor/texture state can affect native draws or silently omit
   corrected pixels while all CPU submission counters still report success.
3. **A snapshot/transaction that does not encompass all visible work.** A missed
   native operation or changed ordering can leave legitimate native pixels out
   of the working image; full-screen writeback then removes them. Alternating
   coverage would appear as disappearing/reappearing UI.

Ordinary asynchronous D3D11 execution, a released local `ComPtr`, four-bit font
alpha, and fixed HDR math are weaker explanations on the current evidence.
No generic flush, extra buffer, whitelist change, hook rewrite, or color change
is justified yet.

**Smallest next diagnostic:** retain modes 1/2/3 and add two cumulative stop
points: **Begin only, no corrected draw/no End**, and **Begin plus corrected
draws, no End**. Preserve every original draw. These isolate observable side
effects before writeback; if both are clean, compare full-mode transaction
outcomes and inspect the working image before blaming the encode pass itself.
The proposed modes below are not implemented by this report.

## Evidence and limits

Reviewed root and game `AGENTS.md`, the resource-analysis workflow and applicable
game-development guidance, the current three-file tracked diff, the untracked
ImGui shader, and these retained documents:

- [Stage 3 implementation notes](FFXIV_HDR10_DALAMUD_STAGE3_NOTES.md).
- [Interception investigation](FFXIV_DALAMUD_STAGE3_INTERCEPTION_INVESTIGATION.md).
- [B4G4R4A4/flicker investigation](FFXIV_DALAMUD_STAGE3_B4G4R4A4_FLICKER_INVESTIGATION.md).
- [Color/composition investigation](FFXIV_HDR10_DALAMUD_COLOR_INVESTIGATION.md)
  and [initial integration investigation](FFXIV_HDR10_DALAMUD_INVESTIGATION.md).
- [Implemented diagnostic documentation](src/games/ffxiv/DALAMUD_STAGE3_DIAGNOSTIC.md).

The setup remains the existing FFXIV x64/D3D11 mod, with game files under
`H:/Games/FFXIV/game`. No setup, DevKit, shader, or link changes were needed.
Line references below describe the inspected working tree, not HEAD.

| Runtime evidence | Interpretation |
|---|---|
| User: diagnostic mode 1 clean with FG on/off | Retained callback wrapper/hooks/native forwarding alone did not reproduce flicker. |
| User: diagnostic mode 2 clean | Target observation and validation did not reproduce it. |
| User: diagnostic mode 3 flickers with FG on/off | Added rendering is necessary in this comparison; a generated-frame-only explanation is insufficient. |
| Current game `ReShade.log` | 152,734 bytes, last written 2026-09-27 17:09:11 local. All 82 retained summaries are mode 1: 81 empty, one pass-through. No mode 2/3 transaction series survives in this file. |
| Root `ReShade.log` | Historical Stage 1 evidence, 130,546 bytes, last written September 26. |
| Earlier report's 64 commits | Historical evidence that the format-115 path reached writeback; not a capture of this new mode comparison. |

The one nonempty current summary is at game-log line 878, frame 580:
`native_indexed_draws=1`, `outcome=pass-through`, `reason=none`,
`composition_started=0`, `committed=0`, `effects_finished_this_frame=1`.
Sparse indexed draws also occur in this clean control. Empty callbacks alone
therefore do not prove the Stage 3 defect. The summaries are bounded captures,
not a census of the full session.

Current game-log SHA256:
`EB8790356AA378EAE95FD17EB5D955D5AB1725C66FFBF0AB53384E56D3E82768`.
Build-directory and game-directory addons both have length 4,473,344 and SHA256
`275C044D363113A4F8EE587F6F7C5FDA8664CCA9EBB4E31D4F264A53161E2361`.
This verifies matching files on disk, not which binary was loaded during every
user test. Visual outcomes for modes 2/3 are user evidence; their rejection,
commit, and draw counts cannot be reconstructed from the current log.

The old observation of flicker with zero conversions belongs to an earlier
build/run. It must not override the new controlled comparison or be silently
combined with it. The current whitelist already includes B4G4R4A4.

## Exact rendering and ownership sequence

Relevant implementation:
[draw validation/submission and callback](src/games/ffxiv/dalamud_overlay.hpp),
[round trip](src/games/ffxiv/dalamud_roundtrip.hpp),
[decode](src/games/ffxiv/0x4453AD2C.ps_5_0.hlsl),
[linear ImGui](src/games/ffxiv/0xE95DEEC4.ps_5_0.hlsl),
[encode](src/games/ffxiv/0xE67E155C.ps_5_0.hlsl).

1. `OverlayCallback` resets candidate/start/count state each invocation
   (`dalamud_overlay.hpp:734`). It checks the existing main HDR10 single-buffer
   identity contract, then invokes the original callback once.
2. The native `DrawIndexed` hook observes each matching-context draw. Validation
   ends at `:511`; mode 2 returns at `:514` before any round-trip initialization.
3. On the first supported nonzero draw, `Begin` (`dalamud_roundtrip.hpp:137`)
   initializes persistent resources if the presentation identity changed,
   updates the UI-white constant buffer, and runs the preparation pass.
4. Preparation swaps to a private D3D11.1 state, copies presentation into a
   separate RGB10A2 snapshot, decodes that snapshot into a full-size FP16
   working texture, clears the private bindings, and restores the saved state
   (`:163-195`). The snapshot precedes this draw's original native submission.
5. The corrected draw saves PS b0 including its range, binds working RTV,
   replacement PS and UI-white b0, invokes the native indexed-draw trampoline,
   then restores b0, PS and original presentation RTV (`overlay:528-538`).
   It depends on restored/inherited IA, VS, VS constants, sampler, t0, rasterizer,
   viewport, scissor and blend state.
6. The hook unconditionally invokes the original native draw with its original
   arguments (`:567`), after the corrected draw and its restoration. Later
   unsupported commands invalidate the candidate but still execute natively.
7. Once the original callback returns, `End` runs only if candidate, started,
   and nonzero corrected-draw count all hold (`:797`). It swaps to private state,
   samples working FP16, encodes to presentation with RGB-only writes, clears
   private bindings and restores the state present **after** Dalamud returned.
   The callback summary follows this work.

The pinned renderer uploads/unmaps vertex, index and projection buffers before
its indexed commands, and binds the command texture and scissor immediately
before each draw. It also uses a scoped native state backup. Thus Begin occurs
inside a prepared render loop, whereas End occurs after that backup is disposed.
See [renderer at 15.0.3.5](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/Dalamud/Interface/ImGuiBackend/Renderers/Dx11Renderer.cs#L250)
and [state backup](https://github.com/goatcorp/Dalamud/blob/15.0.3.5/Dalamud/Interface/ImGuiBackend/Helpers/D3D11/D3D11DeviceContextStateBackup.cs#L29).

HEAD's Stage 2 background experiment completed decode and encode together in
one isolated block **before** the callback. Stage 3 separates those passes into
two state swaps spanning native rendering. The historical successful Stage 2
test supports the basic conversion concept; it does not validate these new
boundaries or the duplicate-draw restoration.

## Likely mechanisms and checks against the code

### Alternating whole transactions: strongest simple explanation for brightness flicker

`RejectComposition` is sticky only within one callback (`overlay:126`). The next
callback starts afresh. A texture/shader/target/blend mismatch, auxiliary
operation, nested callback or another-context draw can therefore produce:

```text
callback A: supported draws -> corrected working image -> commit -> corrected UI
callback B: supported prefix -> later rejection -> no End -> native PQ-space UI
callback C: supported draws -> commit -> corrected UI again
```

This does not require broken synchronization or partial pixels. Because native
SDR values blended directly into PQ have a substantially different appearance,
alternation can be visible even when UI geometry never disappears. Mode 2's
clean appearance is compatible with exactly this sequence of validation
outcomes: it never displays a corrected result.

The known unsupported blur/custom paths are examples, not a claim that blur was
enabled during the user's tests. Auxiliary hooks can also reject work on another
context on the same thread. The next bounded mode 3 capture should correlate
`committed`, `reason`, `converted_draws`, `native_indexed_draws`, and frame/runtime
with the visible bright/dim transition before choosing a rendering fix.

An `empty` callback is different from fallback with native draws: it reports no
matching indexed submissions in this scope. It cannot explain a visible native
UI frame without identifying where that UI was rendered or retained. Do not
treat every empty callback as a rejected UI frame.

### Begin and corrected-draw state: strongest candidate for actual missing geometry

Begin temporarily replaces almost the whole native pipeline at the exact point
where ImGui expects it ready. A restore failure can lose VB/IB/input layout,
projection CB, t0/s0, scissor/viewport or blend state. The first corrected draw
would then be incomplete, and the original draw may be incomplete too. Later
commands need not rebind every lost state, making symptoms depend on command
ordering and render-state resets.

There is no obvious misuse of the ordinary D3D11.1 state API in the source:
initialization requires an immediate context, selects D3D11 behavior, and owns
the previous state until restoration. Microsoft specifies that swapping saves
and restores the previous/default state; it does not restore pixels. State
swaps do not isolate active asynchronous queries, and require serialized
immediate-context access. These are limits of the isolation, not proof it is
broken here. [Microsoft state-swap contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-swapdevicecontextstate).

The corrected draw restores exactly the explicit RTV/PS/b0 changes; its gates
require other RTVs and depth view absent and no shader class instances. Saving
the b0 range avoids a simple constant-buffer-offset loss. However, that snapshot
is taken **after Begin**, so it cannot reveal state already lost by Begin.
No before/after comparison currently proves that inherited bindings survived.

RTV binding can implicitly null conflicting resource bindings. That would make
"only three bindings change" an incomplete description if an unexpected alias
existed. In the intended path the working image is private, is cleared out of
private state before restoration, and is not an ImGui texture, so no concrete
alias was found. It is a check to make if the no-writeback controls flicker,
not a reason to add blanket rebindings now.
[Microsoft OM binding/hazard semantics](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetrendertargets).

Native state isolation also does not promise to preserve another interceptor's
software bookkeeping or neutralize external hook side effects. The local
ReShade state-block source itself records a CreateDeviceContextState/RTSS
compatibility concern (`external/reshade/source/d3d11/d3d11_impl_state_block.cpp:14`).
That is an example of a possible interop class, not evidence that RTSS caused
this run. Mode 2 never creates or swaps the Stage 3 private state.

### End can expose an earlier defect without being its origin

Successful End replaces presentation RGB with the entire working image. It
does not blend the corrected UI over the already native-composited RGB.
Consequently the planned path does **not** inherently double-composite the UI.
Native alpha remains because writeback masks alpha out; nothing here infers UI
coverage from those two presentation alpha bits.

If a corrected draw submitted no pixels because of bad geometry/bindings,
`converted_draws` still increments. If preparation failed to fill part of the
working image, those texels can retain old/uninitialized values. End then makes
that bad image visible. `Draw`, `CopyResource` and state setters have no HRESULT
success result; `Begin=true` and `committed=1` are CPU control-flow/submission
markers, not pixel-completeness or GPU-completion proofs.

Both fullscreen passes set a full-resource viewport, no culling, disabled depth,
and a rasterizer with scissoring disabled. The shaders use integer pixel loads,
not a sampler; snapshot/working sizes match presentation. Therefore no missing
sampler, obvious inherited ImGui scissor, or explicit partial decode is apparent
in the intended private state. A failure needs runtime evidence.

The commit condition requires only a supported transaction with at least one
corrected draw; it does not independently prove that all visible presentation
writes were represented. Known auxiliary commands abort, but the interception
is not a complete command recorder: for example Map/Unmap and arbitrary plugin
or other-thread activity are not comprehensively modeled. Missed work **after**
the snapshot can be erased by End; already-native UI **before** the snapshot can
be baked into its background with the wrong composition semantics. These are
coverage/order hypotheses, not evidence that ordinary font sampling is wrong.

### Resource lifetime and ordering: no demonstrated need for a GPU wait

Snapshot, working texture/views, shaders, constants, private state and the
presentation RTV are persistent `ComPtr` members. Local original PS/RTV/CB
references remain alive through restoration. Reset releases resources on
presentation change and existing resize/destroy/unregister/teardown paths.
The code clears private bindings before restoring the caller so private state
does not retain an additional hidden presentation binding. The explicit
presentation RTV remains owned until reset, as intended.

Each successful Begin re-copies and re-decodes the current background even when
allocation is reused. An aborted scratch transaction is not intentionally
replayed on the next callback. Initialization failure is latched for that
presentation identity; by itself it predicts sustained fallback, not alternate
frame commits. Stale resources would need a missed lifecycle transition or
incorrect identity, neither demonstrated by the supplied mode results.

The intended command sequence is on one native immediate context:

```text
copy PQ -> decode working -> corrected draw 1 -> native draw 1
        -> corrected draw 2 -> native draw 2 -> ... -> encode working to PQ
```

Asynchronous execution does not require the CPU to wait between these dependent
commands on the same ordered context. CopyResource is explicitly a queued GPU
operation. The stack array passed to UpdateSubresource is also valid: the API
captures source data before returning. Reusing a default constant buffer through
that API is not a CPU use-after-free.
[CopyResource](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource),
[UpdateSubresource](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-updatesubresource).

If another thread uses the same immediate context, CPU serialization is required;
the addon's thread-local `active`/`observing` flags are not a lock. Likewise, a
separate device/interop consumer would need its actual handoff examined. FG-off
reproduction lowers a specifically FG-generated-frame explanation, but does not
remove every downstream wrapper. No concurrent access has been established.
Adding Flush or a GPU wait would change timing without diagnosing that problem.
[Microsoft threading contract](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro).

### What a partial transaction can and cannot explain

| Situation | Expected visible consequence under correct state restoration |
|---|---|
| Begin and some corrected draws, then recognized rejection | No End; complete native fallback. Partial FP16 contents remain invisible. |
| Different callbacks commit and fall back | Corrected/native brightness alternation; plausible flicker. |
| Every callback commits but some corrected commands render no pixels | End can erase native UI and reveal background/stale working pixels. |
| End executes with wrong target/SRV/state or later work overwrites its output | Commit counter can be true while final visible output is wrong. |
| Empty callback, no Begin | No new Stage 3 writeback; a separate explanation is needed for visible UI in that frame. |

This is a logical all-or-nothing callback transaction, not an atomic GPU
transaction or rollback of arbitrary state. Its correctness relies on full
working-image preparation, complete interception within the supported contract,
and exact native restoration.

## Smallest diagnostic split to implement next, if requested

Extend only the existing game-local restart-only control. Keep current meanings
and default unchanged. Proposed values:

| Mode | Validate | Begin once on first supported draw | Corrected draws | End after callback | Original calls |
|---|---|---|---|---|---|
| 1, existing | No | No | No | No | All retained |
| 2, existing | Yes | No | No | No | All retained |
| 4, proposed preparation-only | Yes | Yes | No | No | All retained |
| 5, proposed working-image-only | Yes | Yes | Yes | No | All retained |
| 3, existing full | Yes | Yes | Yes | Yes, existing eligibility | All retained |

Implementation boundaries are already present in `dalamud_overlay.hpp`:

- Preserve mode 2's return at `:514`.
- For mode 4, return immediately after successful Begin/start handling and before
  b0 capture/RTV substitution at `:528`. Subsequent supported draws still validate,
  see the same started flag, and return without re-running Begin. Do not clear
  candidate merely to implement the diagnostic stop.
- Mode 5 follows the existing corrected/native draw sequence completely. Gate
  post-callback End explicitly to mode 3; relying on `converted_draws==0` would
  accidentally allow mode 5 to commit.
- Do not change callback placement, original trampolines, rejection conditions,
  resource allocation/reset, state restoration, shaders, whitelist, HDR math,
  shared infrastructure, or supported rendering contract.

The split itself needs no new rendering API or changes inside `BackgroundRoundTrip`.
It needs the selector bounds, these two stop decisions, and documentation/summary
labels in the game-local diagnostic. A proposed value of 4 or 5 in the **current**
build is out of range and falls back to 3; do not use it until implemented.

Retain only the existing bounded post-callback summary. Existing
`composition_started`, `validated_draws`, `converted_draws`, `committed`,
`reason`, runtime/frame/context and outcome are sufficient for the initial split.
Give deliberate mode-4/5 stops explicit outcomes such as `prepared-no-commit`
and `converted-no-commit` when work was actually submitted; preserve genuine
`aborted`/`empty` reporting. No per-draw log, synchronous readback, new query,
unbounded trace, or resource dump is needed. Document that commit means writeback
was submitted, not that GPU pixels were verified.

Start with mode 5 versus 3, then mode 4 versus 5 if pre-writeback flicker occurs;
running the complete 2/4/5/3 ladder is also useful:

| Result | Narrowed conclusion and next evidence |
|---|---|
| Mode 4 flickers, mode 2 clean | Preparation/initialization/state restoration or its timing is sufficient to disturb native output. Compare native state before/after Begin. |
| Mode 4 clean, mode 5 flickers | Corrected-draw substitution/submission/restoration, or its additional timing, is necessary for visible pre-writeback corruption. Compare native state immediately before each original draw. |
| Modes 4/5 clean, mode 3 flickers | Writeback is necessary to expose the problem. Check commit/fallback alternation, then working-image contents and End state/output. This does **not** exonerate decode or corrected draws whose bad pixels were hidden in modes 4/5. |
| Mode 3 alternates committed/aborted with matching visual changes | Find the first rejection and why eligibility changes; avoid modifying encode math. |
| Mode 3 consistently commits during flicker | Inspect decoded background, working image after corrected draws, presentation after End, and downstream output to locate first pixel divergence. |

Keep a continuously visible plain window, same scene/settings, blur disabled and
FG off for the first comparison. Preserve the existing capture budget/rearm
behavior and archive each log before the next run overwrites it. Correlate an
actual visual sequence, not isolated summary lines; observe after logging ends.
FG need only be repeated after the decisive split. Test proposals here were not
executed.

Do **not** test "Begin, skip corrected UI, End after the callback" as a neutral
control: it deliberately overwrites native UI with the pre-UI background and
can manufacture apparent missing UI. A later background-only round-trip control
would need to finish before the original native draw, changing the boundary and
answering a different question.

If the two stop points implicate state, the next targeted diagnostic can compare
native bindings before/after the implicated block, accumulating mismatch bits
and reporting once after the callback. Include IA/VS/projection, PS/t0/s0/b0
ranges, viewport/scissor/rasterizer, OM blend/depth/RTV and relevant predicate/UAV
state; release query references promptly. Do not add that inspection everywhere
before the simpler split determines whether it is needed.

If only publication of the working image is implicated, use a separately
authorized capture of the four image boundaries rather than changing several
rendering stages at once. Distinguish a correct working image plus faulty End
from an incomplete working image that End faithfully publishes.

## Repository preservation

The pre-existing modified files remain `addon.cpp`, `dalamud_overlay.hpp` and
`dalamud_roundtrip.hpp` under `src/games/ffxiv`. Existing reports, root log,
untracked shader, diagnostic documentation, build artifacts and game files were
preserved. This investigation adds only this root Markdown report. No build or
runtime validation result is claimed for the proposed modes.
