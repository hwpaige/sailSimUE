# Nantucket structures (SailSimUE)

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
