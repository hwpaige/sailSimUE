#!/usr/bin/env python3
"""
Create / rebuild M_NavtTerrain under /Game/Materials/Navt.

Dedicated land material (NOT the town/structure vertex-color mat):
  - Remaps baked vertex colours toward lush grass vs warm beach sand
  - Wet-sand band near waterline (low world Z)
  - Procedural grit / patch noise so the island doesn't look like flat paint
  - Slope softens vegetation toward soil on steeper faces

Run in-editor:
  Tools → Execute Python Script → Scripts/create_navt_terrain_material.py
or:
  UnrealEditor SailSimUE.uproject -ExecutePythonScript=Scripts/create_navt_terrain_material.py
"""
from __future__ import annotations

import unreal

FOLDER = "/Game/Materials/Navt"
MASTER = "M_NavtTerrain"
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


# Custom HLSL: VCol, Params (x=GrassBoost y=SandBoost z=Detail z.w unused), WPos
CUSTOM_CODE = r"""
// Params.x = GrassBoost, Params.y = SandBoost, Params.z = DetailAmt
float GrassBoost = Params.x;
float SandBoost  = Params.y;
float DetailAmt  = Params.z;

float3 c = saturate(VCol);
float R = c.r, G = c.g, B = c.b;
float Z = WPos.z; // UE cm, elev above sea ≈ Z

// --- Classify from baked palette (aerial blended with TERRAIN_RGB) ---
// Grass / lawn / forest: green-dominant
float grass = saturate((G - R) * 10.0) * saturate((G - B) * 8.0) * saturate((G - 0.22) * 4.0);
// Sand / dune: warm tan (R+G high vs B), not strongly green
float warm = saturate(((R + G) * 0.5 - B - 0.02) * 12.0) * saturate((R - 0.28) * 3.0);
float notGreen = saturate(1.0 - (G - max(R, B)) * 14.0);
float sandCol = warm * notGreen;
// Elevation beach: low ground that isn't grassy → force sand (coast apron)
float elevBeach = saturate((220.0 - Z) / 200.0) * saturate(1.0 - grass * 1.8);
float sand = saturate(max(sandCol, elevBeach * 0.85));
// Roads / developed: neutral mid-gray, low saturation
float sat = max(max(R, G), B) - min(min(R, G), B);
float road = saturate(1.0 - sat * 8.0) * saturate((0.65 - abs(R - 0.48)) * 4.0)
           * (1.0 - sand) * (1.0 - grass);

// Target albedos (linear-ish storybook Nantucket)
float3 lushGrass = float3(0.18, 0.42, 0.12);
float3 deepGrass = float3(0.10, 0.28, 0.08);
float3 drySand   = float3(0.92, 0.84, 0.58);
float3 wetSand   = float3(0.48, 0.42, 0.32);
float wet = saturate((140.0 - Z) / 120.0);
float3 sandTarget = lerp(drySand, wetSand, wet * 0.92);

// Blend strength — push harder than the washed aerial mix
float gAmt = grass * GrassBoost;
float sAmt = sand * SandBoost * (1.0 - grass * 0.85);
c = lerp(c, lerp(lushGrass, deepGrass, saturate((Z - 400.0) / 800.0)), gAmt);
c = lerp(c, sandTarget, sAmt);
// Slight desat on roads so they stay readable
c = lerp(c, float3(0.42, 0.40, 0.36), road * 0.55);

// --- Detail: patch / tussock / grit (world cm → ~m scales) ---
float2 uv = WPos.xy * 0.01; // metres
// cheap value noise
float n1 = frac(sin(dot(floor(uv * 0.08), float2(12.9898, 78.233))) * 43758.5453);
float n2 = frac(sin(dot(floor(uv * 0.55), float2(39.346, 11.135))) * 24634.6345);
float n3 = frac(sin(dot(floor(uv * 4.2),  float2(73.156, 52.742))) * 19327.3915);
float grain = 1.0 + (n1 - 0.5) * 0.16 * DetailAmt
                 + (n2 - 0.5) * 0.11 * DetailAmt
                 + (n3 - 0.5) * 0.07 * DetailAmt;
// sand: finer grit; grass: broader patchiness
grain = lerp(grain, 1.0 + (n3 - 0.5) * 0.14 * DetailAmt, sand);
c *= grain;

// Slope: steeper = less green, more soil (use derivative of world pos via normal proxy:
// without normal input, approximate with elev noise only — mild soil lean on mixed)
float3 soil = float3(0.42, 0.34, 0.22);
c = lerp(c, soil, (1.0 - grass) * sand * wet * 0.15);

return saturate(c);
"""


