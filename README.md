# LoomaXRViewerUE58

Unreal Engine **5.8** port of the LoomaXR viewer: mirrors the web app's scene in
real time through the `LoomaSceneSync` plugin (spawn/move/delete both ways; meshes
stream at runtime from the demo backend's datalake).

The module is `LoomaXRViewerUE58`; `Content/` contains the demo level, its
Blueprints and the remote-selection outline assets. The 5.6 project lives in
[CYENS/looma-xr-viewer](https://github.com/CYENS/looma-xr-viewer); the two are
separate repos on purpose so the 5.6 build keeps working while 5.8 is shaken out.

Part of the [LoomaXR](https://github.com/CYENS/hamlet-loomaxr) project.

## Plugins

All three are git submodules under `Plugins/`:

| Path | Repository | Branch |
| --- | --- | --- |
| `Plugins/LoomaSceneSync` | [CYENS/looma-scene-sync-plugin](https://github.com/CYENS/looma-scene-sync-plugin) | `main` |
| `Plugins/glTFRuntime` | [CYENS/glTFRuntimeLoomaXR](https://github.com/CYENS/glTFRuntimeLoomaXR) | `master` |
| `Plugins/glTFRuntimeWebP` | [CYENS/glTFRuntimeWebPLoomaXR](https://github.com/CYENS/glTFRuntimeWebPLoomaXR) | `master` |

`LoomaSceneSync` declares `glTFRuntime` in its `.uplugin` and links it as a public
module, so it will not compile without it.

```bash
git clone --recurse-submodules git@github.com:CYENS/looma-xr-viewer-ue58.git
# or, after a plain clone:
git submodule update --init --recursive
```

> [!warning] The plugin pins are 5.6-era
> All three submodules are pinned at commits authored against UE 5.6. Opening this
> project in 5.8 is expected to prompt for a plugin rebuild/upgrade. Any edits Unreal
> makes land inside the submodules — and `LoomaSceneSync` is **shared with the 5.6
> viewer and the asset demo**, so branch it before committing 5.8-specific changes
> rather than pushing them to `main`.

## Remote selection outlines (HAM-159 / HAM-209)

`LoomaSceneSync` marks what everybody else in the room has selected; it never draws.
It writes a slot number into the custom stencil buffer and publishes the matching
colours into a Material Parameter Collection, and the project turns the two into an
outline. The plugin's `.uplugin` sets `"CanContainContent": false`, so it can ship no
material and no collection: the drawing half has to live here.

The project supplies both assets and attaches the material to `LVL_Demo` through
the **Looma Remote Selection Outline** post-process volume. It is enabled, unbound
and uses material weight 1, so it applies to the active camera throughout the level.
The volume has no exposure or colour-grading overrides.

| Config, already set | Where |
| --- | --- |
| `r.CustomDepth=3` (*Enabled with Stencil*) | `Config/DefaultEngine.ini` |
| `RemoteSelectionCollection` → `/Game/Presence/MPC_LoomaRemoteSelection` | `Config/DefaultGame.ini` |

`MPC_LoomaRemoteSelection` contains `LoomaClient1` … `LoomaClient8` vectors
(linear RGB, alpha 1 for occupied / 0 for free) and the scalar `LoomaClientCount`.
All defaults are zero. Slots may have gaps; the material checks each slot's alpha
rather than treating the client count as a highest slot number.

`M_LoomaRemoteSelectionOutline` runs at **Scene Color After DOF**, UE 5.8's name
for the former *Before Tonemapping* location. Stencils `1`–`8` draw direct
selections with a two-render-pixel outline; `129`–`136` draw descendants with a
one-render-pixel outline. These widths are separate from the web effect's strength
5/2: the material uses relative blend strengths 1/0.4. Occluded edges keep the
owner's hue at one quarter of their visible strength. Zero, other stencil values
and unoccupied slots draw nothing.

The `.uasset` and `.umap` files are committed through Git LFS; a normal checkout
needs no manual material authoring. Run `git lfs pull` if they are still pointer
files. To rebuild the two assets or restore the level attachment, close the editor
and run these commands from this repository:

```powershell
$editor = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$project = Join-Path (Get-Location) 'LoomaXRViewerUE58.uproject'
& $editor $project -run=pythonscript "-script=$PWD\Scripts\build_remote_selection_assets.py" -EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities -AllowCommandletRendering -RenderOffscreen -Unattended -NoSound -SCCProvider=None
& $editor $project -run=pythonscript "-script=$PWD\Scripts\wire_remote_selection_outline.py" -EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities -NullRHI -Unattended -NoSound -SCCProvider=None
```

The first script rebuilds only the two named assets and embeds the adjacent HLSL
file into the material, so the HLSL file is not needed at runtime. The second loads
and saves only `LVL_Demo`; it finds its dedicated volume by the actor tag
`Looma.RemoteSelectionOutline`, so running it again does not create a duplicate.
It preserves other actors and post-process volumes. For another level, add this
material to an enabled unbound post-process volume in that level.

Shader compilation may continue after Python returns. Inspect the completed
editor log and wait for shaders before checking the image. `-NullRHI` can verify
the level attachment and asset loading; it cannot verify rendering.

`Plugins/LoomaSceneSync/README.md`, *Wiring the outline*, is the normative version of
both, and `Looma.Room` / `Looma.Claims` in the console report the same state as text,
which is how to check the protocol half without either asset existing yet.

See [the HAM-209 handoff](docs/handoffs/HAM-209-handoff.md) for verification
evidence and the remaining cross-client acceptance gate.

## Status

UE 5.8 integration project. Remote-selection assets and demo-level attachment are
implemented; live web ↔ Unreal acceptance and the dependent plugin merge remain
separate review gates.
