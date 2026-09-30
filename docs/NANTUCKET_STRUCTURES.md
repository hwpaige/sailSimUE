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
        │
        ▼  Scripts/cook_nantucket_structures_nanite.py  (Phase C1, editor)
Content/Structures/nantucket/Cooked/
  SM_Struct_{tx}_{ty}_LOD0.uasset   (+ LOD1 when fileLod1)
  Nanite on dense LODs; material M_NavtVertexColor
        │
        ▼  cook patches ue_manifest (or re-run export)
  tiles[].staticMesh / staticMeshLod1
```

### Rebuild (NAVT stream — always)

```bash
# (optional) re-bake from OSM/ENC/DEM
cd sail-sim/frontend && node scripts/bake-voxel-structures.mjs

python3 SailSimUE/Scripts/export_nantucket_structures_ue.py

# Vertex-color material (once, in-editor Python):
# Tools → Execute Python Script → Scripts/create_navt_materials.py
```

### Rebuild (Phase C1 cooked StaticMesh + Nanite)

Cook offline after NAVT tiles exist. Runtime C2 prefers `staticMesh` /
`staticMeshLod1` when loadable; otherwise PMC continues from `.mesh`.

```bash
# 1) Ensure export + material
python3 SailSimUE/Scripts/export_nantucket_structures_ue.py
# create_navt_materials.py in editor if M_NavtVertexColor missing

# 2) Unattended cook (same pattern as enable_nanite_buoys.py)
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor" \
  "/Users/harrison/PycharmProjects/SailSimUE/SailSimUE.uproject" \
  -ExecutePythonScript="Scripts/cook_nantucket_structures_nanite.py" \
  -unattended -nop4

# 3) Optional: re-export so schema/cook block stays in sync with disk
python3 SailSimUE/Scripts/export_nantucket_structures_ue.py
```

**Debug / partial cook**

```bash
# Outside editor: parse NAVT + plan paths + write Saved/CookStructures/*.ply
python3 Scripts/cook_nantucket_structures_nanite.py --dry-run --max-tiles 2

# Single tile in editor:
# ... -ExecutePythonScript="Scripts/cook_nantucket_structures_nanite.py" --tx 5 --ty 2

# Only write staticMesh paths for existing Cooked/*.uasset:
python3 Scripts/cook_nantucket_structures_nanite.py --manifest-only
```

**Nanite**

| Rule | Value |
|------|--------|
| Enable when | `num_verts >= 5000` (override `--nanite-min N`) |
| Targets | Dense LOD0 house tiles (and dense LOD1 if above threshold) |
| Material | `/Game/Materials/Navt/M_NavtVertexColor` (vertex color + night emissive) |
| Precedent | `Scripts/enable_nanite_buoys.py`; moored hull `BuildHullNaniteMesh` |

Sparse tiles (few ENC poles / seagrass) stay non-Nanite StaticMesh to avoid
overhead on tiny meshes.

**Cook strategy inside the editor script**

1. Parse NAVT (same binary as `FNavtMeshLoader`: positions ft/m → UE cm, RGB8 colors, u32 indices).
2. **Large tiles (≥50k verts):** temp **PLY** (pos / nrm / vertex color) → `AssetImportTask` first
   (Geometry Script per-vertex append is too slow on harbor tiles).
3. **Smaller tiles:** prefer **Geometry Script** DynamicMesh →
   `create_new_static_mesh_asset_from_mesh` when plugin APIs exist; else PLY.
4. Assign `M_NavtVertexColor`, enable Nanite on dense LODs, save under
   `/Game/Structures/nantucket/Cooked/`.
5. Patch `ue_manifest.json` with `staticMesh` / `staticMeshLod1`.

If both Geometry Script and PLY import fail, enable **GeometryScripting** (and
Modeling Tools) in the project plugins, or cook one tile interactively and re-run
with `--manifest-only`. Do **not** delete NAVT `tiles/*.mesh` — they remain the
dev fallback and source of truth for re-cooks.

### Manifest schema (streaming + cook)

Unchanged stream fields:

| Field | Meaning |
|-------|---------|
| `stream.loadRadius` / `unloadRadius` / `lod0Radius` | Chebyshev ring (default 2 / 3 / 1) |
| `stream.openSeaSkipDistanceCm` | Beyond → unload all / no loads (default 250000 = 2.5 km) |
| `stream.harborFullDistanceCm` | Inside → full radii (default 150000 = 1.5 km) |
| `tiles[].file` / `fileLod1` | Relative NAVT paths under structures root |
| `tiles[].worldMin/Max/Center` | UE world cm AABB / center |

Optional cook fields (per tile):

| Field | Example |
|-------|---------|
| `staticMesh` | `/Game/Structures/nantucket/Cooked/SM_Struct_5_2_LOD0` |
| `staticMeshLod1` | `/Game/Structures/nantucket/Cooked/SM_Struct_5_2_LOD1` |

Top-level `cook` block documents naming and C2 fallback behavior. Export auto-fills
`staticMesh*` when the corresponding `.uasset` exists under `Content/.../Cooked/`;
the cook script also patches the manifest after a successful batch.

## What the bake contains

| Source | Content |
|--------|---------|
| OSM buildings | ~3.4k parametric houses (cape/gable/hip, shutters, chimneys, hydrangeas) |
| Heroes | Brant Point Light, wharves, church, landmark shingle houses, Old Mill |
| ENC | Lights, pilings, maritime structures |
| Land cover + DEM | Trees, lawn/meadow grass, dune beach grass, marsh grass (OSM + elevation biotopes) |
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
| Cooked SM (C2) | Prefer `staticMesh` / `staticMeshLod1` when set; else NAVT → ProceduralMesh |
| Open-sea importance | Skip structures when nearest tile center > `openSeaSkipDistanceCm` |

Chrome status shows `sm=` / `pmc=` resident counts after C2.

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
5. After C1 cook: Content Browser → `/Game/Structures/nantucket/Cooked/` has
   `SM_Struct_*` assets; dense ones show Nanite enabled; manifest has `staticMesh`.
6. After C2 runtime: houses draw as StaticMesh/Nanite; night windows still work.

## Foliage HISM (2026-07-19) — AAA path

**Bake** (`sail-sim/frontend/scripts/bake-voxel-structures.mjs`):
- `tiles/{tx}_{ty}.mesh` — **buildings only** (heroes, ENC, OSM)
- `tiles/{tx}_{ty}_l1.mesh` — shell LOD
- `tiles/{tx}_{ty}_foliage.json` — **instance points** (species, lat/lon, elevM, yaw, scale)
- Legacy `*_veg.mesh` voxel crowns are **removed** on rebake

Species: `oak`, `cedar`, `scrub`, `lawn`, `beach`, `marsh`  
Placement rules: same biotopes as the old voxel scatter (`foliage-instances.mjs`).

**Runtime** (`EnsureFoliageLayer`):
- Within `VegLoadDistanceCm` of focus → load JSON → one **HISM per species** per tile
- Meshes: `/Game/Foliage/SM_{Species}` if present, else **runtime prototypes** (`FFoliagePrototypeFactory`)
- Season/night via same `M_NavtVertexColor` MIDs (green verts = foliage season path)
- Drop free Plant packs into `Content/Foliage/` named `SM_Oak`, `SM_Cedar`, etc. to replace prototypes

**Cook** (buildings only):
```bash
UnrealEditor SailSimUE.uproject \
  -ExecutePythonScript=Scripts/cook_nantucket_structures_nanite.py -unattended -nop4
```

