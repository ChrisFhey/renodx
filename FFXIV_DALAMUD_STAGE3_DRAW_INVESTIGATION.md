# Stage 3 draw-count investigation

2026-09-27; current working tree on `feature/hdr10`, HEAD `3856f13e`.
Investigation only. Only this report was added; no source/configuration edits
or build.

**Conclusion:** there is no first-draw-only conversion bug in the inspected
control flow. The confirmed log records only **one intercepted native indexed
draw** in each committed callback, and that draw is converted. It does not show
additional observed draws silently reverting to PQ inside those callbacks.
Whether the visible UI uses one batch or other draws escape the observed scope
is not established by these counters.

## Evidence

The user confirmed `ReShade(6).log` means
`H:/Games/FFXIV/game/ReShade.log`. The inspected file is 169,695 bytes, SHA256
`98F0F3717BD798B35BA46C74D2A9BE1A8AF1C5DC5B6DA32F1A3FBA625E93B096`.
It contains the current `STAGE3_DIAGNOSTIC` summaries, rather than the older
`IMGUI_LINEAR_DRAW`/`IMGUI_COMMIT` messages.

Of 120 bounded summaries, **two commit and 118 are empty; none reports rejection**.
Lines 916–917, frames 1197–1198, both report:

```text
mode=3 native_indexed_draws=1 other_context_draws=0 presentation_draws=1
validated_draws=1 converted_draws=1 outcome=committed reason=none
composition_started=1 committed=1 effects_finished_this_frame=1
```

The subsequent captured frames 1199–1256 are empty. These windows do not cover
the whole session, and there is no synchronized visual evidence identifying
which UI was present in them. This capture supports intermittent correction
activity, **not observed alternation between commits and rejected callbacks**.
An empty callback is not evidence that a native UI draw occurred there.

## Why the count is one

In [dalamud_overlay.hpp](src/games/ffxiv/dalamud_overlay.hpp):

- `DrawLinearImGui` runs for every observed matching-context indexed draw
  (`:544-570`). `observing` is cleared before forwarding the original call and
  before the next ordinary renderer call; it is not left enabled after draw one.
- `composition_started` gates only Begin (`:517-525`). Corrected submission and
  `++converted_draws` are outside that block (`:531-541`). Every later supported
  nonzero draw would increment the count again.
- A later unsupported observed draw/auxiliary operation clears the candidate.
  That prevents the entire callback's End (`:803`), rather than committing just
  its supported first draw.
- The retained pre-diagnostic source in
  `build/dalamud_overlay.before-flicker-diagnostic.hpp:631` logs
  `IMGUI_LINEAR_DRAW` only when `converted_draws == 1`. That limits messages,
  not conversions. Its commit message prints the accumulated count.

One draw can contain many triangles/glyphs; it does not mean one widget.
The log lacks index counts and draw-data contents, so it cannot distinguish a
large same-texture UI batch from a small incidental draw. The exact reason only
one native submission appears in these callbacks remains unproven.

## Do native draws survive or overwrite correction?

All original calls still execute, including the original counterpart of every
corrected draw. Their normal ordering is:

```text
Begin -> corrected draw 1 to FP16 -> original draw 1 to PQ
      -> [additional corrected/original pairs, if observed]
      -> original callback returns -> End replaces presentation RGB
```

Thus originals temporarily render through the native PQ path, but cannot
overwrite the **later** End within the same synchronous callback. End disables
blending and replaces RGB; it does not blend two UI copies. A later recognized
unsupported command preserves native output by cancelling End entirely.

Draws outside the thread-local active callback, on unobserved contexts/threads
or through unobserved entry points are a different question. Native pixels
already present before Begin are included in the background snapshot; missing
work after that snapshot can be erased by End; work after End can overwrite it.
The present counters do not demonstrate any such draw. The existing
`post_draw_seen` observation identifies a ReShade caller, not Dalamud ownership,
and does not establish a later Dalamud overwrite.

**Flicker correlation:** if the affected UI was continuously visible during
these captured empty callbacks, its rendering is not represented by the
in-callback indexed counts. Occasional correction/writeback of an incidental
draw, while the main UI remains native elsewhere, would explain both brief
flashes and a predominantly radioactive appearance. Native UI already in
Begin's snapshot would retain its wrong appearance through decode/re-encode.
This makes incomplete coverage of the visible UI a concrete lead, not proof of
which draw was missed. The log cannot identify the sole draw as a cursor, text,
or the whole UI, nor prove an after-End overwrite without visual/draw correlation.

## Smallest next evidence

With the affected window continuously visible, correlate the existing bounded
summaries with its actual appearance. If it remains visible during empty
callbacks, establish its rendering location before changing the shader.

The narrow next instrumentation is a bounded count of verified ImGui indexed
submissions **before, inside and after** the wrapped callback through the
presentation boundary, including context/target identity and total index count.
Accumulate counters and emit a summary, not per-draw logs. Account explicitly
for the thread-local scope; do not treat zero in-scope counts as zero device-wide
rendering. If no outside drawing is found, inspect the FP16 image and immediate
post-End image: a commit marker proves submission, not correct pixels.

No whitelist expansion, removal of original draws, or first-draw gate fix is
justified by this log.
