"""
Creates (or updates) the FlyingTrucks content assets in /FlyingTrucks/.

Run headless from PowerShell:
  & 'C:\\Program Files\\Unreal Engine - CSS\\Engine\\Binaries\\Win64\\UnrealEditor-Cmd.exe' `
    'C:\\Modding\\SatisfactoryModLoader\\FactoryGame.uproject' -run=pythonscript `
    -script='C:\\Modding\\SatisfactoryModLoader\\Mods\\FlyingTrucks\\Scripts\\create_assets.py' `
    -unattended -nullrhi -nosplash -stdout

Or in the editor: Tools > Execute Python Script.
"""
import unreal

ROOT = "/FlyingTrucks"
LOG = "[FlyingTrucks] "

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary
# Commandlets don't finish the asset registry scan by themselves; needed to see assets from a previous run.
unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([ROOT], True)


def log(msg):
    unreal.log_warning(LOG + str(msg))


def load_cls(path):
    cls = unreal.load_class(None, path)
    if cls is None:
        raise RuntimeError("Could not load class " + path)
    return cls


def make_blueprint(name, parent_cls, folder=ROOT):
    path = folder + "/" + name
    if eal.does_asset_exist(path):
        bp = eal.load_asset(path)
        log("Reusing " + path)
        return bp
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", parent_cls)
    bp = asset_tools.create_asset(name, folder, unreal.Blueprint, factory)
    if bp is None:
        raise RuntimeError("Failed to create " + path)
    log("Created " + path)
    return bp


def compile_bp(bp):
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)


def cdo_of(bp):
    return unreal.get_default_object(bp.generated_class())


def save(asset):
    if not eal.save_loaded_asset(asset, False):
        raise RuntimeError("Failed to save " + asset.get_path_name())


# --- Game classes -------------------------------------------------------
BP_Truck = load_cls("/Game/FactoryGame/Buildable/Vehicle/Truck/BP_Truck.BP_Truck_C")
Desc_Truck = load_cls("/Game/FactoryGame/Buildable/Vehicle/Truck/Desc_Truck.Desc_Truck_C")
Recipe_Truck = load_cls("/Game/FactoryGame/Recipes/Vehicle/Recipe_Truck.Recipe_Truck_C")

# --- 1. Vehicle: child of BP_Truck with the flight component -------------
bp_truck = make_blueprint("BP_FlyingTruck", BP_Truck)
sds = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
handles = sds.k2_gather_subobject_data_for_blueprint(bp_truck)
has_component = False
for h in handles:
    data = unreal.SubobjectDataBlueprintFunctionLibrary.get_data(h)
    obj = unreal.SubobjectDataBlueprintFunctionLibrary.get_associated_object(data)
    if obj is not None and isinstance(obj, unreal.FlyingTruckComponent):
        has_component = True
if not has_component:
    params = unreal.AddNewSubobjectParams(
        parent_handle=handles[0],
        new_class=unreal.FlyingTruckComponent,
        blueprint_context=bp_truck,
    )
    new_handle, fail_reason = sds.add_new_subobject(params)
    if not fail_reason.is_empty():
        raise RuntimeError("add_new_subobject failed: " + str(fail_reason))
    sds.rename_subobject(new_handle, unreal.Text("FlyingTruck"))
    log("Added FlyingTruckComponent to BP_FlyingTruck")
compile_bp(bp_truck)
# Vehicle descriptors take their name/description from the vehicle class, not the descriptor.
truck_cdo = cdo_of(bp_truck)
truck_cdo.set_editor_property("m_display_name", unreal.Text("Flying Truck"))
truck_cdo.set_editor_property("m_description", unreal.Text(
    "A truck that can fly. Press F (Y on gamepad) while driving to toggle flight mode; "
    "Space/A pitches up, C or Ctrl/B pitches down."))
save(bp_truck)

# --- 2. Descriptor -------------------------------------------------------
bp_desc = make_blueprint("Desc_FlyingTruck", Desc_Truck)
compile_bp(bp_desc)
desc = cdo_of(bp_desc)
desc.set_editor_property("m_vehicle_class", bp_truck.generated_class())
desc.set_editor_property("m_display_name", unreal.Text("Flying Truck"))
desc.set_editor_property("m_description", unreal.Text(
    "A truck that can fly. Press F (Y on gamepad) while driving to toggle flight mode; "
    "Space/A pitches up, C or Ctrl/B pitches down."))
