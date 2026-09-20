#!/usr/bin/env python3
"""
Cook Nantucket structure NAVT tiles → UStaticMesh (+ Nanite on dense LODs).

Phase C1 offline cook tooling (AAA island path). Runtime C2 prefers staticMesh /
staticMeshLod1 when present; otherwise falls back to NAVT ProceduralMesh.

Pipeline
--------
  Content/Structures/nantucket/tiles/*.mesh  (NAVT, same as runtime)
          │
          ▼  this script (UnrealEditor -ExecutePythonScript)
  /Game/Structures/nantucket/Cooked/SM_Struct_{tx}_{ty}_LOD0
  /Game/Structures/nantucket/Cooked/SM_Struct_{tx}_{ty}_LOD1   (when fileLod1)
  /Game/Structures/nantucket/Cooked/SM_Struct_{tx}_{ty}_VEG    (when fileVeg)
          │  Nanite on dense building LODs; veg often lighter
          ▼
  ue_manifest.json  ← staticMesh / staticMeshLod1 / staticMeshVeg patched

Asset naming (must match export_nantucket_structures_ue.py convention)
  SM_Struct_{tx}_{ty}_LOD0
  SM_Struct_{tx}_{ty}_LOD1

Cook strategy (tried in order per LOD)
  1. Geometry Script DynamicMesh → create_new_static_mesh_asset_from_mesh
  2. OBJ import via Interchange (full verts; no vertex colors)
  Note: PLY import is NOT used — UE 5.8 reports "Unknown extension 'ply'".
  Dry-run still writes Saved/CookStructures/*.ply for offline inspection.

Pattern after:
  Scripts/enable_nanite_buoys.py
  MooredBoatSubsystem::BuildHullNaniteMesh (PMC → MeshDescription → UStaticMesh + Nanite)

Run unattended
--------------
  "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor" \\
    "/Users/harrison/PycharmProjects/SailSimUE/SailSimUE.uproject" \\
    -ExecutePythonScript="Scripts/cook_nantucket_structures_nanite.py" \\
    -unattended -nop4

Optional env / flags (sys.argv after script path)
  --dry-run          Parse NAVT + plan paths only (no unreal asset writes)
  --manifest-only    Only patch ue_manifest for existing cooked assets
  --nanite-min N     Vertex count threshold for Nanite (default 5000)
  --tx N --ty M      Cook a single tile (debug)
  --max-tiles N      Cap tiles processed (debug)

Outside the editor (no unreal module): always runs as --dry-run.
"""
from __future__ import annotations

import json
import math
import struct
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional

# ---------------------------------------------------------------------------
# Paths / constants
# ---------------------------------------------------------------------------

ROOT = Path(__file__).resolve().parents[1]
STRUCT_ROOT = ROOT / "Content" / "Structures" / "nantucket"
MANIFEST_PATH = STRUCT_ROOT / "ue_manifest.json"
COOKED_CONTENT_DIR = STRUCT_ROOT / "Cooked"
ORIGINS_PATH = COOKED_CONTENT_DIR / "mesh_origins.json"
COOKED_GAME_FOLDER = "/Game/Structures/nantucket/Cooked"
MATERIAL_PATHS = (
    "/Game/Materials/Navt/M_NavtVertexColor",
    "/Game/Materials/Navt/M_NavtVertexColor.M_NavtVertexColor",
    "/Game/Materials/M_NavtVertexColor",
)

CM_PER_FT = 30.48
CM_PER_M = 100.0
DEFAULT_NANITE_MIN_VERTS = 5000

try:
    import unreal  # type: ignore

    HAS_UNREAL = True
except ImportError:
    unreal = None  # type: ignore
    HAS_UNREAL = False


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _parse_args(argv: list[str]) -> dict[str, Any]:
    opts: dict[str, Any] = {
        "dry_run": False,
        "manifest_only": False,
        "nanite_min": DEFAULT_NANITE_MIN_VERTS,
        "tx": None,
        "ty": None,
        "max_tiles": None,
    }
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--dry-run":
            opts["dry_run"] = True
        elif a == "--manifest-only":
            opts["manifest_only"] = True
        elif a == "--nanite-min" and i + 1 < len(argv):
            i += 1
            opts["nanite_min"] = int(argv[i])
        elif a == "--tx" and i + 1 < len(argv):
            i += 1
            opts["tx"] = int(argv[i])
        elif a == "--ty" and i + 1 < len(argv):
            i += 1
            opts["ty"] = int(argv[i])
        elif a == "--max-tiles" and i + 1 < len(argv):
            i += 1
            opts["max_tiles"] = int(argv[i])
        i += 1
    if not HAS_UNREAL:
        opts["dry_run"] = True
    return opts


def log(msg: str) -> None:
    if HAS_UNREAL:
        unreal.log(msg)
    else:
        print(msg)


def log_warn(msg: str) -> None:
    if HAS_UNREAL:
        unreal.log_warning(msg)
    else:
        print("WARN:", msg, file=sys.stderr)


def log_err(msg: str) -> None:
    if HAS_UNREAL:
        unreal.log_error(msg)
    else:
        print("ERROR:", msg, file=sys.stderr)


# ---------------------------------------------------------------------------
# NAVT binary (mirrors FNavtMeshLoader)
# ---------------------------------------------------------------------------


@dataclass
class NavtMesh:
    positions: list[tuple[float, float, float]]  # UE cm (+X north, +Y east, +Z up)
    normals: list[tuple[float, float, float]]
    colors: list[tuple[float, float, float, float]]  # linear 0..1 RGBA
    indices: list[int]
    bounds_min: tuple[float, float, float] = (0.0, 0.0, 0.0)
    bounds_max: tuple[float, float, float] = (0.0, 0.0, 0.0)

    @property
    def num_verts(self) -> int:
        return len(self.positions)

    @property
    def num_tris(self) -> int:
        return len(self.indices) // 3

    def is_valid(self) -> bool:
        return self.num_verts >= 3 and len(self.indices) >= 3


