#!/usr/bin/env python3
"""
Create DefaultLit yacht materials under /Game/Materials/Yacht for SailSimUE.

Critical hull paint note:
  The J/105 loft hull is only ~1.5k verts. Painting stripes by *vertex color* or by
  *assigning whole triangles to Z-bands* produces stair-stepped / "pixelated" lines
  because edges follow coarse triangles, not a true waterline plane.

  MI_Yacht_HullPaint uses *local position Z* with hard thresholds so antifoul / boot /
  gelcoat / cove stripe are infinitely sharp horizontal bands independent of mesh density.

Run:
  "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor" \\
    "/Users/harrison/PycharmProjects/SailSimUE/SailSimUE.uproject" \\
    -ExecutePythonScript="Scripts/create_yacht_materials.py" -unattended -nop4
"""
from __future__ import annotations

import unreal

FOLDER = "/Game/Materials/Yacht"
MASTER = "M_Yacht_PBR"
MASTER_2S = "M_Yacht_PBR_TwoSided"
MASTER_HULL = "M_Yacht_HullPaint"


def ensure_folder(path: str) -> None:
    if not unreal.EditorAssetLibrary.does_directory_exist(path):
        unreal.EditorAssetLibrary.make_directory(path)


def _scalar(mat, name: str, default: float, y: int, group: str = "Surface", lo=0.0, hi=1.0):
    n = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionScalarParameter, -480, y
    )
    n.set_editor_property("parameter_name", name)
    n.set_editor_property("default_value", default)
    n.set_editor_property("group", group)
    n.set_editor_property("slider_min", lo)
    n.set_editor_property("slider_max", hi)
    return n


def _vector(mat, name: str, default: unreal.LinearColor, y: int, group: str = "Surface"):
    n = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionVectorParameter, -480, y
    )
    n.set_editor_property("parameter_name", name)
    n.set_editor_property("default_value", default)
    n.set_editor_property("group", group)
    return n


