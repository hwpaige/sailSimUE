#!/usr/bin/env python3
"""
Create / rebuild M_NavtVertexColor under /Game/Materials/Navt.

Base Color = VertexColor * Tint * ColorBoost
Emissive   = night window glow (dark blue glass verts) + street-lamp / lantern glow

Runtime scalars (set on MIDs by UNantucketStructuresSubsystem):
  Night01, WindowEmissive, LampEmissive, DayGlassGlint, ColorBoost, Tint, Roughness, Metallic
"""
from __future__ import annotations

import unreal

FOLDER = "/Game/Materials/Navt"
MASTER = "M_NavtVertexColor"
MEL = unreal.MaterialEditingLibrary


def ensure_folder(path: str) -> None:
    if not unreal.EditorAssetLibrary.does_directory_exist(path):
        unreal.EditorAssetLibrary.make_directory(path)


def scalar(mat, name: str, default: float, x: int, y: int, group="Surface", lo=0.0, hi=1.0):
    n = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, x, y)
    n.set_editor_property("parameter_name", name)
    n.set_editor_property("default_value", default)
    n.set_editor_property("group", group)
    n.set_editor_property("slider_min", lo)
    n.set_editor_property("slider_max", hi)
    return n


def vector(mat, name: str, default: unreal.LinearColor, x: int, y: int, group="Surface"):
    n = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, x, y)
    n.set_editor_property("parameter_name", name)
    n.set_editor_property("default_value", default)
    n.set_editor_property("group", group)
    return n


def const(mat, v: float, x: int, y: int):
    n = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, x, y)
    n.set_editor_property("r", v)
    return n


def mul(mat, a, a_out, b, b_out, x, y):
    n = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, x, y)
    MEL.connect_material_expressions(a, a_out, n, "A")
    MEL.connect_material_expressions(b, b_out, n, "B")
    return n


def add(mat, a, a_out, b, b_out, x, y):
    n = MEL.create_material_expression(mat, unreal.MaterialExpressionAdd, x, y)
    MEL.connect_material_expressions(a, a_out, n, "A")
    MEL.connect_material_expressions(b, b_out, n, "B")
    return n


def sub(mat, a, a_out, b, b_out, x, y):
    n = MEL.create_material_expression(mat, unreal.MaterialExpressionSubtract, x, y)
    MEL.connect_material_expressions(a, a_out, n, "A")
    MEL.connect_material_expressions(b, b_out, n, "B")
    return n


def sat(mat, a, a_out, x, y):
    n = MEL.create_material_expression(mat, unreal.MaterialExpressionSaturate, x, y)
    MEL.connect_material_expressions(a, a_out, n, "")
    return n


