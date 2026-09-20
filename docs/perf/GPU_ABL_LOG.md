# GPU A/B measurement log — AAA lighting / water / island stack

Log rows for PIE A/B against the plan in [`AAA_STACK_IMPLEMENTATION_PLAN.md`](./AAA_STACK_IMPLEMENTATION_PLAN.md). Fill after each phase; do not judge cloth/GT opts while GPU is the long pole.

---

## Protocol

1. Same PIE scene: **harbor** approach **or** **open** water mid-channel (note which).
2. Wait streaming settle ~5 s.
3. Record: `stat unit` (GT / RT / GPU / Frame), FSailSimPerf chrome, optional `ProfileGPU`.
4. Note knobs: LocalTess, Lumen SPG DS, RT on/off, structure tiles / verts.
5. Add a row below.

**Do not** compare cloth opts while GPU is long pole.

---

## How to capture

| Source | What to read |
|--------|----------------|
| **`stat unit`** | Console → Game / Draw / GPU / Frame (ms). Map to columns: `GT_ms`, `RT_ms`, `GPU_ms`, `frame_ms`. FPS ≈ 1000 / frame_ms (or use `stat fps`). |
| **FSailSimPerf chrome** | Settings → **PERFORMANCE** (or on-screen chrome). Wall / GT / RT / GPU EMAs, bottleneck label, structure tile/vert counts when streaming. Cross-check unit times. |
| **`ProfileGPU`** | Console → hierarchical GPU ms (Lumen, reflections, water, shadows). Optional; paste peak categories into **notes**. |
| **Scene knobs** | Local tess diameter (cm); `r.Lumen.ScreenProbeGather.DownsampleFactor`; `r.RayTracing`; resident structure tiles / verts from chrome or subsystem log. |

**Tips**

- Capture both **harbor** and **open** when a phase can affect land cost differently.
- Prefer settled PIE after streaming; avoid hitch frames.
- One config change set per row (or note multi-change in **notes**).

---

## Log table

| date | scene | fps | frame_ms | GT_ms | RT_ms | GPU_ms | LocalTess_cm | Lumen_SPG_DS | RT_on | struct_tiles | struct_verts | notes |
|------|-------|-----|----------|-------|-------|--------|--------------|--------------|-------|--------------|--------------|-------|
| 2026-07-19 | open | ~18 | ~55 | ~18 | | ~52–55 | ~120000 | 32 | True | | | **Baseline** Mac PIE; open ocean / harbor similar class; GPU/RT long pole |
| 2026-07-19 | harbor | ~18 | ~55 | ~18 | | ~52–55 | ~120000 | 32 | True | ~… | ~2.2M? | **Baseline** Mac PIE (same ballpark as open; refine when re-measured) |
| 2026-07-19 | harbor | **~9** | **~108–110** | **~29** | **~110** | **~107–110** | **120000** | 48? (ini) | False? (ini) | **17** | **2221k** | **REPROFILE — A+B C++ NOT LOADED** (binary 11:30; sources 12:05). Live log: `localTess=120000`. T=36/1088k, moored=48, aids=305. Bottleneck **GPU**. Sails GT~7ms, Moored GT~3.5ms. **1727** terrain parse fails (nI=0 tiles retried every stream tick). Cooked SM=0. See profile notes below. |
| | | | | | | | | | | | | **After Phase A+B** (water tess 400–700 m + lighting ocean profile / AE / RT decision) |
| | open | | | | | | | | | | | after A+B + **Live Coding / restart** — expect localTess≈60000 |
| | harbor | | | | | | | | | | | after A+B + LC — same |
| | | | | | | | | | | | | **After Phase C2** (structures cooked SM + Nanite stream path) |
| | open | | | | | | | | | | | after C2 |
| | harbor | | | | | | | | | | | after C2 |
| | | | | | | | | | | | | **Optional Phase D** (terrain LOD tighten) |
| | open | | | | | | | | | | | after D |
| | harbor | | | | | | | | | | | after D |
| | | | | | | | | | | | | **Optional Phase E** (HLOD / far shore importance) |
| | open | | | | | | | | | | | after E |
| | harbor | | | | | | | | | | | after E |
| | | | | | | | | | | | | **Optional Phase F** (water reflections polish if still Lumen-hot) |
| | open | | | | | | | | | | | after F |
| | harbor | | | | | | | | | | | after F |

Leave unused optional rows blank if that phase is skipped.

---

## Column reference

| Column | Meaning |
|--------|---------|
| `scene` | `harbor` or `open` |
| `fps` / `frame_ms` | Average FPS and wall frame time |
| `GT_ms` / `RT_ms` / `GPU_ms` | Game / render / GPU unit times |
| `LocalTess_cm` | Water local tessellation diameter (cm) |
| `Lumen_SPG_DS` | `r.Lumen.ScreenProbeGather.DownsampleFactor` |
| `RT_on` | Hardware ray tracing enabled |
| `struct_tiles` / `struct_verts` | Resident Nantucket structure tiles / verts |
| `notes` | Phase tag, ProfileGPU peaks, cvar deltas, anomalies |

---

## 2026-07-19 live re-profile (why it still feels terrible)

**Verdict: changes mostly did not apply in the running process.** Source was edited ~12:03; editor module binary was still **11:30**. Live log proves water tess still **1.2 km**.

### Settled harbor numbers (from `LogSailSim: [perf]`)

