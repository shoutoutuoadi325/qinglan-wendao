"""Original sanctuary kit. Blender 4.5 LTS --background --python; no gameplay collision."""

import bpy, math, random, json, sys, argparse
from pathlib import Path
from mathutils import Vector
from mathutils import noise

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument("--render", action="store_true")
a = p.parse_args(sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else [])
OUT = ROOT / "assets/original/environment"
OUT.mkdir(parents=True, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
COLORS = {
    "RoofTile": (0.075, 0.10, 0.115),
    "Wood": (0.12, 0.045, 0.025),
    "Stone": (0.30, 0.34, 0.32),
    "Leaf": (0.045, 0.18, 0.065),
    "Bamboo": (0.19, 0.26, 0.08),
    "Bronze": (0.43, 0.27, 0.085),
    "Glow": (1, 0.39, 0.085),
    "Ground": (0.43, 0.45, 0.44),
}
MATS = {}
for name, c in COLORS.items():
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    s = m.node_tree.nodes.get("Principled BSDF")
    s.inputs["Base Color"].default_value = (*c, 1)
    s.inputs["Roughness"].default_value = 0.36 if name == "Bronze" else 0.72
    s.inputs["Metallic"].default_value = 0.72 if name == "Bronze" else 0
    if name == "Glow":
        s.inputs["Emission Color"].default_value = (*c, 1)
        s.inputs["Emission Strength"].default_value = 2
    else:
        n = m.node_tree.nodes.new("ShaderNodeTexNoise")
        n.inputs["Scale"].default_value = 18
        b = m.node_tree.nodes.new("ShaderNodeBump")
        b.inputs["Strength"].default_value = 0.16
        b.inputs["Distance"].default_value = 0.035
        m.node_tree.links.new(n.outputs["Fac"], b.inputs["Height"])
        m.node_tree.links.new(b.outputs["Normal"], s.inputs["Normal"])
    # Reuse the licensed in-game PBR sets in the asset preview. FBX carries
    # material slot names; the Unreal bootstrap binds the same texture sets.
    sets = {
        "Stone": "cliff_side",
        "Wood": "dark_wooden_planks",
        "RoofTile": "grey_roof_tiles",
        "Bamboo": "bamboo_veneer",
        "Ground": "cobblestone_01",
    }
    if name in sets:
        set_id = sets[name]
        folder = ROOT / ".local/assets-raw/polyhaven/textures" / set_id
        coord = m.node_tree.nodes.new("ShaderNodeTexCoord")
        scale = m.node_tree.nodes.new("ShaderNodeVectorMath")
        scale.operation = "SCALE"
        scale.inputs[3].default_value = 0.45
        m.node_tree.links.new(coord.outputs["Object"], scale.inputs[0])
        for token, socket in [("diff", "Base Color"), ("rough", "Roughness")]:
            path = folder / (set_id + "_" + token + "_2k.jpg")
            if path.exists():
                tex = m.node_tree.nodes.new("ShaderNodeTexImage")
                tex.image = bpy.data.images.load(str(path))
                tex.projection = "BOX"
                tex.projection_blend = 0.25
                if token == "rough":
                    tex.image.colorspace_settings.name = "Non-Color"
                m.node_tree.links.new(scale.outputs["Vector"], tex.inputs["Vector"])
                m.node_tree.links.new(tex.outputs["Color"], s.inputs[socket])
    MATS[name] = m
parts = []


def finish(o, mat, bevel=0):
    o.data.materials.append(MATS[mat])
    if bevel:
        mod = o.modifiers.new("Crafted edge", "BEVEL")
        mod.width = bevel
        mod.segments = 3
        bpy.context.view_layer.objects.active = o
        bpy.ops.object.modifier_apply(modifier=mod.name)
    for f in o.data.polygons:
        f.use_smooth = True
    parts.append(o)
    return o


def box(c, size, mat, bevel=0.025):
    bpy.ops.mesh.primitive_cube_add(size=1, location=c)
    o = bpy.context.object
    o.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return finish(o, mat, bevel)


def tube(start, end, r, mat, top=None, sides=12):
    a, b = Vector(start), Vector(end)
    d = b - a
    bpy.ops.mesh.primitive_cone_add(
        vertices=sides,
        radius1=r,
        radius2=r if top is None else top,
        depth=d.length,
        location=(a + b) / 2,
    )
    o = bpy.context.object
    o.rotation_euler = d.to_track_quat("Z", "Y").to_euler()
    return finish(o, mat)


def geom(name, v, f, mat):
    m = bpy.data.meshes.new(name)
    m.from_pydata(v, [], f)
    m.update()
    o = bpy.data.objects.new(name, m)
    bpy.context.collection.objects.link(o)
    return finish(o, mat)


def roof():
    w, d = 17, 6.2

    def h(v):
        return 7.2 + 2.8 * (1 - v) ** 1.65 + 0.72 * v**9

    for side in (-1, 1):
        v = [
            (
                (x / 48 - 0.5) * w,
                side * y / 24 * d,
                h(y / 24) + 0.36 * (abs(x / 48 - 0.5) * 2) ** 8 * (y / 24) ** 6,
            )
            for x in range(49)
            for y in range(25)
        ]
        f = []
        for x in range(48):
            for y in range(24):
                i = x * 25 + y
                f.append(
                    (i, i + 1, i + 26, i + 25)
                    if side > 0
                    else (i + 25, i + 26, i + 1, i)
                )
        o = geom("Swept tile bed", v, f, "RoofTile")
        mod = o.modifiers.new("Tile thickness", "SOLIDIFY")
        mod.thickness = 0.11
        bpy.context.view_layer.objects.active = o
        bpy.ops.object.modifier_apply(modifier=mod.name)
        for col in range(57):
            x = (col / 56 - 0.5) * w
            for row in range(12):
                v1, v2 = row / 12, (row + 1) / 12
                tube(
                    (x, side * v1 * d, h(v1) + 0.055),
                    (x, side * v2 * d, h(v2) + 0.055),
                    0.065,
                    "RoofTile",
                    sides=8,
                )
        tube(
            (-w / 2, side * d, h(1) - 0.16),
            (w / 2, side * d, h(1) - 0.16),
            0.13,
            "Wood",
        )
    tube((-8.6, 0, 10.13), (8.6, 0, 10.13), 0.18, "RoofTile", sides=16)
    for side in (-1, 1):
        for i in range(7):
            tube(
                (side * (8.2 + i * 0.10), 0, 10.12 + i * 0.12),
                (side * (8.3 + i * 0.10), 0, 10.24 + i * 0.12),
                0.15 - i * 0.012,
                "Bronze",
            )
    for x in (-7.6, -4, 0, 4, 7.6):
        box((x, 0, 7), (0.16, 11.3, 0.24), "Wood")


def frame():
    for x in (-6.8, -3.8, 3.8, 6.8):
        for y in (-4.4, 4.4):
            box((x, y, 0.25), (0.7, 0.7, 0.5), "Stone", 0.05)
            tube((x, y, 0.48), (x, y, 6.9), 0.20, "Wood", sides=20)
            for z in (0.65, 6.35):
                tube((x, y, z), (x, y, z + 0.14), 0.245, "Bronze", sides=20)
            for sign in (-1, 1):
                tube((x, y, 5.8), (x + sign * 0.65, y, 6.7), 0.12, "Wood")
    for y in (-4.4, 4.4):
        box((0, y, 6.85), (14.8, 0.32, 0.48), "Wood", 0.06)
        box((0, y, 6.44), (14.5, 0.16, 0.14), "Bronze", 0.02)
    for x in (-6.8, 6.8):
        box((x, 0, 6.85), (0.32, 9.1, 0.48), "Wood", 0.06)
        for y in (-2.8, -0.9, 0.9, 2.8):
            box((x, y, 1.1), (0.22, 1.6, 0.12), "Wood")
            for dy in (-0.55, 0, 0.55):
                box((x, y + dy, 0.7), (0.14, 0.14, 0.85), "Wood")


def rock(seed):
    rng = random.Random(seed)
    ph = [rng.random() * 6 for _ in range(3)]
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=6, radius=1)
    o = bpy.context.object
    for v in o.data.vertices:
        x, y, z = v.co
        # Three-dimensional erosion avoids the uniformly horizontal ripples
        # of a sine field, which read as turned columns in the native camera.
        domain = Vector((x * 2.8 + ph[0], y * 2.4 + ph[1], z * 3.1 + ph[2]))
        v.co *= 1 + 0.22 * noise.fractal(domain, 0.55, 2.0, 4)
        v.co.x *= 2.6
        v.co.y *= 2.1
        v.co.z = v.co.z * 3.5 + 3
    finish(o, "Stone")


