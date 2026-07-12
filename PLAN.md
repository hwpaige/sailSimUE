# SailSimUE — Port Plan (working doc)

**Owner:** Claude Code, driving the Unreal editor over MCP (`unreal` server, `http://127.0.0.1:8765/mcp`).
**Derived from:** the Grok "Port SailSim to Unreal Engine 5" plan. That plan is the strategic reference; this file is what we execute against day-to-day and supersedes `../sail-sim/unreal-port/PROGRESS.md` going forward.
**Last grounded against the live editor:** 2026-07-11 (map `/Game/Maps/SailSim_Ocean`).

## North star

Port the browser SailSim (Angular UI + FastAPI numpy loft + ~12k-line Three.js/WebGPU runtime + commercial `water-pro` FFT ocean) to a native **UE5** app, for the frame budget and native systems. Feature parity with better frame time — Unreal-native systems where they beat a 1:1 JS port.

### Load-bearing decisions (carried from Grok, still in force)
- **Do not port `threejs-water-pro` shaders/source into this repo.** Rebuild the ocean with UE-native FFT (plugin or custom Tessendorf); keep water-pro only as the visual/feature **bar**. License + rewrite risk.
- **C++ for dynamics & loft**, Blueprint/UMG for designer UI.
- **SI meters** in UE (1 uu = 1 cm); convert web presets from feet once. (Dynamics internals stay in the web model's imperial units, converted at the boundary — see `BoatDynamics.h`.)
- Physics source of truth: `../sail-sim/frontend/public/webgl-utils.sailing.js` (`hBoat`, cloth) + `../sail-sim/backend/sail_geom.py`.
- Keep the Python loft as a **golden oracle** (regression JSON) until C++ parity is proven.

## Current state (verified in-editor, not just from the tracker)

Phases 0–1 done (bootstrap, MCP). **Phase 2 vertical slice is essentially complete** — confirmed live:
- `ASailBoatPawn` renders a **procedural J/105 loft** (hull + deck + cabin + main + jib + mast + boom) from `Content/Data/j105_boat3d.json`, visible in-editor via `OnConstruction`.
- `FBoatDynamics` — Fossen-style 3-DOF VPP (`u,v,r` + heel), J/105 imperial defaults, apparent-wind, **sheet-ease** stub sail force; JSON `sailing`/`dims_ft` applied at BeginPlay.
- Controls: A/D helm, W/S sheet (boom swings), orbit spring-arm camera (RMB), water-surface float sampling, open-water spawn (forces XY ~2 km offshore, hides landscape island in game), on-screen debug HUD (speed/heel/heading/sheet%).
- Scene: single `SailBoatPawn` (dedupe worked), `WaterBodyOcean` + `WaterZone`, Nantucket-ish `Landscape` (WP, 8×8 streaming proxies + HLOD), `PlayerStart`, `WorldPartitionMiniMap`.
- Build `SailSimUEEditor` (Mac Development) succeeded at last session.

### Issues found while grounding (fix these before "P2.5 verified")
1. ✅ **FIXED (2026-07-11).** Duplicate environment actors → engine warned "multiple directional lights competing for forward shading." The env had been placed ~3× (**3 DirectionalLights, 3 SkyAtmospheres, 3 SkyLights, 2 ExponentialHeightFogs**). Deleted the 7 duplicates via MCP, kept one coherent set, `save_assets`. Verified: warning gone, exposure coherent (soft dusk), git shows exactly 7 external-actor deletions.
2. **In-flight uncommitted work** in the tree from the prior session: `Source/SailSimUE/Sailing/SailBoatPawn.cpp/.h` modified, plus one map external-actor `.uasset`. Review + commit before layering new changes so history stays clean. (Task 2.)
3. Cosmetic: the pawn's **editor** placement sits near origin on the checkerboard (no water there); harmless because BeginPlay teleports it to open water. Optionally move the editor placement onto the ocean for a nicer non-play view.

### Ocean water — diagnosed; zone enlarged (2026-07-12)
The "broken water" is **not** the ocean: `WaterBodyOcean_Main` has all the correct Water-plugin materials (`Water_Material_Ocean`, etc.). The problems:
1. **Placeholder OpenWorld landscape covers the ocean.** The checkerboard everywhere is `Landscape` using **`M_ProcGrid`** (the OpenWorld-template grid material), a flat sea-level terrain rendered by 64 WP streaming proxies (+ HLOD). It's hidden in *game* (boat code) but blankets the *editor* view and is why the boat needed the "spawn 2 km offshore" hack. Deleting it via MCP is impractical (100+ streaming/HLOD actors) — **best deleted in-editor**: select `Landscape` in the Outliner → Delete (removes all proxies at once).
2. **WaterZone was far too small.** `ZoneExtent` was ±256 m at origin while the boat spawns at (200000,150000) ≈ 2.5 km out → spawned outside the ocean entirely. **FIXED:** enlarged `ZoneExtent` to (1000000,1000000) ≈ ±5 km and saved, so the ocean now covers the spawn + sailing area.
3. Once the landscape is gone, simplify `SailBoatPawn` open-water spawn to ~origin (zone center) and drop the landscape-hide hack.

Verification was hampered by the dusk lighting + the low chase camera — fixing the camera (Task 6) is the fastest way to actually see the boat on the water.

## Immediate backlog (execution queue)

| # | Task | Where | Risk | Verify |
|---|------|-------|------|--------|
| 1 | ✅ **DONE** — Env dedup: deleted 7 duplicate DirectionalLight/SkyAtmosphere/SkyLight/Fog actors, kept one coherent set, saved | MCP (map) | done | ✅ warning gone, exposure clean, 7 external-actors removed |
| 2 | Review + commit in-flight `SailBoatPawn.cpp/.h` + this session's `.mcp.json` / config INIs | git | low | `git status` clean; editor build OK |
| 3 | ✅ **Hull mesh shading fixed** — flipped inward normals outward + double-sided; compiled & verified (Unlit shows correct light base color). PBR master (glossy hull gelcoat + matte two-sided sail) still TODO beyond flat BasicShapeMaterial | `BoatMeshFromJson.cpp` | done | ✅ Unlit capture |
| 4 | **P3 HUD** — replace `DrawHud` debug text with a UMG widget (speed/heel/heading/AWA/sheet) | UMG + C++ bind | med | PIE: widget shows live values |
| 5 | ✅ **P2.5 sign-off (partial)** — PIE ran clean: no NaN/crash, J/105 dynamics loaded from JSON, deduped env confirmed in outliner. Still want an input-driven helm/sheet feel check | PIE | done | ✅ log + capture |
| 6 | **Chase camera framing** — PIE cam sits low/close/backlit; raise orbit pitch + pull back for a readable sailing view | `SailBoatPawn.cpp` | low (recompile) | PIE capture |
| 7 | Dynamics calibration — steady-state speed/leeway/heel vs web + `tools/j105-*-calibrate.mjs` (~10% band) | C++ + oracle | med | golden compare |
| 8 | **Lighting / mood** — hull reads dark under low dusk sun + modest ambient. Decide: brighter ambient (boost SkyLight_Main) + higher sun for a "readable" look, or keep the dusk mood. Real lever for "brighter hull" | map actors (MCP) | low | Lit capture |

### C++ recompile loop — SOLVED ✅
Editor MCP can't trigger a C++ build, so Claude drives it via **computer-use**:
1. Focus editor → click the **viewport** (must have focus) → press **`Cmd+Option+Shift+P`** ("Recompile Game Code" — the Mac binding; `Ctrl+Alt+F11` is eaten by macOS and there's no toolbar Compile button in 5.8).
2. Wait for the "Compiling C++ Code" toast to clear (~30–90 s; slow under memory pressure). Confirm a new `Binaries/Mac/libUnrealEditor-SailSimUE-*.dylib` appeared.
3. MCP `load_level /Game/Maps/SailSim_Ocean` to re-run `OnConstruction` and apply the patch, then `CaptureViewport` to verify.

### Hull "darkness" — diagnosed (NOT a mesh bug)
Unlit view shows the hull is correctly **light cream** (base color fine). It looks near-black in **Lit** only because the dusk sun is low and its topsides face away, with modest sky ambient (the env dedup removed the old excess-ambient wash). Fixed a real **inward-normal defect** in the hull mesh (2968/2970 tris were inward-wound → flipped outward, so it shades correctly *when* sun-lit). Making it read bright in the Lit dusk view is a **lighting choice**, not a mesh fix — see Task 8.

## Roadmap after the vertical slice (from Grok, condensed)

- **P3 — Generative boat builder:** port `sail_geom.py` loft → `SailCore` C++; rebuild meshes on spec change; Python golden JSON regression; presets J/105, Endeavour, Melges 24, Cruiser 36; Plans tab.
- **P4 — Cloth + trim + sail materials:** main/jib grids from loft; fabric/shear/bend/batten/sheet/vang/outhaul constraints; live API matching `SailEngineService`; auto-trim. GPU cloth if viable, else C++ solver + LOD.
- **P5 — Ocean (FFT), sea state, FX:** decision gate — production FFT plugin (default) vs custom Tessendorf; `IOceanHeightSample` abstraction over the heightfield; sea-mood data assets; Niagara wake/spray/foam/rain; underwater state; quality tiers. **water-pro = checklist only.**
- **P6 — Nantucket nav world:** offline DEM/chart/structure pipeline → Landscape/WP/Nanite; port `nav-geo.js`; UMG chart minimap, waypoints, autopilot hdg/awa/nav.
- **P7 — Full UI / audio / polish:** UMG = all Angular control sections; MetaSounds moods; SaveGame; macOS packaging; scalability tiers Low/Med/High/Ultra.

## How we work
- **MCP-first:** drive the editor via the `unreal` server; write C++/config to disk; recompile via editor Live Coding / build.
- **Verify visually:** `CaptureViewport` for editor look; `StartPIE` + capture + log read for behavior. Don't mark a step done on "build succeeded" alone.
- **Ask the user only for:** credentials, GUI-only actions MCP can't do, genuine design choices, and final feel/look verification.
- **Never** copy `threejs-water-pro/src` into this repo.
- Keep this file current: check tasks off, append findings.
