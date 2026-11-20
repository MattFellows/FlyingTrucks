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

# --- 5. Root game world module -------------------------------------------
bp_root = make_blueprint("RootGameWorld_FlyingTrucks", unreal.GameWorldModule)
compile_bp(bp_root)
root = cdo_of(bp_root)
root.set_editor_property("root_module", True)
root.set_editor_property("m_schematics", [bp_schem.generated_class()])
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

log("All FlyingTrucks assets created and saved.")
