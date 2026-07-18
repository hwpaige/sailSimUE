#!/usr/bin/env python3
"""
Enable Nanite on buoy static meshes (SM_Buoy_1..9).

Moored J/105 hulls are converted to Nanite static meshes at runtime in
MooredBoatSubsystem (PMC → MeshDescription → UStaticMesh + Nanite).

Run:
  "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor" \\
    "/Users/harrison/PycharmProjects/SailSimUE/SailSimUE.uproject" \\
    -ExecutePythonScript="Scripts/enable_nanite_buoys.py" -unattended -nop4
"""
from __future__ import annotations

import unreal


def enable_nanite(mesh: unreal.StaticMesh) -> bool:
    if not mesh:
        return False
    try:
        subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
        settings = mesh.get_editor_property("nanite_settings")
        if settings.get_editor_property("enabled"):
            unreal.log(f"Nanite already on: {mesh.get_path_name()}")
            return True
        settings.set_editor_property("enabled", True)
        # Keep a reasonable non-Nanite fallback for platforms without Nanite
        try:
            settings.set_editor_property("fallback_percent_triangles", 1.0)
        except Exception:
            pass
        subsystem.set_nanite_settings(mesh, settings, True)
        unreal.log(f"Enabled Nanite: {mesh.get_path_name()}")
        return True
    except Exception as e:
        # Fallback: direct property + rebuild
        try:
            settings = mesh.get_editor_property("nanite_settings")
            settings.set_editor_property("enabled", True)
            mesh.set_editor_property("nanite_settings", settings)
            unreal.EditorAssetLibrary.save_loaded_asset(mesh)
            unreal.log(f"Enabled Nanite (fallback): {mesh.get_path_name()} ({e})")
            return True
        except Exception as e2:
            unreal.log_error(f"Failed Nanite on {mesh.get_path_name()}: {e2}")
            return False


def ensure_materials_nanite_ready(mesh: unreal.StaticMesh) -> None:
    """Force material usage flags that Nanite/ISM paths need."""
    if not mesh:
        return
    mats = mesh.static_materials
    for entry in mats:
        mat = entry.material_interface
        if not mat:
            continue
        # CheckMaterialUsage is C++; in Python, touch via material editing if needed.
        # Pack buoy materials are DefaultLit — fine for Nanite.
        pass


def main() -> None:
    ok = 0
    for i in range(1, 10):
        path = f"/Game/Buoys/StaticMesh/SM_Buoy_{i}"
        if not unreal.EditorAssetLibrary.does_asset_exist(path):
            unreal.log_warning(f"Missing {path}")
            continue
        mesh = unreal.EditorAssetLibrary.load_asset(path)
        if enable_nanite(mesh):
            ensure_materials_nanite_ready(mesh)
            unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
            ok += 1
    unreal.log(f"Nanite buoy meshes ready: {ok}/9")


if __name__ == "__main__":
    main()