def build_pbr_graph(mat: unreal.Material, two_sided: bool, with_fresnel: bool) -> None:
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property("two_sided", two_sided)

    base = _vector(mat, "BaseColor", unreal.LinearColor(0.96, 0.97, 0.99, 1.0), -120)
    metal = _scalar(mat, "Metallic", 0.0, 420)
    spec = _scalar(mat, "Specular", 0.5, 500)
    rough = _scalar(mat, "Roughness", 0.14 if with_fresnel else 0.7, 580)
    boost = _scalar(mat, "ClearCoatBoost", 0.35 if with_fresnel else 0.05, 200)
    emi = _scalar(mat, "EmissiveBoost", 0.0, 660)
    emi.set_editor_property("slider_max", 0.2)

    if with_fresnel:
        fresnel = unreal.MaterialEditingLibrary.create_material_expression(
            mat, unreal.MaterialExpressionFresnel, -480, 80
        )
        mul_f = unreal.MaterialEditingLibrary.create_material_expression(
            mat, unreal.MaterialExpressionMultiply, -280, 120
        )
        unreal.MaterialEditingLibrary.connect_material_expressions(fresnel, "", mul_f, "A")
        unreal.MaterialEditingLibrary.connect_material_expressions(boost, "", mul_f, "B")

        bright = _vector(mat, "SpecularTint", unreal.LinearColor(1.0, 1.0, 1.0, 1.0), 320)
        lerp_col = unreal.MaterialEditingLibrary.create_material_expression(
            mat, unreal.MaterialExpressionLinearInterpolate, -120, 0
        )
        unreal.MaterialEditingLibrary.connect_material_expressions(base, "", lerp_col, "A")
        unreal.MaterialEditingLibrary.connect_material_expressions(bright, "", lerp_col, "B")
        unreal.MaterialEditingLibrary.connect_material_expressions(mul_f, "", lerp_col, "Alpha")
        base_out = lerp_col
    else:
        _vector(mat, "SpecularTint", unreal.LinearColor(1.0, 1.0, 1.0, 1.0), 320)
        base_out = base

    mul_e = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionMultiply, -120, 660
    )
    unreal.MaterialEditingLibrary.connect_material_expressions(base, "", mul_e, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(emi, "", mul_e, "B")

    unreal.MaterialEditingLibrary.connect_material_property(
        base_out, "", unreal.MaterialProperty.MP_BASE_COLOR
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        metal, "", unreal.MaterialProperty.MP_METALLIC
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        spec, "", unreal.MaterialProperty.MP_SPECULAR
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        rough, "", unreal.MaterialProperty.MP_ROUGHNESS
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        mul_e, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR
    )

    unreal.MaterialEditingLibrary.layout_material_expressions(mat)
    unreal.MaterialEditingLibrary.recompile_material(mat)


def enable_instanced_usage(mat) -> None:
    """Scenery HISM draws nothing unless yacht parents allow instanced static meshes."""
    if not mat:
        return
    mat.set_editor_property("used_with_instanced_static_meshes", True)


def create_or_load_master(name: str, two_sided: bool, with_fresnel: bool) -> unreal.Material:
    path = f"{FOLDER}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.log(f"Exists: {path}")
        mat = unreal.EditorAssetLibrary.load_asset(path)
        enable_instanced_usage(mat)
        unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
        return mat

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.MaterialFactoryNew()
    mat = asset_tools.create_asset(name, FOLDER, unreal.Material, factory)
    if not mat:
        raise RuntimeError(f"Failed to create {name}")
    build_pbr_graph(mat, two_sided=two_sided, with_fresnel=with_fresnel)
    enable_instanced_usage(mat)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    unreal.log(f"Created master {path}")
    return mat


def create_hull_paint_master() -> unreal.Material:
    """
    Local-Z hard-band paint + ClearCoat gelcoat gloss.
    ClearCoat amount/roughness use CustomData0/1 (UE ClearCoat shading model).
    """
    path = f"{FOLDER}/{MASTER_HULL}"
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        # Wipe graph by deleting only after we can recreate.
        unreal.EditorAssetLibrary.delete_asset(path)

    factory = unreal.MaterialFactoryNew()
    mat = asset_tools.create_asset(MASTER_HULL, FOLDER, unreal.Material, factory)
    if not mat:
        raise RuntimeError("Failed to create hull paint master")

    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    # Prefer ClearCoat shading when the Python enum exists; else DefaultLit + ultra-low roughness.
    sm = unreal.MaterialShadingModel.MSM_DEFAULT_LIT
    b_clear = False
    if hasattr(unreal.MaterialShadingModel, "MSM_CLEAR_COAT"):
        sm = unreal.MaterialShadingModel.MSM_CLEAR_COAT
        b_clear = True
    mat.set_editor_property("shading_model", sm)
    mat.set_editor_property("two_sided", False)

    # Resolve CustomData pins for ClearCoat amount/roughness (enum names vary by UE version).
    prop_cc_amt = None
    prop_cc_rough = None
    if b_clear:
        for n0, n1 in (
            ("MP_CUSTOM_DATA0", "MP_CUSTOM_DATA1"),
            ("MP_CUSTOM_DATA_0", "MP_CUSTOM_DATA_1"),
        ):
            if hasattr(unreal.MaterialProperty, n0) and hasattr(unreal.MaterialProperty, n1):
                prop_cc_amt = getattr(unreal.MaterialProperty, n0)
                prop_cc_rough = getattr(unreal.MaterialProperty, n1)
                break
        if prop_cc_amt is None:
            # Enum not exposed — stay ClearCoat with constant defaults via scalar if possible,
            # otherwise drop to DefaultLit (still very glossy via RoughTopsides).
            try:
                vals = list(unreal.MaterialProperty)
                # SceneTypes: CustomData0=16, CustomData1=17 in UE5.8
                if len(vals) > 17:
                    prop_cc_amt = vals[16]
                    prop_cc_rough = vals[17]
            except Exception:
                mat.set_editor_property(
                    "shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT
                )
                b_clear = False

    local_pos = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionLocalPosition, -900, 0
    )
    z_mask = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionComponentMask, -720, 0
    )
    z_mask.set_editor_property("r", False)
    z_mask.set_editor_property("g", False)
    z_mask.set_editor_property("b", True)
    z_mask.set_editor_property("a", False)
    unreal.MaterialEditingLibrary.connect_material_expressions(local_pos, "", z_mask, "")

    z_boot_lo = _scalar(mat, "ZBootLo", 4.0, -200, group="PaintBands", lo=-200.0, hi=200.0)
    z_boot_hi = _scalar(mat, "ZBootHi", 16.0, -100, group="PaintBands", lo=-200.0, hi=200.0)
    z_stripe_lo = _scalar(mat, "ZStripeLo", 52.0, 0, group="PaintBands", lo=-200.0, hi=300.0)
    z_stripe_hi = _scalar(mat, "ZStripeHi", 64.0, 100, group="PaintBands", lo=-200.0, hi=300.0)

    c_anti = _vector(mat, "ColorAntifoul", unreal.LinearColor(0.16, 0.10, 0.08, 1.0), 200, "PaintColors")
    c_boot = _vector(mat, "ColorBoot", unreal.LinearColor(0.03, 0.06, 0.14, 1.0), 300, "PaintColors")
    c_top = _vector(mat, "ColorTopsides", unreal.LinearColor(0.96, 0.975, 0.995, 1.0), 400, "PaintColors")
    c_stripe = _vector(mat, "ColorStripe", unreal.LinearColor(0.04, 0.10, 0.28, 1.0), 500, "PaintColors")

    # Base-layer roughness (under the clear coat). Topsides very smooth.
    r_anti = _scalar(mat, "RoughAntifoul", 0.55, 600, "PaintRough")
    r_boot = _scalar(mat, "RoughBoot", 0.16, 680, "PaintRough")
    r_top = _scalar(mat, "RoughTopsides", 0.06, 760, "PaintRough")
    r_stripe = _scalar(mat, "RoughStripe", 0.10, 840, "PaintRough")

    metal = _scalar(mat, "Metallic", 0.0, 920)
    spec = _scalar(mat, "Specular", 0.5, 1000)
    # Clear coat layer (gelcoat varnish)
    clear_amt = _scalar(mat, "ClearCoat", 1.0, 1080, group="ClearCoat")
    clear_rough = _scalar(mat, "ClearCoatRoughness", 0.05, 1160, group="ClearCoat")

    def _make_custom(name: str, code: str, out_type, input_names, y: int):
        node = unreal.MaterialEditingLibrary.create_material_expression(
            mat, unreal.MaterialExpressionCustom, -200, y
        )
        node.set_editor_property("description", name)
        node.set_editor_property("output_type", out_type)
        node.set_editor_property("code", code)
        custom_inputs = []
        for n in input_names:
            inp = unreal.CustomInput()
            inp.set_editor_property("input_name", n)
            custom_inputs.append(inp)
        node.set_editor_property("inputs", custom_inputs)
        return node

    custom = _make_custom(
        "HullPaintBands",
        "float z = Z;\n"
        "float3 col = Topsides;\n"
        "if (z < BootLo) col = Antifoul;\n"
        "else if (z < BootHi) col = Boot;\n"
        "else if (z >= StripeLo && z < StripeHi) col = Stripe;\n"
        "return col;\n",
        unreal.CustomMaterialOutputType.CMOT_FLOAT3,
        ("Z", "BootLo", "BootHi", "StripeLo", "StripeHi", "Antifoul", "Boot", "Topsides", "Stripe"),
        0,
    )
    unreal.MaterialEditingLibrary.connect_material_expressions(z_mask, "", custom, "Z")
    unreal.MaterialEditingLibrary.connect_material_expressions(z_boot_lo, "", custom, "BootLo")
    unreal.MaterialEditingLibrary.connect_material_expressions(z_boot_hi, "", custom, "BootHi")
    unreal.MaterialEditingLibrary.connect_material_expressions(z_stripe_lo, "", custom, "StripeLo")
    unreal.MaterialEditingLibrary.connect_material_expressions(z_stripe_hi, "", custom, "StripeHi")
    unreal.MaterialEditingLibrary.connect_material_expressions(c_anti, "", custom, "Antifoul")
    unreal.MaterialEditingLibrary.connect_material_expressions(c_boot, "", custom, "Boot")
    unreal.MaterialEditingLibrary.connect_material_expressions(c_top, "", custom, "Topsides")
    unreal.MaterialEditingLibrary.connect_material_expressions(c_stripe, "", custom, "Stripe")

    custom_r = _make_custom(
        "HullPaintRough",
        "float z = Z;\n"
        "float r = RTop;\n"
        "if (z < BootLo) r = RAnti;\n"
        "else if (z < BootHi) r = RBoot;\n"
        "else if (z >= StripeLo && z < StripeHi) r = RStripe;\n"
        "return r;\n",
        unreal.CustomMaterialOutputType.CMOT_FLOAT1,
        ("Z", "BootLo", "BootHi", "StripeLo", "StripeHi", "RAnti", "RBoot", "RTop", "RStripe"),
        280,
    )
    unreal.MaterialEditingLibrary.connect_material_expressions(z_mask, "", custom_r, "Z")
    unreal.MaterialEditingLibrary.connect_material_expressions(z_boot_lo, "", custom_r, "BootLo")
    unreal.MaterialEditingLibrary.connect_material_expressions(z_boot_hi, "", custom_r, "BootHi")
    unreal.MaterialEditingLibrary.connect_material_expressions(z_stripe_lo, "", custom_r, "StripeLo")
    unreal.MaterialEditingLibrary.connect_material_expressions(z_stripe_hi, "", custom_r, "StripeHi")
    unreal.MaterialEditingLibrary.connect_material_expressions(r_anti, "", custom_r, "RAnti")
    unreal.MaterialEditingLibrary.connect_material_expressions(r_boot, "", custom_r, "RBoot")
    unreal.MaterialEditingLibrary.connect_material_expressions(r_top, "", custom_r, "RTop")
    unreal.MaterialEditingLibrary.connect_material_expressions(r_stripe, "", custom_r, "RStripe")

    # Clear coat only above waterline (not on matte antifoul).
    custom_cc = _make_custom(
        "HullClearCoatAmt",
        "float z = Z;\n"
        "if (z < BootLo) return 0.0;\n"
        "return saturate(ClearAmt);\n",
        unreal.CustomMaterialOutputType.CMOT_FLOAT1,
        ("Z", "BootLo", "ClearAmt"),
        480,
    )
    unreal.MaterialEditingLibrary.connect_material_expressions(z_mask, "", custom_cc, "Z")
    unreal.MaterialEditingLibrary.connect_material_expressions(z_boot_lo, "", custom_cc, "BootLo")
    unreal.MaterialEditingLibrary.connect_material_expressions(clear_amt, "", custom_cc, "ClearAmt")

    unreal.MaterialEditingLibrary.connect_material_property(
        custom, "", unreal.MaterialProperty.MP_BASE_COLOR
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        metal, "", unreal.MaterialProperty.MP_METALLIC
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        spec, "", unreal.MaterialProperty.MP_SPECULAR
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        custom_r, "", unreal.MaterialProperty.MP_ROUGHNESS
    )
    if b_clear and prop_cc_amt is not None and prop_cc_rough is not None:
        unreal.MaterialEditingLibrary.connect_material_property(custom_cc, "", prop_cc_amt)
        unreal.MaterialEditingLibrary.connect_material_property(clear_rough, "", prop_cc_rough)
    else:
        # DefaultLit gloss path: fresnel edge sheen so gelcoat still "pops".
        fresnel = unreal.MaterialEditingLibrary.create_material_expression(
            mat, unreal.MaterialExpressionFresnel, -200, 560
        )
        fresnel.set_editor_property("exponent_in", 5.0)
        mul_f = unreal.MaterialEditingLibrary.create_material_expression(
            mat, unreal.MaterialExpressionMultiply, -40, 560
        )
        boost = _scalar(mat, "ClearCoatBoost", 0.35, 1240, group="ClearCoat")
        unreal.MaterialEditingLibrary.connect_material_expressions(fresnel, "", mul_f, "A")
        unreal.MaterialEditingLibrary.connect_material_expressions(boost, "", mul_f, "B")
        white = unreal.MaterialEditingLibrary.create_material_expression(
            mat, unreal.MaterialExpressionConstant3Vector, -200, 640
        )
        white.set_editor_property("constant", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))
        lerp_col = unreal.MaterialEditingLibrary.create_material_expression(
            mat, unreal.MaterialExpressionLinearInterpolate, 120, 0
        )
        unreal.MaterialEditingLibrary.connect_material_expressions(custom, "", lerp_col, "A")
        unreal.MaterialEditingLibrary.connect_material_expressions(white, "", lerp_col, "B")
        unreal.MaterialEditingLibrary.connect_material_expressions(mul_f, "", lerp_col, "Alpha")
        # Re-bind base color through fresnel sheen
        unreal.MaterialEditingLibrary.connect_material_property(
            lerp_col, "", unreal.MaterialProperty.MP_BASE_COLOR
        )

    unreal.MaterialEditingLibrary.layout_material_expressions(mat)
    unreal.MaterialEditingLibrary.recompile_material(mat)
    enable_instanced_usage(mat)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    unreal.log(
        f"Created hull paint master {path} (clearcoat={b_clear} cc_pins={prop_cc_amt is not None})"
    )
    return mat