save(bp_desc)

# --- 3. Recipe (same cost as the truck) ----------------------------------
bp_recipe = make_blueprint("Recipe_FlyingTruck", Recipe_Truck)
compile_bp(bp_recipe)
recipe = cdo_of(bp_recipe)
recipe.set_editor_property("m_product", [unreal.ItemAmount(item_class=bp_desc.generated_class(), amount=1)])
# Recipe_Truck already has mDisplayNameOverride = true ("Truck"); that flag isn't editable but is inherited,
# so setting mDisplayName is enough.
recipe.set_editor_property("m_display_name", unreal.Text("Flying Truck"))
save(bp_recipe)

# --- 4. Schematic (granted automatically by AFlyingTrucksSubsystem) ------
bp_schem = make_blueprint("Schematic_FlyingTruck", unreal.FGSchematic)
compile_bp(bp_schem)
schem = cdo_of(bp_schem)
schem.set_editor_property("m_type", unreal.SchematicType.EST_CUSTOM)
schem.set_editor_property("m_display_name", unreal.Text("Flying Trucks"))
schem.set_editor_property("m_tech_tier", 0)
schem.set_editor_property("m_time_to_complete", 0.0)
BP_UnlockRecipe = load_cls("/Game/FactoryGame/Unlocks/BP_UnlockRecipe.BP_UnlockRecipe_C")  # FGUnlockRecipe is abstract
unlock = unreal.new_object(BP_UnlockRecipe, outer=schem)
unlock.set_editor_property("m_recipes", [bp_recipe.generated_class()])
schem.set_editor_property("m_unlocks", [unlock])
save(bp_schem)

# --- 4b. Acclivity Stone: flight fuel, mined from converted iron/limestone nodes ---
import os
import struct
import zlib
import math as _m


def write_crystal_png(path, size=256):
    """A pale-blue glowing crystal cluster on a transparent background (RGBA PNG, pure Python)."""
    def facets():
        # (centre x, base y, half width, height, tilt) in 0..1 units; the middle one is the biggest.
        return [(0.50, 0.88, 0.15, 0.74, 0.00), (0.31, 0.90, 0.11, 0.48, -0.20), (0.70, 0.90, 0.11, 0.52, 0.22)]

    def crystal_alpha(x, y):
        """Returns (inside, shade) for the frontmost crystal at x, y."""
        best = None
        for cx, by, hw, h, tilt in facets():
            # Axis along the crystal, tilted.
            ax, ay = _m.sin(tilt), -_m.cos(tilt)
            dx, dy = x - cx, y - by
            along = dx * ax + dy * ay           # 0 at base, h at tip
            across = dx * _m.cos(tilt) + dy * _m.sin(tilt)  # signed sideways distance
            if along < 0 or along > h:
                continue
            tip_start = h * 0.72
            width = hw if along < tip_start else hw * (h - along) / (h - tip_start)
            if abs(across) <= width:
                shade = 0.8 + 0.35 * (across / max(width, 1e-6))  # lit from the right
                # Centre ridge highlight.
                if abs(across) < width * 0.12:
                    shade = 1.25
                cand = (along / h, shade)
                if best is None or hw > best[2]:
                    best = (cand[0], cand[1], hw)
        return best

    rows = []
    for j in range(size):
        row = bytearray([0])
        for i in range(size):
            x, y = (i + 0.5) / size, (j + 0.5) / size
            hit = crystal_alpha(x, y)
            if hit:
                t, shade, _ = hit
                base = (0.10 + 0.50 * t, 0.45 + 0.45 * t, 1.0)   # deeper blue at the base, near-white tips
                r, g, b = [min(1.0, c * shade) for c in base]
                a = 1.0
            else:
                # Soft blue halo around the cluster.
                d = _m.hypot(x - 0.5, (y - 0.6) * 0.9)
                glow = max(0.0, 1.0 - d / 0.45) ** 2 * 0.55
                r, g, b, a = 0.25, 0.6, 1.0, glow
            row += bytes([int(r * 255), int(g * 255), int(b * 255), int(a * 255)])
        rows.append(bytes(row))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