def mountain_ridge():
    # A continuous height field gives the distant skyline broad ridges and
    # asymmetric summits instead of enlarging a crumpled foreground boulder.
    count = 65
    verts, faces = [], []
    for row in range(count):
        y = row / (count - 1) * 2 - 1
        for column in range(count):
            x = column / (count - 1) * 2 - 1
            envelope = max(0, 1 - x*x - y*y) ** 0.7
            summit = 2.0
            for px, py, height, width in [(-.48, -.12, 3.7, .10),
                                         (.05, .18, 4.6, .075),
                                         (.48, -.06, 3.0, .12)]:
                summit += height * math.exp(-((x-px)**2 + (y-py)**2) / width)
            erosion = noise.fractal(Vector((x*5.2+4, y*6.8+7, 2.1)), .65, 2, 4)
            verts.append((x * 4.0, y * 3.5, envelope * max(.1, summit + 1.25*erosion)))
    for row in range(count - 1):
        for column in range(count - 1):
            index = row * count + column
            faces.append((index, index+1, index+count+1, index+count))
    data = bpy.data.meshes.new("Eroded mountain height field")
    data.from_pydata(verts, [], faces)
    data.update()
    obj = bpy.data.objects.new("Mountain ridge", data)
    bpy.context.collection.objects.link(obj)
    finish(obj, "Stone")


