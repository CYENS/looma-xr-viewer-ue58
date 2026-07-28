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

## Status

Scaffold. Engine association `5.8`; no content yet.
