# SailSimUE — Port Plan (working doc)

**Owner:** Claude Code, driving the Unreal editor over MCP (`unreal` server, `http://127.0.0.1:8765/mcp`).
**Derived from:** the Grok "Port SailSim to Unreal Engine 5" plan. That plan is the strategic reference; this file is what we execute against day-to-day and supersedes `../sail-sim/unreal-port/PROGRESS.md` going forward.
**Last grounded against the live editor:** 2026-07-12 (map `/Game/Maps/SailSim_Ocean`, PIE open-ocean verified).

## North star

Port the browser SailSim (Angular UI + FastAPI numpy loft + ~12k-line Three.js/WebGPU runtime + commercial `water-pro` FFT ocean) to a native **UE5** app, for the frame budget and native systems. Feature parity with better frame time — Unreal-native systems where they beat a 1:1 JS port.

### Load-bearing decisions (carried from Grok, still in force)
- **Do not port `threejs-water-pro` shaders/source into this repo.** Rebuild the ocean with UE-native FFT (plugin or custom Tessendorf); keep water-pro only as the visual/feature **bar**. License + rewrite risk.
- **C++ for dynamics & loft**, Blueprint/UMG for designer UI.
- **SI meters** in UE (1 uu = 1 cm); convert web presets from feet once. (Dynamics internals stay in the web model's imperial units, converted at the boundary — see `BoatDynamics.h`.)
- Physics source of truth: `../sail-sim/frontend/public/webgl-utils.sailing.js` (`hBoat`, cloth) + `../sail-sim/backend/sail_geom.py`.
- Keep the Python loft as a **golden oracle** (regression JSON) until C++ parity is proven.

## Current state (verified in-editor, 2026-07-12)

Phases 0–2 done. **P3/P4 partial. P5.2 open-ocean foundation done** (PIE verified):
- Single session `SailBoatPawn_0` at open water `(95000, 72000)`; map boat + Landscape proxies removed.
- Mooring model: **1 hero** (`MaxBoats` = 1, `MaxNearFullBoats` = 1) + harbor fill `MooringSceneryInstanceCount` = 96 (baked J/105 hull HISM + `/Game/Materials/Yacht/` slots, not `SM_Buoy`). Prefer-ON density gate reports scenery (floor ~64 / soft ~96) and heroes. `MaxBoats` is not harbor fill (`docs/perf/MOORING_SCENERY.md`).
- Native Water + **28-wave Gerstner**, material polish, local tess ~140 km, backend `GerstnerWaterBody`.
- Cloth: Verlet + shear/batten springs; fill × scale into VPP.
- Presets 1–4, LOA scale −/=, Python re-loft **R**.
- Chase cam raised; heel golden retuned.
- **MCP tip:** after editor crash, `CrashReportClient` can steal port **8765** — kill it, then `ModelContextProtocol.StartServer 8765`.

**P4 note (sheet/vang → VPP):** `Vang01`/`Outhaul01` now scale Cl (and mild Cd) inside `FBoatDynamics::ComputeSailForceStub` to match web trim feel; `SheetEase` IdealEase path unchanged. `UpdateSailCloth` already pushes Outhaul01/Vang01 and passes SheetEase into `MainCloth.Step` each tick.

## Immediate backlog (execution queue)

| # | Task | Where | Risk | Verify |
|---|------|-------|------|--------|
| 1 | ✅ Env dedup | MCP | done | ✅ |
| 2 | Commit in-flight open-ocean + cloth + spawn map/actor deletions | git | low | clean history |
| 3 | ✅ Hull normals / double-sided | `BoatMeshFromJson` | done | ✅ |
| 4 | **P3 HUD UMG** — replace debug canvas with UMG | UMG + C++ | med | PIE widget |
| 5 | ✅ P2.5 PIE sign-off | PIE | done | ✅ |
| 6 | ✅ Chase camera raised (−28°, higher arm offsets) | `SailBoatPawn` | done | re-verify after 0042 |
| 7 | Dynamics calibration vs web + j105 calibrate scripts | C++ | med | golden ~10% |
| 8 | Lighting / sea moods (P5.3) | map + data | low | Lit capture |
| 8b | AAA lighting/water/island perf stack (A–C in progress) | `docs/perf/AAA_STACK_IMPLEMENTATION_PLAN.md` | med | `docs/perf/GPU_ABL_LOG.md` |
| 9 | ✅ P5.2 open ocean (Gerstner + no island + single boat) | ocean subsystem | done | PIE capture |
| 9b | **In progress:** continuous ocean — Gerstner to horizon, soft near→far normals, localTess 400–700 m hard cap (no water-pro) | `SailOceanSubsystem` | med | SailSim_Ocean PIE + CaptureViewport past tess edge |
| 10 | ✅ P4 trim: Vang01/Outhaul01 → ComputeSailForceStub Cl/Cd (sheet IdealEase unchanged; cloth still gets sheet/vang/outhaul each tick) | `BoatDynamics.cpp` | low | PIE sheet/vang/outhaul vs speed/heel |

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
  - ✅ Terrain streaming (`UNantucketTerrainSubsystem`)
  - ✅ Structure streaming (`UNantucketStructuresSubsystem`) — OSM houses, heroes, grass/trees/hydrangeas, ENC lights (docs/NANTUCKET_STRUCTURES.md)

- **P7 — Full UI / audio / polish:** UMG = all Angular control sections; MetaSounds moods; SaveGame; macOS packaging; scalability tiers Low/Med/High/Ultra.

## How we work
- **MCP-first:** drive the editor via the `unreal` server; write C++/config to disk; recompile via editor Live Coding / build.
- **Verify visually:** `CaptureViewport` for the free editor camera; `SailSimToolset.StartPIE` + `CapturePlayerView` (boom socket, log `cam=SpringArmSocket`) for the possessed boat. Don't mark a step done on "build succeeded" alone.
- **Ask the user only for:** credentials, GUI-only actions MCP can't do, genuine design choices, and final feel/look verification.
- **Never** copy `threejs-water-pro/src` into this repo.
- Keep this file current: check tasks off, append findings.

| mcp | **In progress:** SailSimToolset gate tools — CapturePlayerView (PIE boom Lit), StartPIE/EnsurePIE, GetPerfSnapshot, SetCVars, ExecuteConsole, ProfileGPUDump, LoadMap | `Plugins/SailSimToolset` | med | Prefer-ON PIE CapturePlayerView shows hull gelcoat |
