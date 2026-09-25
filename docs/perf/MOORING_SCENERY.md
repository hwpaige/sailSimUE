# Mooring scenery (Static HISM)

End state: **1 player / hero boat** + **the mooring field is scenery**.

## Budgets

| Knob | Default | Role |
|------|---------|------|
| `MaxBoats` | **1** | Hero / full-system boats only (player path; a few allowed). **Not** harbor fill or density. |
| `MaxNearFullBoats` | **1** | Live NearFull heroes. Stays aligned with `MaxBoats`. |
| `MooringSceneryInstanceCount` | **96** | **Harbor fill only.** Static HISM scenery budget (8–400). |

Harbor fill scales with `MooringSceneryInstanceCount` alone. `MaxBoats = 1` does not shrink the slot or HISM budget. Prefer-ON / DSF2 stay unchanged. Scenery instances use `StripMooredReflectionCost`, `CastShadow=false`, Static mobility, distance cull — **no** `SetForcedLodModel` / MinLOD crush.

## Draw

Scenery is the shared baked J/105 hull (`SM_MooredJ105_Hull`, PMC→SM) plus a spar HISM (engine cylinder, `MI_Yacht_Spar`). Materials are `/Game/Materials/Yacht/` only (`MI_Yacht_HullPaint` / Gelcoat / BootStripe / HullStripe / Antifoul / Deck / Cabin / Glass / Keel / Spar, and master `M_Yacht_HullPaint`). Paint variety is a few **shared** gelcoat MIDs on the **hull shell slot only** (white, navy, pale blue, cream). Deck and cabin stay the authored white topside MIs. Not a unique MID per boat, not one solid paint on every slot, and not `SM_Buoy`. HullPaint local-Z antifoul is not used on the HISM (that path darkens the whole shell).

The HISM actor is anchored at the harbor basin (same placement rule as EncAid ISMs). Yacht materials are flagged `Used with Instanced Static Meshes` before the first place. Without that flag, a cold PIE draws an empty field of buoys while the CPU instance count still reads 96. After instance adds, the cluster tree is built synchronously (`BuildTreeIfOutdated`). Heroes (`MaxBoats` / NearFull) stay full actors and are not these instances.

## Ops Prefer-ON gate

`RunPreferOnGate` / `ops_run_prefer_on_gate.py` frame `midHarborMoored` and pass on **scenery count** (floor ~64, soft target ~96 = `MooringSceneryInstanceCount`). JSON and status lines also report **heroes** (`MaxBoats`, `MaxNearFullBoats`, live NearFull count; defaults 1). The pass bar is not a hero cap.

The CPV PNG is a 1x HighResShot of the active editor/PIE Lit viewport during its Draw (`grab=HighResShot`, or `ViewportFramebuffer` if that client did not consume the request; `litOverride=false`), under `Saved/Screenshots/SailSim/` and `Saved/SailSim/last_lit_viewport_grab.png`. Re-run after this toolset is Live Compiled: `SailSim.RunPreferOnGate` or `python3 Scripts/ops_run_prefer_on_gate.py`. Gelcoat close-up: `CapturePlayerView` with `FramingPreset=gelcoatHull` (same grab). Do not score hull color from a scene-capture PNG. See `Plugins/SailSimToolset/README.md` (How Ops captures for look gates).

## World follow-up

World may add mesh variants later — **shared MAT slots only** (instance custom data OK for tint; avoid unique MIDs per instance).