def bamboo():
    rng = random.Random(91)
    for k in range(5):
        angle = k * 2.399
        base = Vector((math.cos(angle) * 0.65, math.sin(angle) * 0.65, 0))
        lean = Vector(
            (rng.uniform(-0.65, 0.65), rng.uniform(-0.65, 0.65), rng.uniform(7.4, 10.8))
        )
        for node in range(14):
            a, b = base + lean * (node / 14), base + lean * ((node + 1) / 14)
            r = 0.085 * (1 - node / 22)
            tube(a, b, r, "Bamboo", top=r * 0.98)
            tube(a, a + lean.normalized() * 0.042, r * 1.2, "Bamboo")
            if node < 7:
                continue
            dr = Vector(
                (math.cos(angle + node * 2.4), math.sin(angle + node * 2.4), 0.18)
            )
            tip = a + dr * rng.uniform(1.15, 1.9)
            tube(a, tip, 0.024, "Bamboo", top=0.005, sides=6)
            for j in range(8):
                start = a.lerp(tip, (j + 1) / 9)
                sign = (-1) ** j
                d = (
                    dr + Vector((-dr.y * sign * 0.8, dr.x * sign * 0.8, -0.20))
                ).normalized()
                end = start + d * rng.uniform(0.38, 0.66)
                cross = Vector((-d.y, d.x, 0)) * 0.09
                mid = start.lerp(end, 0.47)
                geom(
                    "Lanceolate leaf",
                    [start, mid + cross, end, mid - cross, mid + Vector((0, 0, 0.035))],
                    [(0, 1, 4), (1, 2, 4), (2, 3, 4), (3, 0, 4)],
                    "Leaf",
                )


def lantern():
    box((0, 0, 0.15), (0.85, 0.85, 0.3), "Stone", 0.06)
    box((0, 0, 0.55), (0.58, 0.58, 0.5), "Stone", 0.04)
    tube((0, 0, 0.8), (0, 0, 1.9), 0.18, "Stone", sides=8)
    box((0, 0, 1.95), (0.85, 0.85, 0.16), "Bronze")
    box((0, 0, 2.35), (0.58, 0.58, 0.66), "Glow", 0.04)
    for x in (-0.34, 0.34):
        for y in (-0.34, 0.34):
            tube((x, y, 2.02), (x, y, 2.74), 0.038, "Bronze", sides=8)
    bpy.ops.mesh.primitive_cone_add(
        vertices=4,
        radius1=0.75,
        radius2=0.15,
        depth=0.4,
        location=(0, 0, 2.9),
        rotation=(0, 0, math.pi / 4),
    )
    finish(bpy.context.object, "RoofTile", 0.025)
    tube((0, 0, 3.1), (0, 0, 3.3), 0.08, "Bronze", top=0.02)