| Metric | Value |
|--------|-------|
| Wall / FPS | **~108–110 ms / ~9 fps** (worse than ~18 fps baseline) |
| GPU | **~107–110 ms** — long pole |
| GT | **~29 ms** (Sails ~7, Moored ~3.5, Terrain ~0.5, Water ~0.2) |
| GTwait | **~78–83 ms** (GT blocked on GPU) |
| Terrain | **36 tiles / 1.09M verts** (loadR=3, all LOD0 in disc) |
| Structures | **17 tiles / 2.22M verts** PMC (harbor LOD0 monsters ~390–543k each) |
| Moored / aids | **48** boats + **305** aids |
| Water | **`localTess=120000`** extent=240000 — **Phase A not running** |
| Cooked SM | **0** — Phase C content not cooked |

### Why A/B/C “landed on disk” but not in PIE

1. **C++ not hot-reloaded** — `Binaries/Mac/libUnrealEditor-SailSimUE.dylib` stayed at 11:30; UBT later wrote `libUnrealEditor-SailSimUE-0001.dylib` for Live Coding, but modules still point at the base dylib until **Live Coding patch** or **editor restart**.
2. **Cook never ran** — no `Content/Structures/nantucket/Cooked/*`; manifest has no `staticMesh` → always PMC.
3. **`DefaultEngine.ini` Lumen/RT** may reload with map/project, but the biggest water win is C++ defaults that were still **120000**.

### Architectural cost centers (still fundamental even after A/B apply)

| Rank | System | Evidence | Fix path |
|------|--------|----------|----------|
| 1 | **GPU overall** | 107ms GPU vs 29ms GT | After tess/Lumen apply: ProfileGPU (Lumen / water / VSM / Substrate) |
| 2 | **Structure PMC density** | 2.2M verts classic raster | Editor cook → Nanite SM (C1) |
| 3 | **Water local tess 1.2 km** | log `localTess=120000` | Live Coding so 60k ships |
| 4 | **Terrain disc + bad tiles** | 36 tiles full LOD0; **1727** `nI=0` parse fails/session | Blacklist failed tiles (code fix); re-bake broken meshes; tighten radii later |
| 5 | **Moored + aids** | 48 + 305 always in GT/draw | Distance/cull budgets (next) |
| 6 | **Sails cloth GT** | ~7ms | Only after GPU drops |

### Immediate actions

1. In editor: **Cmd+Option+Shift+P** (Live Coding / Recompile Game Code) **or restart editor** so `localTess≈60000` and structure dual-path load.
2. Confirm log line: `Flat ocean: ... localTess=60000` (not 120000).
3. Optional: run structure cook for Nanite (still the harbor GPU win).
4. Re-fill A/B rows after LC.

---

## 2026-07-19 architecture pivot (60 fps, keep Lumen quality)

**User request:** turn **off** aggressive Lumen GI downsampling; hit **60 fps** while staying beautiful — dig architecture, not cvars.

### Live notes after C++ reload
- `localTess=60000` confirmed applied.
- Wall **333 ms / 3 fps** with GPU **~52–65 ms** while editor unfocused → classic **idle/background throttle**, not real GPU. Focused harbor earlier: **~9 fps / ~108 ms GPU**.

### Root cause ranking (not Lumen DS)

| Rank | System | Problem |
|------|--------|---------|
| 1 | Structures | 2.2M **PMC** verts, shadows on, never cull; LOD0 harbor tiles 390–543k each |
| 2 | Water tess | Was 1.2 km until reload; now 600 m |
| 3 | Terrain | loadR=3 all LOD0 ≈ 1M verts |
| 4 | Moored | **48** unique multi-component J/105s + point lights 50 m |
| 5 | Clouds / skylight | 120 km march + real-time sky capture |
| 6 | ENC aids | ISM OK but **never cull** + shadows + 305 in disc |

### Code changes landed (this pass)

| Change | Before → After |
|--------|----------------|
| Lumen SPG DS | 48 → **16** (Epic quality) |
| Probe budget | 75 → **100** |
| ShortRangeAO DS | 2 → **1** |
| MaxBoats | 48 → **16** |
| Moored load radius | 1.4 km → **0.9 km** |
| Moored point lights | always → **off** (emissive lantern only; `bAnchorPointLights`) |
| Enc aid cull | never / 50 km → **1.2–2.2 km fade**, shadows **off** |
| Enc load radius | 1.2 km → **0.9 km** |
| Structure LoadR / Lod0R | 2 / 1 → **1 / 0** (center tile full, shell LOD1) |
| Terrain LoadR / Lod0R | 3 / 3 → **2 / 1** |
| Cloud TraceMax | 120 km → **50 km** |

### Still the #1 content win
Run structure Nanite cook — dual-path runtime is ready; without assets, harbor stays PMC.

### Note on 3 fps readings
If wall is **exactly 333 ms**, focus the editor / PIE window before judging. Real target is focused **GPU ≤ ~16 ms**.

## 2026-07-19 package landed: split bake + cook + HISM + skylight

| Item | Status |
|------|--------|
| Split bake | **Done** — buildings vs trees; ~1.19M verts (was ~2.95M) |
| Nanite cook | **Done** — 29 LOD0 SM + 12 VEG under `Content/Structures/nantucket/Cooked/` |
| Runtime veg layer | **Code in -0005** — `fileVeg` / `staticMeshVeg`, VegLoadDistanceCm |
| Moored HISM | **Code in -0005** — MaxBoats=40, near full ≤80m, mid HISM ≤250m |
| SkyLight | **Code in -0005** — no real-time capture; RequestSkyLightRecapture on change |

**Apply:** Live Coding / restart for `-0005`, then PIE. Expect chrome `struct: … sm=N` and `moored near=X hism=Y (cap 40)`.

