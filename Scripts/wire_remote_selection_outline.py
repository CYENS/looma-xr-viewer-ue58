"""Attach the existing remote-selection material to LVL_Demo, once.

Run in a separate Unreal editor commandlet, with PythonScriptPlugin and
EditorScriptingUtilities enabled. This loads and saves LVL_Demo; use the command
in README.md with the interactive editor closed. Other levels are not changed.
"""

import unreal


LEVEL = "/Game/Levels/LVL_Demo"
MATERIAL = "/Game/Presence/M_LoomaRemoteSelectionOutline"
COLLECTION = "/Game/Presence/MPC_LoomaRemoteSelection"
TAG = "Looma.RemoteSelectionOutline"
LABEL = "Looma Remote Selection Outline"


def find_volume(actors):
    matches = [actor for actor in actors if actor.actor_has_tag(TAG)]
    if len(matches) > 1:
        raise RuntimeError("Multiple remote-selection volumes found; resolve duplicate tags before wiring")
    if matches and not isinstance(matches[0], unreal.PostProcessVolume):
        raise RuntimeError("Remote-selection tag belongs to a non-volume actor")
    return matches[0] if matches else None


def verify(volume, material):
    if not volume.get_editor_property("enabled") or not volume.get_editor_property("unbound"):
        raise RuntimeError("Remote-selection volume must be enabled and unbound")
    if volume.get_editor_property("blend_weight") != 1.0:
        raise RuntimeError("Remote-selection volume must have full blend weight")
    entries = volume.get_editor_property("settings").get_editor_property("weighted_blendables").get_editor_property("array")
    matches = [entry for entry in entries if entry.get_editor_property("object") == material]
    if len(matches) != 1 or matches[0].get_editor_property("weight") != 1.0:
        raise RuntimeError("Expected exactly one outline material with weight 1")


def wire():
    material = unreal.load_asset(MATERIAL)
    collection = unreal.load_asset(COLLECTION)
    if not isinstance(material, unreal.Material) or not isinstance(collection, unreal.MaterialParameterCollection):
        raise RuntimeError("Build the remote-selection material and collection first")
    if material.get_editor_property("material_domain") != unreal.MaterialDomain.MD_POST_PROCESS:
        raise RuntimeError("Remote-selection material must use the post-process domain")

    world = unreal.EditorLoadingAndSavingUtils.load_map(LEVEL)
    if not world:
        raise RuntimeError(f"Could not load {LEVEL}")
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = actor_subsystem.get_all_level_actors()
    volume = find_volume(actors)
    if not volume:
        # An actor label alone is not ownership. Do not silently adopt or overwrite
        # a similarly named volume which somebody configured by hand.
        if any(actor.get_actor_label() == LABEL for actor in actors):
            raise RuntimeError("Outline label already exists without its ownership tag")
        volume = actor_subsystem.spawn_actor_from_class(unreal.PostProcessVolume, unreal.Vector(0, 0, 0))
        volume.set_actor_label(LABEL)
        volume.set_editor_property("tags", [unreal.Name(TAG)])

    # This dedicated volume supplies only a blendable. Other volumes and their
    # exposure, colour grading, camera and rendering settings retain their values.
    volume.set_editor_property("enabled", True)
    volume.set_editor_property("unbound", True)
    volume.set_editor_property("blend_weight", 1.0)
    volume.add_or_update_blendable(material, 1.0)
    verify(volume, material)
    if not unreal.EditorLoadingAndSavingUtils.save_map(world, LEVEL):
        raise RuntimeError(f"Could not save {LEVEL}")
    unreal.log(f"PRESENCE_VOLUME_OK: {volume.get_path_name()} enabled, unbound, material weight 1")
    return volume


if __name__ == "__main__":
    wire()
