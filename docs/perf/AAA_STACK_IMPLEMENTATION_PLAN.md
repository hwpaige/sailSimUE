# AAA GPU stack — implement lighting, water, island meshes

Plan for the three recommendations that move SailSimUE toward a triple‑A *budget discipline* (premium where it shows, cheap where it doesn’t). Not a feature dump: sequenced work with measurable A/B.

**Baseline today (PIE Mac, open ocean / harbor):** ~18 fps, wall ~55 ms, GPU/RT ~52–55 ms, GT ~18 ms. Scene is feature-rich + non-Nanite land PMC + large water tess — not content-starved.

### Implementation status

**As of 2026-07-19:** Code landed for Phase **A** (water tess 60 km + far normals), **B** (ocean Lumen profile SPG=48 / RT off / AE stabilize), and **C** (structures dual-path SM+PMC, open-sea skip, cook script). **Cooked StaticMeshes not yet generated** — run editor cook for Nanite SM win. A/B rows live in [`GPU_ABL_LOG.md`](./GPU_ABL_LOG.md).

---

## Goals

| # | Theme | Outcome |
|---|--------|---------|
| **1** | Lighting | Near-boat quality; far almost free. No full Lumen+RT+max VSM on a horizon shot. |
| **2** | Water | Hero budget owner: small local tess, large zone, cheap open-water reflections. |
| **3** | Island meshes | Cooked StaticMesh (+ Nanite houses); stream by importance; stop runtime PMC tax. |

**Out of scope for this plan:** reintroducing frustum tile culling (crashed), cloth/sail GT micro-opts, full World Partition island rewrite, packaged shipping profiles (PIE A/B first).

---

## Current state (code knobs)

### Lighting (`Config/DefaultEngine.ini` + `SailOceanSubsystem`)

| Setting | Today |
|---------|--------|
| Lumen GI `ScreenProbeGather.DownsampleFactor` | **32** (can go 48–64 for ocean) |
| Lumen reflections DS | **2** (half-res) |
| Probe budget | **100** |
| `r.RayTracing` | **True** (Mac often pays without matching win) |
| SSR | Quality 3, half-res scene color |
| Auto-exposure | Night bias/min-max only; **day speeds/range not locked** (sparkle can fight AE) |
| Sky + sun + fog | Driven by env presets — keep |

No distance tiers yet: one global Lumen/RT quality for whole frame.

### Water (`SailBoatPawn` / `SailOceanSubsystem` / GameMode)

| Setting | Today | Target |
|---------|--------|--------|
| Zone extent | 240000 cm (2.4 km) | Keep large (follow/coverage) |
| Local tess diameter | **120000 cm (1.2 km)** | **40000–70000 cm (400–700 m)** |
| Clamp min local tess | **100000 cm** in `ConfigureWaterZones` | **Lower to ~35000–40000** |
| GameMode hardcode | `PrepareOpenOcean(..., 120000.f)` | Match new default |
| Chop / normals | `SetSurfaceChopIntensity`, polish MIDs | Distance-aware flatten far (material or MID knobs) |
| Reflections | Lumen + SSR project defaults | Prefer SSR + planar/cheap for open sea |

### Island meshes (`NantucketStructuresSubsystem` / `NantucketTerrainSubsystem`)

| Setting | Today |
|---------|--------|
| Mesh type | **Runtime `UProceduralMeshComponent`** from NAVT `.mesh` |
| Structures | 31 tiles, loadR≈2, lod0R=1, ~500k verts/harbor tile; resident often **~2.2M verts** |
| Terrain | PMC tiles, lod0R=3, distance disc only |
| Nanite precedent | Buoys cooked Nanite (`enable_nanite_buoys.py`); moored hulls optional runtime SM bake (`MooredBoatSubsystem::BuildHullNaniteMesh`) |
| HLOD | None |
| Harbor vs open-sea stream | Same disc radii always |

---

## Dependency / sequencing

```
                    ┌── [1 Lighting ocean profile] ── ini + AE + optional RT off
[0 Baseline note] ──┼── [2 Water tess + clamp] ───── code defaults (fastest FPS win)
                    └── [3a Structures cook path] ── offline SM/Nanite + stream swap
                              │
                              └── [3b Terrain LOD tighten] (no full Nanite required first)
                              └── [3c HLOD / far shore] (later phase)

Verify after 1+2 together, then after 3a.
```

**Why this order**