STONE_DIR = ROOT + "/Resources"
art_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "SourceArt")
os.makedirs(art_dir, exist_ok=True)
icon_png = os.path.join(art_dir, "Icon_AcclivityStone.png")
write_crystal_png(icon_png)
icon_task = unreal.AssetImportTask()
icon_task.set_editor_property("filename", icon_png)
icon_task.set_editor_property("destination_path", STONE_DIR)
icon_task.set_editor_property("destination_name", "Icon_AcclivityStone")
icon_task.set_editor_property("automated", True)
icon_task.set_editor_property("replace_existing", True)
icon_task.set_editor_property("save", True)
asset_tools.import_asset_tasks([icon_task])
icon = eal.load_asset(STONE_DIR + "/Icon_AcclivityStone")
if icon is None:
    raise RuntimeError("Icon import failed")
# Match the game's own item icons (Satisfactory uses PROJECT01 for them; the UI group came out green in game).
icon.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_PROJECT01)
icon.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_DEFAULT)
icon.set_editor_property("srgb", True)
icon.set_editor_property("never_stream", False)
log("Icon settings: group %s, compression %s" % (icon.get_editor_property("lod_group"), icon.get_editor_property("compression_settings")))
save(icon)
log("Imported Acclivity Stone icon")

Desc_Stone = load_cls("/Game/FactoryGame/Resource/RawResources/Stone/Desc_Stone.Desc_Stone_C")
Desc_RawQuartz = load_cls("/Game/FactoryGame/Resource/RawResources/RawQuartz/Desc_RawQuartz.Desc_RawQuartz_C")
bp_stone = make_blueprint("Desc_AcclivityStone", Desc_Stone, STONE_DIR)
compile_bp(bp_stone)
stone = cdo_of(bp_stone)
stone.set_editor_property("m_display_name", unreal.Text("Acclivity Stone"))
stone.set_editor_property("m_description", unreal.Text(
    "A pale blue stone that pushes against gravity when charged. "
    "Flying Trucks burn one per minute of flight; keep some in the truck's cargo to take off."))
stone.set_editor_property("m_small_icon", icon)
stone.set_editor_property("m_persistent_big_icon", icon)
stone.set_editor_property("m_ping_color", unreal.LinearColor(0.1, 0.45, 1.0, 1.0))
# A crystal looks more like it than a lump of limestone on the belts.
stone.set_editor_property("m_conveyor_mesh", unreal.get_default_object(Desc_RawQuartz).get_editor_property("m_conveyor_mesh"))
save(bp_stone)

# Scanner unlock goes in its own schematic: saves that already have Schematic_FlyingTruck get it too.
bp_stone_schem = make_blueprint("Schematic_AcclivityStone", unreal.FGSchematic)
compile_bp(bp_stone_schem)
stone_schem = cdo_of(bp_stone_schem)
stone_schem.set_editor_property("m_type", unreal.SchematicType.EST_CUSTOM)
stone_schem.set_editor_property("m_display_name", unreal.Text("Acclivity Stone"))
stone_schem.set_editor_property("m_tech_tier", 0)
stone_schem.set_editor_property("m_time_to_complete", 0.0)
BP_UnlockScannableResource = load_cls("/Game/FactoryGame/Unlocks/BP_UnlockScannableResource.BP_UnlockScannableResource_C")
scan_unlock = unreal.new_object(BP_UnlockScannableResource, outer=stone_schem)
# The pair's fields are EditDefaultsOnly, which Python refuses on a struct value; import it as text instead.
pair = unreal.ScannableResourcePair()
pair.import_text('(ResourceDescriptor="%s",ResourceNodeType=Node)' % bp_stone.generated_class().get_path_name())
log("Scanner pair: %s" % pair.export_text())
scan_unlock.set_editor_property("m_resource_pairs_to_add_to_scanner", [pair])
stone_schem.set_editor_property("m_unlocks", [scan_unlock])
save(bp_stone_schem)
log("Acclivity Stone assets saved")

# --- 4c. Flying Explorer, Tractor and Factory Cart -----------------------------
FLIGHT_HELP = ("Press F (Y on gamepad) while driving to toggle flight mode; Space/A pitches up, C or Ctrl/B pitches down. "
               "Flight burns one Acclivity Stone per minute from the cargo or your inventory.")


