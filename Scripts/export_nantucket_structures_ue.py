#!/usr/bin/env python3
"""Export Nantucket structure tiles for SailSimUE streaming.

Reads sail-sim baked NAVT structure tiles (OSM houses, heroes, ENC lights,
trees, grass, hydrangeas) and writes:

  Content/Structures/nantucket/
    ue_manifest.json   — streaming metadata in UE world cm
    tiles/             — symlink to sail-sim bake (no 80MB duplicate)
    palette.json       — material palette reference
    README.md

Optional per-tile cooked StaticMesh fields (Phase C1 cook / C2 runtime):
  staticMesh       — e.g. /Game/Structures/nantucket/Cooked/SM_Struct_{tx}_{ty}_LOD0
  staticMeshLod1   — e.g. /Game/Structures/nantucket/Cooked/SM_Struct_{tx}_{ty}_LOD1

Sources for those fields (first match wins):
  1) Cooked .uasset on disk at Content/Structures/nantucket/Cooked/SM_Struct_*
  2) Pass-through from sail-sim source manifest if present
  3) Omitted → runtime PMC fallback from file / fileLod1

Existing streaming fields (loadRadius, unloadRadius, lod0Radius, file*, open-sea
distances) are preserved.

Cook rebuild (editor):
  UnrealEditor SailSimUE.uproject \\
    -ExecutePythonScript=Scripts/cook_nantucket_structures_nanite.py -unattended -nop4

Runtime: UNantucketStructuresSubsystem streams a chebyshev ring around the boat
(same pattern as terrain). Harbor tiles are heavy (~500k verts) — keep load
radius tight.

Usage:
  python3 Scripts/export_nantucket_structures_ue.py
"""
from __future__ import annotations

import json
import math
import os
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SAIL_STRUCT = ROOT.parent / "sail-sim" / "frontend" / "public" / "structures" / "nantucket"
OUT = ROOT / "Content" / "Structures" / "nantucket"
COOKED_DIR = OUT / "Cooked"
COOKED_GAME_FOLDER = "/Game/Structures/nantucket/Cooked"
ORIGINS_PATH = COOKED_DIR / "mesh_origins.json"

ORIGIN_LAT = 41.48
ORIGIN_LON = -70.22
FT_PER_DEG_LAT = 364000.0
CM_PER_FT = 30.48


def ft_per_deg_lon(lat: float) -> float:
    return FT_PER_DEG_LAT * math.cos(math.radians(lat))


def lat_lon_to_world_cm(lat: float, lon: float) -> tuple[float, float]:
    x_ft = (lat - ORIGIN_LAT) * FT_PER_DEG_LAT
    y_ft = (lon - ORIGIN_LON) * ft_per_deg_lon(ORIGIN_LAT)
    return x_ft * CM_PER_FT, y_ft * CM_PER_FT


def cooked_game_path(tx: int, ty: int, lod: int) -> str:
    return f"{COOKED_GAME_FOLDER}/SM_Struct_{tx}_{ty}_LOD{lod}"


def cooked_uasset_exists(tx: int, ty: int, lod: int) -> bool:
    return (COOKED_DIR / f"SM_Struct_{tx}_{ty}_LOD{lod}.uasset").is_file()


def cooked_veg_game_path(tx: int, ty: int) -> str:
    return f"{COOKED_GAME_FOLDER}/SM_Struct_{tx}_{ty}_VEG"


def cooked_veg_uasset_exists(tx: int, ty: int) -> bool:
    return (COOKED_DIR / f"SM_Struct_{tx}_{ty}_VEG.uasset").is_file()


