#!/usr/bin/env python3
"""
Create M_WindCompass_Unlit — two-sided opaque unlit instrument chrome for the wind rose.

Always self-lit (Emissive = BaseColor * scale) so the rose is readable over bright water
without PBR sides going black. Opaque avoids translucency sort / SSR mirror issues.

Params (WindCompassRing MIDs):
  BaseColor       vector  — instrument color
  EmissiveBoost   scalar  — night soft fill (0 day)

Emissive = BaseColor * (1 + EmissiveBoost * 12)

Also writes the legacy path name M_WindCompass_Overlay as a copy alias if needed
(runtime loads Unlit first, then Overlay).

Run:
  "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor" \\
    "/Users/harrison/PycharmProjects/SailSimUE/SailSimUE.uproject" \\
    -ExecutePythonScript="Scripts/create_wind_compass_material.py" -unattended -nop4
"""
from __future__ import annotations

import unreal

FOLDER = "/Game/Materials/Yacht"
NAMES = ("M_WindCompass_Unlit", "M_WindCompass_Overlay")


def ensure_folder(path: str) -> None:
    if not unreal.EditorAssetLibrary.does_directory_exist(path):
        unreal.EditorAssetLibrary.make_directory(path)


def _scalar(mat, name: str, default: float, y: int, lo=0.0, hi=1.0):
    n = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionScalarParameter, -480, y
    )
    n.set_editor_property("parameter_name", name)
    n.set_editor_property("default_value", default)
    n.set_editor_property("group", "Overlay")
    n.set_editor_property("slider_min", lo)
    n.set_editor_property("slider_max", hi)
    return n


def _vector(mat, name: str, default: unreal.LinearColor, y: int):
    n = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionVectorParameter, -480, y
    )
    n.set_editor_property("parameter_name", name)
    n.set_editor_property("default_value", default)
    n.set_editor_property("group", "Overlay")
    return n


def build_unlit(mat: unreal.Material) -> None:
    # Opaque + unlit + two-sided: solid instrument, no sort, visible both faces.
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    for prop, val in (
        ("b_use_translucency_vertex_fog", False),
        ("b_allow_negative_emissive_color", False),
    ):
        try:
            mat.set_editor_property(prop, val)
        except Exception:
            pass

    base = _vector(mat, "BaseColor", unreal.LinearColor(0.92, 0.93, 0.95, 1.0), 0)
    emi = _scalar(mat, "EmissiveBoost", 0.0, 120, lo=0.0, hi=0.5)

    k = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionConstant, -300, 200
    )
    k.set_editor_property("r", 12.0)
    scaled_boost = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionMultiply, -220, 140
    )
    unreal.MaterialEditingLibrary.connect_material_expressions(emi, "", scaled_boost, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(k, "", scaled_boost, "B")

    one = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionConstant, -300, 80
    )
    one.set_editor_property("r", 1.0)
    add = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionAdd, -140, 80
    )
    unreal.MaterialEditingLibrary.connect_material_expressions(one, "", add, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(scaled_boost, "", add, "B")

    mul = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionMultiply, -40, 20
    )
    unreal.MaterialEditingLibrary.connect_material_expressions(base, "", mul, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(add, "", mul, "B")

    # Unlit path: emissive is the only color output that matters.
    unreal.MaterialEditingLibrary.connect_material_property(
        mul, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR
    )

    unreal.MaterialEditingLibrary.layout_material_expressions(mat)
    unreal.MaterialEditingLibrary.recompile_material(mat)


def create_one(name: str) -> None:
    path = f"{FOLDER}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.MaterialFactoryNew()
    mat = asset_tools.create_asset(name, FOLDER, unreal.Material, factory)
    if not mat:
        raise RuntimeError(f"Failed to create {name}")
    build_unlit(mat)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    unreal.log(f"Created {path}")


def main() -> None:
    ensure_folder(FOLDER)
    for name in NAMES:
        create_one(name)
    unreal.log("Wind compass materials ready (opaque unlit two-sided)")


if __name__ == "__main__":
    main()