def add_flight_component(bp):
    handles = sds.k2_gather_subobject_data_for_blueprint(bp)
    for h in handles:
        obj = unreal.SubobjectDataBlueprintFunctionLibrary.get_associated_object(unreal.SubobjectDataBlueprintFunctionLibrary.get_data(h))
        if obj is not None and isinstance(obj, unreal.FlyingTruckComponent):
            return
    params = unreal.AddNewSubobjectParams(parent_handle=handles[0], new_class=unreal.FlyingTruckComponent, blueprint_context=bp)
    new_handle, fail_reason = sds.add_new_subobject(params)
    if not fail_reason.is_empty():
        raise RuntimeError("add_new_subobject failed: " + str(fail_reason))
    sds.rename_subobject(new_handle, unreal.Text("FlyingTruck"))
    log("Added FlyingTruckComponent to " + bp.get_name())


def flight_template(bp):
    for h in sds.k2_gather_subobject_data_for_blueprint(bp):
        obj = unreal.SubobjectDataBlueprintFunctionLibrary.get_associated_object(unreal.SubobjectDataBlueprintFunctionLibrary.get_data(h))
        if obj is not None and isinstance(obj, unreal.FlyingTruckComponent):
            return obj
    raise RuntimeError("No FlyingTruckComponent in " + bp.get_name())


def make_flying_vehicle(key, display, vehicle_parent, desc_parent):
    bp = make_blueprint("BP_Flying" + key, load_cls(vehicle_parent), ROOT + "/Vehicles")
    add_flight_component(bp)
    compile_bp(bp)
    cdo = cdo_of(bp)
    cdo.set_editor_property("m_display_name", unreal.Text(display))
    cdo.set_editor_property("m_description", unreal.Text("A " + display[7:] + " that can fly. " + FLIGHT_HELP))
    save(bp)
    d = make_blueprint("Desc_Flying" + key, load_cls(desc_parent), ROOT + "/Vehicles")
    compile_bp(d)
    dc = cdo_of(d)
    dc.set_editor_property("m_display_name", unreal.Text(display))
    dc.set_editor_property("m_description", unreal.Text("A " + display[7:] + " that can fly. " + FLIGHT_HELP))
    return bp, d, dc


def make_flying_recipe(key, display, recipe_parent, desc_bp):
    r = make_blueprint("Recipe_Flying" + key, load_cls(recipe_parent), ROOT + "/Vehicles")
    compile_bp(r)
    rc = cdo_of(r)
    rc.set_editor_property("m_product", [unreal.ItemAmount(item_class=desc_bp.generated_class(), amount=1)])
    rc.set_editor_property("m_display_name", unreal.Text(display))
    save(r)
    return r


sds = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
flying_recipes = []
VEH_DIR = "/Game/FactoryGame/Buildable/Vehicle/"
for key, display, folder, recipe in (("Explorer", "Flying Explorer", "Explorer", "Recipe_Explorer"),
                                     ("Tractor", "Flying Tractor", "Tractor", "Recipe_Tractor")):
    bp, d, dc = make_flying_vehicle(key, display, VEH_DIR + "%s/BP_%s.BP_%s_C" % (folder, key, key),
                                    VEH_DIR + "%s/Desc_%s.Desc_%s_C" % (folder, key, key))
    dc.set_editor_property("m_vehicle_class", bp.generated_class())
    save(d)
    if key == "Tractor":
        t = flight_template(bp)
        t.set_editor_property("wing_scale_adjust", 0.8)
        t.set_editor_property("wing_pivot_adjust", unreal.Vector(0.0, 0.0, -60.0))
        compile_bp(bp)
        save(bp)
    flying_recipes.append(make_flying_recipe(key, display, "/Game/FactoryGame/Recipes/Vehicle/%s.%s_C" % (recipe, recipe), d))

# The Factory Cart is an equipment item: the dispenser you equip spawns the cart.
cart_bp, cart_desc, cart_dc = make_flying_vehicle("FactoryCart", "Flying Factory Cart",
                                                  VEH_DIR + "Golfcart/BP_Golfcart.BP_Golfcart_C",
                                                  "/Game/FactoryGame/Resource/Equipment/GolfCart/Desc_GolfCart.Desc_GolfCart_C")
dispenser = make_blueprint("Equip_FlyingFactoryCartDispenser",
                           load_cls("/Game/FactoryGame/Equipment/GolfCart/Equip_GolfCartDispenser.Equip_GolfCartDispenser_C"),
                           ROOT + "/Vehicles")
