# HAM-209 — Unreal remote-selection outlines

Updated: 2026-09-14

## State

The project-owned collection, post-process material and `LVL_Demo` attachment are
implemented. Live desktop web ↔ Unreal acceptance passed using the reviewed
presence implementation in [plugin PR #5](https://github.com/CYENS/looma-scene-sync-plugin/pull/5).
This work continues [viewer PR #2](https://github.com/CYENS/looma-xr-viewer-ue58/pull/2).
Neither PR is merged by this work.

| Change | Record |
| --- | --- |
| Collection, material and reproducible asset builder | `9d3fc3c` |
| Dedicated unbound volume, repeatable wiring and documentation | `00f7636` |

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
measurement used the bundled Python runtime with Pillow and NumPy.

## Live desktop acceptance completed

Two browser clients and the actual viewer were tested against the isolated
backend on port 8001 and web frontend on port 5174. The viewer ran SIE with D3D12
in `LVL_Demo`; the normal active camera exercised the committed volume attachment.
The viewport screenshot shows the purple selected parent and lighter descendant
edges. The following checks passed:

| Case | Observed result |
| --- | --- |
| Web client A selects the parent | Its `#6047e1` colour reaches the linear MPC; parent stencil 1, child/grandchild 129, unrelated nodes 0 |
| Unreal selects the other node | The web Outliner displays Unreal's `#47e160` selection colour |
| Unreal locally selects the parent | The parent's remote stencil and descendant hints clear, along with the remote MPC occupancy |
| Client B claims first, followed by A | B remains the owner, including after unrelated client C joins |
| B deselects | The claim passes to A |
| Switch to another scene reusing the node IDs | Groups and stencils clear; returning to the original scene rehydrates A's claim |
| Run `Looma.Reconnect` | Reconnect succeeds with no stale claims; the existing reconnect behavior returns to the performance's default `untitled-scene` |
| Return with `Looma.Scene ham-153-presence-acceptance` | The connected roster rehydrates A's parent and descendant borders correctly |
| Close client A | All remaining remote stencils and MPC occupancy clear |

Evidence is retained in the umbrella checkout's ignored
`.worktrees/ham-153-validation/` directory:

- `ue-inbound.png`, `ue-inbound-evidence.json`.
- `web-unreal-selection.png`, `ue-outbound-evidence.json`.
- `ue-local-priority-evidence.json`.
- `ue-contested-evidence.json`, `ue-unrelated-join-evidence.json`,
  `ue-handover-evidence.json`.
- `ue-scene-reset-evidence.json`, `ue-scene-return-evidence.json`.
- `ue-reconnect-evidence.json`, `ue-reconnect-hydration-evidence.json`.
- `ue-peer-disconnect-evidence.json`.

The test editor processes were stopped and `Config/DefaultGame.ini` was restored
exactly. The isolated frontend/backend remain available on 5174/8001 for follow-up.
No additional manual gate remains for these desktop acceptance checks.

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
  use its native property names with `get_editor_property`.
- Backend configuration belongs to
  `[/Script/LoomaSceneSync.LoomaSceneSyncSettings]`. Isolated tests must override
  `BackendUrl` to their local backend rather than use the saved deployed address.
  Editing the settings default object through `set_editor_property` can persist
  those test values into `DefaultGame.ini`; preserve and restore that file when
  that has happened. Use launch overrides for temporary addresses, as below,
  instead of editing the settings default object.

## Repeating the isolated desktop check

With the isolated backend running on 8001, launch from the viewer repository:

```powershell
$editor = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$project = Join-Path (Get-Location) 'LoomaXRViewerUE58.uproject'
& $editor $project '-ini:Game:[/Script/LoomaSceneSync.LoomaSceneSyncSettings]:BackendUrl=http://127.0.0.1:8001' -saveddirsuffix=HAM153Acceptance
```

Open `LVL_Demo`, click **Play**, then open the console and enter
`Looma.Scene ham-153-presence-acceptance`. Join that scene from the isolated web
frontend at `http://localhost:5174`, using separate browser sessions for the two
clients. Use `Looma.Room` and `Looma.Claims` to inspect presence while checking the
normal viewer camera. After `Looma.Reconnect`, explicitly select the fixture scene
again: reconnect currently returns to the performance's default scene.

The Saved-directory suffix isolates test session/cache files, and the launch
override keeps the deployed backend address in `DefaultGame.ini` unchanged.

## Remaining merge gate

After review and explicit merge approval, merge the plugin and then advance the
viewer and umbrella gitlinks. Unity is outside this acceptance scope.