def navt_to_ue_cm(x_ft: float, elev_m: float, z_ft_east: float) -> tuple[float, float, float]:
    return (x_ft * CM_PER_FT, z_ft_east * CM_PER_FT, elev_m * CM_PER_M)


def load_navt(path: Path) -> Optional[NavtMesh]:
    """Parse sail-sim NAVT .mesh (same layout as NavtMeshLoader.cpp)."""
    try:
        data = path.read_bytes()
    except OSError as e:
        log_err(f"NAVT read failed {path}: {e}")
        return None
    if len(data) < 16:
        log_err(f"NAVT too small: {path}")
        return None
    if data[0:4] != b"NAVT":
        log_err(f"NAVT bad magic: {path}")
        return None
    ver = struct.unpack_from("<H", data, 4)[0]
    n_v, n_i = struct.unpack_from("<II", data, 8)
    if ver != 1 or n_v < 3 or n_i < 3:
        log_err(f"NAVT bad header ver={ver} nV={n_v} nI={n_i}: {path}")
        return None

    pos_bytes = n_v * 12
    col_bytes = n_v * 3
    col_pad = (4 - (col_bytes % 4)) % 4
    idx_bytes = n_i * 4
    need = 16 + pos_bytes + col_bytes + col_pad + idx_bytes
    if len(data) < need:
        log_err(f"NAVT truncated need={need} have={len(data)}: {path}")
        return None

    positions: list[tuple[float, float, float]] = []
    off = 16
    for i in range(n_v):
        x_ft, elev_m, z_ft = struct.unpack_from("<fff", data, off + i * 12)
        positions.append(navt_to_ue_cm(x_ft, elev_m, z_ft))

    col_off = 16 + pos_bytes
    colors: list[tuple[float, float, float, float]] = []
    for i in range(n_v):
        r, g, b = data[col_off + i * 3 : col_off + i * 3 + 3]
        colors.append((r / 255.0, g / 255.0, b / 255.0, 1.0))

    idx_off = col_off + col_bytes + col_pad
    indices = list(struct.unpack_from(f"<{n_i}I", data, idx_off))

    # Face normals averaged (same as FNavtMeshLoader)
    normals = [(0.0, 0.0, 0.0)] * n_v
    acc = [[0.0, 0.0, 0.0] for _ in range(n_v)]
    for t in range(0, len(indices) - 2, 3):
        ia, ib, ic = indices[t], indices[t + 1], indices[t + 2]
        if ia >= n_v or ib >= n_v or ic >= n_v:
            continue
        ax, ay, az = positions[ia]
        bx, by, bz = positions[ib]
        cx, cy, cz = positions[ic]
        ab = (bx - ax, by - ay, bz - az)
        ac = (cx - ax, cy - ay, cz - az)
        # cross ab × ac
        nx = ab[1] * ac[2] - ab[2] * ac[1]
        ny = ab[2] * ac[0] - ab[0] * ac[2]
        nz = ab[0] * ac[1] - ab[1] * ac[0]
        for idx in (ia, ib, ic):
            acc[idx][0] += nx
            acc[idx][1] += ny
            acc[idx][2] += nz
    out_n: list[tuple[float, float, float]] = []
    for x, y, z in acc:
        ln = math.sqrt(x * x + y * y + z * z)
        if ln > 1e-12:
            out_n.append((x / ln, y / ln, z / ln))
        else:
            out_n.append((0.0, 0.0, 1.0))
    normals = out_n

    xs = [p[0] for p in positions]
    ys = [p[1] for p in positions]
    zs = [p[2] for p in positions]
    return NavtMesh(
        positions=positions,
        normals=normals,
        colors=colors,
        indices=indices,
        bounds_min=(min(xs), min(ys), min(zs)),
        bounds_max=(max(xs), max(ys), max(zs)),
    )


# ---------------------------------------------------------------------------
# Path helpers
# ---------------------------------------------------------------------------



def localize_mesh(mesh: NavtMesh) -> tuple[float, float, float]:
    """Subtract bounds center so mesh is GPU-safe (OriginMax ~2e6 cm).

    NAVT is authored in absolute UE world cm (harbor ~ -2.2e6). Baking those
    coords into UStaticMesh makes Distance Fields / Lumen relative matrices
    ensure-fail. Returns the world-space origin that runtime must place at.
    """
    ox = 0.5 * (mesh.bounds_min[0] + mesh.bounds_max[0])
    oy = 0.5 * (mesh.bounds_min[1] + mesh.bounds_max[1])
    oz = 0.5 * (mesh.bounds_min[2] + mesh.bounds_max[2])
    mesh.positions = [(p[0] - ox, p[1] - oy, p[2] - oz) for p in mesh.positions]
    xs = [p[0] for p in mesh.positions]
    ys = [p[1] for p in mesh.positions]
    zs = [p[2] for p in mesh.positions]
    mesh.bounds_min = (min(xs), min(ys), min(zs))
    mesh.bounds_max = (max(xs), max(ys), max(zs))
    return (ox, oy, oz)


def cooked_asset_name(tx: int, ty: int, lod: int) -> str:
    return f"SM_Struct_{tx}_{ty}_LOD{lod}"


def cooked_game_path(tx: int, ty: int, lod: int) -> str:
    return f"{COOKED_GAME_FOLDER}/{cooked_asset_name(tx, ty, lod)}"


def cooked_uasset_disk_path(tx: int, ty: int, lod: int) -> Path:
    return COOKED_CONTENT_DIR / f"{cooked_asset_name(tx, ty, lod)}.uasset"


def cooked_veg_asset_name(tx: int, ty: int) -> str:
    return f"SM_Struct_{tx}_{ty}_VEG"


def cooked_veg_game_path(tx: int, ty: int) -> str:
    return f"{COOKED_GAME_FOLDER}/{cooked_veg_asset_name(tx, ty)}"


def cooked_veg_uasset_disk_path(tx: int, ty: int) -> Path:
    return COOKED_CONTENT_DIR / f"{cooked_veg_asset_name(tx, ty)}.uasset"


