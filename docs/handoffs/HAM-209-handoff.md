# HAM-209 — Unreal remote-selection outlines

Updated: 2026-09-14

## State

The project-owned collection, post-process material and `LVL_Demo` attachment are
implemented. The remaining acceptance gate is a live web ↔ Unreal session using
the reviewed presence implementation in [plugin PR #5](https://github.com/CYENS/looma-scene-sync-plugin/pull/5).
This work continues [viewer PR #2](https://github.com/CYENS/looma-xr-viewer-ue58/pull/2).
Neither PR is merged by this work.

| Change | Record |
| --- | --- |
| Collection, material and reproducible asset builder | `9d3fc3c` |
| Dedicated unbound volume, repeatable wiring and documentation | This handoff's change |

The viewer still records plugin pin `9aa0186`; the local validation checkout is
`67a5d31`. Advancing that gitlink belongs to the approved plugin/viewer merge
sequence. A fresh checkout at the recorded pin does not include the later plugin
presence-refresh fixes.

## What is installed

- `/Game/Presence/MPC_LoomaRemoteSelection`: eight linear RGBA vectors named
  `LoomaClient1`–`LoomaClient8` plus scalar `LoomaClientCount`. Alpha is occupancy;
  every default is zero.
- `/Game/Presence/M_LoomaRemoteSelectionOutline`: post-process material at
  **Scene Color After DOF**, the UE 5.8 name for the former Before Tonemapping
  location. Stencils 1–8 select direct borders, 129–136 select descendant borders;
  all other values are ignored.
- `/Game/Levels/LVL_Demo`: one enabled, unbound **Looma Remote Selection Outline**
  volume, actor tag `Looma.RemoteSelectionOutline`, volume and material weights 1.
  Its settings add only the material; the existing camera, exposure and other
  level settings are preserved.

The chosen widths are two render pixels for direct selections and one for
descendants. Width is independent of the web renderer's 5/2 strength values: this
material uses normalized strengths 1/0.4. Depth-hidden borders retain the owner's
colour at 25% of visible strength. The depth comparison allows a 1cm tolerance to
avoid self-occlusion from buffer precision.

`Scripts/build_remote_selection_assets.py` embeds the adjacent HLSL into the
material and saves only the two named assets. `Scripts/wire_remote_selection_outline.py`
loads/saves only `LVL_Demo` and reuses its tagged volume. Both are editor authoring
tools; neither script is needed when running the viewer. See the commands in
[README.md](../../README.md#remote-selection-outlines-ham-159--ham-209).

## Verification completed

The actual UE 5.8 D3D12/SM6 renderer was exercised in a content-only scratch
project, with a cube, a nearer occluder and a 512×512 scene capture. The material
was loaded from its saved asset; the screenshots were inspected visually and
measured against the baseline image.

| Case | Observed result |
| --- | --- |
| Each direct slot 1–8 | 1,428 coloured pixels; two pixels per edge on the centre scanline |
| Each descendant slot 129–136 | 712 coloured pixels; one pixel per edge on the centre scanline, lower brightness |
| Slot 8 occupied with client count 1 | Border present; sparse slot allocation works |
| Stencil 0, 9, 128, 137, 255 | No changed pixels versus baseline |
| Occupancy alpha 0 or client count 0 | No changed pixels versus baseline |
| Nearer mesh covers half the selected cube | Hidden half keeps the red hue and dims; visible red channel 208–216, hidden edge down to 63 |

The level wiring script was run twice against the viewer. Both runs reused
`PersistentLevel.PostProcessVolume_0`; the final level contains one tagged volume.
T3D exports before and after contain the same 16 original actors with unchanged
serialized properties after order normalization, plus that one new volume.
The saved level, material and collection were also checked in a separate viewer
process with `-NullRHI`; that check establishes asset loading and attachment, not
rendered runtime acceptance.

Local evidence from this session is retained under ignored
`Saved/PresenceValidation/`:

- `render_live3.log`, `render_live.py`, `analyze.py`.
- `AssetProject/Saved/PresenceValidation/slot_1.png`, `child_1.png`,
  `occluded.png`, and the rest of the stencil matrix.
- `level-before.t3d`, `level-after.t3d`, `compare_level.py`.
- `wiring-load4.log`, `verify_saved_wiring.py`.

These scratch files are local evidence, not repository dependencies. Image
measurement used the bundled Codex Python runtime with Pillow and NumPy.

## Details worth preserving

- A manual `SceneCapture2D` needs `always_persist_rendering_state=True` for
  post-process materials: without a view state, UE skips their blendable setup.
- `MaterialEditingLibrary.recompile_material` can return before asynchronous
  shader compilation finishes. A blank early capture is not a valid result.
  Forcing synchronous shaders during cold viewer startup triggered an engine
  assertion in `ShaderCompiler.cpp:1940`; use normal startup and wait for shaders
  before visual checks. No project render setting was changed to work around it.
- `LoomaSceneSyncSettings` has no direct `unreal.LoomaSceneSyncSettings` Python
  binding. For scratch inspection, load the class by
  `/Script/LoomaSceneSync.LoomaSceneSyncSettings`, obtain its default object and
  use its native property names with `get_editor_property` / `set_editor_property`.
- Backend configuration belongs to
  `[/Script/LoomaSceneSync.LoomaSceneSyncSettings]`. Isolated tests must override
  `BackendUrl` to their local backend rather than use the saved deployed address.
  Editing the settings default object through `set_editor_property` can persist
  those test values into `DefaultGame.ini`; preserve and restore that file when
  using temporary identities or addresses.

## Next acceptance gate

Use two browser clients and the viewer against the same isolated backend and
scene. Check selections in both directions, ownership conflicts, descendant
weighting, local-selection priority, deselection, disconnect/reconnect and an
unrelated join. Observe the viewer's normal active camera so the committed volume
attachment is exercised. Confirm colours remain correct after scene reloads and
newly loaded meshes. Record that result before calling HAM-153 complete.

After review and explicit merge approval, merge the plugin and then advance the
viewer and umbrella gitlinks. Unity is outside this acceptance scope.