def main() -> int:
    man_path = SAIL_STRUCT / "manifest.json"
    if not man_path.is_file():
        print("missing", man_path, file=sys.stderr)
        print("  bake: cd sail-sim/frontend && node scripts/bake-voxel-structures.mjs", file=sys.stderr)
        return 1

    man = json.loads(man_path.read_text())
    OUT.mkdir(parents=True, exist_ok=True)

    tiles_link = OUT / "tiles"
    if tiles_link.is_symlink():
        tiles_link.unlink()
    if not tiles_link.exists():
        try:
            os.symlink(SAIL_STRUCT / "tiles", tiles_link, target_is_directory=True)
            print("symlink tiles ->", SAIL_STRUCT / "tiles")
        except OSError as e:
            print("warn: could not symlink tiles:", e)

    # Palette for material authoring reference
    pal_src = SAIL_STRUCT / "palette.json"
    if pal_src.is_file():
        shutil.copy2(pal_src, OUT / "palette.json")

    (OUT / "source_manifest.json").write_text(json.dumps(man, indent=2))

    stream = man.get("stream") or {}
    # Harbor structure tiles are dense (~500k verts). Prefer tight radii over web
    # terrain defaults (web runtime uses ~1 tile for structures).
    load_r = min(2, int(stream.get("loadRadius", 2)))
    unload_r = max(load_r + 1, min(3, int(stream.get("unloadRadius", 3))))
    lod0 = 1

    tiles_out = []
    total_verts = 0
    source_totals: dict[str, int] = {}
    cooked_lod0 = 0
    cooked_lod1 = 0
    origins = {}
    if ORIGINS_PATH.is_file():
        try:
            origins = json.loads(ORIGINS_PATH.read_text())
        except Exception:
            origins = {}
    for t in man["tiles"]:
        bb = t["bbox"]
        corners = [
            lat_lon_to_world_cm(bb["south"], bb["west"]),
            lat_lon_to_world_cm(bb["south"], bb["east"]),
            lat_lon_to_world_cm(bb["north"], bb["west"]),
            lat_lon_to_world_cm(bb["north"], bb["east"]),
        ]
        xs = [c[0] for c in corners]
        ys = [c[1] for c in corners]
        cx = 0.5 * (min(xs) + max(xs))
        cy = 0.5 * (min(ys) + max(ys))
        verts = int(t.get("vertices", 0))
        total_verts += verts
        for k, v in (t.get("sources") or {}).items():
            source_totals[k] = source_totals.get(k, 0) + int(v or 0)
        file_lod1 = t.get("fileLod1") or ""
        if file_lod1 in (None, "null"):
            file_lod1 = ""
        tx, ty = int(t["tx"]), int(t["ty"])
        # Optional cooked StaticMesh paths (Phase C1).
        # Prefer on-disk Cooked/*.uasset convention; else pass-through from source.
        # Runtime prefers SM when present+loadable; PMC NAVT is the fallback.
        static_mesh = ""
        static_mesh_l1 = ""
        if cooked_uasset_exists(tx, ty, 0):
            static_mesh = cooked_game_path(tx, ty, 0)
            cooked_lod0 += 1
        else:
            static_mesh = t.get("staticMesh") or ""
            if static_mesh in (None, "null"):
                static_mesh = ""
        if file_lod1 and cooked_uasset_exists(tx, ty, 1):
            static_mesh_l1 = cooked_game_path(tx, ty, 1)
            cooked_lod1 += 1
        else:
            static_mesh_l1 = t.get("staticMeshLod1") or ""
            if static_mesh_l1 in (None, "null"):
                static_mesh_l1 = ""
        # AAA foliage: instance JSON (HISM). Legacy voxel _veg.mesh no longer exported.
        file_foliage = t.get("fileFoliage") or ""
        if file_foliage in (None, "null"):
            file_foliage = ""
        if not file_foliage:
            cand = f"tiles/{tx}_{ty}_foliage.json"
            if (SAIL_STRUCT / cand).is_file():
                file_foliage = cand
        foliage_instances = int(t.get("foliageInstances") or 0)
        if foliage_instances <= 0 and file_foliage:
            try:
                fdoc = json.loads((SAIL_STRUCT / file_foliage).read_text())
                foliage_instances = int(fdoc.get("count") or len(fdoc.get("instances") or []))
            except Exception:
                foliage_instances = 0
        # Never point building file at a veg mesh (would double-draw / skip buildings).
        file_main = t.get("file") or ""
        if file_main and ("_veg." in str(file_main) or "_foliage." in str(file_main)):
            file_main = ""
        tile_entry = {
            "id": t["id"],
            "tx": tx,
            "ty": ty,
            "file": file_main,
            "fileLod1": file_lod1,
            "bbox": bb,
            "worldMin": [min(xs), min(ys)],
            "worldMax": [max(xs), max(ys)],
            "worldCenter": [cx, cy],
            "vertices": verts if file_main else 0,
            "verticesLod1": int(t.get("verticesLod1") or 0) if file_lod1 else 0,
            "verticesBuildings": int(t.get("verticesBuildings") or (verts if file_main else 0)),
            "verticesVeg": 0,
            "foliageInstances": foliage_instances,
            "sources": t.get("sources") or {},
        }
        if file_foliage:
            tile_entry["fileFoliage"] = file_foliage
        if static_mesh and file_main:
            tile_entry["staticMesh"] = static_mesh
        if static_mesh_l1 and file_lod1:
            tile_entry["staticMeshLod1"] = static_mesh_l1
        # Placement origins for localized cooked meshes (DF-safe).
        if static_mesh:
            leaf = static_mesh.rstrip("/").rsplit("/", 1)[-1]
            if leaf in origins:
                tile_entry["staticMeshOrigin"] = origins[leaf]
        if static_mesh_l1:
            leaf = static_mesh_l1.rstrip("/").rsplit("/", 1)[-1]
            if leaf in origins:
                tile_entry["staticMeshLod1Origin"] = origins[leaf]
        tiles_out.append(tile_entry)

    ue_man = {
        "id": "nantucket-structures",
        "version": 1,
        "description": (
            "Streamed structure tiles for SailSimUE — OSM houses (gable/cape/brick), "
            "hero landmarks, ENC lights/pilings, trees, grass tufts, hydrangeas. "
            "NAVT meshes from sail-sim bake. Load only near the boat. "
            "Optional staticMesh/staticMeshLod1 from cook_nantucket_structures_nanite.py."
        ),
        "source": str(SAIL_STRUCT),
        "units": {
            "mesh_xy": "feet from NAV_ORIGIN (x=north, z=east)",
            "mesh_y": "elevation meters",
            "ue_world": "cm (+X north, +Y east, +Z up)",
        },
        "origin": {"lat": ORIGIN_LAT, "lon": ORIGIN_LON},
        "bbox": man["bbox"],
        "streamTileCols": man.get("streamTileCols", 8),
        "streamTileRows": man.get("streamTileRows", 8),
        "stream": {
            "loadRadius": load_r,
            "unloadRadius": unload_r,
            "lod0Radius": lod0,
            "openSeaSkipDistanceCm": 250000,
            "harborFullDistanceCm": 150000,
            "vegLoadDistanceCm": 250000,
            "note": (
                "Harbor tile ~500k verts — prefer loadRadius≤2. "
                "Optional per-tile staticMesh / staticMeshLod1 for cooked SM+Nanite; "
                "runtime PMC fallback when missing. Open-sea skip beyond openSeaSkipDistanceCm. "
                "Foliage: fileFoliage JSON → HierarchicalInstancedStaticMesh (species prototypes or /Game/Foliage/*)."
            ),
        },
        "foliage": man.get("foliage") or {
            "format": "instances-v1",
            "species": ["oak", "cedar", "scrub", "lawn", "beach", "marsh"],
        },
        "cook": {
            "version": 1,
            "folder": COOKED_GAME_FOLDER,
            "naming": "SM_Struct_{tx}_{ty}_LOD{0|1}",
            "fields": {
                "staticMesh": "optional soft path to cooked LOD0 UStaticMesh",
                "staticMeshLod1": "optional soft path to cooked LOD1 UStaticMesh",
            },
            "script": "Scripts/cook_nantucket_structures_nanite.py",
            "note": (
                "Missing staticMesh → runtime PMC from file/fileLod1 (dev fallback). "
                "Re-run cook after sail-sim structure rebake."
            ),
        },
        "contents": source_totals,
        "totalVertices": total_verts,
        "tileCount": len(tiles_out),
        "navLights": man.get("navLights") or [],
        "beacons": man.get("beacons") or [],
        "tiles": tiles_out,
    }
    out_man = OUT / "ue_manifest.json"
    out_man.write_text(json.dumps(ue_man, indent=2) + "\n")
    print("wrote", out_man, "tiles", len(tiles_out), "verts", f"{total_verts:,}")
    print("  sources:", source_totals)
    print(
        "  cooked staticMesh:",
        cooked_lod0,
        "lod0,",
        cooked_lod1,
        "lod1 (run cook script if 0)",
    )

    (OUT / "README.md").write_text(
        """# Nantucket structures (SailSimUE)

## What is baked
Hand-tuned heroes (Brant Point Light, wharves, church, shingle houses) + OSM
building footprints (cape/gable/brick storefronts, hydrangeas) + ENC maritime
(lights, pilings) + trees/grass/seagrass from land cover.

## Runtime
`UNantucketStructuresSubsystem` streams tiles around the boat:
- **loadRadius** / **unloadRadius** — chebyshev tile distance (harbor defaults 2/3)
- **LOD0** near boat, **LOD1** shell when `fileLod1` / `staticMeshLod1` present
- **staticMesh** / **staticMeshLod1** — optional cooked UE paths; PMC NAVT fallback
- Open sea: skip loads when nearest tile center > openSeaSkipDistanceCm (default 2.5 km)
- Vertex colors → `M_NavtVertexColor` (shingle, roof, trim, hydrangea blue, foliage)

## Rebuild
```bash
# optional re-bake from OSM/ENC/DEM
cd sail-sim/frontend && node scripts/bake-voxel-structures.mjs

python3 SailSimUE/Scripts/export_nantucket_structures_ue.py

# Phase C1: cook StaticMesh + Nanite (editor, unattended)
# UnrealEditor SailSimUE.uproject \\
#   -ExecutePythonScript=Scripts/cook_nantucket_structures_nanite.py -unattended -nop4
#
# then re-export (or cook patches manifest) so staticMesh paths appear
python3 SailSimUE/Scripts/export_nantucket_structures_ue.py
```

See `docs/NANTUCKET_STRUCTURES.md` for cook + Nanite notes.
"""
    )
    print("done ->", OUT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