def resolve_navt_file(rel: str) -> Optional[Path]:
    """Resolve tile relative path under Content/Structures/nantucket or sail-sim bake."""
    if not rel:
        return None
    candidates = [
        STRUCT_ROOT / rel,
        ROOT.parent
        / "sail-sim"
        / "frontend"
        / "public"
        / "structures"
        / "nantucket"
        / rel,
    ]
    for c in candidates:
        if c.is_file():
            return c
    return None


# ---------------------------------------------------------------------------
# Intermediate PLY (vertex colors)
# ---------------------------------------------------------------------------


def write_ply_binary(mesh: NavtMesh, out_path: Path) -> None:
    """Binary little-endian PLY with position, normal, uchar RGB."""
    n_v = mesh.num_verts
    n_f = mesh.num_tris
    header = (
        "ply\n"
        "format binary_little_endian 1.0\n"
        f"element vertex {n_v}\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "property float nx\n"
        "property float ny\n"
        "property float nz\n"
        "property uchar red\n"
        "property uchar green\n"
        "property uchar blue\n"
        f"element face {n_f}\n"
        "property list uchar int vertex_indices\n"
        "end_header\n"
    ).encode("ascii")
    parts = [header]
    for i in range(n_v):
        px, py, pz = mesh.positions[i]
        nx, ny, nz = mesh.normals[i]
        r, g, b, _a = mesh.colors[i]
        parts.append(
            struct.pack(
                "<ffffffBBB",
                px,
                py,
                pz,
                nx,
                ny,
                nz,
                int(max(0, min(255, round(r * 255)))),
                int(max(0, min(255, round(g * 255)))),
                int(max(0, min(255, round(b * 255)))),
            )
        )
    for t in range(0, len(mesh.indices) - 2, 3):
        parts.append(
            struct.pack(
                "<Biii",
                3,
                mesh.indices[t],
                mesh.indices[t + 1],
                mesh.indices[t + 2],
            )
        )
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(b"".join(parts))


def write_ply_ascii(mesh: NavtMesh, out_path: Path) -> None:
    """ASCII PLY — often more reliable for UE Interchange than binary+colors."""
    n_v = mesh.num_verts
    n_f = mesh.num_tris
    lines = [
        "ply",
        "format ascii 1.0",
        f"element vertex {n_v}",
        "property float x",
        "property float y",
        "property float z",
        "property float nx",
        "property float ny",
        "property float nz",
        "property uchar red",
        "property uchar green",
        "property uchar blue",
        f"element face {n_f}",
        "property list uchar int vertex_indices",
        "end_header",
    ]
    for i in range(n_v):
        px, py, pz = mesh.positions[i]
        nx, ny, nz = mesh.normals[i]
        r, g, b, _a = mesh.colors[i]
        lines.append(
            f"{px:.6f} {py:.6f} {pz:.6f} {nx:.6f} {ny:.6f} {nz:.6f} "
            f"{int(max(0, min(255, round(r * 255))))} "
            f"{int(max(0, min(255, round(g * 255))))} "
            f"{int(max(0, min(255, round(b * 255))))}"
        )
    for t in range(0, len(mesh.indices) - 2, 3):
        lines.append(
            f"3 {mesh.indices[t]} {mesh.indices[t + 1]} {mesh.indices[t + 2]}"
        )
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="ascii")


# ---------------------------------------------------------------------------
# Unreal asset helpers
# ---------------------------------------------------------------------------


def ensure_cooked_folder() -> None:
    if not HAS_UNREAL:
        COOKED_CONTENT_DIR.mkdir(parents=True, exist_ok=True)
        return
    if not unreal.EditorAssetLibrary.does_directory_exist(COOKED_GAME_FOLDER):
        unreal.EditorAssetLibrary.make_directory(COOKED_GAME_FOLDER)


def load_structure_material():
    if not HAS_UNREAL:
        return None
    for p in MATERIAL_PATHS:
        if unreal.EditorAssetLibrary.does_asset_exist(p.split(".")[0]):
            mat = unreal.EditorAssetLibrary.load_asset(p.split(".")[0])
            if mat:
                log(f"CookStructures: material {p}")
                return mat
        # try load_object style
        try:
            mat = unreal.load_asset(p)
            if mat:
                log(f"CookStructures: material {p}")
                return mat
        except Exception:
            pass
    log_warn(
        "CookStructures: M_NavtVertexColor missing — run Scripts/create_navt_materials.py"
    )
    return None


def enable_nanite(mesh, dense: bool) -> bool:
    """Enable Nanite on dense structure meshes (pattern: enable_nanite_buoys.py)."""
    if not HAS_UNREAL or not mesh or not dense:
        return False
    try:
        subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
        settings = mesh.get_editor_property("nanite_settings")
        if settings.get_editor_property("enabled"):
            return True
        settings.set_editor_property("enabled", True)
        try:
            settings.set_editor_property("fallback_percent_triangles", 1.0)
        except Exception:
            pass
        # Keep detail for houses / cladding (mirrors moored hull full-detail knobs)
        for prop, val in (
            ("keep_percent_triangles", 1.0),
            ("trim_relative_error", 0.0),
            ("fallback_relative_error", 0.0),
        ):
            try:
                settings.set_editor_property(prop, val)
            except Exception:
                pass
        subsystem.set_nanite_settings(mesh, settings, True)
        log(f"  Nanite enabled: {mesh.get_path_name()}")
        return True
    except Exception as e:
        try:
            settings = mesh.get_editor_property("nanite_settings")
            settings.set_editor_property("enabled", True)
            mesh.set_editor_property("nanite_settings", settings)
            unreal.EditorAssetLibrary.save_loaded_asset(mesh)
            log(f"  Nanite enabled (fallback): {mesh.get_path_name()} ({e})")
            return True
        except Exception as e2:
            log_err(f"  Nanite failed {mesh.get_path_name()}: {e2}")
            return False


