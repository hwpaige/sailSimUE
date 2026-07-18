# Nantucket terrain (SailSimUE)

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