- **1 + 2 are parallel** (ini/cvars vs water code); both are hours-scale.
- **3 is the multi-day pillar** (cook pipeline + subsystem rewrite). Start cook tooling early so it overlaps 1/2 validation.
- Do **not** block 1/2 on Nanite land.

**Suggested ship phases**

| Phase | Deliverable | Est. effort |
|-------|-------------|-------------|
| **A** | Item 2 water tess 400–700 m + clamp | 0.5–1 day |
| **B** | Item 1 lighting ocean profile + AE stabilize + RT decision | 0.5–1 day |
| **C** | Item 3a cook structures → StaticMesh + Nanite houses; stream SM | 2–4 days |
| **D** | Item 3b terrain: stronger LOD1 / fewer full-res tiles | 1 day |
| **E** | Item 3c HLOD / importance (harbor vs open sea) | 1–2 days |
| **F** | Water reflections polish (planar/SSR bias) if still Lumen-hot | 1 day |

---

## Item 1 — Lighting: quality near boat, cheap far

### Design (distance tiers — conceptual)

| Band | Intent | Practical UE approach (v1) |
|------|--------|----------------------------|
| 0–200 m | Hull gelcoat, deck, nearby water | Lumen refl half-res (or DS=2), good SSR, sun/sky |
| 200 m–1 km | Softer GI | Higher GI downsample, lower probe budget |
| Far | Sky + fog | Almost no Lumen work on empty horizon |

**v1 is global “ocean mode” cvars** (one profile for sailing shots). True per-distance Lumen is limited by engine design; AAA often fakes far with fog/HLOD + scalability, not three live Lumen worlds.

### Concrete changes

**B1. `Config/DefaultEngine.ini` — Ocean / sailing profile**

```ini
; --- SailSim ocean lighting budget (premium near boat, cheap horizon) ---
; GI: open water wastes probe density
r.Lumen.ScreenProbeGather.DownsampleFactor=48   ; was 32; try 64 if still hot
r.Lumen.ScreenProbeGather.RadianceCache.NumProbesToTraceBudget=75  ; was 100

; Reflections: keep half-res for hull; open water leans on SSR
r.Lumen.Reflections.DownsampleFactor=2          ; keep first; only raise to 3 if profiled
r.Lumen.Reflections.MaxRoughnessToTrace=0.35    ; optional tighten
r.SSR.Quality=3
r.SSR.HalfResSceneColor=1

; Mac: Hardware RT often cost without win — disable unless A/B proves otherwise
r.RayTracing=False
; (or Mac-only via DeviceProfiles later)
```

Keep: sky atmosphere, directional sun, height fog, Substrate as-is this phase.

**B2. Auto-exposure stabilize** (`SailOceanSubsystem` camera PPS path ~2268+)

Day + night both:

- Override `AutoExposureSpeedUp` / `SpeedDown` → **slow** (e.g. 1.0 / 0.8 or project-tuned).
- Clamp brightness range day (not only night): modest min/max so water sparkle doesn’t pump exposure.
- Keep night bias ~−1.4.

Optional ini:

```ini
r.EyeAdaptation.SpeedUp=1.0
r.EyeAdaptation.SpeedDown=0.8
```

(Exact names may be `r.EyeAdaptationQuality` + PPS overrides — implement via existing camera PPS, not thrashing CVars every tick.)

**B3. Device / Mac path (optional same PR)**

- `Config/DefaultDeviceProfiles.ini` or Mac profile: `r.RayTracing=0` if project must keep RT True for other platforms later.

**B4. Chrome/debug**

- Perf panel already dumps Lumen/SSR/VSM; add RT on/off + AE note so A/B is visible.

### Success criteria

- [ ] Day sailing: no exposure “pulse” fighting sparkle.
- [ ] Hull still readable reflections (not flat gray).
- [ ] GPU ms down vs baseline *or* ProfileGPU shows Lumen/RT share down.
- [ ] Sky/sun/fog still sell day/night.

### Risks

- Refl DS=3 too soft on gelcoat → stay at 2, cut GI only.
- RT off changes reflections slightly → acceptable for Mac sailing.

---

## Item 2 — Water: own the budget

### Concrete changes

**W1. Clamp + defaults**

| File | Change |
|------|--------|
| `SailOceanSubsystem.cpp` `ConfigureWaterZones` | `LocalDiam` clamp min **100000 → 35000** (allow 400 m). Comment: tess = draw window, not physics. |
| `SailBoatPawn.h` | `LocalWaterTessellationDiameterCm` **120000 → 60000** (middle of 400–700). |
| `SailSimGameMode.cpp` | `PrepareOpenOcean(Hint, 240000.f, 60000.f)`. |
| Keep | `WaterZoneExtentCm` / `NeedFull` min 200000 — boat is ~24 km from origin. |

