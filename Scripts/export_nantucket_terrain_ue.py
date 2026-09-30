#!/usr/bin/env python3
"""Export Nantucket terrain for SailSimUE streaming.

Reads sail-sim baked NAVT tiles + manifest and writes:
  Content/Terrain/nantucket/
    ue_manifest.json   — streaming metadata in UE world cm
    heightmap_16bit.r16 — full DEM for optional Landscape import
    heightmap_meta.json

Design (game-dev practice):
  • Do NOT load 64MB of meshes at once — runtime streams a chebyshev radius.
  • LOD0 near boat, LOD1 ring outside (matches web loadRadius / runtimeFullRadius).
  • Binary NAVT reused as-is (positions feet, elev meters) — converted on load.
  • Optional Landscape heightmap for editor-authored WP worlds later.

Usage:
  python3 Scripts/export_nantucket_terrain_ue.py
"""
from __future__ import annotations

import json
import math
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SAIL_TERRAIN = ROOT.parent / "sail-sim" / "frontend" / "public" / "terrain" / "nantucket"
OUT = ROOT / "Content" / "Terrain" / "nantucket"

ORIGIN_LAT = 41.48
ORIGIN_LON = -70.22
FT_PER_DEG_LAT = 364000.0
CM_PER_FT = 30.48
CM_PER_M = 100.0


def ft_per_deg_lon(lat: float) -> float:
    return FT_PER_DEG_LAT * math.cos(math.radians(lat))


def lat_lon_to_world_cm(lat: float, lon: float) -> tuple[float, float]:
    x_ft = (lat - ORIGIN_LAT) * FT_PER_DEG_LAT
    y_ft = (lon - ORIGIN_LON) * ft_per_deg_lon(ORIGIN_LAT)
    return x_ft * CM_PER_FT, y_ft * CM_PER_FT