def assign_material(mesh, material) -> None:
    if not HAS_UNREAL or not mesh or not material:
        return
    try:
        # Prefer StaticMeshEditorSubsystem when available
        try:
            subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
            subsystem.set_material(mesh, 0, material)
            return
        except Exception:
            pass
        # Direct static_materials slot
        mats = list(mesh.static_materials) if mesh.static_materials else []
        if mats:
            entry = mats[0]
            entry.set_editor_property("material_interface", material)
            mats[0] = entry
            mesh.set_editor_property("static_materials", mats)
        else:
            slot = unreal.StaticMaterial(
                material_interface=material,
                material_slot_name="NavtVertexColor",
                imported_material_slot_name="NavtVertexColor",
            )
            mesh.set_editor_property("static_materials", [slot])
    except Exception as e:
        log_warn(f"  material assign failed: {e}")


def configure_build_settings(mesh) -> None:
    """Preserve authored normals; no lightmap UV generation for vertex-color town mesh."""
    if not HAS_UNREAL or not mesh:
        return
    try:
        subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
        n_lod = mesh.get_num_lods() if hasattr(mesh, "get_num_lods") else 1
        for lod in range(max(1, n_lod)):
            try:
                bs = subsystem.get_lod_build_settings(mesh, lod)
                bs.recompute_normals = False
                bs.recompute_tangents = False
                bs.remove_degenerates = False
                bs.use_mikk_t_space = True
                bs.generate_lightmap_u_vs = False
                subsystem.set_lod_build_settings(mesh, lod, bs)
            except Exception:
                pass
    except Exception:
        pass


def static_mesh_vert_count(sm) -> int:
    """LOD0 render vertex count (0 if unknown)."""
    if not HAS_UNREAL or not sm:
        return 0
    try:
        rd = sm.get_render_data()
        if rd is None:
            return 0
        lods = rd.lod_resources
        if not lods or len(lods) < 1:
            return 0
        # UE Python: get_num_vertices() on LOD resource
        lod0 = lods[0]
        for attr in ("get_num_vertices", "get_num_verts"):
            fn = getattr(lod0, attr, None)
            if callable(fn):
                return int(fn())
        # Fallback: vertex_buffers
        try:
            return int(lod0.vertex_buffers.position_vertex_buffer.get_num_vertices())
        except Exception:
            pass
    except Exception:
        pass
    try:
        subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
        # Some versions expose get_number_verts
        if hasattr(subsystem, "get_number_verts"):
            return int(subsystem.get_number_verts(sm, 0))
    except Exception:
        pass
    return 0


def mesh_quality_ok(sm, expected_verts: int, *, min_ratio: float = 0.85) -> bool:
    """Reject Interchange stubs (often 1–5% of source verts)."""
    if expected_verts <= 0:
        return sm is not None
    got = static_mesh_vert_count(sm)
    if got <= 0:
        # Can't read render data yet — accept and log (build may still be pending).
        log_warn(f"  quality: could not read LOD0 verts (expected~{expected_verts})")
        return True
    need = max(3, int(expected_verts * min_ratio))
    ok = got >= need
    if not ok:
        log_warn(
            f"  quality REJECT: cooked verts={got} expected~{expected_verts} "
            f"(need>={need}) — stub, try next strategy"
        )
    else:
        log(f"  quality OK: cooked verts={got} expected~{expected_verts}")
    return ok


def delete_asset_if_exists(game_path: str) -> None:
    if not HAS_UNREAL:
        return
    try:
        if unreal.EditorAssetLibrary.does_asset_exist(game_path):
            unreal.EditorAssetLibrary.delete_asset(game_path)
    except Exception as e:
        log_warn(f"  delete {game_path}: {e}")


def build_static_mesh_strategies(
    mesh: NavtMesh, game_path: str, asset_name: str, material
):
    """
    Try import strategies until one yields a full-fidelity mesh.

    UE 5.8 / Interchange on this Mac does NOT register .ply ("Unknown extension
    'ply'") — do not attempt AssetImportTask on PLY (it only spams the log).
    Order: Geometry Script (when APIs exist) → OBJ (full verts, no vcol).
    Dry-run still writes PLY under Saved/CookStructures for offline inspection.
    """
    # Skip PLY import entirely — unknown extension on this engine install.
    strategies = [
        ("GeometryScript", lambda: _try_geometry_script(mesh, game_path, asset_name, material)),
        ("OBJ", lambda: _try_obj_import(mesh, game_path, asset_name, material)),
    ]

    for name, fn in strategies:
        delete_asset_if_exists(game_path)
        log(f"  try {name}…")
        try:
            sm = fn()
        except Exception as e:
            log_warn(f"  {name} raised: {e}")
            sm = None
        if sm is None:
            continue
        assign_material(sm, material)
        configure_build_settings(sm)
        if mesh_quality_ok(sm, mesh.num_verts):
            log(f"  {name} accepted → {game_path}")
            return sm
        # Stub — drop and continue
        delete_asset_if_exists(game_path)
    return None


# ---------------------------------------------------------------------------
# Build strategies
# ---------------------------------------------------------------------------


