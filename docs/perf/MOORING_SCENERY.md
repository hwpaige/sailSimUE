# Mooring scenery (Static HISM)

End state: **1 player / hero boat** + **the mooring field is scenery**.

## Budgets

| Knob | Default | Role |
|------|---------|------|
| `MaxBoats` | **1** | Hero / full-system boats only (player path; a few allowed). **Not** harbor fill or density. |
| `MaxNearFullBoats` | **1** | Live NearFull heroes. Stays aligned with `MaxBoats`. |
| `MooringSceneryInstanceCount` | **96** | **Harbor fill only.** Static HISM scenery budget (8–400, shared hull SM + Yacht mats). |

Harbor fill scales with `MooringSceneryInstanceCount` alone. `MaxBoats = 1` does not shrink the slot or HISM budget. Prefer-ON / DSF2 stay unchanged. Scenery instances use `StripMooredReflectionCost`, `CastShadow=false`, Static mobility, distance cull — **no** `SetForcedLodModel` / MinLOD crush.

## What the HISM draws

The scenery component instances the baked J/105 hull static mesh (`SM_MooredJ105_Hull`, PMC→SM) with `/Game/Materials/Yacht/` slots. Topsides use `MI_Yacht_HullPaint` (gelcoat / boot / stripe / antifoul bands, same shader as hero boats). Deck, cabin, glass, and keel stay on their own yacht MIs. Spars are hero cylinder components and are not part of the hull bake, so the HISM does not instance them.

Fleet color is a few **shared** MIDs of those yacht MIs (`BaseColor` / `Roughness`, plus HullPaint’s `ColorTopsides` / `RoughTopsides`). Not unique material assets, and not one MID per instance.

ENC mooring buoys (`SM_Buoy`) are navaids. They are not harbor fill and not an AAA stand-in for the fleet. A Prefer-ON `midHarborMoored` frame that shows only buoys means the scenery HISM is not drawing the hull (count can still read ~96). Yacht materials must be compiled with the instanced-static-mesh usage flag or that HISM draws nothing.

## Ops Prefer-ON gate

`RunPreferOnGate` / `ops_run_prefer_on_gate.py` frame `midHarborMoored` and pass on **scenery count** (floor ~64, soft target ~96 = `MooringSceneryInstanceCount`). JSON and status lines also report **heroes** (`MaxBoats`, `MaxNearFullBoats`, live NearFull count; defaults 1). The pass bar is not a hero cap.

## World follow-up

World may add mesh variants later — **shared yacht MAT slots only** (instance custom data OK for tint; avoid unique MIDs per instance). Do not fill the harbor with `SM_Buoy` or other proxies.
