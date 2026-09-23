# Mooring scenery (Static HISM)

End state: **1 player / hero boat** + **the mooring field is scenery**.

## Budgets

| Knob | Default | Role |
|------|---------|------|
| `MaxBoats` | **1** | Hero / full-system boats only (player path; a few allowed). **Not** harbor fill or density. |
| `MaxNearFullBoats` | **1** | Live NearFull heroes. Stays aligned with `MaxBoats`. |
| `MooringSceneryInstanceCount` | **96** | **Harbor fill only.** Static HISM scenery budget (8–400, shared hull SM + Yacht mats). |

Harbor fill scales with `MooringSceneryInstanceCount` alone. `MaxBoats = 1` does not shrink the slot or HISM budget. Prefer-ON / DSF2 stay unchanged. Scenery instances use `StripMooredReflectionCost`, `CastShadow=false`, Static mobility, distance cull — **no** `SetForcedLodModel` / MinLOD crush.

## Ops Prefer-ON gate

`RunPreferOnGate` / `ops_run_prefer_on_gate.py` frame `midHarborMoored` and pass on **scenery count** (floor ~64, soft target ~96 = `MooringSceneryInstanceCount`). JSON and status lines also report **heroes** (`MaxBoats`, `MaxNearFullBoats`, live NearFull count; defaults 1). The pass bar is not a hero cap.

## World follow-up

World may add mesh variants later — **shared MAT slots only** (instance custom data OK for tint; avoid unique MIDs per instance).
