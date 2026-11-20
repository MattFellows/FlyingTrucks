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

log("All FlyingTrucks assets created and saved.")