compile_bp(dispenser)
cdo_of(dispenser).set_editor_property("m_golf_cart_class", cart_bp.generated_class())
save(dispenser)
cart_dc.set_editor_property("m_equipment_class", dispenser.generated_class())
save(cart_desc)
flying_recipes.append(make_flying_recipe("FactoryCart", "Flying Factory Cart",
                                         "/Game/FactoryGame/Recipes/Vehicle/Recipe_FactoryCart.Recipe_FactoryCart_C", cart_desc))

bp_veh_schem = make_blueprint("Schematic_FlyingVehicles", unreal.FGSchematic)
compile_bp(bp_veh_schem)
veh_schem = cdo_of(bp_veh_schem)
veh_schem.set_editor_property("m_type", unreal.SchematicType.EST_CUSTOM)
veh_schem.set_editor_property("m_display_name", unreal.Text("Flying Vehicles"))
veh_schem.set_editor_property("m_tech_tier", 0)
veh_schem.set_editor_property("m_time_to_complete", 0.0)
veh_unlock = unreal.new_object(BP_UnlockRecipe, outer=veh_schem)
veh_unlock.set_editor_property("m_recipes", [r.generated_class() for r in flying_recipes])
veh_schem.set_editor_property("m_unlocks", [veh_unlock])
save(bp_veh_schem)
log("Flying vehicles saved")

# --- 5. Root game world module -------------------------------------------
bp_root = make_blueprint("RootGameWorld_FlyingTrucks", unreal.GameWorldModule)
compile_bp(bp_root)
root = cdo_of(bp_root)
root.set_editor_property("root_module", True)
root.set_editor_property("m_schematics", [bp_schem.generated_class(), bp_stone_schem.generated_class(), bp_veh_schem.generated_class()])
root.set_editor_property("mod_subsystems", [unreal.FlyingTrucksSubsystem.static_class()])
save(bp_root)

# --- 6. Flight ring: generated torus mesh + self-glowing material ---------
import math
import os

RING_MAJOR_RADIUS = 100.0   # cm; the component scales it to the wanted diameter
RING_MINOR_RADIUS = 9.0     # cm tube radius at that size (1.5x the first version)
RING_MAJOR_SEGMENTS = 96
RING_MINOR_SEGMENTS = 16


def write_torus_obj(path, R, r, n_major, n_minor):
    """Torus around the Z axis, centred on the origin, Z up (Unreal axes, cm)."""
    lines = ["# FlyingTrucks ring, generated by create_assets.py"]
    for i in range(n_major):
        u = 2.0 * math.pi * i / n_major
        for j in range(n_minor):
            v = 2.0 * math.pi * j / n_minor
            nx, ny, nz = math.cos(v) * math.cos(u), math.cos(v) * math.sin(u), math.sin(v)
            x, y, z = (R + r * math.cos(v)) * math.cos(u), (R + r * math.cos(v)) * math.sin(u), r * math.sin(v)
            lines.append("v %.4f %.4f %.4f" % (x, y, z))
            lines.append("vn %.5f %.5f %.5f" % (nx, ny, nz))
            lines.append("vt %.5f %.5f" % (i / float(n_major), j / float(n_minor)))

    def idx(i, j):
        return (i % n_major) * n_minor + (j % n_minor) + 1

    for i in range(n_major):
        for j in range(n_minor):
            a, b, c, d = idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1)
            # Counter-clockwise seen from outside.
            lines.append("f %d/%d/%d %d/%d/%d %d/%d/%d" % (a, a, a, b, b, b, c, c, c))
            lines.append("f %d/%d/%d %d/%d/%d %d/%d/%d" % (a, a, a, c, c, c, d, d, d))
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


mod_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
source_dir = os.path.join(mod_dir, "SourceArt")
os.makedirs(source_dir, exist_ok=True)
obj_path = os.path.join(source_dir, "SM_FlyingRing.obj")
write_torus_obj(obj_path, RING_MAJOR_RADIUS, RING_MINOR_RADIUS, RING_MAJOR_SEGMENTS, RING_MINOR_SEGMENTS)
log("Wrote " + obj_path)