ASSETS = {}
manifest = []
for name, build in [
    ("SM_SanctuaryRoof", roof),
    ("SM_SanctuaryFrame", frame),
    ("SM_ScholarRock", lambda: rock(13)),
    ("SM_CliffRock", mountain_ridge),
    ("SM_BambooCluster", bamboo),
    ("SM_StoneLantern", lantern),
]:
    parts.clear()
    build()
    bpy.ops.object.select_all(action="DESELECT")
    for o in parts:
        o.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.join()
    o = bpy.context.object
    o.name = name
    bpy.context.scene.cursor.location = (0, 0, 0)
    bpy.ops.object.origin_set(type="ORIGIN_CURSOR")
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.cube_project(cube_size=2)
    bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.export_scene.fbx(
        filepath=str(OUT / (name + ".fbx")),
        use_selection=True,
        object_types={"MESH"},
        add_leaf_bones=False,
        bake_anim=False,
        mesh_smooth_type="FACE",
        axis_forward="-Y",
        axis_up="Z",
        apply_unit_scale=True,
    )
    ASSETS[name] = o
    manifest.append(
        {
            "name": name,
            "vertices": len(o.data.vertices),
            "polygons": len(o.data.polygons),
            "slots": [m.name for m in o.data.materials],
            "units": "metres; FBX converts to cm",
        }
    )
    o.hide_render = True
    o.hide_set(True)
(OUT / "kit-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
if a.render:

    def instance(name, pos=(0, 0, 0), scale=(1, 1, 1), yaw=0):
        src = ASSETS[name]
        o = src.copy()
        o.data = src.data
        bpy.context.collection.objects.link(o)
        o.hide_render = False
        o.hide_set(False)
        o.location = pos
        o.scale = scale
        o.rotation_euler.z = yaw

    instance("SM_SanctuaryRoof")
    instance("SM_SanctuaryFrame")
    parts.clear()
    box((0, 5, -0.4), (40, 42, 0.8), "Ground", 0.1)
    for x, y in [(-14, -5), (-13, 5), (-16, 14), (13, -8), (15, 3), (17, 13)]:
        instance("SM_BambooCluster", (x, y, 0), yaw=x)
        instance("SM_ScholarRock", (x + 1, y + 3, -0.3), (0.7, 0.65, 0.7), yaw=y)
    for x in (-8, 8):
        for y in (6, 13):
            instance("SM_StoneLantern", (x, y, 0))
    for i in range(20):
        angle = i * 2.399
        r = 75 + (i % 3) * 20
        instance(
            "SM_CliffRock",
            (math.cos(angle) * r, math.sin(angle) * r, -14),
            (5 + i % 3, 5 + i % 2, 8 + i % 4),
            yaw=angle,
        )
    scene = bpy.context.scene
    scene.world = bpy.data.worlds.new("Mist sky")
    scene.world.use_nodes = True
    bg = scene.world.node_tree.nodes.get("Background")
    bg.inputs[0].default_value = (0.25, 0.40, 0.48, 1)
    bg.inputs[1].default_value = 0.45
    bpy.ops.object.light_add(type="SUN", location=(0, 0, 30))
    sun = bpy.context.object
    sun.rotation_euler = (0.5, -0.55, -0.6)
    sun.data.energy = 2.8
    sun.data.angle = 0.12
    sun.data.color = (1, 0.83, 0.64)
    bpy.ops.object.light_add(type="AREA", location=(0, 5, 13))
    bpy.context.object.data.energy = 1900
    bpy.context.object.data.shape = "DISK"
    bpy.context.object.data.size = 14
    bpy.ops.object.camera_add(location=(24, 33, 15))
    camera = bpy.context.object
    camera.rotation_euler = (
        (Vector((0, 0, 4)) - camera.location).to_track_quat("-Z", "Y").to_euler()
    )
    camera.data.lens = 38
    scene.camera = camera
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 32
    scene.cycles.use_denoising = True
    scene.render.resolution_x = 1400
    scene.render.resolution_y = 900
    scene.render.resolution_percentage = 100
    scene.view_settings.view_transform = "AgX"
    scene.render.filepath = str(OUT / "sanctuary-lookdev.png")
    bpy.ops.render.render(write_still=True)
bpy.ops.wm.save_as_mainfile(filepath=str(OUT / "sanctuary-kit.blend"))
