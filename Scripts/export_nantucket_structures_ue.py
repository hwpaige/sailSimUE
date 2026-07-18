#!/usr/bin/env python3
"""Export Nantucket structure tiles for SailSimUE streaming.

Reads sail-sim baked NAVT structure tiles (OSM houses, heroes, ENC lights,
trees, grass, hydrangeas) and writes:

  Content/Structures/nantucket/
    ue_manifest.json   — streaming metadata in UE world cm
    tiles/             — symlink to sail-sim bake (no 80MB duplicate)
    palette.json       — material palette reference
    README.md

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
        tiles_out.append(
            {
                "id": t["id"],
                "tx": t["tx"],
                "ty": t["ty"],
                "file": t["file"],
                "fileLod1": file_lod1,
                "bbox": bb,
                "worldMin": [min(xs), min(ys)],
                "worldMax": [max(xs), max(ys)],
                "worldCenter": [cx, cy],
                "vertices": verts,
                "verticesLod1": int(t.get("verticesLod1") or 0),
                "sources": t.get("sources") or {},
            }
        )

    ue_man = {
        "id": "nantucket-structures",
        "version": 1,
        "description": (
            "Streamed structure tiles for SailSimUE — OSM houses (gable/cape/brick), "
            "hero landmarks, ENC lights/pilings, trees, grass tufts, hydrangeas. "
            "NAVT meshes from sail-sim bake. Load only near the boat."
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
            "note": "Harbor tile ~500k verts — prefer loadRadius≤2",
        },
        "contents": source_totals,
        "totalVertices": total_verts,
        "tileCount": len(tiles_out),
        "navLights": man.get("navLights") or [],
        "beacons": man.get("beacons") or [],
        "tiles": tiles_out,
    }
    out_man = OUT / "ue_manifest.json"
    out_man.write_text(json.dumps(ue_man, indent=2))
    print("wrote", out_man, "tiles", len(tiles_out), "verts", f"{total_verts:,}")
    print("  sources:", source_totals)

    (OUT / "README.md").write_text(
        """# Nantucket structures (SailSimUE)

## What is baked
Hand-tuned heroes (Brant Point Light, wharves, church, shingle houses) + OSM
building footprints (cape/gable/brick storefronts, hydrangeas) + ENC maritime
(lights, pilings) + trees/grass/seagrass from land cover.

## Runtime
`UNantucketStructuresSubsystem` streams NAVT tiles around the boat:
- **loadRadius** / **unloadRadius** — chebyshev tile distance
- **LOD0** near boat, **LOD1** shell when `fileLod1` present
- Vertex colors → `M_NavtVertexColor` (shingle, roof, trim, hydrangea blue, foliage)

## Rebuild
```bash
# optional re-bake from OSM/ENC/DEM
cd sail-sim/frontend && node scripts/bake-voxel-structures.mjs

python3 SailSimUE/Scripts/export_nantucket_structures_ue.py
```
"""
    )
    print("done ->", OUT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