task = unreal.AssetImportTask()
task.set_editor_property("filename", obj_path)
task.set_editor_property("destination_path", ROOT + "/Ring")
task.set_editor_property("destination_name", "SM_FlyingRing")
task.set_editor_property("automated", True)
task.set_editor_property("replace_existing", True)
task.set_editor_property("save", True)
asset_tools.import_asset_tasks([task])
ring_mesh = eal.load_asset(ROOT + "/Ring/SM_FlyingRing")
if ring_mesh is None or not isinstance(ring_mesh, unreal.StaticMesh):
    raise RuntimeError("Ring import failed; imported: %s" % (task.get_editor_property("imported_object_paths"),))
bounds = ring_mesh.get_bounds()
log("Ring mesh imported, extent %s" % (bounds.box_extent,))

mat_path = ROOT + "/Ring/M_FlyingRingGlow"
if eal.does_asset_exist(mat_path):
    ring_mat = eal.load_asset(mat_path)
else:
    ring_mat = asset_tools.create_asset("M_FlyingRingGlow", ROOT + "/Ring", unreal.Material, unreal.MaterialFactoryNew())
    ring_mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mel = unreal.MaterialEditingLibrary
    color = mel.create_material_expression(ring_mat, unreal.MaterialExpressionVectorParameter, -500, 0)
    color.set_editor_property("parameter_name", "Color")
    color.set_editor_property("default_value", unreal.LinearColor(0.1, 0.45, 1.0, 1.0))
    intensity = mel.create_material_expression(ring_mat, unreal.MaterialExpressionScalarParameter, -500, 250)
    intensity.set_editor_property("parameter_name", "Intensity")
    intensity.set_editor_property("default_value", 20.0)
    mul = mel.create_material_expression(ring_mat, unreal.MaterialExpressionMultiply, -250, 100)
    mel.connect_material_expressions(color, "", mul, "A")
    mel.connect_material_expressions(intensity, "", mul, "B")
    mel.connect_material_property(mul, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.recompile_material(ring_mat)
    log("Created " + mat_path)
save(ring_mat)

ring_mesh.set_material(0, ring_mat)
save(ring_mesh)


# --- 7. Delta wings: generated triangular wedge + dark grey material ----------
WING_ROOT_CHORD = 320.0   # cm, along the truck (front point to trailing edge)
WING_SPAN = 230.0         # cm, sideways from the root edge to the tip
WING_THICKNESS = 10.0     # cm


def write_wing_obj(path, chord, span, thick):
    """Right-hand delta wing with its pivot (front point) at the origin: front (0,0), rear root (-chord,0),
    tip (-chord, span). Flat-shaded prism, Z up (Unreal axes, cm)."""
    h = thick / 2.0
    tri = [(0.0, 0.0), (-chord, 0.0), (-chord, span)]
    verts, norms, faces = [], [], []

    def face(points, normal):
        base = len(verts)
        for p in points:
            verts.append(p)
        norms.append(normal)
        n = len(norms)
        faces.append([(base + i + 1, n) for i in range(len(points))])

    top = [(x, y, h) for x, y in tri]
    bot = [(x, y, -h) for x, y in tri]
    face(top, (0, 0, 1))
    face(list(reversed(bot)), (0, 0, -1))
    for i in range(3):
        a, b = tri[i], tri[(i + 1) % 3]
        ex, ey = b[0] - a[0], b[1] - a[1]
        ln = math.hypot(ex, ey)
        nx, ny = ey / ln, -ex / ln
        # Outward = away from the triangle's centroid.
        cx, cy = sum(p[0] for p in tri) / 3, sum(p[1] for p in tri) / 3
        if (a[0] - cx) * nx + (a[1] - cy) * ny < 0:
            nx, ny = -nx, -ny
        face([(a[0], a[1], -h), (b[0], b[1], -h), (b[0], b[1], h), (a[0], a[1], h)], (nx, ny, 0))
    lines = ["# FlyingTrucks delta wing, generated by create_assets.py"]
    lines += ["v %.4f %.4f %.4f" % v for v in verts]
    lines += ["vn %.5f %.5f %.5f" % n for n in norms]
    lines.append("vt 0 0")
    for f in faces:
        lines.append("f " + " ".join("%d/1/%d" % (vi, ni) for vi, ni in f))
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


wing_obj = os.path.join(source_dir, "SM_FlyingWing.obj")
write_wing_obj(wing_obj, WING_ROOT_CHORD, WING_SPAN, WING_THICKNESS)
wtask = unreal.AssetImportTask()
wtask.set_editor_property("filename", wing_obj)
wtask.set_editor_property("destination_path", ROOT + "/Wings")
wtask.set_editor_property("destination_name", "SM_FlyingWing")
wtask.set_editor_property("automated", True)
wtask.set_editor_property("replace_existing", True)
wtask.set_editor_property("save", True)
asset_tools.import_asset_tasks([wtask])
wing_mesh = eal.load_asset(ROOT + "/Wings/SM_FlyingWing")
if wing_mesh is None or not isinstance(wing_mesh, unreal.StaticMesh):
    raise RuntimeError("Wing import failed")
wb = wing_mesh.get_bounds()
log("Wing mesh imported, origin %s extent %s" % (wb.origin, wb.box_extent))

wing_mat_path = ROOT + "/Wings/M_FlyingWing"
if eal.does_asset_exist(wing_mat_path):
    wing_mat = eal.load_asset(wing_mat_path)
else:
    wing_mat = asset_tools.create_asset("M_FlyingWing", ROOT + "/Wings", unreal.Material, unreal.MaterialFactoryNew())
    wing_mat.set_editor_property("two_sided", True)
    mel = unreal.MaterialEditingLibrary
    wcolor = mel.create_material_expression(wing_mat, unreal.MaterialExpressionVectorParameter, -500, 0)
    wcolor.set_editor_property("parameter_name", "Color")
    wcolor.set_editor_property("default_value", unreal.LinearColor(0.035, 0.035, 0.04, 1.0))
    mel.connect_material_property(wcolor, "", unreal.MaterialProperty.MP_BASE_COLOR)
    metal = mel.create_material_expression(wing_mat, unreal.MaterialExpressionConstant, -500, 200)
    metal.set_editor_property("r", 0.6)
    mel.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)
    rough = mel.create_material_expression(wing_mat, unreal.MaterialExpressionConstant, -500, 300)
    rough.set_editor_property("r", 0.45)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.recompile_material(wing_mat)
    log("Created " + wing_mat_path)
