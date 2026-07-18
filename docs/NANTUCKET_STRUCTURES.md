# Nantucket structures — houses, props, vegetation

## Goals
- Realistic townscape from the harbor approach (cedar-shingle houses, brick Main St,
  hydrangeas, grass, trees, ENC lights, wharves)
- Same geo frame as terrain / charts / boat (`FNavGeo`)
- Stream only nearby tiles — harbor full-detail tile is ~500k verts

## Pipeline

```
OSM footprints + heroes + ENC + DEM + land cover
        │
        ▼  sail-sim/frontend/scripts/bake-voxel-structures.mjs
public/structures/nantucket/{manifest.json, tiles/*.mesh, palette.json}
        │
        ▼  SailSimUE/Scripts/export_nantucket_structures_ue.py
Content/Structures/nantucket/
  ue_manifest.json      streaming metadata (world cm AABB)
  tiles/ → symlink      NAVT binaries
  palette.json
```

### Rebuild
```bash
# (optional) re-bake from OSM/ENC/DEM
cd sail-sim/frontend && node scripts/bake-voxel-structures.mjs

python3 SailSimUE/Scripts/export_nantucket_structures_ue.py

# Vertex-color material (once, in-editor Python):
# Tools → Execute Python Script → Scripts/create_navt_materials.py
```

## What the bake contains

| Source | Content |
|--------|---------|
| OSM buildings | ~3.4k parametric houses (cape/gable/hip, shutters, chimneys, hydrangeas) |
| Heroes | Brant Point Light, wharves, church, landmark shingle houses, Old Mill |
| ENC | Lights, pilings, maritime structures |
| Land cover | Trees, grass tufts, seagrass nearshore |
| Terrain bake | Roads / lawns as colored ground (separate terrain subsystem) |

## Runtime (`UNantucketStructuresSubsystem`)

| Principle | Implementation |
|-----------|----------------|
| Spatial partition | 8×8 cells over island bbox |
| Load only nearby | Chebyshev **loadRadius=2** |
| Hysteresis | **unloadRadius=3** |
| LOD | LOD0 within **lod0Radius=1**, LOD1 shell when `*_l1.mesh` present |
| Material | `/Game/Materials/Navt/M_NavtVertexColor` (vertex RGB → Base Color + emissive) |
| Night lights | Dark blue-dominant glass verts → warm window glow; glare/red lantern verts → street-lamp / light emissive. Driven by env preset (`Night01`) |
| Collision | Off (visual townscape; terrain remains the land mesh) |

### Window & lamp emissives
`M_NavtVertexColor` emissive Custom HLSL:
- **Windows:** GLASS-like vertex colors (blue-dominant, dark) with a sparse hash so ~25% of panes light, ~60% of buildings occupied.
- **Lamps:** warm near-white glare tips + red lantern boxes (ENC / lighthouse palette).
- **Night01:** 0 on Fair Day, ~0.7 at Dusk, 1.0 at Night (synced from `USailOceanSubsystem` env preset).

Settings → **PERFORMANCE** shows game-thread CPU EMA for Boat / Sails / Water / Terrain / Houses plus resident tile & vert counts.

Tick (~3 Hz): focus = player boat → nearest tile → ensure ring → drop far tiles.

## Verify
1. Recompile C++ (Live Coding / Cmd+Option+Shift+P).
2. Create `M_NavtVertexColor` if missing (Python script above).
3. PIE at Nantucket Harbor — status line should show `struct: N tiles`.
4. Look shoreward: gray shingles, white trim, blue hydrangeas, green trees/grass.