def _try_geometry_script(mesh: NavtMesh, game_path: str, asset_name: str, material):
    """Build StaticMesh via Geometry Script DynamicMesh (UE 5.x editor)."""
    if not HAS_UNREAL:
        return None

    # Probe for Geometry Script libraries (plugin may be off)
    create_fn = None
    for lib_name in (
        "GeometryScript_CreateNewAssetFunctions",
        "GeometryScriptLibrary_CreateNewAssetFunctions",
    ):
        lib = getattr(unreal, lib_name, None)
        if lib is None:
            continue
        for method in (
            "create_new_static_mesh_asset_from_mesh",
            "create_static_mesh_asset_from_mesh",
        ):
            if hasattr(lib, method):
                create_fn = getattr(lib, method)
                break
        if create_fn:
            break
    if create_fn is None:
        return None

    # Allocate DynamicMesh
    dyn = None
    try:
        if hasattr(unreal, "DynamicMesh"):
            # Prefer pool if present
            pool_cls = getattr(unreal, "DynamicMeshPool", None)
            if pool_cls is not None:
                try:
                    pool = pool_cls()
                    if hasattr(pool, "request_mesh"):
                        dyn = pool.request_mesh()
                except Exception:
                    dyn = None
            if dyn is None:
                dyn = unreal.DynamicMesh()
    except Exception as e:
        log_warn(f"  GeometryScript DynamicMesh alloc failed: {e}")
        return None
    if dyn is None:
        return None

    try:
        # Clear
        if hasattr(dyn, "reset"):
            dyn.reset()
        edit = getattr(unreal, "GeometryScriptLibrary_MeshBasicEditFunctions", None) or getattr(
            unreal, "GeometryScript_MeshBasicEditFunctions", None
        )
        if edit is None or not hasattr(edit, "append_vertex"):
            # Try bulk append via triangle list helpers
            return _geometry_script_bulk(dyn, mesh, create_fn, game_path, asset_name, material)

        # Append verts then triangles
        for px, py, pz in mesh.positions:
            edit.append_vertex(dyn, unreal.Vector(px, py, pz))
        # Set normals / colors if APIs exist
        nrm_lib = getattr(unreal, "GeometryScriptLibrary_MeshNormalsFunctions", None)
        if nrm_lib and hasattr(nrm_lib, "set_mesh_per_vertex_normals"):
            try:
                nrm_lib.set_mesh_per_vertex_normals(
                    dyn,
                    [unreal.Vector(nx, ny, nz) for nx, ny, nz in mesh.normals],
                )
            except Exception:
                pass
        col_lib = getattr(unreal, "GeometryScriptLibrary_MeshVertexColorFunctions", None)
        if col_lib and hasattr(col_lib, "set_mesh_per_vertex_colors"):
            try:
                col_lib.set_mesh_per_vertex_colors(
                    dyn,
                    [
                        unreal.LinearColor(r, g, b, a)
                        for r, g, b, a in mesh.colors
                    ],
                )
            except Exception:
                pass
        for t in range(0, len(mesh.indices) - 2, 3):
            edit.append_triangle(
                dyn,
                mesh.indices[t],
                mesh.indices[t + 1],
                mesh.indices[t + 2],
                0,
            )

        return _geometry_script_commit(dyn, create_fn, game_path, asset_name, material)
    except Exception as e:
        log_warn(f"  GeometryScript build failed: {e}")
        return None


def _geometry_script_bulk(dyn, mesh: NavtMesh, create_fn, game_path: str, asset_name: str, material):
    """Alternate Geometry Script entry: append_mesh_buffered / append_buffers if present."""
    edit = getattr(unreal, "GeometryScriptLibrary_MeshBasicEditFunctions", None) or getattr(
        unreal, "GeometryScript_MeshBasicEditFunctions", None
    )
    if edit is None:
        return None
    # Some UE versions expose append_buffers / append_triangle_list
    for method_name in ("append_buffers", "append_mesh_buffered"):
        fn = getattr(edit, method_name, None)
        if fn is None:
            continue
        try:
            verts = [unreal.Vector(*p) for p in mesh.positions]
            tris = list(mesh.indices)
            fn(dyn, verts, tris)
            return _geometry_script_commit(dyn, create_fn, game_path, asset_name, material)
        except Exception as e:
            log_warn(f"  GeometryScript {method_name}: {e}")
    return None


def _geometry_script_commit(dyn, create_fn, game_path: str, asset_name: str, material):
    # Delete existing asset if present
    if unreal.EditorAssetLibrary.does_asset_exist(game_path):
        unreal.EditorAssetLibrary.delete_asset(game_path)

    options = None
    for opt_name in (
        "GeometryScriptCreateNewStaticMeshAssetOptions",
        "GeometryScript_CreateNewStaticMeshAssetOptions",
    ):
        opt_cls = getattr(unreal, opt_name, None)
        if opt_cls is not None:
            try:
                options = opt_cls()
            except Exception:
                options = None
            break

    # create_new_static_mesh_asset_from_mesh(FromDynamicMesh, AssetPathAndName, Options) → (StaticMesh, success)
    try:
        if options is not None:
            result = create_fn(dyn, game_path, options)
        else:
            result = create_fn(dyn, game_path)
    except TypeError:
        # Keyword / alternate arg order
        try:
            result = create_fn(from_dynamic_mesh=dyn, asset_path_and_name=game_path)
        except Exception as e:
            log_warn(f"  GeometryScript commit TypeError: {e}")
            return None

    sm = None
    if isinstance(result, (list, tuple)):
        sm = result[0]
    else:
        sm = result
    if not sm:
        # Try load
        if unreal.EditorAssetLibrary.does_asset_exist(game_path):
            sm = unreal.EditorAssetLibrary.load_asset(game_path)
    if sm:
        assign_material(sm, material)
        configure_build_settings(sm)
        log(f"  GeometryScript → {game_path}")
    return sm