save(wing_mat)
wing_mesh.set_material(0, wing_mat)
save(wing_mesh)


# --- 8. M-Bot: a sleek black starfighter on an Explorer chassis that hovers ----------
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mbot_mesh

MBOT_DIR = ROOT + "/MBot"
mbot_meshes = {}
for part, mesh in mbot_mesh.build_mbot().items():
    obj = os.path.join(source_dir, "SM_MBot_%s.obj" % part)
    mesh.write_obj(obj, "M-Bot %s, generated by Scripts/mbot_mesh.py" % part)
    t = unreal.AssetImportTask()
    t.set_editor_property("filename", obj)
    t.set_editor_property("destination_path", MBOT_DIR)
    t.set_editor_property("destination_name", "SM_MBot_" + part)
    t.set_editor_property("automated", True)
    t.set_editor_property("replace_existing", True)
    t.set_editor_property("save", True)
    asset_tools.import_asset_tasks([t])
    m = eal.load_asset(MBOT_DIR + "/SM_MBot_" + part)
    if m is None:
        raise RuntimeError("M-Bot import failed: " + part)
    mbot_meshes[part] = m
    log("M-Bot %s: %d triangles, bounds %s" % (part, len(mesh.tris), m.get_bounds().box_extent))

# One-sided lit material, so a camera inside the hull (first person) sees out.
hull_mat_path = MBOT_DIR + "/M_MBotHull"
if eal.does_asset_exist(hull_mat_path):
    hull_mat = eal.load_asset(hull_mat_path)
