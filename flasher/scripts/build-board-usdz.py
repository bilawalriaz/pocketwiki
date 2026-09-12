"""Import the tessellated ESP board, restore its visual materials, and export USDZ.

The source OBJ is a tessellated export of an ESP32-S3-WROOM-1 assembly. That
CAD source is not stored in this repository, so pass your own export. Run with
Blender, for example:
  Blender --background --factory-startup --python scripts/build-board-usdz.py -- \
    my-board.obj Sources/ESPFlashGUI/Resources/ESP32Board.usdz
"""

import bpy
import re
import sys
from pathlib import Path


def make_material(name, color, metallic=0.0, roughness=0.45):
    material = bpy.data.materials.new(name)
    material.diffuse_color = (*color, 1.0)
    material.use_nodes = True
    shader = material.node_tree.nodes.get("Principled BSDF")
    shader.inputs["Base Color"].default_value = (*color, 1.0)
    shader.inputs["Metallic"].default_value = metallic
    shader.inputs["Roughness"].default_value = roughness
    return material


def mesh_number(obj):
    match = re.search(r"mesh_(\d+)", obj.name)
    return int(match.group(1)) if match else -1


def set_material(obj, material):
    obj.data.materials.clear()
    obj.data.materials.append(material)


args = sys.argv[sys.argv.index("--") + 1 :]
source = Path(args[0]).resolve()
destination = Path(args[1]).resolve()
destination.parent.mkdir(parents=True, exist_ok=True)

bpy.ops.wm.obj_import(filepath=str(source), forward_axis="Y", up_axis="Z")

materials = {
    "pcb": make_material("PCB — charcoal solder mask", (0.018, 0.024, 0.022), 0.0, 0.32),
    "plastic": make_material("Component — black epoxy", (0.025, 0.030, 0.028), 0.0, 0.38),
    "gold": make_material("Contacts — gold", (0.82, 0.55, 0.12), 0.78, 0.23),
    "silver": make_material("Connectors — brushed steel", (0.62, 0.66, 0.65), 0.72, 0.28),
    "shield": make_material("ESP32 shield — warm nickel", (0.54, 0.53, 0.45), 0.66, 0.31),
    "white": make_material("LED and switch ceramic", (0.76, 0.80, 0.78), 0.06, 0.36),
    "yellow": make_material("Tantalum capacitor", (0.84, 0.67, 0.08), 0.08, 0.34),
}

for obj in [item for item in bpy.context.scene.objects if item.type == "MESH"]:
    number = mesh_number(obj)
    size = obj.dimensions
    max_xy = max(size.x, size.y)

    chosen = materials["plastic"]
    if number in {0, 261}:
        chosen = materials["pcb"]
    elif number == 333:
        chosen = materials["shield"]
    elif number in {422, 436, 450, 452}:
        chosen = materials["silver"]
    elif 334 <= number <= 356 or 378 <= number <= 400:
        chosen = materials["gold"]
    elif 423 <= number <= 449:
        chosen = materials["plastic"]
    elif number in {451, 453}:
        chosen = materials["plastic"]
    elif 454 <= number <= 487:
        chosen = materials["white"] if number in {462, 475} else materials["silver"]
    elif number in {488, 492, 496, 499, 502, 505, 506}:
        chosen = materials["plastic"]
    elif 489 <= number <= 522:
        chosen = materials["gold"]
    elif number in {523, 524}:
        chosen = materials["yellow"]
    elif 525 <= number <= 579:
        # These multipart SMDs are ordered as terminals, body, and top marker.
        part = (number - 525) % 5
        chosen = materials["silver"] if part in {0, 3, 4} else materials["plastic"]
    elif 580 <= number <= 586:
        chosen = materials["white"]
    elif size.z <= 0.03 and max_xy < 10.0:
        # The STEP exporter emitted pads and board artwork as thin bodies.
        chosen = materials["gold"]

    set_material(obj, chosen)

# The STEP coordinates are millimetres. Converting once here gives the USDZ
# conventional metre scale while the SceneKit view still normalises its bounds.
for obj in bpy.context.scene.objects:
    if obj.type == "MESH":
        obj.scale = (0.001, 0.001, 0.001)

bpy.ops.wm.usd_export(
    filepath=str(destination),
    selected_objects_only=False,
    export_materials=True,
    convert_orientation=True,
    export_global_forward_selection="Y",
    export_global_up_selection="Z",
)

print(f"exported {destination} ({destination.stat().st_size} bytes)")