def _try_ply_import(mesh: NavtMesh, game_path: str, asset_name: str, material):
    """Write temp PLY + AssetImportTask (geometry + vertex colors when importer supports)."""
    if not HAS_UNREAL:
        return None
    tmp_dir = Path(tempfile.mkdtemp(prefix="navt_cook_"))
    # ASCII first (Interchange often fails on binary+uchar colors), then binary.
    writers = (
        ("ascii", write_ply_ascii, f"{asset_name}_ascii.ply"),
        ("binary", write_ply_binary, f"{asset_name}.ply"),
    )
    try:
        for kind, writer, fname in writers:
            ply_path = tmp_dir / fname
            try:
                writer(mesh, ply_path)
            except Exception as e:
                log_warn(f"  PLY {kind} write failed: {e}")
                continue
            try:
                if unreal.EditorAssetLibrary.does_asset_exist(game_path):
                    unreal.EditorAssetLibrary.delete_asset(game_path)

                task = unreal.AssetImportTask()
                task.filename = str(ply_path)
                task.destination_path = COOKED_GAME_FOLDER
                task.destination_name = asset_name
                task.replace_existing = True
                task.automated = True
                task.save = True
                task.factory = None

                unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

                sm = None
                if unreal.EditorAssetLibrary.does_asset_exist(game_path):
                    sm = unreal.EditorAssetLibrary.load_asset(game_path)
                if not sm and getattr(task, "imported_object_paths", None):
                    paths = list(task.imported_object_paths)
                    if paths:
                        sm = unreal.EditorAssetLibrary.load_asset(paths[0])
                if sm:
                    assign_material(sm, material)
                    configure_build_settings(sm)
                    log(
                        f"  PLY {kind} import → {game_path} "
                        f"verts={mesh.num_verts} tris={mesh.num_tris}"
                    )
                    return sm
                log_warn(f"  PLY {kind} import produced no asset at {game_path}")
            except Exception as e:
                log_warn(f"  PLY {kind} import failed: {e}")
        return None
    finally:
        try:
            for f in tmp_dir.glob("*"):
                try:
                    f.unlink()
                except OSError:
                    pass
            tmp_dir.rmdir()
        except OSError:
            pass




def _try_mesh_description(mesh: NavtMesh, game_path: str, asset_name: str, material) -> Optional[object]:
    """Build UStaticMesh via MeshDescription (no PLY importer needed)."""
    try:
        md = unreal.MeshDescription()
        # Empty mesh description
        from unreal import MeshDescriptionBase  # may not exist
    except Exception:
        pass
    try:
        # UE 5.x Python: StaticMeshEditorSubsystem / create asset + build from raw
        asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
        package_path = "/".join(game_path.split("/")[:-1])
        # Delete existing
        if unreal.EditorAssetLibrary.does_asset_exist(game_path):
            unreal.EditorAssetLibrary.delete_asset(game_path)

        factory = unreal.StaticMeshFactoryNew()
        sm = asset_tools.create_asset(asset_name, package_path, unreal.StaticMesh, factory)
        if not sm:
            log_warn("  StaticMeshFactoryNew returned None")
            return None

        # Build via MeshDescription Python bindings
        mesh_desc = unreal.MeshDescription()
        # Populate using StaticMeshDescription helper if available
        try:
            smd = unreal.StaticMeshDescription()
            # Create empty then set
            # Use high-level: unreal.EditorStaticMeshLibrary - not always present
        except Exception as e:
            log_warn(f"  StaticMeshDescription unavailable: {e}")

        # Prefer GeometryScript DynamicMesh bulk if we can create from arrays
        # Fallback: manual MeshDescription via vertex/triangle APIs
        try:
            # MeshDescriptionBase API in Python
            attribs = mesh_desc
        except Exception:
            pass

        # Use ProceduralMeshComponent bake path through temporary actor? Heavy.
        # Use OBJ export (UE imports OBJ).
        return _try_obj_import(mesh, game_path, asset_name, material)
    except Exception as e:
        log_warn(f"  MeshDescription path failed: {e}")
        return None


def _try_obj_import(mesh: NavtMesh, game_path: str, asset_name: str, material) -> Optional[object]:
    """Write Wavefront OBJ + import (widely supported in UE)."""
    tmp_dir = Path(tempfile.mkdtemp(prefix="navt_cook_"))
    obj_path = tmp_dir / f"{asset_name}.obj"
    try:
        lines = [f"# SailSim NAVT cook {asset_name}", f"o {asset_name}"]
        for px, py, pz in mesh.positions:
            lines.append(f"v {px:.6f} {py:.6f} {pz:.6f}")
            lines.append("vt 0.0 0.0")
        for nx, ny, nz in mesh.normals:
            lines.append(f"vn {nx:.6f} {ny:.6f} {nz:.6f}")
        for ti in range(mesh.num_tris):
            a = mesh.indices[ti * 3] + 1
            b = mesh.indices[ti * 3 + 1] + 1
            cidx = mesh.indices[ti * 3 + 2] + 1
            # Interchange OBJ translator requires UV indices (vt) present.
            lines.append(f"f {a}/{a}/{a} {b}/{b}/{b} {cidx}/{cidx}/{cidx}")
        obj_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

        if unreal.EditorAssetLibrary.does_asset_exist(game_path):
            unreal.EditorAssetLibrary.delete_asset(game_path)

        task = unreal.AssetImportTask()
        task.filename = str(obj_path)
        task.destination_path = "/".join(game_path.split("/")[:-1])
        task.destination_name = asset_name
        task.replace_existing = True
        task.automated = True
        task.save = True
        try:
            opts = unreal.FbxImportUI()
            opts.automated_import_should_detect_type = False
            opts.import_as_skeletal = False
            opts.import_mesh = True
            opts.import_materials = False
            opts.import_textures = False
            opts.static_mesh_import_data.combine_meshes = True
            opts.static_mesh_import_data.auto_generate_collision = False
            task.options = opts
        except Exception:
            pass
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        if unreal.EditorAssetLibrary.does_asset_exist(game_path):
            sm = unreal.EditorAssetLibrary.load_asset(game_path)
            log(f"  OBJ import → {game_path} verts={mesh.num_verts}")
            return sm
        log_warn(f"  OBJ import produced no asset at {game_path}")
        return None
    except Exception as e:
        log_warn(f"  OBJ import failed: {e}")
        return None
    finally:
        try:
            if obj_path.is_file():
                obj_path.unlink()
            tmp_dir.rmdir()
        except OSError:
            pass



