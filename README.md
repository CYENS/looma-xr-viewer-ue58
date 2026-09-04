# LoomaXRViewerUE58

Unreal Engine **5.8** port of the LoomaXR viewer: mirrors the web app's scene in
real time through the `LoomaSceneSync` plugin (spawn/move/delete both ways; meshes
stream at runtime from the demo backend's datalake).

This is a **fresh 5.8 project shell**, not a copy of the 5.6 viewer — the module is
`LoomaXRViewerUE58` and `Content/` is currently empty. The 5.6 project lives in
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

## Remote selection outlines (HAM-159)

`LoomaSceneSync` marks what everybody else in the room has selected; it never draws.
It writes a slot number into the custom stencil buffer and publishes the matching
colours into a Material Parameter Collection, and the project turns the two into an
outline. The plugin's `.uplugin` sets `"CanContainContent": false`, so it can ship no
material and no collection: the drawing half has to live here.

This repo supplies the configuration. Two things it cannot supply are assets, and
they have to be authored in the editor once:

| Config, already set | Where |
| --- | --- |
| `r.CustomDepth=3` (*Enabled with Stencil*) | `Config/DefaultEngine.ini` |
| `RemoteSelectionCollection` → `/Game/Presence/MPC_LoomaRemoteSelection` | `Config/DefaultGame.ini` |

**1. The parameter collection.** Create a Material Parameter Collection at exactly
`Content/Presence/MPC_LoomaRemoteSelection`, since that is the path the setting above
already points at. It needs eight vector parameters `LoomaClient1` … `LoomaClient8`,
whose alpha is the occupancy flag rather than an opacity, and one scalar
`LoomaClientCount`. Until it exists the plugin logs one line and publishes no colours;
the stencil is written either way, so nothing else breaks.

**2. The post-process material.** Blendable location *Before Tonemapping*, reading
`SceneTexture:CustomStencil`. `0` means no border, `1`-`8` are slot *n* thick (a node
that client selected and won), `129`-`136` are slot *n* thin (a descendant of one of
those, the "this moves with it" hint). So `IsChild = Stencil > 128` and
`Slot = Stencil - (IsChild ? 128 : 0)`, then look up `LoomaClient<Slot>`. Match the web
client's weighting so the two viewers agree: thick at strength 5, thin at 2, and dim
the occluded half of an edge rather than recolouring it.

`Plugins/LoomaSceneSync/README.md`, *Wiring the outline*, is the normative version of
both, and `Looma.Room` / `Looma.Claims` in the console report the same state as text,
which is how to check the protocol half without either asset existing yet.

## Status

Scaffold. Engine association `5.8`; no content yet.