def create_instance(
    name: str,
    parent_name: str,
    base_color: unreal.LinearColor | None = None,
    metallic: float = 0.0,
    specular: float = 0.5,
    roughness: float = 0.2,
    clearcoat: float = 0.1,
    emissive: float = 0.0,
    extra_scalars: dict | None = None,
    extra_vectors: dict | None = None,
) -> None:
    path = f"{FOLDER}/{name}"
    parent_path = f"{FOLDER}/{parent_name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mi = unreal.EditorAssetLibrary.load_asset(path)
    else:
        asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
        factory = unreal.MaterialInstanceConstantFactoryNew()
        mi = asset_tools.create_asset(name, FOLDER, unreal.MaterialInstanceConstant, factory)
        if not mi:
            raise RuntimeError(f"Failed to create {name}")
        parent = unreal.EditorAssetLibrary.load_asset(parent_path)
        mi.set_editor_property("parent", parent)

    parent = unreal.EditorAssetLibrary.load_asset(parent_path)
    if parent:
        mi.set_editor_property("parent", parent)

    if base_color is not None:
        unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(
            mi, "BaseColor", base_color
        )
        unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(
            mi, "SpecularTint", unreal.LinearColor(1.0, 1.0, 1.0, 1.0)
        )
    unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value(
        mi, "Metallic", metallic
    )
    unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value(
        mi, "Specular", specular
    )
    unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value(
        mi, "Roughness", roughness
    )
    unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value(
        mi, "ClearCoatBoost", clearcoat
    )
    unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value(
        mi, "EmissiveBoost", emissive
    )
    if extra_scalars:
        for k, v in extra_scalars.items():
            unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value(mi, k, v)
    if extra_vectors:
        for k, v in extra_vectors.items():
            unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(mi, k, v)

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    unreal.log(f"Saved instance {path}")


