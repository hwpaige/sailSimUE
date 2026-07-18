#!/usr/bin/env python3
"""
Create M_WindCompass_Overlay — two-sided unlit translucent chrome for the wind rose.

Why translucent/unlit (not opaque PBR):
  Water reflections here are mostly screen-space (SSR). SSR samples SceneColor from the
  opaque/GBuffer path and ignores PrimitiveComponent.bVisibleInReflections. Drawing the
  compass after that path keeps it visible in the main view without a water mirror image.

Graph params (same names as yacht PBR so WindCompassRing MIDs keep working):
  BaseColor       vector  — day/night instrument color
  EmissiveBoost   scalar  — night soft fill
  Opacity         scalar  — default 1 (solid-looking overlay)

Emissive output = BaseColor * (1 + EmissiveBoost * 12)
  Day (boost 0): BaseColor
  Night (boost ~0.1–0.14): ~2.2–2.7× BaseColor soft instrument glow

Run (editor closed, or paste into Output Log Python):
  "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor" \\
    "/Users/harrison/PycharmProjects/SailSimUE/SailSimUE.uproject" \\
    -ExecutePythonScript="Scripts/create_wind_compass_material.py" -unattended -nop4
"""
from __future__ import annotations

import unreal

FOLDER = "/Game/Materials/Yacht"
NAME = "M_WindCompass_Overlay"


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


def build_overlay(mat: unreal.Material) -> None:
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    for prop, val in (
        ("b_disable_depth_test", False),
        ("b_use_translucency_vertex_fog", False),
        ("b_allow_negative_emissive_color", False),
    ):
        try:
            mat.set_editor_property(prop, val)
        except Exception:
            pass
    try:
        mat.set_editor_property(
            "translucency_lighting_mode",
            unreal.TranslucencyLightingMode.TLM_VOLUMETRIC_NON_DIRECTIONAL,
        )
    except Exception:
        pass

    base = _vector(mat, "BaseColor", unreal.LinearColor(0.9, 0.9, 0.92, 1.0), 0)
    emi = _scalar(mat, "EmissiveBoost", 0.0, 120, lo=0.0, hi=0.5)
    opacity = _scalar(mat, "Opacity", 1.0, 240, lo=0.0, hi=1.0)

    # scale = 1 + EmissiveBoost * 12
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

    unreal.MaterialEditingLibrary.connect_material_property(
        mul, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        opacity, "", unreal.MaterialProperty.MP_OPACITY
    )

    unreal.MaterialEditingLibrary.layout_material_expressions(mat)
    unreal.MaterialEditingLibrary.recompile_material(mat)


def main() -> None:
    ensure_folder(FOLDER)
    path = f"{FOLDER}/{NAME}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.MaterialFactoryNew()
    mat = asset_tools.create_asset(NAME, FOLDER, unreal.Material, factory)
    if not mat:
        raise RuntimeError(f"Failed to create {NAME}")
    build_overlay(mat)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    unreal.log(f"Created {path}")


if __name__ == "__main__":
    main()
