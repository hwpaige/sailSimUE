# GPU A/B log

Stuck render decisions after a measured Prefer-ON (or equivalent) A/B. One row per stick.

| Date | Scene | Change | Design | Frame | Stick | Caveat |
|------|-------|--------|--------|-------|-------|--------|
| 2026-09-22 | Prefer-ON harbor `RunPreferOnGate` midHarborMoored (after a2711e1) | Ini reconcile of runtime SetCVars: `r.Lumen.ScreenProbeGather.DownsampleFactor` 32→16, `r.RayTracing`=False, `r.LumenScene.SurfaceCache.CardCapturesPerFrame`=32, `r.LumenScene.Radiosity.UpdateFactor`=256. Reflections DownsampleFactor stays 2; Allow stays 1. | Design density PASS (hitch CPV) | ~30.5 ms / ~32.7 fps vs ~36.5 ms band | YES | Reflections DSF 4 was verify FAIL and is not stuck. MaxBoats hero path stays 16. |
