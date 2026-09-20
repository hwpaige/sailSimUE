# Why Nantucket still feels “off” — architecture diagnosis & simple AAA path

**Date:** 2026-07-19  
**Scope:** Land, water, foliage, buildings, lighting — the *world*, not boat VPP.

This is not “add more features.” The sim already has a lot of systems. It still looks non‑AAA because **the wrong primitives are carrying the world**.

---

## 1. Current architecture (what you actually have)

```
                    ┌─────────────────────────────────────┐
  sail-sim bake     │  Voxel / vertex-color NAVT meshes   │
  (Node scripts)    │  terrain tiles + structure tiles    │
                    └──────────────┬──────────────────────┘
                                   │ export + symlink
                                   ▼
                    ┌─────────────────────────────────────┐
  UE runtime        │  ProceduralMeshComponent streaming  │
                    │  (terrain + houses + voxel trees)   │
                    │  optional cook → StaticMesh (off)   │
                    └──────────────┬──────────────────────┘
                                   │
         ┌─────────────────────────┼─────────────────────────┐
         ▼                         ▼                         ▼
  Water Body Ocean          SkyAtmosphere              Env/TOD/season
  + exclusion volume        + fog + Lumen              (good scaffolding)
  + flat plane (no FFT)
```

| Layer | Implementation today | AAA equivalent |
|-------|----------------------|----------------|
| **Land shape** | Streamed DEM triangle soup (PMC) | Landscape (or Nanite landscape mesh) + collision |
| **Land look** | Vertex colors + shader remaps | Layered Landscape material (sand / grass / dirt / rock) + RVT |
| **Foliage** | Voxel trees/grass in same mesh as houses | Hierarchical Instanced Static Meshes (HISM) of real plant meshes |
| **Buildings** | Parametric poly-house vertex soup | Cooked StaticMesh (+ Nanite), one mesh per house or per block |
| **Water** | Infinite Water body, flat, exclusion hole | FFT/Gerstner water + coast material + proper land exclusion or carved Landscape |
| **Lighting** | Solid (TOD, AE, sky) | Keep — already the most AAA part |
| **Streaming** | Custom tile subsystems | Keep pattern; change *what* streams |

**Bottom line:** you built a high-quality *data pipeline* (DEM, OSM, ENC, geo frame) and a solid *streaming skeleton*, then filled both with **web-era voxel geometry**. AAA games use that data to *place* assets, not to *be* the assets.

---

## 2. Why it still looks wrong (root causes, not symptoms)

### A. Geometry is the texture
Houses, trees, grass, and sand are all **colored triangles**. No albedo maps, no normal maps, no roughness variation, no contact shadows between real foliage and ground.  
→ Reads as “Minecraft harbor” even with good lighting.

### B. ProceduralMesh is anti-AAA
Runtime PMC:
- no Nanite
- expensive draw / shadow cost
- bad LODs
- fights Lumen/VSM  
Cook path exists (`bPreferCookedStaticMesh`) but is **off** because vertex colors were lost on OBJ — so production path is still the prototype path.

### C. Water and land are two different worlds glued with hacks
- Water: infinite sheet at Z=0  
- Land: DEM + `LAND_LIFT_M` + exclusion volume  
When that glue slips → houses in water, dark land, wet streets.  
AAA coasts are either **Landscape carved by the water system** or **authored shoreline** — not infinite ocean + mesh island + exclusion box.

### D. Foliage is density without silhouette
Voxel crowns never look like pitch pine / scrub oak from 200 m. AAA silhouette is **instance count × good mesh LODs**, not more voxels.

### E. Too many parallel half-systems
NAVT PMC **and** r16 Landscape export **and** SM cook **and** veg split **and** season tint on vertex green.  
Each papered over the last issue. None is the full vertical slice of “island looks real.”

### F. What is already good (keep)
- `FNavGeo` single frame (charts, boat, land)
- Tile stream + hysteresis + open-sea skip
- TOD / season / night emissive scaffolding
- Shared `land-elev.mjs` vertical contract
- Water exclusion for Nantucket (correct direction)
- Perf plan for tess/Lumen budget

---

## 3. Simple AAA architecture (one clear target)

**Replace painted geometry with placed assets. Keep the geo pipeline.**

```
DEM + coast  ──► Landscape (or one Nanite terrain mesh) + layered mat
OSM + DEM    ──► House StaticMeshes (Nanite) + HISM props
Land cover   ──► HISM foliage (free Megascans / Plant Vol / Quixel)
ENC          ──► lights / pilings as small SM instances
Water        ──► UE Water + exclusion = Landscape bounds (not PMC hole)
Lighting     ──► keep TOD subsystem (already strong)
```

### Target stack (minimal)

| System | One job | Tech |
|--------|---------|------|
| **Terrain** | Shape + ground material | Landscape from `heightmap_16bit.r16` **or** cooked terrain SM + Nanite |
| **Foliage** | Trees / grass / dunes | `UHierarchicalInstancedStaticMeshComponent` from bake **points**, not voxel meshes |
| **Buildings** | Town | Cooked SM per tile (Nanite on), **no PMC in shipping** |
| **Water** | Ocean | Water body + **Landscape/island exclusion only** |
| **Heroes** | Lighthouse, mill, wharves | Hand-placed or high-quality unique SMs |
| **Stream** | Memory | Keep chebyshev rings; stream **instances + SM refs**, not raw NAVT |

