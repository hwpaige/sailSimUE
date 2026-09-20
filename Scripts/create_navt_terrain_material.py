#!/usr/bin/env python3
"""
Create / rebuild M_NavtTerrain + MI_NavtTerrain (AAA-style land material).

Pattern used by Epic samples for vertex-colored ground meshes:
  Base Color = VertexColor.rgb * ColorBoost * Tint
  Optional mild season multiply on green-dominant pixels only
  Roughness high (soil/grass)
  No WorldPosition hacks (those flash with camera/Lumen)

Assign MI_NavtTerrain (material instance) at runtime; override Season01 on MIDs.

Run:
  UnrealEditor SailSimUE.uproject -ExecutePythonScript=Scripts/create_navt_terrain_material.py
"""
from __future__ import annotations

import unreal

FOLDER = "/Game/Materials/Navt"
MASTER = "M_NavtTerrain"
INSTANCE = "MI_NavtTerrain"
MEL = unreal.MaterialEditingLibrary


def ensure_folder(path: str) -> None:
    if not unreal.EditorAssetLibrary.does_directory_exist(path):
        unreal.EditorAssetLibrary.make_directory(path)


def scalar(mat, name, default, x, y, group="Surface", lo=0.0, hi=1.0):
    n = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, x, y)
    n.set_editor_property("parameter_name", name)
    n.set_editor_property("default_value", default)
    n.set_editor_property("group", group)
    n.set_editor_property("slider_min", lo)
    n.set_editor_property("slider_max", hi)
    return n


def vector(mat, name, default, x, y, group="Surface"):
    n = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, x, y)
    n.set_editor_property("parameter_name", name)
    n.set_editor_property("default_value", default)
    n.set_editor_property("group", group)
    return n


# Shore + season: push sand warmer, wet band darker, foliage season multiply only.
SEASON_CODE = r"""
float3 c = saturate(Base);
float R = c.r, G = c.g, B = c.b;
// Sand: warm high R+G, not green-dominant
float warm = saturate(((R + G) * 0.5 - B) * 10.0) * saturate((R - 0.35) * 3.0);
float notGreen = saturate(1.0 - (G - max(R, B)) * 12.0);
float sand = saturate(warm * notGreen);
// Wet sand: darker / cooler when overall dark warm
float wet = sand * saturate((0.55 - (R + G + B) / 3.0) * 4.0);
float3 drySand = float3(0.92, 0.82, 0.58);
float3 wetSand = float3(0.55, 0.48, 0.36);
c = lerp(c, drySand, sand * 0.55);
c = lerp(c, wetSand, wet * 0.7);

// Green foliage season only
float foliage = saturate((G - R) * 8.0) * saturate((G - B) * 6.0) * saturate((G - 0.15) * 4.0);
foliage *= saturate(1.0 - sand);
float s = saturate(Season);
float3 springM = float3(0.95, 1.12, 0.92);
float3 summerM = float3(1.0, 1.0, 1.0);
float3 autumnM = float3(1.25, 0.85, 0.55);
float3 winterM = float3(0.85, 0.88, 0.90);
float3 m = summerM;
if (s < 0.25) { float t = s / 0.25; m = lerp(winterM, springM, t); }
else if (s < 0.5) { float t = (s - 0.25) / 0.25; m = lerp(springM, summerM, t); }
else if (s < 0.75) { float t = (s - 0.5) / 0.25; m = lerp(summerM, autumnM, t); }
else { float t = (s - 0.75) / 0.25; m = lerp(autumnM, winterM, t); }
c = lerp(c, saturate(c * m), foliage * 0.65);
return saturate(c);
"""


