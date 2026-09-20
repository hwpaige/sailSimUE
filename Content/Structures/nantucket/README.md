# Nantucket structures (SailSimUE)

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
# UnrealEditor SailSimUE.uproject \
#   -ExecutePythonScript=Scripts/cook_nantucket_structures_nanite.py -unattended -nop4
#
# then re-export (or cook patches manifest) so staticMesh paths appear
python3 SailSimUE/Scripts/export_nantucket_structures_ue.py
```

See `docs/NANTUCKET_STRUCTURES.md` for cook + Nanite notes.