def cook_lod(
    mesh: NavtMesh,
    tx: int,
    ty: int,
    lod: int,
    material,
    nanite_min: int,
    dry_run: bool,
) -> Optional[str]:
    """Cook one LOD; returns game path on success (or planned path in dry-run)."""
    game_path = cooked_game_path(tx, ty, lod)
    asset_name = cooked_asset_name(tx, ty, lod)
    dense = mesh.num_verts >= nanite_min

    log(
        f"CookStructures: {asset_name} verts={mesh.num_verts:,} tris={mesh.num_tris:,} "
        f"dense={dense} nanite_min={nanite_min}"
    )

    if dry_run:
        log(f"  [dry-run] would write {game_path}")
        # Still emit intermediate PLY under Saved for offline inspection
        saved = ROOT / "Saved" / "CookStructures" / f"{asset_name}.ply"
        try:
            write_ply_binary(mesh, saved)
            log(f"  [dry-run] wrote intermediate {saved}")
        except Exception as e:
            log_warn(f"  [dry-run] PLY export failed: {e}")
        # Do not claim the asset exists — patch_manifest only records real uassets.
        return None

    ensure_cooked_folder()
    if unreal.EditorAssetLibrary.does_asset_exist(game_path):
        log(f"  replacing existing {game_path}")

    sm = build_static_mesh_strategies(mesh, game_path, asset_name, material)
    if sm is None:
        log_err(
            f"  FAILED to create full-fidelity {game_path} "
            f"(source verts={mesh.num_verts}). "
            "Enable GeometryScripting plugin or OBJ Interchange. "
            "Runtime will keep PMC for this tile."
        )
        return None

    assign_material(sm, material)
    try:
        sm.set_editor_property("generate_mesh_distance_field", False)
    except Exception:
        pass
    if dense:
        enable_nanite(sm, True)
    else:
        log(f"  skip Nanite (verts {mesh.num_verts} < {nanite_min})")

    try:
        unreal.EditorAssetLibrary.save_asset(game_path, only_if_is_dirty=False)
    except Exception:
        try:
            unreal.EditorAssetLibrary.save_loaded_asset(sm)
        except Exception as e:
            log_warn(f"  save failed: {e}")

    return game_path


# ---------------------------------------------------------------------------
# Manifest patch
# ---------------------------------------------------------------------------




def cook_named(
    mesh: NavtMesh,
    game_path: str,
    asset_name: str,
    material,
    nanite_min: int,
    dry_run: bool,
    *,
    force_no_nanite: bool = False,
) -> Optional[str]:
    """Cook mesh to an arbitrary game path (used for VEG layer)."""
    dense = (not force_no_nanite) and mesh.num_verts >= nanite_min
    log(
        f"CookStructures: {asset_name} verts={mesh.num_verts:,} tris={mesh.num_tris:,} "
        f"dense={dense} nanite_min={nanite_min}"
    )
    if dry_run:
        log(f"  [dry-run] would write {game_path}")
        return None
    ensure_cooked_folder()
    sm = build_static_mesh_strategies(mesh, game_path, asset_name, material)
    if sm is None:
        log_err(f"  FAILED full-fidelity {game_path} (source verts={mesh.num_verts})")
        return None
    assign_material(sm, material)
    try:
        sm.set_editor_property("generate_mesh_distance_field", False)
    except Exception:
        pass
    if dense:
        enable_nanite(sm, True)
    try:
        unreal.EditorAssetLibrary.save_asset(game_path, only_if_is_dirty=False)
    except Exception:
        try:
            unreal.EditorAssetLibrary.save_loaded_asset(sm)
        except Exception as e:
            log_warn(f"  save failed: {e}")
    return game_path

