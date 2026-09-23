# Mooring scenery (Static HISM)

## Budgets

| Knob | Default | Role |
|------|---------|------|
| `MaxNearFullBoats` | 1 | Live multi-component heroes (NearFull radius) |
| `MaxBoats` | 16 | Legacy Prefer-ON full-system reference — **do not raise for fill** |
| `MooringSceneryInstanceCount` | **96** | Additive **Static HISM** harbor fill (shared hull SM + Yacht mats) |

Harbor AAA density comes from `MooringSceneryInstanceCount`, not `MaxBoats`. Prefer-ON / DSF2 stay unchanged. Scenery instances use `StripMooredReflectionCost`, `CastShadow=false`, Static mobility, distance cull — **no** `SetForcedLodModel` / MinLOD crush.

## Ops Prefer-ON gate

`RunPreferOnGate` / `ops_run_prefer_on_gate.py` frame `midHarborMoored` and expect **filled** mid-harbor (`moored >= ~2/3 of scenery budget`, floor 48–64), not the old exact `moored==16`.

## World follow-up

World may add mesh variants later — **shared MAT slots only** (instance custom data OK for tint; avoid unique MIDs per instance).