else:
    hull_mat = asset_tools.create_asset("M_MBotHull", MBOT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel = unreal.MaterialEditingLibrary
    hc = mel.create_material_expression(hull_mat, unreal.MaterialExpressionVectorParameter, -500, 0)
    hc.set_editor_property("parameter_name", "Color")
    hc.set_editor_property("default_value", unreal.LinearColor(0.02, 0.02, 0.025, 1.0))
    mel.connect_material_property(hc, "", unreal.MaterialProperty.MP_BASE_COLOR)
    hm = mel.create_material_expression(hull_mat, unreal.MaterialExpressionConstant, -500, 200)
    hm.set_editor_property("r", 0.7)
    mel.connect_material_property(hm, "", unreal.MaterialProperty.MP_METALLIC)
    hr = mel.create_material_expression(hull_mat, unreal.MaterialExpressionConstant, -500, 300)
    hr.set_editor_property("r", 0.3)
    mel.connect_material_property(hr, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.recompile_material(hull_mat)
    log("Created " + hull_mat_path)
save(hull_mat)

glow_mat = eal.load_asset(ROOT + "/Ring/M_FlyingRingGlow")
MBOT_SCALE = 0.9
parts = []
for part, mat, color, glow in (("Hull", hull_mat, unreal.LinearColor(0.02, 0.02, 0.025, 1.0), 0.0),
                               ("Canopy", glow_mat, unreal.LinearColor(0.03, 0.15, 0.25, 1.0), 1.5),
                               ("Engine", glow_mat, unreal.LinearColor(0.1, 0.45, 1.0, 1.0), 25.0)):
    bp_part = unreal.FlyingBodyPart()
    bp_part.set_editor_property("mesh", mbot_meshes[part])
    bp_part.set_editor_property("material", mat)
    bp_part.set_editor_property("scale", unreal.Vector(MBOT_SCALE, MBOT_SCALE, MBOT_SCALE))
    bp_part.set_editor_property("color", color)
    bp_part.set_editor_property("glow_intensity", glow)
    parts.append(bp_part)

mbot_desc_text = ("A sleek black starfighter that hovers on its acclivity ring. Press F (Y on gamepad) to lift off; "
                  "while hovering Space/A rises and C, Ctrl/B sinks, and once moving it flies like the Flying Truck. "
                  "Burns one Acclivity Stone per minute of flight from its cargo or your inventory.")
bp_mbot = make_blueprint("BP_MBot", load_cls(VEH_DIR + "Explorer/BP_Explorer.BP_Explorer_C"), MBOT_DIR)
add_flight_component(bp_mbot)
compile_bp(bp_mbot)
mt = flight_template(bp_mbot)
mt.set_editor_property("hide_vehicle_mesh", True)
mt.set_editor_property("body_parts", parts)
mt.set_editor_property("flight_wings", False)
mt.set_editor_property("hover", True)
mt.set_editor_property("apply_default_paint", False)
compile_bp(bp_mbot)
mcdo = cdo_of(bp_mbot)
mcdo.set_editor_property("m_display_name", unreal.Text("M-Bot"))
mcdo.set_editor_property("m_description", unreal.Text(mbot_desc_text))
save(bp_mbot)

mbot_desc = make_blueprint("Desc_MBot", load_cls(VEH_DIR + "Explorer/Desc_Explorer.Desc_Explorer_C"), MBOT_DIR)
compile_bp(mbot_desc)
mdc = cdo_of(mbot_desc)
mdc.set_editor_property("m_vehicle_class", bp_mbot.generated_class())
mdc.set_editor_property("m_display_name", unreal.Text("M-Bot"))
mdc.set_editor_property("m_description", unreal.Text(mbot_desc_text))
save(mbot_desc)

mbot_recipe = make_blueprint("Recipe_MBot", load_cls("/Game/FactoryGame/Recipes/Vehicle/Recipe_Explorer.Recipe_Explorer_C"), MBOT_DIR)
compile_bp(mbot_recipe)
mrc = cdo_of(mbot_recipe)
mrc.set_editor_property("m_product", [unreal.ItemAmount(item_class=mbot_desc.generated_class(), amount=1)])
mrc.set_editor_property("m_display_name", unreal.Text("M-Bot"))
save(mbot_recipe)

bp_mbot_schem = make_blueprint("Schematic_MBot", unreal.FGSchematic, MBOT_DIR)
compile_bp(bp_mbot_schem)
ms = cdo_of(bp_mbot_schem)
ms.set_editor_property("m_type", unreal.SchematicType.EST_CUSTOM)
ms.set_editor_property("m_display_name", unreal.Text("M-Bot"))
ms.set_editor_property("m_tech_tier", 0)
ms.set_editor_property("m_time_to_complete", 0.0)
m_unlock = unreal.new_object(BP_UnlockRecipe, outer=ms)
m_unlock.set_editor_property("m_recipes", [mbot_recipe.generated_class()])
ms.set_editor_property("m_unlocks", [m_unlock])
save(bp_mbot_schem)

root.set_editor_property("m_schematics", list(root.get_editor_property("m_schematics")) + [bp_mbot_schem.generated_class()])
save(bp_root)
log("M-Bot saved")

log("All FlyingTrucks assets created and saved.")