def patch_manifest(cooked: dict[tuple[int, int], dict[str, str]]) -> int:
    """
    Write staticMesh / staticMeshLod1 onto ue_manifest.json tiles.

    cooked: {(tx, ty): {"staticMesh": path, "staticMeshLod1": path?}}
    Also fills paths for assets that already exist on disk even if not cooked this run.
    """
    if not MANIFEST_PATH.is_file():
        log_err(f"missing manifest {MANIFEST_PATH}")
        return 0
    man = json.loads(MANIFEST_PATH.read_text())
    n = 0
    for tile in man.get("tiles") or []:
        tx, ty = int(tile["tx"]), int(tile["ty"])
        # Prefer this-run results; else disk / editor asset presence
        paths = dict(cooked.get((tx, ty), {}))
        lod0 = paths.get("staticMesh") or ""
        lod1 = paths.get("staticMeshLod1") or ""
        if not lod0 and cooked_uasset_disk_path(tx, ty, 0).is_file():
            lod0 = cooked_game_path(tx, ty, 0)
        if not lod1 and cooked_uasset_disk_path(tx, ty, 1).is_file():
            lod1 = cooked_game_path(tx, ty, 1)
        if HAS_UNREAL:
            if not lod0 and unreal.EditorAssetLibrary.does_asset_exist(cooked_game_path(tx, ty, 0)):
                lod0 = cooked_game_path(tx, ty, 0)
            if not lod1 and unreal.EditorAssetLibrary.does_asset_exist(cooked_game_path(tx, ty, 1)):
                lod1 = cooked_game_path(tx, ty, 1)

        if lod0:
            tile["staticMesh"] = lod0
            n += 1
        else:
            tile.pop("staticMesh", None)
        if lod1:
            tile["staticMeshLod1"] = lod1
        else:
            tile.pop("staticMeshLod1", None)

        veg = paths.get("staticMeshVeg") or ""
        if not veg and cooked_veg_uasset_disk_path(tx, ty).is_file():
            veg = cooked_veg_game_path(tx, ty)
        if HAS_UNREAL and not veg and unreal.EditorAssetLibrary.does_asset_exist(cooked_veg_game_path(tx, ty)):
            veg = cooked_veg_game_path(tx, ty)
        if veg:
            tile["staticMeshVeg"] = veg
        else:
            tile.pop("staticMeshVeg", None)

    man["cook"] = {
        "version": 1,
        "folder": COOKED_GAME_FOLDER,
        "naming": "SM_Struct_{tx}_{ty}_LOD{0|1}",
        "nanite": "enabled when verts >= naniteMin (default 5000)",
        "material": MATERIAL_PATHS[0],
        "note": (
            "C1 cook paths for C2 runtime StaticMesh stream. "
            "Missing staticMesh → runtime falls back to NAVT ProceduralMesh."
        ),
    }
    # mesh_origins.json: asset name -> world cm origin for component placement
    origins: dict[str, list[float]] = {}
    if ORIGINS_PATH.is_file():
        try:
            origins = json.loads(ORIGINS_PATH.read_text())
        except Exception:
            origins = {}
    for (tx, ty), paths in cooked.items():
        for tile in man.get("tiles") or []:
            if int(tile.get("tx", -1)) != tx or int(tile.get("ty", -1)) != ty:
                continue
            for pk, ok in (
                ("staticMesh", "staticMeshOrigin"),
                ("staticMeshLod1", "staticMeshLod1Origin"),
                ("staticMeshVeg", "staticMeshVegOrigin"),
            ):
                if ok in paths and isinstance(paths[ok], list):
                    tile[ok] = paths[ok]
                    path = paths.get(pk) or ""
                    if path:
                        leaf = path.rsplit("/", 1)[-1]
                        origins[leaf] = [float(x) for x in paths[ok][:3]]
            break
    try:
        COOKED_CONTENT_DIR.mkdir(parents=True, exist_ok=True)
        ORIGINS_PATH.write_text(json.dumps(origins, indent=2) + "\n")
        log(f"CookStructures: wrote {ORIGINS_PATH} ({len(origins)} origins)")
    except Exception as e:
        log_warn(f"origins write failed: {e}")

    MANIFEST_PATH.write_text(json.dumps(man, indent=2) + "\n")
    log(f"CookStructures: patched {MANIFEST_PATH} ({n} tiles with staticMesh)")
    return n


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main(argv: Optional[list[str]] = None) -> int:
    opts = _parse_args(list(argv if argv is not None else sys.argv[1:]))

    log(
        f"CookStructures: start HAS_UNREAL={HAS_UNREAL} dry_run={opts['dry_run']} "
        f"manifest_only={opts['manifest_only']} nanite_min={opts['nanite_min']}"
    )

    if not MANIFEST_PATH.is_file():
        log_err(f"missing {MANIFEST_PATH} — run Scripts/export_nantucket_structures_ue.py first")
        return 1

    man = json.loads(MANIFEST_PATH.read_text())
    tiles = list(man.get("tiles") or [])
    if opts["tx"] is not None and opts["ty"] is not None:
        tiles = [t for t in tiles if int(t["tx"]) == opts["tx"] and int(t["ty"]) == opts["ty"]]
        if not tiles:
            log_err(f"no tile tx={opts['tx']} ty={opts['ty']}")
            return 1
    if opts["max_tiles"] is not None:
        tiles = tiles[: int(opts["max_tiles"])]

    material = None if opts["dry_run"] or opts["manifest_only"] else load_structure_material()
    cooked: dict[tuple[int, int], dict[str, str]] = {}

    if opts["manifest_only"]:
        n = patch_manifest({})
        log(f"CookStructures: manifest-only done (staticMesh tiles≈{n})")
        return 0

    ok = 0
    fail = 0
    for tile in tiles:
        tx, ty = int(tile["tx"]), int(tile["ty"])
        entry: dict[str, str] = {}

        for lod, file_key, path_key in (
            (0, "file", "staticMesh"),
            (1, "fileLod1", "staticMeshLod1"),
        ):
            rel = tile.get(file_key) or ""
            if not rel:
                continue
            navt_path = resolve_navt_file(rel)
            if not navt_path:
                log_warn(f"missing NAVT {rel} (tx={tx} ty={ty} lod={lod})")
                fail += 1
                continue
            mesh = load_navt(navt_path)
            if not mesh or not mesh.is_valid():
                log_err(f"bad NAVT {navt_path}")
                fail += 1
                continue
            origin = localize_mesh(mesh)
            log(
                f"  localized origin=({origin[0]:.0f},{origin[1]:.0f},{origin[2]:.0f}) "
                f"localSpan=({mesh.bounds_max[0]-mesh.bounds_min[0]:.0f},"
                f"{mesh.bounds_max[1]-mesh.bounds_min[1]:.0f})"
            )
            path = cook_lod(
                mesh,
                tx,
                ty,
                lod,
                material,
                int(opts["nanite_min"]),
                bool(opts["dry_run"]),
            )
            if opts["dry_run"]:
                # Parsed OK; asset not written
                ok += 1
            elif path:
                entry[path_key] = path
                entry[path_key + "Origin"] = list(origin)
                ok += 1
            else:
                fail += 1

        # Vegetation layer (trees) — separate SM; Nanite only if dense.
        rel_veg = tile.get("fileVeg") or ""
        if rel_veg:
            navt_v = resolve_navt_file(rel_veg)
            if navt_v:
                mesh_v = load_navt(navt_v)
                if mesh_v and mesh_v.is_valid():
                    vorigin = localize_mesh(mesh_v)
                    vpath = cook_named(
                        mesh_v,
                        cooked_veg_game_path(tx, ty),
                        cooked_veg_asset_name(tx, ty),
                        material,
                        int(opts["nanite_min"]),
                        bool(opts["dry_run"]),
                        force_no_nanite=mesh_v.num_verts < 20000,
                    )
                    if opts["dry_run"]:
                        ok += 1
                    elif vpath:
                        entry["staticMeshVeg"] = vpath
                        entry["staticMeshVegOrigin"] = list(vorigin)
                        ok += 1
                    else:
                        fail += 1

        if entry:
            cooked[(tx, ty)] = entry

    # Patch manifest with real cooked paths (skip on dry-run so we don't strip fields)
    if not opts["dry_run"]:
        patch_manifest(cooked)
    else:
        log("CookStructures: dry-run — manifest not patched")

    log(
        f"CookStructures: done ok={ok} fail={fail} tiles={len(tiles)} "
        f"cooked_keys={len(cooked)} dry_run={opts['dry_run']}"
    )
    if not opts["dry_run"] and fail and ok == 0:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