---

## 4. Why this is simpler (not a rewrite)

You do **not** throw away the bake.

| Keep | Change |
|------|--------|
| DEM sample / elev contract | Output heightmap + collision, not colored tris for “look” |
| OSM footprints | Emit **instance transforms** (pos/rot/scale + house type id) |
| Tree scatter rules in `voxel-trees.mjs` | Emit **points + species id**, not voxel crowns |
| Tile streaming subsystems | Load SM/HISM instead of PMC |
| Water exclusion + Z=0 | Bound exclusion to Landscape/island AABB; stop fighting elev with more lift |
| TOD / season MPC | Drive foliage MIDs and grass color, not vertex-green hacks |

Net: **fewer mesh types, fewer materials, fewer elev knobs** — more *instances* of good assets.

---

## 5. Visual priority (what fixes “off” first)

Order by how much the eye notices from the boat:

| Pri | Change | Effort | Impact |
|-----|--------|--------|--------|
| **1** | Real foliage HISM (trees + beach grass) from free plant packs | 1–2 days | Huge silhouette / green |
| **2** | Terrain as Landscape *or* textured Nanite mesh (sand/grass layers) | 2–3 days | Shoreline + fields stop looking painted |
| **3** | Cook buildings → Nanite SM, **enable** cook path; drop PMC default | 1–2 days (pipeline mostly exists) | Town stops looking like colored blocks; GPU win |
| **4** | Water/land: exclusion = island bounds only; verify elev once | 0.5 day | No more floating/sinking feel |
| **5** | Hero props (Brant Point, pier planks) as unique meshes | 1 day | Harbor postcard shot |
| **6** | FFT / better water later | later | Open ocean quality (separate from island look) |

**Do not** invest more in voxel tree density, vertex beach paint, or extra LAND_LIFT knobs. Those polish a wrong representation.

---

## 6. Concrete “simple” implementation plan

### Phase 1 — Stop painting plants (highest ROI) ✅ implemented

1. Bake writes `tiles/{tx}_{ty}_foliage.json` (species + lat/lon + elevM + yaw + scale).
2. Runtime HISM per species (`EnsureFoliageLayer`); voxel `*_veg.mesh` removed on rebake.
3. Prototypes via `FFoliagePrototypeFactory`; override with `/Game/Foliage/SM_*`.
4. Rebuild: `node bake-voxel-structures.mjs` → `export_nantucket_structures_ue.py` → recompile C++.

### Phase 2 — Ground that looks like ground

**Preferred (UE-native):** import `heightmap_16bit.r16` as Landscape; material layers sand / dry grass / wet line by height + slope; hide or retire PMC terrain when Landscape is in.

**Fallback (less editor work):** cook terrain tiles to Nanite SM + one good terrain material (not vertex-color only).

### Phase 3 — Buildings as assets

1. Re-cook with PLY/geometry path that **keeps vertex color** *or* assign real shingle materials by face groups.
2. Set `bPreferCookedStaticMesh = true`.
3. Eventually: bake **per-building** SMs (better LODs/culling) instead of 500k-vert mega-tiles.

### Phase 4 — Water/land one contract

Already started (`land-elev` + exclusion). Finish by:
- Exclusion volume = island bbox (+ margin), not template origin volumes.
- Landscape/terrain Z = DEM×100; water Z = 0; **no extra lift stacked at runtime**.
- One verify: pier feet wet, Main St dry, beach apron only seaward.

### Phase 5 — Lighting budget (already planned)

Keep `docs/perf/AAA_STACK_IMPLEMENTATION_PLAN.md` Phases A–B (tess size, Lumen/RT ocean profile). World look and GPU budget are separate; both needed.

---

## 7. What *not* to do

| Tempting | Why skip |
|----------|----------|
| More voxel detail / finer cells | Amplifies wrong art path |
| Another elev lift / floor knob | Masks water/land architecture |
| Full World Partition rewrite first | Unnecessary for one island at sailing distances |
| Port water-pro shaders | License + rewrite risk (PLAN.md) |
| Perfecting PMC materials | Polishing the prototype renderer |
| Frustum culling rewrites before Nanite/HISM | Premature; wrong bottleneck class |

---

## 8. Success criteria (feels AAA from the harbor approach)

From ~500 m offshore, camera toward town:

1. **Tree line** has real silhouettes (not blocky crowns).
2. **Beach** reads sand texture + sparse grass, not tan vertices.
3. **Roofs** have material response (sun on shingles), not flat RGB.
4. **Waterline** clean — no ocean through streets, no floating houses.
5. **FPS** improves when PMC veg/houses leave and HISM/Nanite take over.
6. **Open sea** still unloads town (existing importance distance).

---

## 9. One-sentence strategy

> **Use the Nantucket data pipeline to place AAA-ready meshes and Landscape; stop using that pipeline to generate the final look as vertex-colored voxels.**

That is the entire architectural fix. Everything else (TOD, charts, VPP, streaming rings) can stay.