**W2. A/B ladder (user PIE)**

1. 70000 cm (safer look)
2. 60000 cm (default ship)
3. 40000–50000 cm if still GPU-hot and edge OK

**W3. Far normals / chop (same or follow-up PR)**

- Existing: `PolishWaterMaterials` / `SetSurfaceChopIntensity`.
- Add **distance or camera-based** reduction of high-frequency normal scale beyond ~local tess half-radius (MID scalar or material parameter). Far flat normals + sky/fog hide the small tess window.

**W4. Reflections (phase F if needed)**

- Prefer SSR for open water; do **not** require full Lumen reflections over infinite sea.
- Optional: `APlanarReflection` under boat only, or water material SSR weight ↑ / Lumen weight ↓.
- Keep Lumen reflections for hull-adjacent quality if cheap enough after GI cut.

### Success criteria

- [ ] Continuous water under boat; no dry ring at normal camera pitch.
- [ ] Log/applied LocalDiam ≈ 60k.
- [ ] Clear FPS/GPU win (often largest single change).
- [ ] Physics height queries unchanged (world XY).

### Risks

- Too-small tess: LOD pop looking down / zoom out → raise to 70k.
- Rebuild hitch if clamp thrash — existing 1 m change threshold stays.

---

## Item 3 — Island meshes: cook like a real game

### Strategy

Two tracks: **structures first** (highest vert density, town), **terrain second** (stronger LOD often enough without Nanite).

### Phase C — Structures → cooked StaticMesh + Nanite

**C1. Offline cook pipeline** (new script, pattern after `enable_nanite_buoys.py` + moored `BuildFromMeshDescriptions`)

```
Content/Structures/nantucket/tiles/*.mesh (NAVT)
        │
        ▼  Editor Python or commandlet
Content/Structures/nantucket/Cooked/SM_Struct_{tx}_{ty}_LOD0
Content/Structures/nantucket/Cooked/SM_Struct_{tx}_{ty}_LOD1
        │  Nanite enabled on house-heavy LOD0 (and LOD1 if dense)
        ▼
ue_manifest.json gains "staticMesh" / "staticMeshLod1" paths
```

Implementation options (pick one in implementation):

| Option | Pros | Cons |
|--------|------|------|
| **A. Editor Python** load NAVT via temp PMC → MeshDescription → UStaticMesh + Nanite | Reuses runtime loader knowledge | Needs editor; batch time |
| **B. Runtime one-shot cook in editor-only code** | Same as moored hull | Easy to leave in shipping by mistake |
| **C. External glTF/OBJ intermediate** | Tooling flexible | Extra format + materials |

**Recommended: A** — unattended editor script like buoys; materials from existing structure palette/MID path baked as static materials slots.

**C2. Runtime stream swap** (`NantucketStructuresSubsystem`)

- `FResidentTile`: prefer `UStaticMeshComponent` when manifest has cooked path; **fallback PMC** if missing (dev iteration).
- Load: `LoadObject<UStaticMesh>` + `SetStaticMesh`, Nanite allowed.
- Keep chebyshev stream + Lod0/Lod1 file choice.
- Night emissive: switch from MID on PMC to **MID on SMC** or material instance per tile (preserve `SetNightLighting`).

**C3. Streaming importance (light touch in C)**

- Harbor: keep LoadRadius 2 / Lod0Radius 1.
- Open sea (distance to island center or min structure tile > N km): **LoadRadius 1**, or skip structures until approach.
- Simple: if nearest structure tile center > e.g. 3 km, unload all / don’t load.

### Phase D — Terrain (no full Nanite required)

- Tighten Lod0Radius (3 → 1–2) and/or LoadRadius if profiled.
- Improve LOD1 bake density in sail-sim if LOD1 still heavy.
- Optional later: cook terrain StaticMesh (Nanite optional).

### Phase E — HLOD / far shore

- Far shoreline: single low-poly island mesh or HLOD proxy when no structure tiles resident.
- Can be a single pre-baked “Nantucket silhouette” mesh at island center — big visual win for open-sea shots without streaming town.

### Success criteria

- [ ] Harbor: structures draw as StaticMesh/Nanite; vert/draw cost down vs PMC.
- [ ] Night windows still work.
- [ ] Open sea: fewer or zero structure tiles loaded far from island.
- [ ] No regression: missing tiles fallback, no crash on stream.

