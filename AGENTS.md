# SailSimUE — agent notes

Port of [sail-sim](../sail-sim) sailing simulator to Unreal Engine 5.8.

## Source of truth (web reference)

- Physics / cloth / VPP: `../sail-sim/frontend/public/webgl-utils.sailing.js`
- Geometry loft: `../sail-sim/backend/sail_geom.py`
- Ocean **features** checklist: `../sail-sim/threejs-water-pro/docs/**` — do **not** port water-pro source/shaders
- Progress tracker: `../sail-sim/unreal-port/PROGRESS.md`

## Conventions

- C++ for dynamics and loft; Blueprint/UMG for designer UI
- Units: prefer SI meters in UE; convert presets from feet once
- MCP: Unreal editor server at `http://127.0.0.1:8765/mcp` (not 8000)
- Never copy `threejs-water-pro` source into this repo

## Current phase

**P2 done** · **P3** presets/scale/oracle · **P4** cloth scaffold · **P5** ocean interface (Gerstner backend; FFT later).  
Ocean: world-space height field; local tessellation is render-only. Never port water-pro shaders.  
Trackers: `../sail-sim/unreal-port/PROGRESS.md`, `PHASE5_OCEAN.md`.