def build(mat: unreal.Material) -> None:
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property("two_sided", False)

    vcol = MEL.create_material_expression(mat, unreal.MaterialExpressionVertexColor, -1200, 0)
    tint = vector(mat, "Tint", unreal.LinearColor(1, 1, 1, 1), -1200, 120)
    boost = scalar(mat, "ColorBoost", 1.15, -1200, 220, lo=0.5, hi=2.5)
    base = mul(mat, vcol, "", tint, "", -900, 40)
    base = mul(mat, base, "", boost, "", -700, 40)

    metal = scalar(mat, "Metallic", 0.0, -1200, 400)
    rough = scalar(mat, "Roughness", 0.72, -1200, 480)
    spec = scalar(mat, "Specular", 0.35, -1200, 560)

    MEL.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.connect_material_property(spec, "", unreal.MaterialProperty.MP_SPECULAR)

    # Component masks for R/G/B
    r = MEL.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -1000, 700)
    r.set_editor_property("r", True)
    r.set_editor_property("g", False)
    r.set_editor_property("b", False)
    r.set_editor_property("a", False)
    MEL.connect_material_expressions(vcol, "", r, "")

    g = MEL.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -1000, 780)
    g.set_editor_property("r", False)
    g.set_editor_property("g", True)
    g.set_editor_property("b", False)
    g.set_editor_property("a", False)
    MEL.connect_material_expressions(vcol, "", g, "")

    b = MEL.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -1000, 860)
    b.set_editor_property("r", False)
    b.set_editor_property("g", False)
    b.set_editor_property("b", True)
    b.set_editor_property("a", False)
    MEL.connect_material_expressions(vcol, "", b, "")

    night = scalar(mat, "Night01", 0.0, -1200, 960, group="Lights", lo=0.0, hi=1.5)
    win_str = scalar(mat, "WindowEmissive", 3.2, -1200, 1040, group="Lights", lo=0.0, hi=10.0)
    lamp_str = scalar(mat, "LampEmissive", 5.0, -1200, 1120, group="Lights", lo=0.0, hi=14.0)
    day_glint = scalar(mat, "DayGlassGlint", 0.10, -1200, 1200, group="Lights", lo=0.0, hi=0.6)

    # maxc ≈ max(R,G,B) via max nodes
    max_rg = MEL.create_material_expression(mat, unreal.MaterialExpressionMax, -800, 700)
    MEL.connect_material_expressions(r, "", max_rg, "A")
    MEL.connect_material_expressions(g, "", max_rg, "B")
    max_c = MEL.create_material_expression(mat, unreal.MaterialExpressionMax, -620, 700)
    MEL.connect_material_expressions(max_rg, "", max_c, "A")
    MEL.connect_material_expressions(b, "", max_c, "B")

    # glass = sat(B-R)*sat(B-G)*sat(0.42-maxc)
    br = sat(mat, sub(mat, b, "", r, "", -800, 780), "", -620, 780)
    bg = sat(mat, sub(mat, b, "", g, "", -800, 840), "", -620, 840)
    thr = const(mat, 0.42, -800, 900)
    dark = sat(mat, sub(mat, thr, "", max_c, "", -620, 900), "", -440, 900)
    glass = mul(mat, mul(mat, br, "", bg, "", -440, 800), "", dark, "", -260, 820)

    # night windows + soft day glint
    win_n = mul(mat, mul(mat, glass, "", night, "", -80, 780), "", win_str, "", 100, 780)
    win_d = mul(mat, glass, "", day_glint, "", -80, 860)
    win_amt = add(mat, win_n, "", win_d, "", 280, 820)
    warm = vector(mat, "WindowColor", unreal.LinearColor(1.0, 0.72, 0.38, 1), -1200, 1280, "Lights")
    win_emi = mul(mat, warm, "", win_amt, "", 460, 820)

    # glare lamp tip: sat(R-0.90)*sat(G-0.88)*sat(G-B-0.04)
    c90 = const(mat, 0.90, -800, 1000)
    c88 = const(mat, 0.88, -800, 1060)
    c04 = const(mat, 0.04, -800, 1120)
    g1 = sat(mat, sub(mat, r, "", c90, "", -620, 1000), "", -440, 1000)
    g2 = sat(mat, sub(mat, g, "", c88, "", -620, 1060), "", -440, 1060)
    gbm = sub(mat, g, "", b, "", -620, 1120)
    g3 = sat(mat, sub(mat, gbm, "", c04, "", -440, 1120), "", -260, 1120)
    glare = mul(mat, mul(mat, g1, "", g2, "", -80, 1020), "", g3, "", 100, 1040)

    # red lantern: sat(R-max(G,B)-0.12)*sat(R-0.45)
    max_gb = MEL.create_material_expression(mat, unreal.MaterialExpressionMax, -800, 1200)
    MEL.connect_material_expressions(g, "", max_gb, "A")
    MEL.connect_material_expressions(b, "", max_gb, "B")
    c12 = const(mat, 0.12, -800, 1260)
    c45 = const(mat, 0.45, -800, 1320)
    rdom = sub(mat, r, "", max_gb, "", -620, 1200)
    lr1 = sat(mat, sub(mat, rdom, "", c12, "", -440, 1200), "", -260, 1200)
    lr2 = sat(mat, sub(mat, r, "", c45, "", -440, 1260), "", -260, 1260)
    lantern = mul(mat, lr1, "", lr2, "", -80, 1220)

    # always-on + night boost: 0.45 + 0.75*Night01
    c45b = const(mat, 0.45, -80, 1340)
    c75 = const(mat, 0.75, -80, 1400)
    night_boost = add(mat, c45b, "", mul(mat, night, "", c75, "", 100, 1380), "", 280, 1360)

    lamp_col_param = vector(
        mat, "LampColor", unreal.LinearColor(1.0, 0.92, 0.7, 1), -1200, 1360, "Lights"
    )
    red_col = vector(
        mat, "LanternRedColor", unreal.LinearColor(1.0, 0.22, 0.08, 1), -1200, 1440, "Lights"
    )
    glare_amt = mul(mat, mul(mat, glare, "", lamp_str, "", 100, 1040), "", night_boost, "", 280, 1040)
    lamp_glare_emi = mul(mat, lamp_col_param, "", glare_amt, "", 460, 1040)
    lan_amt = mul(mat, mul(mat, lantern, "", lamp_str, "", 100, 1240), "", night_boost, "", 280, 1240)
    lamp_red_emi = mul(mat, red_col, "", lan_amt, "", 460, 1240)
    lamp_emi = add(mat, lamp_glare_emi, "", lamp_red_emi, "", 700, 1120)

    emi = add(mat, win_emi, "", lamp_emi, "", 1180, 900)
    MEL.connect_material_property(emi, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)


def main() -> None:
    ensure_folder(FOLDER)
    asset_path = f"{FOLDER}/{MASTER}"
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        mat = unreal.EditorAssetLibrary.load_asset(asset_path)
        MEL.delete_all_material_expressions(mat)
        unreal.log(f"Rebuilding {asset_path}")
    else:
        asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
        factory = unreal.MaterialFactoryNew()
        mat = asset_tools.create_asset(MASTER, FOLDER, unreal.Material, factory)
        unreal.log(f"Created {asset_path}")

    build(mat)
    MEL.layout_material_expressions(mat)
    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(asset_path)
    unreal.log(f"Saved {asset_path} — windows + street lamps")


if __name__ == "__main__":
    main()
