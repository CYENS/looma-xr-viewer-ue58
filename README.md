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
| `Plugins/glTFRuntimeWebP` | [CYENS/glTFRuntimeWebPLoomaXR](https://github.com/CYENS/glTFRuntimeWebPLoomaXR) | `main` |

`LoomaSceneSync` declares `glTFRuntime` in its `.uplugin` and links it as a public
module, so it will not compile without it.

```bash
git clone --recurse-submodules git@github.com:CYENS/looma-xr-viewer-ue58.git
# or, after a plain clone:
git submodule update --init --recursive
```

> [!note] `LoomaSceneSync` is developed here
> This project is the one the plugin is built and tested against. The 5.6 viewer
> ([CYENS/looma-xr-viewer](https://github.com/CYENS/looma-xr-viewer)) carries a second
> copy of the same repository, so a change that only compiles on 5.8 goes on a branch
> of the plugin, not straight to its `main`.

## Live mocap (HAM-316)

A mocap performer drives a generated, rigged character, here and in every other
client of the scene. The code is in `Source/LoomaXRViewerUE58/LiveLink/`.

- **Sources.** The viewer reads Moverse (JSON on UDP 54321) and Xsens MVN (UDP 9763)
  itself, as Live Link subjects. Any other Live Link source works too if a profile
  knows its skeleton.
- **Profiles are data.** `Config/LiveLinkProfiles.json` says, for each mocap
  skeleton, which of its bones drives which rig bone and its axes.
  Adding a vendor is adding a profile; the file documents every field.
- **Who is driven** is a `livelink` component on the scene node, set in the web
  inspector or with `Looma.LiveLink.Assign`.
- **The backend can drive a character itself** (HAM-328). Its Live Link hub reads
  mocap on the server, in named streams set up in its `/admin`, and a component can
  name one: `"source": "hub", "stream": "studio-b"`. This viewer then never drives
  that character from its own Live Link, even when it has a subject of the same
  name, because that is another performer. It shows the hub's poses instead
  (HAM-329).

| Console | What it does |
| --- | --- |
| `Looma.LiveLink.Status` | Sources, subjects, the hub's streams, and what each character follows |
| `Looma.LiveLink.Assign <subject> [name]` / `Unassign [name]` | Choose which character one of this viewer's subjects drives: the selected one, or one whose name contains `name` |
| `Looma.LiveLink.AssignStream <stream> <subject> [name]` | Have the backend's hub drive it instead, from one of its streams (`*` for any) |
| `Looma.LiveLink.Listen` / `ListenXsens` | Start a reader by hand; both start on their own |
| `Looma.LiveLink.FindFloor 0` | Trust the mocap system's zero height, not the performer's feet |

`-LoomaNoLiveLink` on the command line starts a viewer that reads no mocap and only
shows what other clients send: a second viewer on the same PC.

**Moverse's own Unreal plugin is optional and not in this repository.** The viewer's
reader takes the same stream. If you want Moverse's plugin as the Live Link source,
unpack it into `Plugins/` yourself; the folder is ignored by git, and the
`moverse-plugin` profile reads its subjects.

**Tests.** The Live Link automation tests run headless, with the editor
closed:

```powershell
$editor = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
& $editor "$PWD\LoomaXRViewerUE58.uproject" -ExecCmds="Automation RunTests Looma.LiveLink; Quit" -unattended -nullrhi -nosplash -nosound -stdout
```

> [!warning] The demo level submits a generation job on Play
> `BP_LoomaGenerationComponentDemo` in `LVL_Demo` sends a text-to-3D job to the
> backend when play begins, so every `-game` launch of this project adds one to the
> queue. Do not start the viewer in a loop or from a script without knowing that.

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
evidence, a safe isolated test launch and the remaining merge sequence.

## Status

UE 5.8 integration project. Remote-selection assets and demo-level attachment are
implemented, and live desktop web ↔ Unreal acceptance passed. The dependent
plugin merge and viewer/umbrella gitlink updates still require explicit merge
approval.
