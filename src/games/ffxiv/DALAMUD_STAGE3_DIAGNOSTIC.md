# Dalamud direct-PQ development path

The retained implementation targets the inspected ReShade 6.8.0.2155 binary,
Dalamud 15.0.3.5, and the main single-buffer HDR10 presentation target. It wraps
Dalamud's owned overlay callback without changing callback ownership/order,
tags verified native ImGui shaders, and substitutes PS/b0 when the ordinary
pixel shader is bound through native PSSetShader (slot 9). DrawIndexed is no
longer intercepted. Substitution lasts until the next PS bind, a rejection, or
callback exit. The original PS and D3D11.1 constant-buffer range are restored
only while those bindings are still owned by RenoDX; caller replacements win.

Direct PQ is now the default. The legacy restart-only setting remains:

```ini
[renodx-ffxiv-dalamud]
Stage3DiagnosticMode=6
```

Use 1 for native pass-through. A missing key or any value other than 1 selects
direct PQ; the former validation/FP16 modes 2–5 have been removed. Restart FFXIV
after changing this setting. This control does not select Dalamud's own
ReShade handling mode.

The shader forms vertex color times texture, decodes sRGB, converts BT.709 to
BT.2020 and encodes PQ using RenoDX UI Brightness (toneMapUINits). Straight alpha,
the original render target, geometry, texture/sampler and blend state remain.
PQ-space blending is intentional and can darken text edges/translucent panels.
The shader, blend, native context/device and presentation checks gate the PS
bind. The pinned renderer binds t0 after PS and after clearing GS/HS/DS, so a
narrow PSSetShaderResources guard (slot 8) checks the actual SDR UNORM texture
and shader stages, and rechecks target/blend/VS before each ordinary command.
This relies on the pinned renderer's texture-before-command ordering, without
detouring draw submissions. A failed check or unsupported auxiliary operation
restores the active substitution and stops further correction in that callback;
earlier corrected draws cannot be rolled back. All original operations execute.

There are no FP16 working resources, background copies, duplicate UI draws or
fullscreen writeback. Capture counters/groups, per-frame summaries, caller
traces and overlay-toggle rearming have been removed. Only initialization/
compatibility warnings remain. Direct-PQ resource initialization failures fall
back to native drawing until reset/restart.

Coverage of the actual /xlsettings window remains unresolved. Successful
compilation does not establish that this window reaches the retained hook.
Historical investigation reports describe older snapshots and removed modes.

With FFXIV closed, build only the game target:

```powershell
cmake --build --preset clang-x64-debug --target ffxiv
git diff --check
```

Deploy build/Debug/renodx-ffxiv.addon64 to H:/Games/FFXIV/game, then restart.
The direct-PQ shader embed is build/ffxiv.include/embed/0xC046E20B.h.
Compare default/6 with pass-through/1 using continuously visible /xlsettings,
blur off, then check opaque colors, text, transparency and UI Brightness. Verify
ReShade's own overlay, detached SDR viewports, blur/custom shaders and FG off/on
for state leakage. Runtime coverage of the new binding hooks still needs this
in-game validation; compilation alone does not establish that coverage.