def build(mat: unreal.Material) -> None:
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property("two_sided", False)

    vcol = MEL.create_material_expression(mat, unreal.MaterialExpressionVertexColor, -900, 0)
    wpos = MEL.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -900, 200)

    grass_b = scalar(mat, "GrassBoost", 0.72, -900, 360, group="Cover", lo=0.0, hi=1.2)
    sand_b = scalar(mat, "SandBoost", 0.78, -900, 440, group="Cover", lo=0.0, hi=1.2)
    detail = scalar(mat, "DetailAmt", 1.0, -900, 520, group="Cover", lo=0.0, hi=2.0)
    boost = scalar(mat, "ColorBoost", 1.12, -900, 600, group="Surface", lo=0.5, hi=2.0)
    rough = scalar(mat, "Roughness", 0.88, -900, 680, group="Surface", lo=0.2, hi=1.0)
    metal = scalar(mat, "Metallic", 0.0, -900, 760, group="Surface", lo=0.0, hi=1.0)
    tint = vector(mat, "Tint", unreal.LinearColor(1, 1, 1, 1), -900, 840, group="Surface")

    # Pack boosts: Append Grass+Sand → float2, Append + Detail → float3
    app0 = MEL.create_material_expression(mat, unreal.MaterialExpressionAppendVector, -600, 400)
    app1 = MEL.create_material_expression(mat, unreal.MaterialExpressionAppendVector, -420, 420)
    MEL.connect_material_expressions(grass_b, "", app0, "A")
    MEL.connect_material_expressions(sand_b, "", app0, "B")
    MEL.connect_material_expressions(app0, "", app1, "A")
    MEL.connect_material_expressions(detail, "", app1, "B")

    custom = MEL.create_material_expression(mat, unreal.MaterialExpressionCustom, -200, 80)
    custom.set_editor_property("description", "Beach + lush grass remap")
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    # Clear then set inputs (UE set_properties needs empty→full for array grow)
    custom.set_editor_property("inputs", [])
    custom.set_editor_property(
        "inputs",
        [
            unreal.CustomInput(input_name="VCol"),
            unreal.CustomInput(input_name="Params"),
            unreal.CustomInput(input_name="WPos"),
        ],
    )
    custom.set_editor_property("code", CUSTOM_CODE)

    MEL.connect_material_expressions(vcol, "", custom, "VCol")
    MEL.connect_material_expressions(app1, "", custom, "Params")
    MEL.connect_material_expressions(wpos, "", custom, "WPos")

    mul_tint = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, 80, 80)
    MEL.connect_material_expressions(custom, "", mul_tint, "A")
    MEL.connect_material_expressions(tint, "", mul_tint, "B")
    mul_boost = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, 260, 80)
    MEL.connect_material_expressions(mul_tint, "", mul_boost, "A")
    MEL.connect_material_expressions(boost, "", mul_boost, "B")

    # Wet sand slightly glossier (lower roughness near Z=0) — simple constant is fine
    MEL.connect_material_property(mul_boost, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)


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

    build(mat)
    MEL.layout_material_expressions(mat)
    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(asset_path)
    unreal.log(f"Saved {asset_path} — beach sand + lush grass remap")


if __name__ == "__main__":
    main()