def main() -> None:
    ensure_folder(FOLDER)
    create_or_load_master(MASTER, two_sided=False, with_fresnel=True)
    create_or_load_master(MASTER_2S, two_sided=True, with_fresnel=False)
    create_hull_paint_master()

    create_instance(
        "MI_Yacht_Gelcoat",
        MASTER,
        unreal.LinearColor(0.96, 0.975, 0.995, 1.0),
        metallic=0.0,
        specular=0.5,
        roughness=0.10,
        clearcoat=0.40,
    )
    # Full hull with crisp Z-band paint (player + moored)
    create_instance(
        "MI_Yacht_HullPaint",
        MASTER_HULL,
        base_color=None,
        metallic=0.0,
        specular=0.5,
        roughness=0.06,
        clearcoat=1.0,
        extra_scalars={
            "ZBootLo": 4.0,
            "ZBootHi": 16.0,
            "ZStripeLo": 52.0,
            "ZStripeHi": 64.0,
            "RoughAntifoul": 0.55,
            "RoughBoot": 0.16,
            "RoughTopsides": 0.06,
            "RoughStripe": 0.10,
            "ClearCoat": 1.0,
            "ClearCoatRoughness": 0.05,
        },
        extra_vectors={
            "ColorAntifoul": unreal.LinearColor(0.16, 0.10, 0.08, 1.0),
            "ColorBoot": unreal.LinearColor(0.03, 0.06, 0.14, 1.0),
            "ColorTopsides": unreal.LinearColor(0.96, 0.975, 0.995, 1.0),
            "ColorStripe": unreal.LinearColor(0.04, 0.10, 0.28, 1.0),
        },
    )
    create_instance(
        "MI_Yacht_BootStripe",
        MASTER,
        unreal.LinearColor(0.03, 0.06, 0.14, 1.0),
        metallic=0.0,
        specular=0.5,
        roughness=0.22,
        clearcoat=0.22,
    )
    create_instance(
        "MI_Yacht_HullStripe",
        MASTER,
        unreal.LinearColor(0.04, 0.10, 0.28, 1.0),
        metallic=0.0,
        specular=0.5,
        roughness=0.18,
        clearcoat=0.28,
    )
    create_instance(
        "MI_Yacht_Antifoul",
        MASTER,
        unreal.LinearColor(0.16, 0.10, 0.08, 1.0),
        metallic=0.0,
        specular=0.4,
        roughness=0.62,
        clearcoat=0.02,
    )
    # One-sided white deck (dual-face deck geo + two-sided mat caused z-fight glitches).
    create_instance(
        "MI_Yacht_Deck",
        MASTER,
        unreal.LinearColor(0.97, 0.97, 0.98, 1.0),
        metallic=0.0,
        specular=0.4,
        roughness=0.62,
        clearcoat=0.0,
    )
    create_instance(
        "MI_Yacht_Cabin",
        MASTER_2S,
        unreal.LinearColor(0.93, 0.94, 0.96, 1.0),
        metallic=0.0,
        specular=0.5,
        roughness=0.22,
        clearcoat=0.18,
    )
    create_instance(
        "MI_Yacht_Glass",
        MASTER_2S,
        unreal.LinearColor(0.08, 0.12, 0.16, 1.0),
        metallic=0.0,
        specular=0.5,
        roughness=0.05,
        clearcoat=0.55,
        emissive=0.008,
    )
    create_instance(
        "MI_Yacht_Keel",
        MASTER,
        unreal.LinearColor(0.10, 0.10, 0.11, 1.0),
        metallic=0.0,
        specular=0.45,
        roughness=0.55,
        clearcoat=0.05,
    )
    create_instance(
        "MI_Yacht_Spar",
        MASTER,
        unreal.LinearColor(0.913, 0.921, 0.925, 1.0),
        metallic=1.0,
        specular=0.5,
        roughness=0.28,
        clearcoat=0.10,
    )
    create_instance(
        "MI_Yacht_Rope",
        MASTER,
        unreal.LinearColor(0.12, 0.10, 0.08, 1.0),
        metallic=0.0,
        specular=0.4,
        roughness=0.85,
        clearcoat=0.0,
    )
    create_instance(
        "MI_Yacht_Sail",
        MASTER_2S,
        unreal.LinearColor(0.98, 0.97, 0.94, 1.0),
        metallic=0.0,
        specular=0.45,
        roughness=0.68,
        clearcoat=0.0,
    )

    unreal.EditorAssetLibrary.save_directory(FOLDER)
    unreal.log("Yacht materials ready under /Game/Materials/Yacht (incl. MI_Yacht_HullPaint)")


if __name__ == "__main__":
    main()