### Risks

- Nanite + thin geometry / vertex colors: materials must be Nanite-compatible.
- Cook time / asset size in Content — git LFS or cooked-only locally.
- One-time cook must re-run when sail-sim rebakes tiles.

---

## Measurement protocol (before / after each phase)

1. Same PIE scene: harbor approach + open water mid-channel.
2. Wait streaming settle ~5 s.
3. Record: `stat unit` (GT/RT/GPU/Frame), FSailSimPerf chrome, optional `ProfileGPU`.
4. Note cvars: LocalTess, Lumen DS, RT on/off, structure tiles/verts.
5. Log row in `docs/perf/GPU_ABL_LOG.md` (create on first run).

**Do not** judge cloth opts while GPU is long pole.

---

## Agent roster (spool for implementation)

| Agent | Type | Owns | Files / outputs |
|-------|------|------|-----------------|
| **Lighting Agent** | general-purpose / optimizer | Item 1: ini ocean profile, AE stabilize, RT off (Mac), chrome labels | `DefaultEngine.ini`, `SailOceanSubsystem.cpp` (PPS), maybe DeviceProfiles |
| **Water Agent** | general-purpose | Item 2: clamp, defaults 60k, GameMode, chop/far normal if easy | `SailOceanSubsystem.cpp`, `SailBoatPawn.h`, `SailSimGameMode.cpp` |
| **Island Cook Agent** | general-purpose | Item 3 cook script + Nanite enable batch | `Scripts/cook_nantucket_structures_nanite.py` (new), manifest schema |
| **Island Runtime Agent** | general-purpose | Item 3 stream SM path, PMC fallback, night lights, open-sea radii | `NantucketStructuresSubsystem.*`, maybe terrain radii only |
| **Profiler / Docs Agent** | researcher / docs | A/B template, `docs/perf/*`, interpret user ProfileGPU pastes | `docs/perf/GPU_ABL_LOG.md`, update this plan status |
| **Verifier** | reviewer / check-work | After A+B and after 3a: consistency, no frustum reintro, build sanity | Diff review |

### Spawn graph

```
Lighting Agent ──┐
Water Agent ─────┼── Verifier(A+B) ── user PIE
Island Cook ─────┤                         │
                 └── Island Runtime ───────┴── Verifier(3a) ── PIE harbor
Profiler/Docs ── continuous (templates + log)
```

**Parallelism:** Lighting ∥ Water ∥ Island Cook start immediately after plan approval. Island Runtime depends on Cook manifest fields (can implement fallback-first so Runtime starts in parallel with stub paths).

### Orchestrator (main session)

- Merge conflicts rare (ini vs water vs terrain).
- User runs PIE (agents cannot drive editor GPU reliably).
- Do not spawn frustum/culling work.

---

## File change map (summary)

| Path | Items |
|------|--------|
| `Config/DefaultEngine.ini` | 1 |
| `Source/.../SailOceanSubsystem.cpp` | 1 (AE), 2 (clamp, optional normals) |
| `Source/.../SailBoatPawn.h` | 2 |
| `Source/.../SailSimGameMode.cpp` | 2 |
| `Source/.../NantucketStructuresSubsystem.*` | 3 |
| `Source/.../NantucketTerrainSubsystem.*` | 3b (radii only first) |
| `Scripts/cook_nantucket_structures_*.py` | 3 (new) |
| `Scripts/export_nantucket_structures_ue.py` | 3 (manifest fields) |
| `Content/Structures/nantucket/*` | 3 cooked assets + manifest |
| `docs/perf/*` | measurement |

---

## Definition of done (all three)

1. **Lighting:** ocean Lumen profile + stable AE; RT off on Mac unless measured win; sky/sun kept.
2. **Water:** local tess default in 400–700 m band; zone still large; continuous water under boat.
3. **Island:** structures load as cooked StaticMesh with Nanite for houses (or documented interim); PMC only fallback; stream radii favor harbor; terrain at least LOD-tightened.
4. Documented A/B: baseline vs after A+B vs after 3a with fps/GPU notes.
5. No frustum reintro; no night-window regression.

---

## Immediate next actions (after approval)

1. Spool **Lighting**, **Water**, **Island Cook** agents in parallel.
2. Land Phase A+B; Verifier; user PIE.
3. Land cook + runtime SM path; Verifier; harbor PIE.
4. Schedule Phase D/E/F from remaining ProfileGPU split.
