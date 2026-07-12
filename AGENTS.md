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

**Phase 2 done** · **P3.1** multi-preset · **P4.0** multipoint buoyancy + Verlet cloth scaffold.  
Next: richer cloth/force measure, live loft sliders, FFT ocean (not water-pro code).  
Tracker: `../sail-sim/unreal-port/PROGRESS.md`.