def build_master(mat: unreal.Material) -> None:
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property("two_sided", False)
    # Epic outdoor ground: no specular hotspots
    try:
        mat.set_editor_property("b_tangent_space_normal", True)
    except Exception:
        pass

    vcol = MEL.create_material_expression(mat, unreal.MaterialExpressionVertexColor, -1000, 0)
    tint = vector(mat, "Tint", unreal.LinearColor(1, 1, 1, 1), -1000, 140)
    boost = scalar(mat, "ColorBoost", 1.35, -1000, 240, lo=0.5, hi=2.5)
    season = scalar(mat, "Season01", 0.5, -1000, 340, group="Season", lo=0.0, hi=1.0)
    rough = scalar(mat, "Roughness", 0.92, -1000, 440, lo=0.2, hi=1.0)
    metal = scalar(mat, "Metallic", 0.0, -1000, 520, lo=0.0, hi=1.0)
    spec = scalar(mat, "Specular", 0.2, -1000, 600, lo=0.0, hi=1.0)

    # Base = VertexColor * Tint * ColorBoost  (standard UE vertex-color mesh path)
    mul1 = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -700, 40)
    MEL.connect_material_expressions(vcol, "", mul1, "A")
    MEL.connect_material_expressions(tint, "", mul1, "B")
    mul2 = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -500, 40)
    MEL.connect_material_expressions(mul1, "", mul2, "A")
    MEL.connect_material_expressions(boost, "", mul2, "B")

    def make_custom_input(name: str):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", name)
        return ci

    season_fx = MEL.create_material_expression(mat, unreal.MaterialExpressionCustom, -280, 40)
    season_fx.set_editor_property("description", "Mild seasonal foliage multiply")
    season_fx.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    season_fx.set_editor_property("inputs", [])
    season_fx.set_editor_property(
        "inputs",
        [make_custom_input("Base"), make_custom_input("Season")],
    )
    season_fx.set_editor_property("code", SEASON_CODE)
    MEL.connect_material_expressions(mul2, "", season_fx, "Base")
    MEL.connect_material_expressions(season, "", season_fx, "Season")

    MEL.connect_material_property(season_fx, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.connect_material_property(spec, "", unreal.MaterialProperty.MP_SPECULAR)


def ensure_instance(master_path: str) -> None:
    inst_path = f"{FOLDER}/{INSTANCE}"
    parent = unreal.EditorAssetLibrary.load_asset(master_path)
    if not parent:
        unreal.log_error(f"Missing parent {master_path}")
        return
    if unreal.EditorAssetLibrary.does_asset_exist(inst_path):
        mic = unreal.EditorAssetLibrary.load_asset(inst_path)
        if mic:
            MEL.set_material_instance_parent(mic, parent)
            unreal.EditorAssetLibrary.save_asset(inst_path)
            unreal.log(f"Updated parent on {inst_path}")
        return
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.MaterialInstanceConstantFactoryNew()
    # UE 5.8: set parent on the factory via InitialParent if present, else after create
    for prop in ("InitialParent", "initial_parent", "Parent"):
        try:
            factory.set_editor_property(prop, parent)
            break
        except Exception:
            pass
    mic = tools.create_asset(INSTANCE, FOLDER, unreal.MaterialInstanceConstant, factory)
    if mic:
        MEL.set_material_instance_parent(mic, parent)
        unreal.EditorAssetLibrary.save_asset(inst_path)
        unreal.log(f"Created {inst_path}")


def main() -> None:
    ensure_folder(FOLDER)
    asset_path = f"{FOLDER}/{MASTER}"
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        mat = unreal.EditorAssetLibrary.load_asset(asset_path)
        MEL.delete_all_material_expressions(mat)
        unreal.log(f"Rebuilding {asset_path}")
    else:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        mat = tools.create_asset(MASTER, FOLDER, unreal.Material, unreal.MaterialFactoryNew())
        unreal.log(f"Created {asset_path}")

    build_master(mat)
    MEL.layout_material_expressions(mat)
    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(asset_path)
    ensure_instance(asset_path)
    unreal.log(f"Saved {asset_path} — AAA vertex-color land (assign MI_NavtTerrain)")


if __name__ == "__main__":
    main()
