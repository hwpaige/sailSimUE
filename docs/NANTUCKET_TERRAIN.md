# Nantucket terrain — scalable streaming design

## Goals
- Real DEM-based island (USGS 3DEP via sail-sim bake)
- **Do not** load the whole mesh (~65 MB / 96 tiles) every frame
- Distance LOD + hysteresis unload (standard open-world pattern)
- Same geo frame as charts / boat (`FNavGeo`)

## Pipeline

```
USGS DEM + imagery + ENC coast
        │
        ▼  sail-sim/frontend/scripts/bake-terrain-tiles.mjs
public/terrain/nantucket/{manifest.json, tiles/*.mesh}
        │
        ▼  SailSimUE/Scripts/export_nantucket_terrain_ue.py
Content/Terrain/nantucket/
  ue_manifest.json      streaming metadata (world cm AABB)
  tiles/ → symlink      NAVT binaries (not duplicated)
  heightmap_16bit.r16   optional Landscape import
  heightmap_meta.json
```

### Rebuild
```bash
# (optional) re-bake from DEM if source changed
cd sail-sim/frontend && node scripts/bake-terrain-tiles.mjs

# UE streaming manifest + heightmap
python3 SailSimUE/Scripts/export_nantucket_terrain_ue.py
```

## Runtime (`UNantucketTerrainSubsystem`)

| Principle | Implementation |
|-----------|----------------|
| Spatial partition | 16×16 cells over DEM bbox |
| Load only nearby | Chebyshev **loadRadius=3** (~49 cells max) |
| Hysteresis | **unloadRadius=5** — no thrash at boundaries |
| LOD | LOD0 within **lod0Radius=3** (full ring = no LOD seams) |
| Seams | Absolute DEM mesh step + tile edge overlap (shared verts) |
| Coast | Seaward apron only below MSL; inland DEM + single `LAND_LIFT_M` |
| Water align | DEM m MSL × 100 = UE Z; water plane **Z = 0**; exclusion over island |
| Land elev | Shared `lib/land-elev.mjs` — terrain + houses use same lift |
| Memory | Resident tiles only; destroy components on unload |
| Collision | Complex mesh for land contact (query+physics) |
| Coordinates | NAVT ft/m → UE cm via `FNavtMeshLoader` (local-space tiles) |

Tick (~4 Hz): focus = player boat → nearest tile → ensure ring → drop far tiles.

## Optional Landscape / World Partition

For shipping a WP-authored map later:

1. Import `heightmap_16bit.r16` as Landscape (see `heightmap_meta.json` for Z scale / world extents).
2. Use Runtime Virtual Texture + layered material for aerial.
3. Keep streamed tiles **or** replace them with Landscape once authored.

Runtime NAVT streaming is the default for PIE (no manual Landscape import).

## Vertical contract (water ↔ land)

| Source | Value |
|--------|--------|
| Sea level | UE **Z = 0** = 0 m MSL |
| DEM | USGS 3DEP metres (≈ NAVD88 / LMSL) |
| Dry land elev | `dryLandElevM(dem) = max(dem, DRY_FLOOR_M) + LAND_LIFT_M` (`scripts/lib/land-elev.mjs`) |
| Structures | `structurePadElevM` / `groundElevM` — same lift |
| Seaward apron | Below Z=0 only (`signedFt < 0`); does **not** lift |
| Water over island | `SailSim_NantucketWaterExclusion` (do not destroy on open-ocean prep) |

## Spawn

Boat world origin is NAV_ORIGIN (41.48 N, 70.22 W).  
Nantucket island is **south-east** of that origin (~−2.2 km northing, +1 km easting in cm).  
`OpenWaterSpawnXY` should place the boat near the island approaches so tiles stream in immediately.

## HUD

Status line reports `terrain: N tiles resident`.

## Material (`M_NavtTerrain`)

Land uses a **dedicated** material (not the town/structure mat):

| Feature | Behavior |
|---------|----------|
| Grass / lawn | Green-dominant verts remapped toward lush moor green |
| Beach sand | Warm tan + low elev (waterline apron) → dry/wet sand |
| Wet band | Darker sand when world Z is near sea level |
| Detail | Patch / tussock / grit noise so surfaces aren’t flat paint |

Create / rebuild in-editor:

```bash
# Tools → Execute Python Script, or:
# UnrealEditor SailSimUE.uproject -ExecutePythonScript=Scripts/create_navt_terrain_material.py
```

Script: `Scripts/create_navt_terrain_material.py` → `/Game/Materials/Navt/M_NavtTerrain`.

Optional stronger bake palette (re-run after changing `terrain-cover.mjs` weights):

```bash
cd sail-sim/frontend && node scripts/bake-terrain-tiles.mjs
python3 SailSimUE/Scripts/export_nantucket_terrain_ue.py
```

## Related: buildings & props

Houses, hydrangeas, grass tufts, trees, ENC lights, and hero landmarks stream via
`UNantucketStructuresSubsystem` — see [NANTUCKET_STRUCTURES.md](NANTUCKET_STRUCTURES.md).
