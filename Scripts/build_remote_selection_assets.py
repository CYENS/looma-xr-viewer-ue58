"""Rebuild HAM-209's project-owned assets in the Unreal 5.8 Python editor.

Run with UnrealEditor-Cmd <project> -run=pythonscript -script=<this file>
-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities -AllowCommandletRendering
-RenderOffscreen -Unattended -SCCProvider=None
A real RHI is required for shader compilation. Compilation can finish after this
script returns; inspect the completed editor log and render the saved material.
The matching HLSL is embedded in the saved material: it is not a runtime file.
This only saves the two named assets; it does not modify or save an open level.
"""

from pathlib import Path

import unreal


ASSET_DIR = "/Game/Presence"
COLLECTION_NAME = "MPC_LoomaRemoteSelection"
MATERIAL_NAME = "M_LoomaRemoteSelectionOutline"
EDIT = unreal.MaterialEditingLibrary


def asset(name, asset_class, factory_class):
    path = f"{ASSET_DIR}/{name}"
    existing = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if existing:
        if not isinstance(existing, asset_class):
            raise RuntimeError(f"{ASSET_DIR}/{name} has an unexpected asset type")
        return existing
    return unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, ASSET_DIR, asset_class, factory_class()
    )


def build():
    collection = asset(
        COLLECTION_NAME, unreal.MaterialParameterCollection,
        unreal.MaterialParameterCollectionFactoryNew,
    )
    # Keep parameter IDs on rebuild: other materials may already reference them.
    previous = {
        str(p.get_editor_property("parameter_name")): p
        for p in collection.get_editor_property("vector_parameters")
    }
    vectors = []
    for slot in range(1, 9):
        name = f"LoomaClient{slot}"
        parameter = previous.get(name, unreal.CollectionVectorParameter())
        parameter.set_editor_property("parameter_name", name)
        parameter.set_editor_property("default_value", unreal.LinearColor(0, 0, 0, 0))
        vectors.append(parameter)
    scalars = collection.get_editor_property("scalar_parameters")
    count = next((p for p in scalars if str(p.get_editor_property("parameter_name")) == "LoomaClientCount"),
                 unreal.CollectionScalarParameter())
    count.set_editor_property("parameter_name", "LoomaClientCount")
    count.set_editor_property("default_value", 0.0)
    collection.set_editor_property("vector_parameters", vectors)
    collection.set_editor_property("scalar_parameters", [count])
    if not unreal.EditorAssetLibrary.save_loaded_asset(collection, False):
        raise RuntimeError("Could not save the remote-selection collection")

    material = asset(MATERIAL_NAME, unreal.Material, unreal.MaterialFactoryNew)
    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_POST_PROCESS)
    # UE 5.8 renamed the old Before Tonemapping location to Scene Color After DOF.
    material.set_editor_property("blendable_location", unreal.BlendableLocation.BL_SCENE_COLOR_AFTER_DOF)
    EDIT.delete_all_material_expressions(material)
    custom = EDIT.create_material_expression(material, unreal.MaterialExpressionCustom, -200, 0)
    custom.set_editor_property("description", "Remote selection: stencil slots, occupancy, depth")
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    custom.set_editor_property("code", Path(__file__).with_suffix(".hlsl").read_text())
    names = ["SceneColor", "Stencil", "CustomDepth", "SceneDepth", "ClientCount"]
    names += [f"Client{slot}" for slot in range(1, 9)]
    inputs = []
    for name in names:
        entry = unreal.CustomInput()
        entry.set_editor_property("input_name", name)
        inputs.append(entry)
    custom.set_editor_property("inputs", inputs)

    def connect(node, input_name, output=""):
        if not EDIT.connect_material_expressions(node, output, custom, input_name):
            raise RuntimeError(f"Could not connect {input_name}")

    # Explicit scene-texture nodes tell the compiler which buffers the HLSL reads.
    for index, (name, texture) in enumerate([
        ("SceneColor", unreal.SceneTextureId.PPI_POST_PROCESS_INPUT0),
        ("Stencil", unreal.SceneTextureId.PPI_CUSTOM_STENCIL),
        ("CustomDepth", unreal.SceneTextureId.PPI_CUSTOM_DEPTH),
        ("SceneDepth", unreal.SceneTextureId.PPI_SCENE_DEPTH),
    ]):
        node = EDIT.create_material_expression(material, unreal.MaterialExpressionSceneTexture, -800, index * 180)
        node.set_editor_property("scene_texture_id", texture)
        connect(node, name, "Color")

    for index, name in enumerate(["LoomaClientCount"] + [f"LoomaClient{s}" for s in range(1, 9)]):
        node = EDIT.create_material_expression(material, unreal.MaterialExpressionCollectionParameter, -1200, index * 180)
        node.set_editor_property("collection", collection)
        node.set_editor_property("parameter_name", name)
        connect(node, "ClientCount" if index == 0 else f"Client{index}")

    if not EDIT.connect_material_property(custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        raise RuntimeError("Could not connect the outline to emissive colour")
    errors = EDIT.recompile_material(material)
    if errors:
        raise RuntimeError("Outline shader compilation failed: " + "\n".join(errors))
    if not unreal.EditorAssetLibrary.save_loaded_asset(material, False):
        raise RuntimeError("Could not save the outline material")
    unreal.log("PRESENCE_ASSETS_SAVED: collection and outline saved; no current material errors")
    return collection, material


if __name__ == "__main__":
    build()