def main() -> int:
    man_path = SAIL_TERRAIN / "manifest.json"
    if not man_path.is_file():
        print("missing", man_path, file=sys.stderr)
        return 1

    man = json.loads(man_path.read_text())
    OUT.mkdir(parents=True, exist_ok=True)

    # Symlink/copy pointer to source tiles (don't duplicate 64MB)
    tiles_link = OUT / "tiles"
    if tiles_link.is_symlink() or tiles_link.exists():
        if tiles_link.is_symlink():
            tiles_link.unlink()
        elif tiles_link.is_dir():
            # leave existing; prefer symlink
            pass
    if not tiles_link.exists():
        try:
            os.symlink(SAIL_TERRAIN / "tiles", tiles_link, target_is_directory=True)
            print("symlink tiles ->", SAIL_TERRAIN / "tiles")
        except OSError as e:
            print("warn: could not symlink tiles:", e)

    # Copy/link manifest source for reference
    src_man = OUT / "source_manifest.json"
    src_man.write_text(json.dumps(man, indent=2))

    tiles_out = []
    for t in man["tiles"]:
        bb = t["bbox"]
        # corners in lat/lon → UE cm AABB
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
        tiles_out.append(
            {
                "id": t["id"],
                "tx": t["tx"],
                "ty": t["ty"],
                "file": t["file"],
                "fileLod1": t.get("fileLod1", ""),
                "bbox": bb,
                "worldMin": [min(xs), min(ys)],
                "worldMax": [max(xs), max(ys)],
                "worldCenter": [cx, cy],
                "vertices": t.get("vertices", 0),
                "verticesLod1": t.get("verticesLod1", 0),
            }
        )

    ue_man = {
        "id": "nantucket",
        "version": 1,
        "description": (
            "Streamed terrain tiles for SailSimUE. NAVT meshes from sail-sim bake. "
            "Load only tiles near the boat (chebyshev radius). LOD0 within fullRadius, "
            "LOD1 in the outer ring."
        ),
        "source": str(SAIL_TERRAIN),
        "units": {
            "mesh_xy": "feet from NAV_ORIGIN (x=north, z=east)",
            "mesh_y": "elevation meters",
            "ue_world": "cm (+X north, +Y east, +Z up)",
        },
        "origin": {"lat": ORIGIN_LAT, "lon": ORIGIN_LON},
        "bbox": man["bbox"],
        "streamTileCols": man["streamTileCols"],
        "streamTileRows": man["streamTileRows"],
        "stream": {
            # Budget: loadR=2, lod0=1 (shell LOD1). Old 3/3 forced ~1M full-res verts.
            "loadRadius": man.get("stream", {}).get("loadRadius", 2),
            "unloadRadius": man.get("stream", {}).get("unloadRadius", 4),
            "lod0Radius": man.get("stream", {}).get(
                "lod0Radius",
                man.get("lod", {}).get("runtimeFullRadius", 1),
            ),
        },
        "elev": {"minM": man["minElev"], "maxM": man["maxElev"]},
        "tileCount": len(tiles_out),
        "tiles": tiles_out,
    }
    out_man = OUT / "ue_manifest.json"
    out_man.write_text(json.dumps(ue_man, indent=2))
    print("wrote", out_man, "tiles", len(tiles_out))

    # --- Optional Landscape heightmap (16-bit R16, row-major north→south) ---
    dem_bin = SAIL_TERRAIN / "heights.bin"
    dem_meta = SAIL_TERRAIN / "dem-source.json"
    if dem_bin.is_file() and dem_meta.is_file():
        dm = json.loads(dem_meta.read_text())
        cols, rows = int(dm["cols"]), int(dm["rows"])
        raw = dem_bin.read_bytes()
        # assume float32 elevations meters
        n = cols * rows
        if len(raw) >= n * 4:
            elevs = struct.unpack(f"<{n}f", raw[: n * 4])
            e_min = float(dm.get("minElev", min(elevs)))
            e_max = float(dm.get("maxElev", max(elevs)))
            span = max(1e-3, e_max - e_min)
            # Landscape likes 16-bit height: 0..65535 maps to Z scale
            out_r16 = OUT / "heightmap_16bit.r16"
            with open(out_r16, "wb") as f:
                for e in elevs:
                    if e < -9000:  # noData
                        u = 0
                    else:
                        t = (e - e_min) / span
                        u = int(max(0, min(65535, round(t * 65535))))
                    f.write(struct.pack("<H", u))
            # Physical size of DEM in world cm
            sw = lat_lon_to_world_cm(dm["bbox"]["south"], dm["bbox"]["west"])
            ne = lat_lon_to_world_cm(dm["bbox"]["north"], dm["bbox"]["east"])
            hm = {
                "file": "heightmap_16bit.r16",
                "cols": cols,
                "rows": rows,
                "format": "r16_le",
                "elevMinM": e_min,
                "elevMaxM": e_max,
                "zScaleCm": (e_max - e_min) * CM_PER_M,
                "worldMinXY": [min(sw[0], ne[0]), min(sw[1], ne[1])],
                "worldMaxXY": [max(sw[0], ne[0]), max(sw[1], ne[1])],
                "note": (
                    "Import as Landscape heightmap in editor if desired. "
                    "Runtime path uses streamed NAVT tiles (preferred for sailing sim)."
                ),
            }
            (OUT / "heightmap_meta.json").write_text(json.dumps(hm, indent=2))
            print("wrote", out_r16, f"{cols}x{rows}")
        else:
            print("warn: heights.bin size unexpected", len(raw), "need", n * 4)
    else:
        print("skip heightmap (no heights.bin)")

    # README
    (OUT / "README.md").write_text(
        """# Nantucket terrain (SailSimUE)

## Runtime (recommended)
`UNantucketTerrainSubsystem` streams NAVT mesh tiles around the boat:
- **loadRadius** / **unloadRadius** — chebyshev tile distance
- **LOD0** inside lod0Radius, **LOD1** in the outer shell
- Vertex colors from bake (aerial + terrain classification)

Never load all ~96 tiles at once (~65 MB). Typical resident set is ~25–50 tiles.

## Optional Landscape
`heightmap_16bit.r16` + `heightmap_meta.json` for editor Landscape import
(World Partition). Prefer runtime tiles for PIE without manual import.

## Rebuild
```bash
python3 Scripts/export_nantucket_terrain_ue.py
```
Source bake: `sail-sim/frontend/scripts/bake-terrain-tiles.mjs`
"""
    )
    print("done ->", OUT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
